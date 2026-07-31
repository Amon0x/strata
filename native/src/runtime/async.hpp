#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/value.hpp"

namespace strata::runtime {

enum class AsyncStatus { idle, loading, ready, failed };

struct AsyncProgress final {
    double completed = 0.0;
    std::optional<double> total;
    std::string message;
};

struct AsyncFailure final {
    std::string message;
    std::string code;
};

struct AsyncRequest final {
    std::uint64_t id = 0U;
    std::string binding;
    std::string owner;
    Value payload;
};

struct AsyncHostAdapter final {
    std::function<void(const AsyncRequest&)> begin;
    std::function<void(std::uint64_t request_id)> cancel;
};

/** Deterministic typed query lifecycle with debounce, latest-wins, and owner cancellation. */
class AsyncDataService final {
public:
    using BindingValidator = std::function<bool(std::string_view)>;
    using ResultNormalizer = std::function<std::optional<Value>(std::string_view, const Value&)>;
    using Publisher = std::function<void(const Value& roots)>;

    AsyncDataService(
        BindingValidator validator = {},
        ResultNormalizer result_normalizer = {},
        Publisher publisher = {}
    );
    ~AsyncDataService();

    void set_adapter(AsyncHostAdapter adapter);
    /** Declares idle roots before first frame. */
    void initialize(std::set<std::string, std::less<>> bindings);
    [[nodiscard]] std::optional<std::uint64_t> query(
        std::string binding,
        Value payload,
        std::string owner,
        std::int64_t now_nanos,
        std::int64_t debounce_nanos = 0
    );
    void advance(std::int64_t now_nanos);
    /** Owner-thread completion paths, primarily for deterministic core tests and adapters. */
    [[nodiscard]] bool progress(std::uint64_t request_id, AsyncProgress progress);
    [[nodiscard]] bool succeed(std::uint64_t request_id, Value value);
    [[nodiscard]] bool fail(std::uint64_t request_id, AsyncFailure failure);
    /** Thread-safe host mailbox. Posted completions are adopted on the next owner-thread advance. */
    [[nodiscard]] bool post_progress(std::uint64_t request_id, AsyncProgress progress);
    [[nodiscard]] bool post_succeed(std::uint64_t request_id, Value value);
    [[nodiscard]] bool post_fail(std::uint64_t request_id, AsyncFailure failure);
    [[nodiscard]] bool cancel(std::uint64_t request_id);
    [[nodiscard]] std::size_t cancel_owner(std::string_view owner);
    [[nodiscard]] std::size_t retain_owners(
        std::string_view owner_prefix,
        const std::set<std::string, std::less<>>& attached
    );

    [[nodiscard]] Value state(std::string_view binding) const;
    [[nodiscard]] std::size_t active_count() const noexcept;

private:
    struct Binding final {
        AsyncStatus status = AsyncStatus::idle;
        std::uint64_t request_id = 0U;
        std::string owner;
        Value payload;
        std::optional<Value> value;
        std::optional<AsyncProgress> progress;
        std::optional<AsyncFailure> failure;
        std::int64_t due_nanos = 0;
        bool begun = false;
    };

    enum class CompletionKind { progress, succeed, fail };
    struct Completion final {
        CompletionKind kind = CompletionKind::progress;
        std::uint64_t request_id = 0U;
        std::optional<AsyncProgress> progress;
        std::optional<Value> value;
        std::optional<AsyncFailure> failure;
    };

    [[nodiscard]] Binding* active(std::uint64_t request_id) noexcept;
    void cancel_binding(Binding& binding);
    void drain_completions();
    void publish();
    [[nodiscard]] static Value value(const Binding& binding);

    std::map<std::string, Binding, std::less<>> bindings_;
    BindingValidator validator_;
    ResultNormalizer result_normalizer_;
    Publisher publisher_;
    AsyncHostAdapter adapter_;
    std::uint64_t next_request_id_ = 1U;
    std::mutex completion_mutex_;
    std::vector<Completion> completions_;
};

} // namespace strata::runtime
