#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/allocator.hpp"
#include "base/arena.hpp"
#include "base/identity.hpp"
#include "base/pending.hpp"
#include "base/status.hpp"
#include "base/task_pool.hpp"
#include "base/utf8.hpp"
#include "core/diagnostics.hpp"
#include "core/runtime.hpp"
#include "data/json.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

struct AllocationCounts final {
    std::size_t allocations = 0U;
    std::size_t deallocations = 0U;
};

void* allocate(void* const user_data, const std::size_t size, const std::size_t alignment) {
    auto& counts = *static_cast<AllocationCounts*>(user_data);
    void* const value = ::operator new(size, std::align_val_t(alignment), std::nothrow);
    if (value != nullptr) {
        ++counts.allocations;
    }
    return value;
}

void deallocate(
    void* const user_data,
    void* const allocation,
    const std::size_t,
    const std::size_t alignment
) {
    auto& counts = *static_cast<AllocationCounts*>(user_data);
    ++counts.deallocations;
    ::operator delete(allocation, std::align_val_t(alignment));
}

void throwing_diagnostic(const strata::core::DiagnosticEvent&) {
    throw std::runtime_error("foreign diagnostic callback");
}

void test_utf8() {
    check(strata::base::valid_utf8("plain ASCII"), "ASCII must be valid UTF-8");
    check(strata::base::valid_utf8("\xC4\xB0stanbul \xF0\x9F\x8C\x8D"), "Unicode must be valid UTF-8");
    check(!strata::base::valid_utf8("\xC0\xAF"), "overlong UTF-8 must be rejected");
    check(!strata::base::valid_utf8("\xED\xA0\x80"), "UTF-8 surrogate must be rejected");
    check(!strata::base::valid_utf8("\xF4\x90\x80\x80"), "out-of-range UTF-8 must be rejected");
    check(!strata::base::valid_utf8("\xE2\x82"), "truncated UTF-8 must be rejected");
}

void test_arena_uses_host_allocator() {
    AllocationCounts counts;
    const strata::base::AllocatorCallbacks callbacks{
        &counts,
        &allocate,
        &deallocate,
    };
    const strata::base::HostAllocator host_allocator(callbacks);
    strata::base::HostMemoryResource resource{host_allocator};
    strata::base::AllocatorStatistics active_statistics{};
    {
        strata::base::Arena arena(resource);
        const auto* const value = arena.create<std::uint64_t>(42U);
        check(*value == 42U, "arena object construction failed");
        active_statistics = host_allocator.statistics();
        check(active_statistics.routed.current_bytes > 0U, "routed bytes were not tracked");
        check(
            active_statistics.arena.current_bytes == active_statistics.routed.current_bytes,
            "arena allocation was not categorized"
        );
        arena.reset();
    }
    const strata::base::AllocatorStatistics released_statistics =
        host_allocator.statistics();
    check(released_statistics.routed.current_bytes == 0U, "released routed bytes remained live");
    check(released_statistics.arena.current_bytes == 0U, "released arena bytes remained live");
    check(
        released_statistics.arena.peak_bytes >= sizeof(std::uint64_t),
        "arena byte high-water mark was not retained"
    );
    check(
        released_statistics.arena.allocation_count ==
            released_statistics.arena.deallocation_count,
        "arena telemetry did not balance"
    );
    check(counts.allocations > 0U, "arena must allocate through the host allocator");
    check(counts.allocations == counts.deallocations, "arena allocations must return to their owner");
}

void test_stable_identity_and_resource_handles() {
    strata::base::StableIdentitySource identities(40U);
    check(identities.next() == 41U, "stable identity seed was not honored");
    check(identities.next() == 42U, "stable identities must be monotonic");
    strata::base::StableIdentitySource exhausted(std::numeric_limits<std::uint64_t>::max());
    check(!exhausted.next().has_value(), "stable identity overflow must fail");

    const auto handle = strata::base::ResourceHandle::create(7U, 19U);
    check(handle.has_value(), "valid resource handle was rejected");
    check(handle->generation() == 7U && handle->slot() == 19U, "resource handle parts changed");
    check(handle->value() == ((UINT64_C(7) << 32U) | UINT64_C(19)), "resource encoding changed");
    check(!strata::base::ResourceHandle::create(0U, 1U).has_value(), "zero generation must be invalid");
    check(!strata::base::ResourceHandle::create(1U, 0U).has_value(), "zero slot must be invalid");
}

