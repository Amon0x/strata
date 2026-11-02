#include <strata/strata.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include "abi_support.hpp"
#include "base/pending.hpp"
#include "base/task_pool.hpp"

struct strata_cancellation final {
    strata::base::TaskCancellation token;
};

namespace {

using strata::abi_detail::EntryGuard;
using strata::abi_detail::Reentry;
using strata::abi_detail::result;

/** Retained separately so release and in-flight wake admission never refer to a deleted handle. */
struct Admission final {
    std::atomic<std::thread::id> active_thread{};
    std::uint32_t call_depth = 0U;
};

/** An untaken result is destroyed on whichever thread owns its final transfer box. */
struct OwnedResult final {
    explicit OwnedResult(const strata_preparation_cleanup_fn cleanup) noexcept : release(cleanup) {}
    ~OwnedResult() { reset(); }
    OwnedResult(const OwnedResult&) = delete;
    OwnedResult& operator=(const OwnedResult&) = delete;

    void reset() noexcept {
        if (void* const previous = std::exchange(value, nullptr); previous != nullptr)
            release(previous);
    }

    strata_preparation_cleanup_fn release;
    void* value = nullptr;
};

/** Worker-exclusive inputs; no runtime, UI service or wake adapter is retained here. */
struct Work final {
    explicit Work(const strata_preparation_config& config) noexcept
        : output(config.release_result), context(config.context), run(config.work),
          release_context(config.release_context) {}
    ~Work() {
        output.reset();
        if (owns_context)
            release_context(context);
    }

    OwnedResult output;
    void* context;
    strata_preparation_work_fn run;
    strata_preparation_cleanup_fn release_context;
    bool owns_context = false;
    strata_status failure_status = STRATA_STATUS_OK;
    std::string failure_message;
};

/** Mutable publication belongs only to serialized endpoint calls, never the worker. */
struct Publication final {
    explicit Publication(const strata_preparation_cleanup_fn cleanup) noexcept : output(cleanup) {}
    OwnedResult output;
    strata_status failure_status = STRATA_STATUS_OK;
    std::string failure_message;
    bool cancelled = false;
    bool taken = false;
};

struct CallbackFailure final {};

strata::base::TaskOwner::Wake wake_callback(const std::shared_ptr<Admission>& admission,
                                           const strata_frame_wake_adapter& adapter) {
    if (adapter.wake == nullptr)
        return {};
    return [admission, adapter] {
        const strata::abi_detail::WakeCallbackScope scope(admission.get());
        if (adapter.wake(adapter.user_data) != STRATA_STATUS_OK)
            throw std::runtime_error("preparation frame wake adapter failed");
    };
}

void execute(Work& work, const strata::base::TaskCancellation& cancellation) {
    const strata_cancellation token{cancellation};
    strata_string_view error{};
    const strata_status status = work.run(work.context, &token, &work.output.value, &error);
    if (cancellation.cancelled())
        return;
    if (status == STRATA_STATUS_OK) {
        if (work.output.value == nullptr)
            throw std::logic_error("preparation returned no owned result");
        return;
    }
    if (!strata::abi_detail::valid_view(error, true) || error.size > 1024U * 1024U)
        throw std::invalid_argument("preparation returned invalid error text");
    work.failure_message = strata::abi_detail::copied_string(error);
    work.failure_status = status;
    throw CallbackFailure{};
}

void adopt(Publication& publication, Work& work, const strata::base::TaskOutcome& outcome) {
    if (publication.cancelled)
        return;
    if (outcome.error == nullptr) {
        publication.output.value = std::exchange(work.output.value, nullptr);
    } else if (work.failure_status != STRATA_STATUS_OK) {
        publication.failure_status = work.failure_status;
        publication.failure_message = std::move(work.failure_message);
    } else {
        try {
            std::rethrow_exception(outcome.error);
        } catch (...) {
            const auto caught = strata::abi_detail::classify_current_exception({"Preparation"});
            publication.failure_status = caught.status;
            if (caught.detail != nullptr)
                publication.failure_message = caught.detail;
        }
    }
}

} // namespace

/** The endpoint owns adoption and notifications; closing it never joins its worker. */
struct strata_preparation final {
    explicit strata_preparation(const strata_preparation_config& config)
        : admission(std::make_shared<Admission>()),
          publication(std::make_shared<Publication>(config.release_result)),
          tasks(static_cast<strata::base::TaskExecution>(config.execution),
                wake_callback(admission, config.frame_wake)) {}
    ~strata_preparation() {
        pending.cancel();
        tasks.close();
    }

    std::shared_ptr<Admission> admission;
    std::shared_ptr<Publication> publication;
    strata::base::TaskOwner tasks;
    strata::base::Pending<void> pending;
};

