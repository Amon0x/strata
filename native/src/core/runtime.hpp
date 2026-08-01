#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <strata/strata.h>

#include "core/allocator.hpp"
#include "core/arena.hpp"
#include "core/diagnostics.hpp"
#include "core/identity.hpp"
#include "runtime/host.hpp"
#include "runtime/application.hpp"

namespace strata::core {

struct SnapshotData final {
    std::uint64_t generation;
    std::int64_t time_nanoseconds;
    std::uint64_t last_stable_identity;
};

class Runtime final {
public:
    Runtime(
        HostAllocator allocator,
        strata_clock clock,
        strata_diagnostic_sink diagnostics,
        std::uint64_t stable_identity_seed
    );

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    [[nodiscard]] HostAllocator allocator() const noexcept;
    [[nodiscard]] AllocatorStatistics allocator_statistics() const noexcept;
    [[nodiscard]] Diagnostics& diagnostics() noexcept;
    [[nodiscard]] strata_result next_identity(std::uint64_t& identity) noexcept;
    [[nodiscard]] strata_result create_snapshot(SnapshotData& snapshot) noexcept;
    [[nodiscard]] bool publish_host_snapshot(
        std::string id,
        std::uint64_t generation,
        const data::JsonValue& values
    );
    [[nodiscard]] const std::shared_ptr<const runtime::HostSnapshot>& host_snapshot() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> host_snapshot_generation(
        std::string_view id
    ) const noexcept;
    [[nodiscard]] std::optional<runtime::Value> read_host_value(std::string_view path) const;
    void configure_application(
        std::string id,
        const data::JsonValue& registry,
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
    HostAllocator allocator_;
    HostMemoryResource memory_resource_;
    Arena scratch_arena_;
    Diagnostics diagnostics_;
    strata_clock clock_;
    StableIdentitySource identities_;
    runtime::HostStore host_store_;
    std::shared_ptr<const runtime::ApplicationBundle> application_bundle_;
    std::unique_ptr<runtime::ApplicationContext> application_;
    std::uint64_t generation_ = 0U;
    std::int64_t last_time_nanoseconds_ = 0;
    bool has_sampled_time_ = false;
};

} // namespace strata::core
