#pragma once

#include <cstddef>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace strata::ui::collection {

/** Stable insertion-ordered set used by collection state and event payloads. */
class KeySet final {
public:
    KeySet() = default;
    explicit KeySet(std::span<const std::string> keys);
    explicit KeySet(std::vector<std::string> keys);

    [[nodiscard]] const std::vector<std::string>& values() const noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] bool insert(std::string key);
    [[nodiscard]] bool erase(std::string_view key);
    void clear() noexcept;

    [[nodiscard]] friend bool operator==(const KeySet& left, const KeySet& right) {
        return left.values_ == right.values_;
    }

private:
    std::vector<std::string> values_;
    std::set<std::string, std::less<>> index_;
};

enum class SelectionMode { none, single, multiple };

struct SelectionModifiers final {
    bool command = false;
    bool shift = false;
};

struct SelectionTransition final {
    KeySet selected;
    std::optional<std::string> anchor;
    std::optional<std::string> active;
};

[[nodiscard]] SelectionTransition select(
    std::span<const std::string> ordered,
    const KeySet& current,
    std::optional<std::string> anchor,
    std::string key,
    SelectionMode mode,
    SelectionModifiers modifiers = {}
);

[[nodiscard]] SelectionTransition select_all(
    std::span<const std::string> ordered,
    SelectionMode mode
);

enum class Navigation {
    previous,
    next,
    first,
    last,
    page_previous,
    page_next,
    column_previous,
    column_next,
};

[[nodiscard]] std::optional<std::string> navigate(
    std::span<const std::string> ordered,
    const std::optional<std::string>& active,
    Navigation navigation,
    std::size_t page_size = 10U,
    std::size_t columns = 1U
);

struct Label final {
    std::string key;
    std::string value;
};

[[nodiscard]] std::optional<std::string> typeahead(
    std::span<const Label> labels,
    const std::optional<std::string>& active,
    std::string_view query
);

[[nodiscard]] double reveal_offset(
    double current_offset,
    double viewport_extent,
    double item_start,
    double item_end,
    double sticky_leading_inset = 0.0
);

} // namespace strata::ui::collection
