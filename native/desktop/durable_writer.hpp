#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace strata::desktop {

/**
 * Background atomic file writer for durable application state.
 *
 * Writes are coalesced per path: only the newest payload of a path is written. Each write goes to
 * a sibling temporary file that is flushed to disk before a write-through rename replaces the
 * destination. A rename or open that another process briefly blocks (sharing, lock, or access
 * violations, typically antivirus or indexers) is retried with exponential backoff.
 *
 * Failures are isolated per path. A path that still fails is reported through the failure
 * callback and parked with its newest payload, which `pending` keeps serving so the session never
 * reads stale state; the next payload enqueued for that path retries it. Other paths keep writing
 * and the worker never stops early.
 */
class DurableWriter final {
  public:
    /** Called on the writer thread with the path and reason of a write that gave up. */
    using FailureReport =
        std::function<void(const std::filesystem::path& path, std::string_view message)>;

    struct Options final {
        /** Attempts per payload, including the first, before a transient failure is reported. */
        unsigned attempts = 6U;
        /** Delay before the first retry; each later retry doubles it. */
        std::chrono::milliseconds initial_backoff{10};
    };

    explicit DurableWriter(FailureReport report = {});
    DurableWriter(FailureReport report, Options options);
    /** Writes every payload still pending, then stops the worker. */
    ~DurableWriter();
    DurableWriter(const DurableWriter&) = delete;
    DurableWriter& operator=(const DurableWriter&) = delete;

    void enqueue(std::filesystem::path path, std::string payload);

    /** Newest payload of `path` not yet on disk, including one parked after a failure. */
    [[nodiscard]] std::optional<std::string> pending(const std::filesystem::path& path) const;

    /** Waits until every path is written or parked and returns the parked paths. */
    [[nodiscard]] std::vector<std::filesystem::path> flush();

  private:
    struct Pending final {
        std::uint64_t version = 0U;
        std::string payload;
        bool parked = false;
    };

    [[nodiscard]] bool has_unwritten() const noexcept;
    void run() noexcept;

    FailureReport report_;
    Options options_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::condition_variable settled_;
    std::map<std::filesystem::path, Pending> pending_;
    std::uint64_t version_ = 0U;
    bool stopping_ = false;
    std::jthread worker_;
};

} // namespace strata::desktop