void test_foreign_diagnostic_exception_is_contained() {
    strata::core::Diagnostics diagnostics(&throwing_diagnostic);
    const strata::base::Result emitted = diagnostics.emit(
        strata::base::Status::internal_error,
        strata::core::DiagnosticSeverity::fatal,
        "STRATA.TEST",
        "test"
    );
    check(emitted.status == strata::base::Status::internal_error, "diagnostic status changed");
    check(emitted.diagnostic_id == 1U, "diagnostic identity must be deterministic per runtime");
}

void test_failed_preparation_publishes_an_owner_diagnostic() {
    using namespace strata;
    const auto owner_thread = std::this_thread::get_id();
    std::string code;
    std::atomic_bool wrong_thread{false};
    core::Runtime runtime(
        base::HostAllocator(base::default_allocator()),
        core::Clock{nullptr, [](void*) -> std::int64_t { return 0; }},
        [&](const core::DiagnosticEvent& event) {
            if (std::this_thread::get_id() != owner_thread) {
                wrong_thread.store(true);
                return;
            }
            code = event.code;
        },
        0U, base::TaskExecution::asynchronous);
    runtime.configure_application("failed-preparation", nullptr, {});
    const auto document = base::Pending<data::JsonValue>::start(
        runtime.tasks(), base::TaskKind::snapshot, base::TaskPriority::frame_critical,
        "malformed-document", [](const base::TaskCancellation&) {
            return data::parse_json("{");
        });
    check(runtime.tasks().wait_for_completion(std::chrono::seconds(5)) && code.empty(),
          "a worker published diagnostics before owner adoption");
    static_cast<void>(runtime.tasks().drain());
    auto& services = runtime.application().services();
    static_cast<void>(services.begin_frame());
    services.end_frame();
    check(document.status() == base::PendingStatus::failed && document.get() == nullptr &&
              code == "STRATA.TASK.PREPARATION_FAILED" && !wrong_thread.load(),
          "malformed preparation failed silently or published from a worker");
}

void test_task_priorities_cancellation_and_owner_adoption() {
    using namespace strata::base;
    TaskPool pool(1U);
    std::atomic_uint wakes{0U};
    const auto owner_thread = std::this_thread::get_id();
    std::atomic_bool wrong_thread{false};
    std::binary_semaphore started(0);
    std::binary_semaphore release(0);
    std::vector<int> worked;
    std::vector<int> adopted;
    bool saw_cancellation = false;
    bool saw_failure = false;
    unsigned reports = 0U;
    TaskOwner owner(
        TaskExecution::asynchronous, [&] { ++wakes; }, &pool,
        [&](const TaskKind kind, const std::exception_ptr& error) {
            if (std::this_thread::get_id() != owner_thread)
                wrong_thread.store(true);
            check(kind == TaskKind::image && error != nullptr,
                  "task failure reporting lost its preparation error");
            ++reports;
        });
    const auto completion = [&](const int value) {
        return [&, value](const TaskOutcome& outcome) {
            if (std::this_thread::get_id() != owner_thread) {
                wrong_thread.store(true);
                return;
            }
            if (outcome.cancelled) {
                saw_cancellation = true;
            } else if (outcome.error != nullptr) {
                try {
                    std::rethrow_exception(outcome.error);
                } catch (const std::runtime_error& error) {
                    saw_failure = std::string_view(error.what()) == "preparation failed";
                }
            } else {
                adopted.push_back(value);
            }
        };
    };
    static_cast<void>(owner.post(
        TaskKind::image, TaskPriority::idle,
        [&](const TaskCancellation&) {
            started.release();
            release.acquire();
            worked.push_back(0);
        }, completion(0)));
    started.acquire();
    static_cast<void>(owner.post(TaskKind::image, TaskPriority::idle,
        [&](const TaskCancellation&) { worked.push_back(3); }, completion(3)));
    static_cast<void>(owner.post(TaskKind::image, TaskPriority::prefetch,
        [&](const TaskCancellation&) { worked.push_back(2); }, completion(2)));
    static_cast<void>(owner.post(TaskKind::image, TaskPriority::frame_critical,
        [&](const TaskCancellation&) { worked.push_back(1); }, completion(1)));
    const TaskCancellation cancelled = owner.post(TaskKind::image, TaskPriority::prefetch,
        [&](const TaskCancellation&) { worked.push_back(4); }, completion(4));
    cancelled.cancel();
    static_cast<void>(owner.post(TaskKind::image, TaskPriority::prefetch,
        [](const TaskCancellation&) { throw std::runtime_error("preparation failed"); },
        completion(5)));
    release.release();
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "worker completion did not reach its owner");
    check(adopted.empty(), "worker adopted a result before the owner drained");
    while (owner.pending() != 0U) {
        check(owner.wait_for_completion(std::chrono::seconds(5)),
              "task completion stopped making progress");
        static_cast<void>(owner.drain());
    }
    check(!wrong_thread.load() && worked == std::vector<int>{0, 1, 2, 3} &&
              adopted == worked && saw_cancellation && saw_failure && reports == 1U &&
              wakes.load() != 0U,
          "task priorities, cancellation, failure or owner-thread adoption changed");
    const TaskStatistics statistics = owner.statistics();
    const auto& images = statistics.kinds[static_cast<std::size_t>(TaskKind::image)];
    check(statistics.pending == 0U && statistics.queued == 0U && statistics.running == 0U &&
              images.submitted == 6U && images.completed == 6U && images.adopted == 6U &&
              images.cancelled == 1U &&
              images.failed == 1U,
          "task accounting lost a terminal transition");
}

