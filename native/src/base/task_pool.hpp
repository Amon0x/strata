#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <thread>
#include <vector>

namespace strata::base {

enum class TaskPriority : std::uint8_t { frame_critical, prefetch, idle, count };
enum class TaskKind : std::uint8_t {
    glyph_raster,
    vector_glyph,
    image,
    font,
    shaping,
    module,
    snapshot,
    geometry,
    lottie,
    other,
    count,
};
enum class TaskExecution : std::uint8_t { asynchronous, deterministic };

[[nodiscard]] std::string_view task_kind_name(TaskKind kind) noexcept;

/** Monotone owner-lifetime totals; independent Surface profilers take their own differences. */
struct TaskKindStatistics final {
    std::uint64_t submitted = 0U;
    std::uint64_t completed = 0U;
    std::uint64_t adopted = 0U;
    std::uint64_t cancelled = 0U;
    std::uint64_t failed = 0U;
    std::uint64_t queue_nanoseconds = 0U;
    std::uint64_t work_nanoseconds = 0U;
    std::uint64_t latency_nanoseconds = 0U;
};

struct TaskStatistics final {
    std::size_t queued = 0U;
    std::size_t running = 0U;
    std::size_t pending = 0U;
    std::array<TaskKindStatistics, static_cast<std::size_t>(TaskKind::count)> kinds{};
};

struct TaskOwnerState;
class TaskOwner;

/** Cancellation is thread-safe and also observes owner shutdown; it never waits for work. */
class TaskCancellation final {
  public:
    TaskCancellation() noexcept = default;
    void cancel() const noexcept;
    [[nodiscard]] bool cancelled() const noexcept;

  private:
    friend class TaskOwner;
    friend class TaskPool;
    TaskCancellation(std::shared_ptr<std::atomic_bool> flag,
                     std::weak_ptr<TaskOwnerState> owner) noexcept;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::weak_ptr<TaskOwnerState> owner_;
};

/** A cancelled result is not adopted, even if preparation finished before cancellation. */
struct TaskOutcome final {
    bool cancelled = false;
    std::exception_ptr error;
};

/**
 * Persistent engine workers shared by every runtime. Blocking batches retain caller participation
 * and do not wait for helpers still queued behind other work. Local pools support isolated tests;
 * the shared pool is process-lived so closing an owner never joins workers. Queued work must retain
 * its own inputs and any service lifetime it needs.
 */
class TaskPool final {
  public:
    /**
     * Half the logical cores less the caller, and at least three for blocking I/O. Measured on 16
     * threads: 3 and 7 workers settle first opens alike, 15 cost the UI thread 2-3 ms per open.
     */
    [[nodiscard]] static std::size_t default_workers() noexcept;
    /** Caps the shared pool for an embedder keeping cores; only before it is first used. */
    [[nodiscard]] static bool limit_shared_workers(std::size_t limit) noexcept;

    explicit TaskPool(std::size_t worker_count = default_workers());
    ~TaskPool();
    TaskPool(const TaskPool&) = delete;
    TaskPool& operator=(const TaskPool&) = delete;

    [[nodiscard]] static TaskPool& shared();
    [[nodiscard]] std::size_t workers() const noexcept { return threads_.size(); }
    /** Runs the caller and up to count helpers; propagates failure after every running lane leaves. */
    void run(std::size_t count, const std::function<void()>& lane);

  private:
    friend class TaskOwner;
    friend struct TaskOwnerState;
    struct Task;
    struct Batch;

    void enqueue(std::shared_ptr<Task> task, TaskPriority priority);
    void serve(std::stop_token stop);
    void unlink(Batch& batch) noexcept;
    static void execute(const std::shared_ptr<Task>& task, bool inline_completion);

    std::mutex mutex_;
    std::condition_variable_any work_;
    std::array<std::deque<std::shared_ptr<Task>>,
               static_cast<std::size_t>(TaskPriority::count)> queues_;
    Batch* first_batch_ = nullptr;
    Batch* last_batch_ = nullptr;
    std::vector<std::jthread> threads_;
};

/**
 * A serialized owner's completion mailbox. Workers retain its independent endpoint, never the
 * runtime. Closing quiesces wake callbacks and discards adoption without waiting for preparation.
 * Wake callbacks only signal the host: they must not block or enter/destroy their owning runtime.
 */
class TaskOwner final {
  public:
    using Work = std::move_only_function<void(const TaskCancellation&)>;
    using Completion = std::move_only_function<void(const TaskOutcome&)>;
    using Wake = std::function<void()>;
    /** Owner-side reporting; handled failures still retain their terminal resource status. */
    using Failure = std::function<void(TaskKind, const std::exception_ptr&)>;

    explicit TaskOwner(TaskExecution mode = TaskExecution::asynchronous,
                       Wake wake = {}, TaskPool* pool = nullptr, Failure failure = {});
    ~TaskOwner();
    TaskOwner(const TaskOwner&) = delete;
    TaskOwner& operator=(const TaskOwner&) = delete;

    [[nodiscard]] TaskCancellation post(TaskKind kind, TaskPriority priority,
                                         Work work, Completion completion);
    /** Adopts a detached batch on the caller; newly posted completions belong to the next drain. */
    [[nodiscard]] std::size_t drain();
    /** Reports an owner-side acquisition failure without queuing work that is already known to fail. */
    void report_failure(TaskKind kind, const std::exception_ptr& error);
    void close() noexcept;
    /** Thread-safe and coalesced until drain; generation lets multiple Surfaces observe one wake. */
    void request_frame() noexcept;
    [[nodiscard]] std::uint64_t wake_generation() const noexcept;
    [[nodiscard]] std::size_t pending() const noexcept;
    [[nodiscard]] bool has_completions() const noexcept;
    [[nodiscard]] TaskExecution execution() const noexcept;
    /** Identities are monotone within this owner and survive completion for readiness inspection. */
    [[nodiscard]] std::uint64_t next_resource_identity();
    [[nodiscard]] TaskStatistics statistics() const;
    /** Test/settle barrier only; ordinary framing never waits for a worker. */
    [[nodiscard]] bool wait_for_completion(std::chrono::milliseconds timeout);

  private:
    std::shared_ptr<TaskOwnerState> state_;
    TaskPool* pool_;
};

/**
 * Works each index once, using caller participation above the threshold. A lane failure stops new
 * indices and is rethrown only after all running lanes leave; nested batches cannot exhaust workers.
 */
void parallel_for(std::size_t count, std::size_t parallel_threshold,
                  const std::function<void(std::size_t)>& work);

} // namespace strata::base
