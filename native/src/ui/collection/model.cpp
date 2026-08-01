#include "ui/collection/model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace strata::ui::collection {
namespace {

[[nodiscard]] std::string lower(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

[[nodiscard]] std::optional<std::size_t> index_of(
    const std::span<const std::string> values,
    const std::string_view key
) noexcept {
    const auto found = std::ranges::find(values, key);
    return found != values.end()
        ? std::optional<std::size_t>(static_cast<std::size_t>(found - values.begin()))
        : std::nullopt;
}

} // namespace

KeySet::KeySet(const std::span<const std::string> keys) {
    for (const std::string& key : keys) static_cast<void>(insert(key));
}

KeySet::KeySet(std::vector<std::string> keys) {
    for (std::string& key : keys) static_cast<void>(insert(std::move(key)));
}

const std::vector<std::string>& KeySet::values() const noexcept { return values_; }

bool KeySet::contains(const std::string_view key) const noexcept {
    return index_.contains(key);
}

bool KeySet::empty() const noexcept { return values_.empty(); }
std::size_t KeySet::size() const noexcept { return values_.size(); }

bool KeySet::insert(std::string key) {
    if (!index_.insert(key).second) return false;
    values_.push_back(std::move(key));
    return true;
}

bool KeySet::erase(const std::string_view key) {
    const auto indexed = index_.find(key);
    if (indexed == index_.end()) return false;
    index_.erase(indexed);
    const auto found = std::ranges::find(values_, key);
    if (found != values_.end()) values_.erase(found);
    return true;
}

void KeySet::clear() noexcept {
    values_.clear();
    index_.clear();
}

SelectionTransition select(
    const std::span<const std::string> ordered,
    const KeySet& current,
    std::optional<std::string> anchor,
    std::string key,
    const SelectionMode mode,
    const SelectionModifiers modifiers
) {
    if (mode == SelectionMode::none || !index_of(ordered, key).has_value()) {
        return SelectionTransition{KeySet{}, std::nullopt, std::move(key)};
    }
    if (mode == SelectionMode::single) {
        KeySet selected;
        static_cast<void>(selected.insert(key));
        return SelectionTransition{std::move(selected), key, std::move(key)};
    }
    if (modifiers.shift) {
        if (!anchor.has_value() || !index_of(ordered, *anchor).has_value()) {
            const auto fallback = std::ranges::find_if(
                current.values().rbegin(),
                current.values().rend(),
                [ordered](const std::string& candidate) {
                    return index_of(ordered, candidate).has_value();
                }
            );
            anchor = fallback != current.values().rend()
                ? std::optional<std::string>(*fallback)
                : std::optional<std::string>(key);
        }
        const std::size_t first = *index_of(ordered, *anchor);
        const std::size_t last = *index_of(ordered, key);
        KeySet selected = modifiers.command ? current : KeySet{};
        const std::size_t low = std::min(first, last);
        const std::size_t high = std::max(first, last);
        for (std::size_t index = low; index <= high; ++index) {
            static_cast<void>(selected.insert(ordered[index]));
        }
        return SelectionTransition{std::move(selected), std::move(anchor), std::move(key)};
    }
    if (modifiers.command) {
        KeySet selected = current;
        if (!selected.erase(key)) static_cast<void>(selected.insert(key));
        if (!anchor.has_value()) anchor = key;
        return SelectionTransition{std::move(selected), std::move(anchor), std::move(key)};
    }
    KeySet selected;
    static_cast<void>(selected.insert(key));
    return SelectionTransition{std::move(selected), key, std::move(key)};
}

SelectionTransition select_all(
    const std::span<const std::string> ordered,
    const SelectionMode mode
) {
    if (mode != SelectionMode::multiple || ordered.empty()) return {};
    return SelectionTransition{
        KeySet(ordered),
        ordered.front(),
        ordered.back(),
    };
}

std::optional<std::string> navigate(
    const std::span<const std::string> ordered,
    const std::optional<std::string>& active,
    const Navigation navigation,
    const std::size_t page_size,
    const std::size_t columns
) {
    if (ordered.empty()) return std::nullopt;
    const std::ptrdiff_t current = active.has_value() && index_of(ordered, *active).has_value()
        ? static_cast<std::ptrdiff_t>(*index_of(ordered, *active))
        : 0;
    const std::ptrdiff_t row = static_cast<std::ptrdiff_t>(std::max<std::size_t>(1U, columns));
    const std::ptrdiff_t page = static_cast<std::ptrdiff_t>(std::max<std::size_t>(1U, page_size));
    std::ptrdiff_t next = current;
    switch (navigation) {
    case Navigation::previous: next -= row; break;
    case Navigation::next: next += row; break;
    case Navigation::first: next = 0; break;
    case Navigation::last: next = static_cast<std::ptrdiff_t>(ordered.size() - 1U); break;
    case Navigation::page_previous: next -= page; break;
    case Navigation::page_next: next += page; break;
    case Navigation::column_previous: --next; break;
    case Navigation::column_next: ++next; break;
    }
    next = std::clamp<std::ptrdiff_t>(
        next,
        0,
        static_cast<std::ptrdiff_t>(ordered.size() - 1U)
    );
    return ordered[static_cast<std::size_t>(next)];
}

std::optional<std::string> typeahead(
    const std::span<const Label> labels,
    const std::optional<std::string>& active,
    const std::string_view query
) {
    if (labels.empty()) return std::nullopt;
    const std::string normalized = lower(query);
    if (normalized.empty()) return std::nullopt;
    std::size_t start = 0U;
    if (active.has_value()) {
        const auto found = std::ranges::find(labels, *active, &Label::key);
        if (found != labels.end()) {
            start = (static_cast<std::size_t>(found - labels.begin()) + 1U) % labels.size();
        }
    }
    for (std::size_t offset = 0U; offset < labels.size(); ++offset) {
        const Label& candidate = labels[(start + offset) % labels.size()];
        if (lower(candidate.value).starts_with(normalized)) return candidate.key;
    }
    return std::nullopt;
}

double reveal_offset(
    const double current_offset,
    const double viewport_extent,
    const double item_start,
    const double item_end,
    const double sticky_leading_inset
) {
    if (!std::isfinite(current_offset) || current_offset < 0.0 ||
        !std::isfinite(viewport_extent) || viewport_extent < 0.0 ||
        !std::isfinite(item_start) || !std::isfinite(item_end) || item_end < item_start ||
        !std::isfinite(sticky_leading_inset) || sticky_leading_inset < 0.0) {
        throw std::invalid_argument("collection reveal geometry must be finite and non-negative");
    }
    const double inset = std::min(sticky_leading_inset, viewport_extent);
    if (item_start < current_offset + inset) return std::max(0.0, item_start - inset);
    if (item_end > current_offset + viewport_extent) {
        return std::max(0.0, item_end - viewport_extent);
    }
    return current_offset;
}

} // namespace strata::ui::collection
