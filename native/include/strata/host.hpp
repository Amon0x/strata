#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <strata/strata.hpp>

namespace strata::host {

/** Structured host value. JSON exists only when this value crosses the stable C ABI. */
class Value final {
  public:
    using Array = std::vector<Value>;
    using Object = std::map<std::string, Value, std::less<>>;
    using Storage =
        std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    Value() noexcept;
    Value(std::nullptr_t) noexcept;
    Value(bool value) noexcept;
    Value(std::int64_t value) noexcept;
    Value(double value);
    Value(const char* value);
    Value(std::string value);
    Value(std::string_view value);
    Value(Array value);
    Value(Object value);

    template <typename Integer>
        requires(std::is_integral_v<Integer> && !std::is_same_v<std::remove_cv_t<Integer>, bool> &&
                 !std::is_same_v<std::remove_cv_t<Integer>, std::int64_t>)
    Value(const Integer value) {
        if constexpr (std::is_unsigned_v<Integer>) {
            if (value > static_cast<std::make_unsigned_t<std::int64_t>>(
                            std::numeric_limits<std::int64_t>::max())) {
                throw std::out_of_range("host integer does not fit signed JSON range");
            }
        }
        storage_ = static_cast<std::int64_t>(value);
    }

    [[nodiscard]] static Value array(std::initializer_list<Value> values);
    [[nodiscard]] static Value object(std::initializer_list<std::pair<std::string, Value>> fields);
    [[nodiscard]] static Value parse(std::string_view json);

    template <typename Range, typename Encode>
    [[nodiscard]] static Value array(const Range& values, Encode&& encode) {
        Array result;
        if constexpr (requires { values.size(); })
            result.reserve(values.size());
        for (const auto& value : values) {
            result.emplace_back(std::invoke(encode, value));
        }
        return Value(std::move(result));
    }

    [[nodiscard]] const Storage& storage() const noexcept;
    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] const bool* boolean() const noexcept;
    [[nodiscard]] const std::int64_t* integer() const noexcept;
    [[nodiscard]] const double* number() const noexcept;
    [[nodiscard]] const std::string* string() const noexcept;
    [[nodiscard]] const Array* array() const noexcept;
    [[nodiscard]] const Object* object() const noexcept;
    [[nodiscard]] const Value* find(std::string_view field) const noexcept;
    [[nodiscard]] const Value& require(std::string_view field) const;
    [[nodiscard]] std::string_view require_string(std::string_view field) const;
    [[nodiscard]] std::optional<std::string_view>
    optional_string(std::string_view field) const noexcept;
    [[nodiscard]] std::optional<std::int64_t>
    optional_integer(std::string_view field) const noexcept;
    [[nodiscard]] std::optional<double> optional_number(std::string_view field) const noexcept;
    [[nodiscard]] std::string json() const;

    /** JSON numbers compare by numeric value even when one side parsed as an integer. */
    [[nodiscard]] friend bool operator==(const Value& left, const Value& right) noexcept;

  private:
    Storage storage_;
};

[[nodiscard]] inline Value object(std::initializer_list<std::pair<std::string, Value>> fields) {
    return Value::object(fields);
}

[[nodiscard]] inline Value array(std::initializer_list<Value> values) {
    return Value::array(values);
}

enum class ActionResult { handled, forwarded, ignored };

/** Owned, decoded action invocation delivered to ordinary C++ application code. */
struct ActionEvent final {
    std::string id;
    Value payload;
    std::string kind;
    std::optional<std::string> source_key;
    Value value;
};

enum class DragPhase { start, enter, move, leave, drop, cancel };
enum class DropPlacement { on, before, after };

/** Typed projection of the framework's shared drag lifecycle event. */
struct DragEvent final {
    DragPhase phase = DragPhase::cancel;
    std::string payload_type;
    Value payload;
    std::optional<std::string> target_key;
    std::optional<std::string> operation;
    DropPlacement placement = DropPlacement::on;
    std::optional<std::string> before_key;
    std::optional<std::string> after_key;
    std::optional<std::size_t> insertion_index;

    [[nodiscard]] static std::optional<DragEvent> from(const ActionEvent& event);
    [[nodiscard]] bool dropped() const noexcept {
        return phase == DragPhase::drop;
    }
};

