#include "base/task_pool.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::base {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::uint64_t elapsed(const Clock::time_point first,
                                    const Clock::time_point last) noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(last - first).count());
}

} // namespace

struct TaskPool::Task final {
    std::weak_ptr<TaskOwnerState> owner;
    std::atomic_bool cancelled{false};
    TaskKind kind = TaskKind::other;
    TaskOwner::Work work;
    TaskOwner::Completion completion;
    TaskOutcome outcome;
    Clock::time_point queued_at;
    std::shared_ptr<Task> next_completion;
};

/** The endpoint outlives its runtime; its mutexes protect queues and wake-callback lifetime. */
struct TaskOwnerState final {
    TaskOwnerState(const TaskExecution mode, TaskOwner::Wake callback, TaskOwner::Failure reporter)
        : execution(mode), report_failure(std::move(reporter)), wake(std::move(callback)) {}

    /** Always publishes terminal state, even if reporting itself fails. Called only by the owner. */
    void adopt(TaskPool::Task& task) {
        std::exception_ptr failure;
        try {
            if (!task.outcome.cancelled && task.outcome.error != nullptr && report_failure)
                report_failure(task.kind, task.outcome.error);
        } catch (...) {
            failure = std::current_exception();
        }
        auto completion = std::move(task.completion);
        try {
            completion(task.outcome);
        } catch (...) {
            if (failure == nullptr)
                failure = std::current_exception();
        }
        if (failure != nullptr)
            std::rethrow_exception(failure);
    }

    void notify() noexcept {
        if (!open.load(std::memory_order_acquire))
            return;
        generation.fetch_add(1U, std::memory_order_release);
        ready.notify_all();
        if (frame_requested.exchange(true, std::memory_order_acq_rel))
            return;
        const std::scoped_lock callback_lock(wake_mutex);
        if (!open.load(std::memory_order_acquire) || !wake)
            return;
        try {
            wake();
        } catch (...) {
            const std::scoped_lock lock(mutex);
            if (wake_failure == nullptr)
                wake_failure = std::current_exception();
        }
    }

    const TaskExecution execution;
    std::atomic_bool open{true};
    std::atomic_bool frame_requested{false};
    std::atomic_uint64_t generation{0U};
    std::mutex mutex;
    std::condition_variable ready;
    TaskStatistics statistics;
    std::uint64_t last_resource_identity = 0U;
    std::shared_ptr<TaskPool::Task> first_completion;
    TaskPool::Task* last_completion = nullptr;
    std::exception_ptr wake_failure;
    /** The worker never invokes this callback; close clears it before runtime destruction. */
    TaskOwner::Failure report_failure;
    std::mutex wake_mutex;
    TaskOwner::Wake wake;
};

/** Stack-owned blocking batches can withdraw unclaimed helpers without allocating queue nodes. */
struct TaskPool::Batch final {
    const std::function<void()>* lane = nullptr;
    Batch* previous = nullptr;
    Batch* next = nullptr;
    bool queued = false;
    std::size_t waiting = 0U;
    std::size_t running = 0U;
    std::exception_ptr failure;
    std::condition_variable done;
};

std::string_view task_kind_name(const TaskKind kind) noexcept {
    static constexpr std::array names{
        std::string_view("glyph_raster"), std::string_view("vector_glyph"),
        std::string_view("image"), std::string_view("font"), std::string_view("shaping"),
        std::string_view("module"), std::string_view("snapshot"), std::string_view("geometry"),
        std::string_view("lottie"), std::string_view("other"),
    };
    const auto index = static_cast<std::size_t>(kind);
    return index < names.size() ? names[index] : std::string_view("unknown");
}

TaskCancellation::TaskCancellation(std::shared_ptr<std::atomic_bool> flag,
                                   std::weak_ptr<TaskOwnerState> owner) noexcept
    : cancelled_(std::move(flag)), owner_(std::move(owner)) {}

void TaskCancellation::cancel() const noexcept {
    if (cancelled_ != nullptr)
        cancelled_->store(true, std::memory_order_release);
}

bool TaskCancellation::cancelled() const noexcept {
    if (cancelled_ == nullptr)
        return false;
    const auto owner = owner_.lock();
    return cancelled_->load(std::memory_order_acquire) || owner == nullptr ||
           !owner->open.load(std::memory_order_acquire);
}

namespace {

std::atomic_size_t shared_worker_limit{0U};
std::atomic_bool shared_pool_started{false};

} // namespace