void test_task_inline_and_late_cancellation() {
    using namespace strata::base;
    const auto caller = std::this_thread::get_id();
    unsigned wakes = 0U;
    TaskOwner inline_owner(TaskExecution::deterministic, [&] { ++wakes; });
    int prepared = 0;
    int adopted = 0;
    static_cast<void>(inline_owner.post(
        TaskKind::module, TaskPriority::prefetch,
        [&](const TaskCancellation&) {
            check(std::this_thread::get_id() == caller, "inline work changed threads");
            prepared = 42;
        },
        [&](const TaskOutcome& outcome) {
            check(std::this_thread::get_id() == caller && outcome.error == nullptr &&
                      !outcome.cancelled,
                  "inline completion changed ownership or failed");
            adopted = prepared;
        }));
    check(adopted == 42 && inline_owner.pending() == 0U && wakes == 0U,
          "deterministic work was deferred or scheduled another frame");

    TaskPool pool(1U);
    TaskOwner owner(TaskExecution::asynchronous, {}, &pool);
    bool cancelled_at_adoption = false;
    const TaskCancellation ticket = owner.post(
        TaskKind::module, TaskPriority::prefetch,
        [&](const TaskCancellation&) { prepared = 73; },
        [&](const TaskOutcome& outcome) {
            cancelled_at_adoption = outcome.cancelled;
            if (!outcome.cancelled)
                adopted = prepared;
        });
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "late-cancellation preparation did not finish");
    ticket.cancel();
    static_cast<void>(owner.drain());
    check(cancelled_at_adoption && adopted == 42 && owner.pending() == 0U,
          "cancellation after preparation adopted the stale result");
}

void test_task_owner_close_does_not_wait_for_preparation() {
    using namespace strata::base;
    auto pool = std::make_unique<TaskPool>(1U);
    std::atomic_uint wakes{0U};
    std::atomic_bool adopted{false};
    std::atomic_bool cancellation_seen{false};
    TaskOwner owner(TaskExecution::asynchronous, [&] { ++wakes; }, pool.get());
    std::binary_semaphore started(0);
    std::binary_semaphore release(0);
    std::binary_semaphore closed(0);
    static_cast<void>(owner.post(
        TaskKind::font, TaskPriority::prefetch,
        [&](const TaskCancellation& cancellation) {
            started.release();
            release.acquire();
            cancellation_seen.store(cancellation.cancelled());
        },
        [&](const TaskOutcome&) { adopted.store(true); }));
    started.acquire();
    std::jthread closer([&] {
        owner.close();
        closed.release();
    });
    const bool closed_without_work = closed.try_acquire_for(std::chrono::seconds(5));
    release.release();
    closer.join();
    pool.reset();
    check(closed_without_work && cancellation_seen.load() && !adopted.load() &&
              wakes.load() == 0U && owner.pending() == 0U,
          "owner close waited for preparation, adopted a result or called a dead wake adapter");
}