/** Monotonic model revision watched by one or more snapshot bindings. */
class Revision final {
  public:
    [[nodiscard]] std::uint64_t value() const noexcept {
        return value_;
    }
    void changed();

  private:
    std::uint64_t value_ = 1U;
};

/** Small observable owner for models that do not need a custom aggregate type. */
template <typename T> class Observable final {
  public:
    Observable() = default;
    explicit Observable(T value) : value_(std::move(value)) {}

    [[nodiscard]] const T& get() const noexcept {
        return value_;
    }
    [[nodiscard]] const Revision& revision() const noexcept {
        return revision_;
    }

    bool set(T value) {
        if constexpr (requires { value_ == value; }) {
            if (value_ == value)
                return false;
        }
        value_ = std::move(value);
        revision_.changed();
        return true;
    }

    template <typename Mutation> decltype(auto) update(Mutation&& mutation) {
        if constexpr (std::is_void_v<std::invoke_result_t<Mutation, T&>>) {
            std::invoke(std::forward<Mutation>(mutation), value_);
            revision_.changed();
        } else {
            decltype(auto) result = std::invoke(std::forward<Mutation>(mutation), value_);
            revision_.changed();
            return result;
        }
    }

  private:
    T value_{};
    Revision revision_;
};

/**
 * High-level host boundary for one runtime. Handlers receive typed values; snapshot sources are
 * republished automatically when their watched revision changes. The stable ABI remains JSON, but
 * application code never assembles or parses it.
 */
class Bindings final {
  public:
    using Handler = std::function<ActionResult(const ActionEvent&)>;
    using RevisionSource = std::function<std::uint64_t()>;
    using SnapshotSource = std::function<Value()>;

    explicit Bindings(Runtime& runtime, std::string owner);
    ~Bindings();
    Bindings(const Bindings&) = delete;
    Bindings& operator=(const Bindings&) = delete;
    Bindings(Bindings&&) noexcept;
    Bindings& operator=(Bindings&&) noexcept;

    void on(std::string action_id, Handler handler);
    void snapshot(std::string snapshot_id, RevisionSource revision, SnapshotSource source);

    template <typename Model, typename Snapshot>
        requires requires(const Model& value) { value.revision().value(); }
    void snapshot(std::string snapshot_id, const Model& model, Snapshot&& source) {
        snapshot(
            std::move(snapshot_id), [&model] { return model.revision().value(); },
            [&model, source = std::forward<Snapshot>(source)]() mutable {
                return std::invoke(source, model);
            });
    }

    template <typename Model, typename Snapshot>
        requires requires(const Model& value) { value.revision().value(); }
    void snapshot(std::string, const Model&&, Snapshot&&) = delete;

    /** Publishes every source whose model revision changed and surfaces contained callback errors.
     */
    void synchronize();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/** Stable-neighbor reorder used by list-backed drag models. Supports move-only item types. */
template <typename T, typename Key>
[[nodiscard]] bool reorder_before_after(std::vector<T>& values, const std::string_view source_key,
                                        const std::optional<std::string>& before_key,
                                        const std::optional<std::string>& after_key, Key&& key) {
    const auto source = std::ranges::find_if(
        values, [&](const T& value) { return std::invoke(key, value) == source_key; });
    if (source == values.end())
        return false;
    const std::size_t source_index = static_cast<std::size_t>(source - values.begin());

    std::optional<std::size_t> boundary;
    if (before_key.has_value() && *before_key != source_key) {
        const auto before = std::ranges::find_if(
            values, [&](const T& value) { return std::invoke(key, value) == *before_key; });
        if (before != values.end())
            boundary = static_cast<std::size_t>(before - values.begin());
    }
    if (!boundary.has_value() && after_key.has_value() && *after_key != source_key) {
        const auto after = std::ranges::find_if(
            values, [&](const T& value) { return std::invoke(key, value) == *after_key; });
        if (after != values.end())
            boundary = static_cast<std::size_t>(after - values.begin()) + 1U;
    }
    if (!boundary.has_value())
        return false;

    const std::size_t insertion = *boundary - (source_index < *boundary ? 1U : 0U);
    if (insertion == source_index)
        return false;
    T moving = std::move(*source);
    values.erase(source);
    values.insert(values.begin() + static_cast<std::ptrdiff_t>(insertion), std::move(moving));
    return true;
}

} // namespace strata::host