std::size_t TaskPool::default_workers() noexcept {
    const std::size_t cores = std::thread::hardware_concurrency();
    return std::max<std::size_t>(3U, cores / 2U > 0U ? cores / 2U - 1U : 0U);
}

bool TaskPool::limit_shared_workers(const std::size_t limit) noexcept {
    if (limit == 0U || shared_pool_started.load(std::memory_order_acquire))
        return false;
    shared_worker_limit.store(limit, std::memory_order_release);
    return true;
}

TaskPool& TaskPool::shared() {
    static TaskPool* const pool = [] {
        shared_pool_started.store(true, std::memory_order_release);
        const std::size_t limit = shared_worker_limit.load(std::memory_order_acquire);
        return new TaskPool(limit != 0U ? std::min(limit, default_workers()) : default_workers());
    }();
    return *pool;
}

TaskPool::TaskPool(const std::size_t worker_count) {
    if (worker_count == 0U)
        throw std::invalid_argument("task pool worker count is outside the supported range");
    threads_.reserve(worker_count);
    for (std::size_t index = 0U; index < worker_count; ++index)
        threads_.emplace_back([this](const std::stop_token stop) { serve(stop); });
}

TaskPool::~TaskPool() {
    for (std::jthread& thread : threads_)
        thread.request_stop();
    work_.notify_all();
    threads_.clear();
    // Local pools require their callers to have left run/post before destruction.
    for (auto& queue : queues_) {
        while (!queue.empty()) {
            auto task = std::move(queue.front());
            queue.pop_front();
            task->cancelled.store(true, std::memory_order_release);
            execute(task, false);
        }
    }
}

void TaskPool::unlink(Batch& batch) noexcept {
    if (!batch.queued)
        return;
    if (batch.previous != nullptr)
        batch.previous->next = batch.next;
    else
        first_batch_ = batch.next;
    if (batch.next != nullptr)
        batch.next->previous = batch.previous;
    else
        last_batch_ = batch.previous;
    batch.queued = false;
    batch.waiting = 0U;
}

void TaskPool::run(const std::size_t count, const std::function<void()>& lane) {
    const std::size_t used = std::min(count, threads_.size());
    if (used == 0U) {
        lane();
        return;
    }
    Batch batch;
    batch.lane = &lane;
    batch.waiting = used;
    {
        const std::scoped_lock lock(mutex_);
        batch.previous = last_batch_;
        batch.queued = true;
        if (last_batch_ != nullptr)
            last_batch_->next = &batch;
        else
            first_batch_ = &batch;
        last_batch_ = &batch;
    }
    for (std::size_t helper = 0U; helper < used; ++helper)
        work_.notify_one();
    try {
        lane();
    } catch (...) {
        const std::scoped_lock lock(mutex_);
        if (batch.failure == nullptr)
            batch.failure = std::current_exception();
    }
    std::unique_lock lock(mutex_);
    unlink(batch);
    batch.done.wait(lock, [&batch] { return batch.running == 0U; });
    if (batch.failure != nullptr)
        std::rethrow_exception(batch.failure);
}

void TaskPool::enqueue(std::shared_ptr<Task> task, const TaskPriority priority) {
    {
        const std::scoped_lock lock(mutex_);
        queues_[static_cast<std::size_t>(priority)].push_back(std::move(task));
    }
    work_.notify_one();
}

void TaskPool::serve(const std::stop_token stop) {
    for (;;) {
        Batch* batch = nullptr;
        std::shared_ptr<Task> task;
        {
            std::unique_lock lock(mutex_);
            if (!work_.wait(lock, stop, [this] {
                    return first_batch_ != nullptr ||
                           std::ranges::any_of(queues_, [](const auto& queue) {
                               return !queue.empty();
                           });
                }))
                return;
            if (stop.stop_requested())
                return;
            if (first_batch_ != nullptr) {
                batch = first_batch_;
                ++batch->running;
                if (--batch->waiting == 0U)
                    unlink(*batch);
            } else {
                for (auto& queue : queues_) {
                    if (queue.empty())
                        continue;
                    task = std::move(queue.front());
                    queue.pop_front();
                    break;
                }
            }
        }
        if (batch != nullptr) {
            try {
                (*batch->lane)();
            } catch (...) {
                const std::scoped_lock lock(mutex_);
                if (batch->failure == nullptr)
                    batch->failure = std::current_exception();
            }
            const std::scoped_lock lock(mutex_);
            --batch->running;
            batch->done.notify_all();
        } else {
            execute(task, false);
        }
    }
}