namespace {

/** The admission storage remains alive even when the body releases its endpoint. */
template <typename Body>
strata_result preparation_call(strata_preparation* const preparation, Body&& body) noexcept {
    if (preparation == nullptr)
        return strata::abi_detail::invalid_argument();
    const auto admission = preparation->admission;
    const EntryGuard entry(admission.get(), &admission->active_thread, &admission->call_depth,
                           Reentry::rejected);
    if (entry.refused())
        return entry.refusal();
    return strata::abi_detail::guarded(std::forward<Body>(body), {"Preparation"});
}

strata_preparation_status status(const strata_preparation& preparation) noexcept {
    if (preparation.publication->taken)
        return STRATA_PREPARATION_TAKEN;
    if (preparation.publication->cancelled)
        return STRATA_PREPARATION_CANCELLED;
    switch (preparation.pending.status()) {
    case strata::base::PendingStatus::pending: return STRATA_PREPARATION_PENDING;
    case strata::base::PendingStatus::ready: return STRATA_PREPARATION_READY;
    case strata::base::PendingStatus::failed:
        return preparation.pending.readiness().failure().cancelled
                   ? STRATA_PREPARATION_CANCELLED : STRATA_PREPARATION_FAILED;
    }
    return STRATA_PREPARATION_FAILED;
}

} // namespace

extern "C" strata_result strata_preparation_start(
    const strata_preparation_config* const input,
    strata_preparation** const out_preparation) noexcept {
    if (out_preparation == nullptr)
        return strata::abi_detail::invalid_argument();
    *out_preparation = nullptr;
    const auto config = strata::abi_detail::read_struct(input);
    if (!config || config->reserved != 0U || config->work == nullptr ||
        config->release_context == nullptr || config->release_result == nullptr ||
        (config->execution != STRATA_TASK_EXECUTION_ASYNCHRONOUS &&
         config->execution != STRATA_TASK_EXECUTION_DETERMINISTIC))
        return strata::abi_detail::invalid_argument();
    const strata_frame_wake_adapter& wake = config->frame_wake;
    if ((wake.struct_size != 0U || wake.user_data != nullptr || wake.wake != nullptr) &&
        (!strata::abi_detail::has_struct(&wake) || wake.wake == nullptr))
        return strata::abi_detail::invalid_argument();
    return strata::abi_detail::guarded([&] {
        auto preparation = std::make_unique<strata_preparation>(*config);
        auto work = std::make_shared<Work>(*config);
        const std::weak_ptr<Publication> publication = preparation->publication;
        preparation->pending = strata::base::Pending<void>::start(
            preparation->tasks, strata::base::TaskKind::other,
            strata::base::TaskPriority::frame_critical, "host preparation",
            [work](const strata::base::TaskCancellation& cancellation) {
                execute(*work, cancellation);
            },
            [publication, work](const strata::base::TaskOutcome& outcome) {
                if (const auto target = publication.lock(); target != nullptr)
                    adopt(*target, *work, outcome);
            });
        // The local reference prevents cleanup until successful submission transfers ownership,
        // including deterministic work which has already completed or failed inside start().
        work->owns_context = true;
        *out_preparation = preparation.release();
        return result(STRATA_STATUS_OK);
    }, {"Preparation start"});
}

extern "C" strata_result strata_preparation_get_info(
    strata_preparation* const preparation, strata_preparation_info* const out_info) noexcept {
    if (!strata::abi_detail::has_struct(out_info))
        return strata::abi_detail::invalid_argument();
    return preparation_call(preparation, [&] {
        static_cast<void>(preparation->tasks.drain());
        strata_preparation_info info{};
        info.struct_size = sizeof(info);
        info.status = status(*preparation);
        if (info.status == STRATA_PREPARATION_FAILED) {
            const Publication& publication = *preparation->publication;
            info.failure_status = publication.failure_status != STRATA_STATUS_OK
                                      ? publication.failure_status : STRATA_STATUS_INTERNAL_ERROR;
            info.failure_message = strata::abi_detail::to_view(
                publication.failure_message.empty() ? std::string_view("Preparation failed.")
                                                    : publication.failure_message);
        }
        static_cast<void>(strata::abi_detail::write_struct(out_info, info));
        return result(STRATA_STATUS_OK);
    });
}

extern "C" strata_result strata_preparation_take_result(
    strata_preparation* const preparation, void** const out_result) noexcept {
    if (out_result == nullptr)
        return strata::abi_detail::invalid_argument();
    *out_result = nullptr;
    return preparation_call(preparation, [&] {
        static_cast<void>(preparation->tasks.drain());
        if (status(*preparation) != STRATA_PREPARATION_READY)
            return result(STRATA_STATUS_NOT_FOUND);
        preparation->publication->taken = true;
        *out_result = std::exchange(preparation->publication->output.value, nullptr);
        return result(STRATA_STATUS_OK);
    });
}

extern "C" strata_result strata_preparation_cancel(strata_preparation* const preparation) noexcept {
    return preparation_call(preparation, [&] {
        if (!preparation->publication->taken) {
            preparation->publication->cancelled = true;
            preparation->pending.cancel();
            preparation->tasks.close();
            preparation->publication->output.reset();
        }
        return result(STRATA_STATUS_OK);
    });
}

extern "C" strata_result strata_preparation_release(strata_preparation* const preparation) noexcept {
    if (preparation == nullptr)
        return result(STRATA_STATUS_OK);
    return preparation_call(preparation, [&] {
        delete preparation;
        return result(STRATA_STATUS_OK);
    });
}

extern "C" std::uint32_t strata_cancellation_requested(
    const strata_cancellation* const cancellation) noexcept {
    return cancellation == nullptr || cancellation->token.cancelled() ? 1U : 0U;
}
