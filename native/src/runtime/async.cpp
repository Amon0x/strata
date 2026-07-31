#include "runtime/async.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/utf8.hpp"

namespace strata::runtime {
namespace {

[[nodiscard]] std::string status_name(const AsyncStatus status) {
    switch (status) {
    case AsyncStatus::idle: return "IDLE";
    case AsyncStatus::loading: return "LOADING";
    case AsyncStatus::ready: return "READY";
    case AsyncStatus::failed: return "FAILED";
    }
    return "IDLE";
}

} // namespace

AsyncDataService::AsyncDataService(
    BindingValidator validator,
    ResultNormalizer result_normalizer,
    Publisher publisher
)
    : validator_(std::move(validator)),
      result_normalizer_(std::move(result_normalizer)),
      publisher_(std::move(publisher)) {}

AsyncDataService::~AsyncDataService() {
    for (auto& [binding, state] : bindings_) {
        static_cast<void>(binding);
        try {
            cancel_binding(state);
        } catch (...) {
            // A host cancellation callback cannot make destruction cross an exception boundary.
        }
    }
}

void AsyncDataService::set_adapter(AsyncHostAdapter adapter) { adapter_ = std::move(adapter); }

void AsyncDataService::initialize(std::set<std::string, std::less<>> bindings) {
    for (const std::string& binding : bindings) bindings_.try_emplace(binding);
    if (!bindings_.empty()) publish();
}

std::optional<std::uint64_t> AsyncDataService::query(
    std::string binding,
    Value payload,
    std::string owner,
    const std::int64_t now_nanos,
    const std::int64_t debounce_nanos
) {
    if (binding.empty() || owner.empty() || !core::valid_utf8(binding) ||
        !core::valid_utf8(owner) || debounce_nanos < 0 ||
        now_nanos > std::numeric_limits<std::int64_t>::max() - debounce_nanos ||
        (validator_ && !validator_(binding))) {
        return std::nullopt;
    }
    Binding& state = bindings_[binding];
    cancel_binding(state);
    state = Binding{
        AsyncStatus::loading,
        next_request_id_++,
        std::move(owner),
        std::move(payload),
        std::nullopt,
        std::nullopt,
        std::nullopt,
        now_nanos + debounce_nanos,
        false,
    };
    const std::uint64_t request_id = state.request_id;
    publish();
    if (debounce_nanos == 0) advance(now_nanos);
    return request_id;
}

void AsyncDataService::advance(const std::int64_t now_nanos) {
    drain_completions();
    std::vector<AsyncRequest> requests;
    for (auto& [binding, state] : bindings_) {
        if (state.status != AsyncStatus::loading || state.begun || state.due_nanos > now_nanos) {
            continue;
        }
        state.begun = true;
        requests.push_back(AsyncRequest{
            state.request_id, binding, state.owner, state.payload,
        });
    }
    // Host callbacks may synchronously complete a request, so never call while iterating bindings_.
    if (adapter_.begin) {
        for (const AsyncRequest& request : requests) adapter_.begin(request);
    } else {
        for (const AsyncRequest& request : requests) {
            static_cast<void>(fail(request.id, AsyncFailure{
                "The host does not provide asynchronous query execution.",
                "SERVICE_UNAVAILABLE",
            }));
        }
    }
    // Synchronous host callbacks publish through the same mailbox as worker-thread callbacks.
    drain_completions();
}

bool AsyncDataService::progress(
    const std::uint64_t request_id,
    AsyncProgress next_progress
) {
    if (request_id == 0U) return false;
    Binding* state = active(request_id);
    if (state == nullptr) return false;
    if (!std::isfinite(next_progress.completed) || next_progress.completed < 0.0 ||
        !core::valid_utf8(next_progress.message) ||
        (next_progress.total.has_value() &&
         (!std::isfinite(*next_progress.total) || *next_progress.total < 0.0 ||
          next_progress.completed > *next_progress.total))) {
        return false;
    }
    state->progress = std::move(next_progress);
    publish();
    return true;
}

bool AsyncDataService::succeed(const std::uint64_t request_id, Value result) {
    Binding* state = active(request_id);
    if (state == nullptr) return false; // Deterministic latest-wins stale response drop.
    const auto binding = std::ranges::find_if(bindings_, [state](const auto& entry) {
        return &entry.second == state;
    });
    if (binding == bindings_.end()) return false;
    if (result_normalizer_) {
        std::optional<Value> normalized = result_normalizer_(binding->first, result);
        if (!normalized.has_value()) {
            state->status = AsyncStatus::failed;
            state->value.reset();
            state->progress.reset();
            state->failure = AsyncFailure{
                "The host result does not match the declared async value type.",
                "INVALID_RESULT",
            };
            state->owner.clear();
            publish();
            return false;
        }
        result = std::move(*normalized);
    }
    state->status = AsyncStatus::ready;
    state->value = std::move(result);
    state->progress.reset();
    state->failure.reset();
    state->owner.clear();
    publish();
    return true;
}

bool AsyncDataService::fail(
    const std::uint64_t request_id,
    AsyncFailure failure
) {
    Binding* state = active(request_id);
    if (state == nullptr || failure.message.empty() || !core::valid_utf8(failure.message) ||
        !core::valid_utf8(failure.code)) {
        return false;
    }
    state->status = AsyncStatus::failed;
    state->value.reset();
    state->progress.reset();
    state->failure = std::move(failure);
    state->owner.clear();
    publish();
    return true;
}

bool AsyncDataService::post_progress(
    const std::uint64_t request_id,
    AsyncProgress next_progress
) {
    if (request_id == 0U || !std::isfinite(next_progress.completed) || next_progress.completed < 0.0 ||
        !core::valid_utf8(next_progress.message) ||
        (next_progress.total.has_value() &&
         (!std::isfinite(*next_progress.total) || *next_progress.total < 0.0 ||
          next_progress.completed > *next_progress.total))) {
        return false;
    }
    std::scoped_lock lock(completion_mutex_);
    completions_.push_back(Completion{
        CompletionKind::progress, request_id, std::move(next_progress), std::nullopt, std::nullopt,
    });
    return true;
}

bool AsyncDataService::post_succeed(const std::uint64_t request_id, Value result) {
    if (request_id == 0U) return false;
    std::scoped_lock lock(completion_mutex_);
    completions_.push_back(Completion{
        CompletionKind::succeed, request_id, std::nullopt, std::move(result), std::nullopt,
    });
    return true;
}

bool AsyncDataService::post_fail(
    const std::uint64_t request_id,
    AsyncFailure failure
) {
    if (request_id == 0U || failure.message.empty() || !core::valid_utf8(failure.message) ||
        !core::valid_utf8(failure.code)) {
        return false;
    }
    std::scoped_lock lock(completion_mutex_);
    completions_.push_back(Completion{
        CompletionKind::fail, request_id, std::nullopt, std::nullopt, std::move(failure),
    });
    return true;
}

bool AsyncDataService::cancel(const std::uint64_t request_id) {
    Binding* state = active(request_id);
    if (state == nullptr) return false;
    cancel_binding(*state);
    publish();
    return true;
}

std::size_t AsyncDataService::cancel_owner(const std::string_view owner) {
    std::size_t cancelled = 0U;
    for (auto& [binding, state] : bindings_) {
        static_cast<void>(binding);
        if (state.status != AsyncStatus::loading || state.owner != owner) continue;
        cancel_binding(state);
        ++cancelled;
    }
    if (cancelled != 0U) publish();
    return cancelled;
}

std::size_t AsyncDataService::retain_owners(
    const std::string_view owner_prefix,
    const std::set<std::string, std::less<>>& attached
) {
    std::size_t cancelled = 0U;
    for (auto& [binding, state] : bindings_) {
        static_cast<void>(binding);
        if (state.status != AsyncStatus::loading || !state.owner.starts_with(owner_prefix) ||
            attached.contains(state.owner)) {
            continue;
        }
        cancel_binding(state);
        ++cancelled;
    }
    if (cancelled != 0U) publish();
    return cancelled;
}

Value AsyncDataService::state(const std::string_view binding) const {
    const auto found = bindings_.find(binding);
    return found != bindings_.end() ? value(found->second) : value(Binding{});
}

std::size_t AsyncDataService::active_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(bindings_, [](const auto& entry) {
        return entry.second.status == AsyncStatus::loading;
    }));
}