void test_pool_size_follows_request_and_cores() {
    using namespace strata::base;
    check(TaskPool::default_workers() >= 3U, "the default pool cannot serve blocking I/O");
    TaskPool wide(8U);
    check(wide.workers() == 8U, "a local pool ignored its requested size");
    std::atomic_size_t ran{0U};
    wide.run(8U, [&ran] { ran.fetch_add(1U); });
    check(ran.load() >= 1U && ran.load() <= 9U, "a batch ran outside its caller and helpers");
    check(!TaskPool::limit_shared_workers(0U), "a zero worker limit was accepted");
    static_cast<void>(TaskPool::shared());
    check(!TaskPool::limit_shared_workers(1U), "a started pool accepted a new limit");
}

void test_blocking_batches_do_not_wait_for_unclaimed_helpers() {
    using namespace strata::base;
    TaskPool pool(1U);
    TaskOwner owner(TaskExecution::asynchronous, {}, &pool);
    std::binary_semaphore started(0);
    std::binary_semaphore release(0);
    std::binary_semaphore finished(0);
    static_cast<void>(owner.post(
        TaskKind::image, TaskPriority::idle,
        [&](const TaskCancellation&) {
            started.release();
            release.acquire();
        }, [](const TaskOutcome&) {}));
    started.acquire();
    std::atomic_int result{0};
    std::jthread caller([&] {
        pool.run(1U, [&] { pool.run(1U, [&] { result = 91; }); });
        finished.release();
    });
    const bool completed_without_worker = finished.try_acquire_for(std::chrono::seconds(5));
    release.release();
    caller.join();
    check(completed_without_worker && result.load() == 91,
          "a nested blocking batch waited for helpers occupied by background work");

    std::array<std::atomic_uint, 64U> visits{};
    parallel_for(visits.size(), 2U, [&](const std::size_t index) {
        parallel_for(5U, 2U, [&](const std::size_t) { ++visits[index]; });
    });
    for (const auto& count : visits)
        check(count.load() == 5U, "nested parallel work lost or repeated an index");
}

void test_pending_resources_publish_on_owner() {
    using namespace strata::base;
    TaskPool pool(1U);
    TaskOwner owner(TaskExecution::asynchronous, {}, &pool);
    auto resource = Pending<std::string>::start(
        owner, TaskKind::image, TaskPriority::prefetch, "image:example",
        [](const TaskCancellation&) { return std::string("prepared pixels"); });
    const ReadinessRead dependency{resource.readiness(), PendingStatus::pending};
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "resource preparation did not finish");
    check(resource.get() == nullptr && dependency.current(),
          "a worker published its resource before owner adoption");
    static_cast<void>(owner.drain());
    check(resource.get() != nullptr && *resource.get() == "prepared pixels" &&
              !dependency.current() && dependency.resource.label() == "image:example",
          "owner adoption did not expose the immutable result or invalidate its dependency");

    auto failed = Pending<std::string>::start(
        owner, TaskKind::font, TaskPriority::prefetch, "font:broken",
        [](const TaskCancellation&) -> std::string { throw std::runtime_error("broken font"); });
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "failed resource did not complete");
    static_cast<void>(owner.drain());
    check(failed.status() == PendingStatus::failed && failed.get() == nullptr &&
              failed.readiness().failure().error != nullptr && *resource.get() == "prepared pixels",
          "failed preparation exposed a payload or damaged an earlier resource");

    std::binary_semaphore started(0);
    std::binary_semaphore release(0);
    static_cast<void>(owner.post(
        TaskKind::other, TaskPriority::prefetch,
        [&](const TaskCancellation&) {
            started.release();
            release.acquire();
        }, [](const TaskOutcome&) {}));
    started.acquire();
    std::atomic_bool abandoned_work{false};
    {
        auto abandoned = Pending<int>::start(
            owner, TaskKind::image, TaskPriority::prefetch, "image:removed",
            [&](const TaskCancellation&) {
                abandoned_work.store(true);
                return 9;
            });
        check(abandoned.status() == PendingStatus::pending, "queued resource became ready");
    }
    release.release();
    while (owner.pending() != 0U) {
        check(owner.wait_for_completion(std::chrono::seconds(5)),
              "abandoned resource prevented settling");
        static_cast<void>(owner.drain());
    }
    check(!abandoned_work.load(), "dropping a queued resource still ran its preparation");
}

