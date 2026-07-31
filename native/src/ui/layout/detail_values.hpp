#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ui/layout.hpp"

namespace strata::ui::layout_detail {

constexpr double infinity = std::numeric_limits<double>::infinity();

[[nodiscard]] inline bool finite_non_negative(const double value) noexcept {
    return std::isfinite(value) && value >= 0.0;
}

[[nodiscard]] inline double finite_or(const double value, const double fallback) noexcept {
    return std::isfinite(value) ? value : fallback;
}

[[nodiscard]] inline double subtract_finite(const double value, const double amount) noexcept {
    return std::isfinite(value) ? std::max(0.0, value - amount) : value;
}

[[nodiscard]] inline const runtime::Value* scalar_property(
    const DescriptionNode& description,
    const std::string_view name
) noexcept {
    const auto found = description.properties.find(name);
    return found != description.properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] inline const runtime::Value* layout_value(const DescriptionNode& description) noexcept {
    if (const runtime::Value* normalized = scalar_property(description, "$layout"); normalized != nullptr) {
        return normalized;
    }
    return scalar_property(description, "layout");
}

[[nodiscard]] inline const runtime::Value* resolved_style_property(
    const DescriptionNode& description,
    const runtime::Value* layout,
    const std::string_view name
) noexcept {
    if (layout != nullptr && layout->object() != nullptr) {
        if (const runtime::Value* value = layout->field(name); value != nullptr) return value;
    }
    return scalar_property(description, name);
}

[[nodiscard]] inline const runtime::Value* field(
    const runtime::Value* object,
    const std::string_view name
) noexcept {
    return object != nullptr ? object->field(name) : nullptr;
}

[[nodiscard]] inline double number(const runtime::Value* value, const double fallback = 0.0) noexcept {
    return value != nullptr && value->number() != nullptr ? *value->number() : fallback;
}

[[nodiscard]] inline bool boolean(const runtime::Value* value, const bool fallback = false) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] inline std::string_view text(const runtime::Value* value) noexcept {
    return value != nullptr && value->string() != nullptr ? std::string_view(*value->string()) : std::string_view{};
}

[[nodiscard]] inline double non_negative_number(const runtime::Value* value, const double fallback = 0.0) noexcept {
    const double candidate = number(value, fallback);
    return finite_non_negative(candidate) ? candidate : fallback;
}

[[nodiscard]] inline Edges edges(const runtime::Value* value) noexcept {
    if (value == nullptr) return {};
    if (value->number() != nullptr) {
        const double all = non_negative_number(value);
        return Edges{all, all, all, all};
    }
    if (value->object() == nullptr) return {};
    const double all = non_negative_number(value->field("all"));
    const double horizontal = non_negative_number(value->field("horizontal"), all);
    const double vertical = non_negative_number(value->field("vertical"), all);
    return Edges{
        non_negative_number(value->field("left"), horizontal),
        non_negative_number(value->field("top"), vertical),
        non_negative_number(value->field("right"), horizontal),
        non_negative_number(value->field("bottom"), vertical),
    };
}

[[nodiscard]] inline LayoutSize layout_size(const runtime::Value* value) {
    LayoutSize result;
    if (value == nullptr) return result;
    if (value->number() != nullptr) {
        result.kind = LayoutSize::Kind::fixed;
        result.value = non_negative_number(value);
        return result;
    }
    if (value->string() != nullptr) {
        result.kind = *value->string() == "content" ? LayoutSize::Kind::content : LayoutSize::Kind::automatic;
        return result;
    }
    if (value->object() == nullptr) return result;
    if (const runtime::Value* weight = value->field("weight"); weight != nullptr) {
        result.kind = LayoutSize::Kind::fill;
        result.value = number(weight, 1.0);
        if (!std::isfinite(result.value) || result.value <= 0.0) result.value = 1.0;
        return result;
    }
    if (const runtime::Value* fraction = value->field("fraction"); fraction != nullptr) {
        result.kind = LayoutSize::Kind::percent;
        result.value = non_negative_number(fraction);
        return result;
    }
    if (value->field("preferred") == nullptr && value->field("min") == nullptr &&
        value->field("max") == nullptr) return result;
    const runtime::Value* minimum = value->field("min");
    const runtime::Value* maximum = value->field("max");
    return LayoutSize::clamp(
        minimum != nullptr && minimum->kind() != runtime::ValueKind::null_value
            ? std::optional<LayoutSize>(layout_size(minimum)) : std::nullopt,
        layout_size(value->field("preferred")),
        maximum != nullptr && maximum->kind() != runtime::ValueKind::null_value
            ? std::optional<LayoutSize>(layout_size(maximum)) : std::nullopt
    );
}

[[nodiscard]] inline std::vector<LayoutSize> layout_tracks(const runtime::Value* value) {
    std::vector<LayoutSize> result;
    if (value == nullptr || value->list() == nullptr) return result;
    result.reserve(value->list()->values.size());
    for (const runtime::Value& track : value->list()->values) result.push_back(layout_size(&track));
    return result;
}

[[nodiscard]] inline std::optional<std::size_t> index_value(const runtime::Value* value) noexcept {
    if (value == nullptr || value->number() == nullptr || !std::isfinite(*value->number()) ||
        *value->number() < 0.0) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(std::floor(*value->number()));
}

[[nodiscard]] inline std::size_t span_value(const runtime::Value* value) noexcept {
    return std::max<std::size_t>(1U, index_value(value).value_or(1U));
}

[[nodiscard]] inline LayoutKind kind(const std::string_view name) noexcept {
    if (name == "STACK") return LayoutKind::stack;
    if (name == "ROW") return LayoutKind::row;
    if (name == "COLUMN") return LayoutKind::column;
    if (name == "GRID") return LayoutKind::grid;
    if (name == "OVERLAY") return LayoutKind::overlay;
    if (name == "SPACER") return LayoutKind::spacer;
    if (name == "SCROLL") return LayoutKind::scroll;
    if (name == "PORTAL") return LayoutKind::portal;
    return LayoutKind::panel;
}

[[nodiscard]] inline LayoutAlign align(const std::string_view name) noexcept {
    if (name == "CENTER") return LayoutAlign::center;
    if (name == "END") return LayoutAlign::end;
    if (name == "STRETCH") return LayoutAlign::stretch;
    return LayoutAlign::start;
}

[[nodiscard]] inline LayoutJustify justify(const std::string_view name) noexcept {
    if (name == "CENTER") return LayoutJustify::center;
    if (name == "END") return LayoutJustify::end;
    if (name == "SPACE_BETWEEN") return LayoutJustify::space_between;
    if (name == "SPACE_AROUND") return LayoutJustify::space_around;
    if (name == "SPACE_EVENLY") return LayoutJustify::space_evenly;
    return LayoutJustify::start;
}

} // namespace strata::ui::layout_detail