AsyncDataService::Binding* AsyncDataService::active(
    const std::uint64_t request_id
) noexcept {
    for (auto& [binding, state] : bindings_) {
        static_cast<void>(binding);
        if (state.status == AsyncStatus::loading && state.request_id == request_id) return &state;
    }
    return nullptr;
}

void AsyncDataService::cancel_binding(Binding& binding) {
    const bool notify = binding.status == AsyncStatus::loading && binding.begun && adapter_.cancel;
    const std::uint64_t request_id = binding.request_id;
    // Invalidate first: a host that synchronously reports completion from cancel observes stale work.
    binding = Binding{};
    if (notify) adapter_.cancel(request_id);
}

void AsyncDataService::drain_completions() {
    std::vector<Completion> pending;
    {
        std::scoped_lock lock(completion_mutex_);
        pending.swap(completions_);
    }
    for (Completion& completion : pending) {
        switch (completion.kind) {
        case CompletionKind::progress:
            static_cast<void>(progress(completion.request_id, std::move(*completion.progress)));
            break;
        case CompletionKind::succeed:
            static_cast<void>(succeed(completion.request_id, std::move(*completion.value)));
            break;
        case CompletionKind::fail:
            static_cast<void>(fail(completion.request_id, std::move(*completion.failure)));
            break;
        }
    }
}

void AsyncDataService::publish() {
    if (!publisher_) return;
    std::vector<std::pair<std::string, Value>> roots;
    roots.reserve(bindings_.size());
    for (const auto& [binding, state] : bindings_) roots.emplace_back(binding, value(state));
    publisher_(Value(std::move(roots)));
}

Value AsyncDataService::value(const Binding& binding) {
    Value progress;
    if (binding.progress.has_value()) {
        progress = Value(std::vector<std::pair<std::string, Value>>{
            {"completed", Value(binding.progress->completed)},
            {"message", Value(binding.progress->message)},
            {"total", binding.progress->total.has_value()
                ? Value(*binding.progress->total) : Value{}},
        });
    }
    Value failure;
    if (binding.failure.has_value()) {
        failure = Value(std::vector<std::pair<std::string, Value>>{
            {"code", Value(binding.failure->code)},
            {"message", Value(binding.failure->message)},
        });
    }
    return Value(std::vector<std::pair<std::string, Value>>{
        {"error", std::move(failure)},
        {"progress", std::move(progress)},
        {"status", Value(status_name(binding.status))},
        {"value", binding.value.has_value() ? *binding.value : Value{}},
    });
}

} // namespace strata::runtime