void test_pending_owner_transfer() {
    using namespace strata::base;
    TaskPool pool(1U);
    TaskOwner owner(TaskExecution::asynchronous, {}, &pool);
    const auto caller = std::this_thread::get_id();
    auto prepared = std::make_shared<std::unique_ptr<int>>(std::make_unique<int>(41));
    std::unique_ptr<int> adopted;
    const auto transfer = Pending<void>::start(
        owner, TaskKind::image, TaskPriority::frame_critical, "decoder-continuation",
        [prepared](const TaskCancellation&) { ++**prepared; },
        [&, prepared](const TaskOutcome& outcome) {
            check(std::this_thread::get_id() == caller && outcome.error == nullptr,
                  "mutable preparation was adopted outside its owner");
            adopted = std::move(*prepared);
        });
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "owned continuation preparation did not complete");
    check(adopted == nullptr && transfer.status() == PendingStatus::pending,
          "worker completion transferred ownership before adoption");
    static_cast<void>(owner.drain());
    check(adopted != nullptr && *adopted == 42 && *prepared == nullptr &&
              transfer.status() == PendingStatus::ready,
          "owner adoption lost exclusive continuation ownership");

    bool cancelled_adoption = false;
    const auto cancelled = Pending<void>::start(
        owner, TaskKind::image, TaskPriority::frame_critical, "removed-continuation",
        [prepared](const TaskCancellation&) { *prepared = std::make_unique<int>(1); },
        [&](const TaskOutcome&) { cancelled_adoption = true; });
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "removed continuation preparation did not complete");
    cancelled.cancel();
    static_cast<void>(owner.drain());
    check(!cancelled_adoption && cancelled.readiness().failure().cancelled,
          "late cancellation still transferred a mutable result to its old owner");

    const auto failed = Pending<void>::start(
        owner, TaskKind::image, TaskPriority::frame_critical, "failed-adoption",
        [prepared](const TaskCancellation&) { ++**prepared; },
        [](const TaskOutcome&) { throw std::runtime_error("adoption failed"); });
    check(owner.wait_for_completion(std::chrono::seconds(5)),
          "failed-adoption preparation did not complete");
    bool threw = false;
    try {
        static_cast<void>(owner.drain());
    } catch (const std::runtime_error&) {
        threw = true;
    }
    check(threw && failed.status() == PendingStatus::failed &&
              failed.readiness().failure().error != nullptr,
          "an adoption exception left its resource pending or hid the owner error");

    TaskOwner inline_owner(TaskExecution::deterministic);
    const auto inline_transfer = Pending<void>::start(
        inline_owner, TaskKind::image, TaskPriority::frame_critical, "inline-continuation",
        [prepared](const TaskCancellation&) { ++**prepared; },
        [&, prepared](const TaskOutcome&) { adopted = std::move(*prepared); });
    check(inline_transfer.status() == PendingStatus::ready && *adopted == 3 &&
              inline_owner.pending() == 0U,
          "inline adoption retained an in-flight ticket or deferred ownership transfer");
}

} // namespace

int strata_test_core() {
    try {
        test_utf8();
        test_arena_uses_host_allocator();
        test_stable_identity_and_resource_handles();
        test_foreign_diagnostic_exception_is_contained();
        test_failed_preparation_publishes_an_owner_diagnostic();
        test_task_priorities_cancellation_and_owner_adoption();
        test_task_inline_and_late_cancellation();
        test_task_owner_close_does_not_wait_for_preparation();
        test_pool_size_follows_request_and_cores();
        test_blocking_batches_do_not_wait_for_unclaimed_helpers();
        test_pending_resources_publish_on_owner();
        test_pending_owner_transfer();
        std::cout << "strata_core_tests: OK\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "strata_core_tests: " << exception.what() << '\n';
        return 1;
    }
}