void TaskPool::execute(const std::shared_ptr<Task>& task, const bool inline_completion) {
    const auto owner = task->owner.lock();
    if (owner == nullptr) {
        task->work = {};
        task->completion = {};
        return;
    }
    const TaskCancellation cancellation(
        std::shared_ptr<std::atomic_bool>(task, &task->cancelled), owner);
    const auto started = Clock::now();
    {
        const std::scoped_lock lock(owner->mutex);
        --owner->statistics.queued;
        ++owner->statistics.running;
        owner->statistics.kinds[static_cast<std::size_t>(task->kind)].queue_nanoseconds +=
            inline_completion ? 0U : elapsed(task->queued_at, started);
    }
    task->outcome.cancelled = cancellation.cancelled();
    if (!task->outcome.cancelled) {
        try {
            task->work(cancellation);
        } catch (...) {
            task->outcome.error = std::current_exception();
        }
    }
    const auto finished = Clock::now();
    task->work = {};
    task->outcome.cancelled = cancellation.cancelled();
    bool adopt_inline = false;
    bool queued_completion = false;
    {
        const std::scoped_lock lock(owner->mutex);
        --owner->statistics.running;
        TaskKindStatistics& statistics =
            owner->statistics.kinds[static_cast<std::size_t>(task->kind)];
        ++statistics.completed;
        statistics.cancelled += task->outcome.cancelled ? 1U : 0U;
        statistics.failed += task->outcome.error != nullptr ? 1U : 0U;
        statistics.work_nanoseconds += elapsed(started, finished);
        if (!owner->open.load(std::memory_order_acquire)) {
            --owner->statistics.pending;
        } else if (inline_completion) {
            --owner->statistics.pending;
            ++statistics.adopted;
            statistics.latency_nanoseconds += elapsed(task->queued_at, Clock::now());
            adopt_inline = true;
        } else {
            if (owner->last_completion != nullptr)
                owner->last_completion->next_completion = task;
            else
                owner->first_completion = task;
            owner->last_completion = task.get();
            queued_completion = true;
        }
    }
    if (queued_completion) {
        owner->notify();
    } else {
        if (adopt_inline)
            owner->adopt(*task);
        else
            task->completion = {};
        owner->ready.notify_all();
    }
}

TaskOwner::TaskOwner(const TaskExecution mode, Wake wake, TaskPool* const pool, Failure failure)
    : pool_(pool) {
    if (mode != TaskExecution::asynchronous && mode != TaskExecution::deterministic)
        throw std::invalid_argument("task execution mode is invalid");
    state_ = std::make_shared<TaskOwnerState>(mode, std::move(wake), std::move(failure));
}

TaskOwner::~TaskOwner() {
    close();
}

TaskCancellation TaskOwner::post(const TaskKind kind, const TaskPriority priority,
                                  Work work, Completion completion) {
    if (kind >= TaskKind::count || priority >= TaskPriority::count || !work || !completion)
        throw std::invalid_argument("task submission requires valid work, completion and category");
    auto task = std::make_shared<TaskPool::Task>();
    task->owner = state_;
    task->kind = kind;
    task->work = std::move(work);
    task->completion = std::move(completion);
    task->queued_at = Clock::now();
    const TaskCancellation cancellation(
        std::shared_ptr<std::atomic_bool>(task, &task->cancelled), state_);
    {
        const std::scoped_lock lock(state_->mutex);
        if (!state_->open.load(std::memory_order_acquire))
            throw std::logic_error("task owner is closed");
        if (state_->execution == TaskExecution::asynchronous)
            (pool_ != nullptr ? *pool_ : TaskPool::shared()).enqueue(task, priority);
        ++state_->statistics.queued;
        ++state_->statistics.pending;
        ++state_->statistics.kinds[static_cast<std::size_t>(kind)].submitted;
    }
    if (state_->execution == TaskExecution::deterministic)
        TaskPool::execute(task, true);
    return cancellation;
}

