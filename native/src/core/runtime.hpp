#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/allocator.hpp"
#include "base/arena.hpp"
#include "base/identity.hpp"
#include "base/pending.hpp"
#include "base/status.hpp"
#include "base/task_pool.hpp"
#include "core/diagnostics.hpp"
#include "resource/runtime_images.hpp"
#include "runtime/application.hpp"
#include "runtime/host.hpp"

namespace strata::core {

/** Caller-owned monotonic clock. The callback may throw; the runtime contains it. */
struct Clock final {
    void* user_data = nullptr;
    std::int64_t (*now_nanoseconds)(void* user_data) = nullptr;
};

struct SnapshotData final {
    std::uint64_t generation;
    std::int64_t time_nanoseconds;
    std::uint64_t last_stable_identity;
};

enum class HostPublicationStatus : std::uint32_t { pending, adopted, failed, cancelled };

/**
 * Retained admission receipt, independent of runtime lifetime. Only the serialized owner finishes
 * it; terminal fields are immutable and published by status. Cancellation never enters the runtime.
 */
class HostPublication final {
public:
    HostPublication(std::string id, std::uint64_t generation);
    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] HostPublicationStatus status() const noexcept {
        return status_.load(std::memory_order_acquire);
    }
    [[nodiscard]] base::Result result() const noexcept {
        return status() == HostPublicationStatus::pending ? base::Result{} : result_;
    }
    [[nodiscard]] std::string_view message() const noexcept {
        return status() == HostPublicationStatus::pending ? std::string_view{} : message_;
    }
    void cancel() noexcept { cancel_requested_.store(true, std::memory_order_release); }

private:
    friend class Runtime;
    void finish(HostPublicationStatus status, base::Result result = {}, std::string message = {});
    std::string id_;
    std::uint64_t generation_;
    std::atomic<HostPublicationStatus> status_{HostPublicationStatus::pending};
    std::atomic_bool cancel_requested_{false};
    base::Result result_;
    std::string message_;
};

class Runtime final {
public:
    Runtime(
        base::HostAllocator allocator,
        Clock clock,
        DiagnosticSink diagnostics,
        std::uint64_t stable_identity_seed,
        base::TaskExecution execution = base::TaskExecution::deterministic,
        base::TaskOwner::Wake wake = {}
    );
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] base::HostAllocator allocator() const noexcept;
    [[nodiscard]] base::AllocatorStatistics allocator_statistics() const noexcept;
    [[nodiscard]] Diagnostics& diagnostics() noexcept;
    [[nodiscard]] base::TaskOwner& tasks() noexcept { return tasks_; }
    [[nodiscard]] const base::TaskOwner& tasks() const noexcept { return tasks_; }
    [[nodiscard]] base::Result next_identity(std::uint64_t& identity) noexcept;
    [[nodiscard]] base::Result create_snapshot(SnapshotData& snapshot) noexcept;
    [[nodiscard]] std::shared_ptr<HostPublication> publish_host_snapshot(
        std::string id,
        std::uint64_t generation,
        std::string values_json,
        bool inline_execution = false
    );
    /** Nonwaiting owner adoption, also called at the start of each Surface frame. */
    void poll_host_snapshots();
    [[nodiscard]] std::optional<std::uint64_t> accepted_host_snapshot_generation(
        std::string_view id
    ) const noexcept;
    [[nodiscard]] const std::shared_ptr<const runtime::HostSnapshot>& host_snapshot() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> host_snapshot_generation(
        std::string_view id
    ) const noexcept;
    [[nodiscard]] std::optional<runtime::Value> read_host_value(std::string_view path) const;
    [[nodiscard]] resource::RuntimeImageStore& images() noexcept { return images_; }
    [[nodiscard]] const resource::RuntimeImageStore& images() const noexcept { return images_; }
    void configure_application(
        std::string id,
        const data::JsonValue* schemas,
        std::span<const data::JsonValue> extension_declarations = {}
    );
    [[nodiscard]] bool has_application() const noexcept;
    [[nodiscard]] runtime::ApplicationContext& application();
    [[nodiscard]] const runtime::ApplicationContext& application() const;
    [[nodiscard]] runtime::ActivationResult compile_and_activate(
        const compiler::ModuleSource& entry,
        const compiler::ModuleLoader& loader,
        std::uint64_t generation
    );
    [[nodiscard]] runtime::ActivationResult activate_compiled(
        std::span<const std::uint8_t> artifact,
        std::uint64_t generation
    );

private:
    struct PendingHostPublication final {
        std::shared_ptr<HostPublication> receipt;
        base::Pending<std::shared_ptr<const runtime::HostSnapshot>> prepared;
        bool application_target = false;
    };
    void adopt_host_snapshots();
    void fail_host_snapshot(HostPublication& receipt, const std::exception_ptr& failure);

    base::HostAllocator allocator_;
    base::HostMemoryResource memory_resource_;
    base::Arena scratch_arena_;
    Diagnostics diagnostics_;
    base::TaskOwner tasks_;
    Clock clock_;
    base::StableIdentitySource identities_;
    runtime::HostStore host_store_;
    std::map<std::string, std::uint64_t, std::less<>> accepted_host_generations_;
    std::deque<PendingHostPublication> host_publications_;
    resource::RuntimeImageStore images_;
    std::shared_ptr<const runtime::ApplicationBundle> application_bundle_;
    std::unique_ptr<runtime::ApplicationContext> application_;
    /** Publication is synchronized for worker-safe mailbox admission during configuration. */
    std::atomic<runtime::ApplicationContext*> published_application_{nullptr};
    std::uint64_t generation_ = 0U;
    std::int64_t last_time_nanoseconds_ = 0;
    bool has_sampled_time_ = false;
};

} // namespace strata::core