std::size_t TaskOwner::drain() {
    state_->frame_requested.store(false, std::memory_order_release);
    std::shared_ptr<TaskPool::Task> task;
    std::exception_ptr failure;
    {
        const std::scoped_lock lock(state_->mutex);
        task = std::move(state_->first_completion);
        state_->last_completion = nullptr;
        failure = std::exchange(state_->wake_failure, nullptr);
    }
    std::size_t adopted = 0U;
    while (task != nullptr) {
        auto next = std::move(task->next_completion);
        const bool cancelled = task->cancelled.load(std::memory_order_acquire);
        {
            const std::scoped_lock lock(state_->mutex);
            --state_->statistics.pending;
            TaskKindStatistics& kind_statistics =
                state_->statistics.kinds[static_cast<std::size_t>(task->kind)];
            if (cancelled && !task->outcome.cancelled)
                ++kind_statistics.cancelled;
            if (state_->open.load(std::memory_order_acquire)) {
                ++kind_statistics.adopted;
                kind_statistics.latency_nanoseconds += elapsed(task->queued_at, Clock::now());
            }
        }
        task->outcome.cancelled = task->outcome.cancelled || cancelled;
        if (state_->open.load(std::memory_order_acquire)) {
            try {
                state_->adopt(*task);
            } catch (...) {
                if (failure == nullptr)
                    failure = std::current_exception();
            }
            ++adopted;
        } else {
            task->completion = {};
        }
        task = std::move(next);
    }
    if (failure != nullptr)
        std::rethrow_exception(failure);
    return adopted;
}

void TaskOwner::report_failure(const TaskKind kind, const std::exception_ptr& error) {
    if (state_->report_failure && error != nullptr)
        state_->report_failure(kind, error);
}

void TaskOwner::close() noexcept {
    {
        const std::scoped_lock callback_lock(state_->wake_mutex);
        if (!state_->open.exchange(false, std::memory_order_acq_rel))
            return;
        state_->wake = {};
    }
    std::shared_ptr<TaskPool::Task> task;
    {
        const std::scoped_lock lock(state_->mutex);
        task = std::move(state_->first_completion);
        state_->last_completion = nullptr;
        state_->report_failure = {};
    }
    while (task != nullptr) {
        auto next = std::move(task->next_completion);
        task->completion = {};
        {
            const std::scoped_lock lock(state_->mutex);
            --state_->statistics.pending;
        }
        task = std::move(next);
    }
    state_->ready.notify_all();
}

void TaskOwner::request_frame() noexcept {
    state_->notify();
}

std::uint64_t TaskOwner::wake_generation() const noexcept {
    return state_->generation.load(std::memory_order_acquire);
}

std::size_t TaskOwner::pending() const noexcept {
    const std::scoped_lock lock(state_->mutex);
    return state_->statistics.pending;
}

bool TaskOwner::has_completions() const noexcept {
    const std::scoped_lock lock(state_->mutex);
    return state_->first_completion != nullptr;
}

TaskExecution TaskOwner::execution() const noexcept {
    return state_->execution;
}

std::uint64_t TaskOwner::next_resource_identity() {
    const std::scoped_lock lock(state_->mutex);
    if (!state_->open.load(std::memory_order_acquire))
        throw std::logic_error("task owner is closed");
    if (state_->last_resource_identity == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("pending resource identities are exhausted");
    return ++state_->last_resource_identity;
}

TaskStatistics TaskOwner::statistics() const {
    const std::scoped_lock lock(state_->mutex);
    return state_->statistics;
}

bool TaskOwner::wait_for_completion(const std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_->mutex);
    return state_->ready.wait_for(lock, timeout, [this] {
        return state_->first_completion != nullptr || state_->statistics.pending == 0U ||
               !state_->open.load(std::memory_order_acquire);
    });
}

void parallel_for(const std::size_t count, const std::size_t parallel_threshold,
                  const std::function<void(std::size_t)>& work) {
    if (count < std::max<std::size_t>(parallel_threshold, 2U)) {
        for (std::size_t index = 0U; index < count; ++index)
            work(index);
        return;
    }
    std::atomic_size_t cursor{0U};
    std::atomic_bool failed{false};
    const std::function<void()> lane = [&] {
        while (!failed.load(std::memory_order_acquire)) {
            const std::size_t index = cursor.fetch_add(1U, std::memory_order_relaxed);
            if (index >= count)
                return;
            try {
                work(index);
            } catch (...) {
                failed.store(true, std::memory_order_release);
                throw;
            }
        }
    };
    const std::size_t available =
        std::max<std::size_t>(1U, static_cast<std::size_t>(std::thread::hardware_concurrency()));
    TaskPool& pool = TaskPool::shared();
    pool.run(std::min({pool.workers(), available - 1U, count - 1U}), lane);
}

} // namespace strata::base
