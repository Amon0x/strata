#include "ui/motion.hpp"

#include <cmath>
#include <limits>
#include <string>
#include <string_view>

#include "ui/motion/config.hpp"
#include "ui/motion/players.hpp"
#include "ui/theme.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const runtime::Value* field(
    const runtime::Value* value,
    const std::string_view name
) noexcept {
    return value != nullptr ? value->field(name) : nullptr;
}

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    if (value->key() != nullptr) return &value->key()->value;
    return nullptr;
}

[[nodiscard]] MotionTiming timing(
    const RetainedNode& node,
    const runtime::Value* value
) noexcept {
    MotionTiming result = theme_motion_timing(node.description(), default_motion_timing_name);
    if (value != nullptr && value->number() != nullptr && std::isfinite(*value->number()) &&
        *value->number() > 0.0 &&
        *value->number() <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        result.duration_nanos = static_cast<std::int64_t>(*value->number());
        result.delay_nanos = 0;
        return result;
    }
    const std::string* name = text(value);
    return name != nullptr ? theme_motion_timing(node.description(), *name) : result;
}

[[nodiscard]] const runtime::Value* named_property(
    const RetainedNode& node,
    const runtime::Value* declaration,
    const std::string_view field_name
) noexcept {
    const std::string* name = text(field(declaration, field_name));
    return name != nullptr ? motion_detail::node_property(node, *name) : nullptr;
}

[[nodiscard]] bool declared_target(
    const RetainedNode& node,
    const runtime::Value* declaration,
    const bool fallback
) noexcept {
    const runtime::Value* value = field(declaration, "expanded");
    if (value == nullptr || value->boolean() == nullptr) {
        value = named_property(node, declaration, "controlled");
    }
    if (value == nullptr || value->boolean() == nullptr) {
        const std::string* retained_name = text(field(declaration, "retained"));
        value = retained_name != nullptr ? node.retained_value(*retained_name) : nullptr;
    }
    if (value == nullptr || value->boolean() == nullptr) {
        value = named_property(node, declaration, "initial");
    }
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

} // namespace

std::optional<DisclosureMotionSpec> disclosure_motion(const RetainedNode& node) {
    const runtime::Value* defaults = motion_detail::node_style(node, "$disclosureDefaults");
    const runtime::Value* authored = motion_detail::node_style(node, "disclosure");
    if ((defaults == nullptr || defaults->object() == nullptr) &&
        (authored == nullptr || authored->object() == nullptr)) {
        return std::nullopt;
    }
    const runtime::Value* declaration = authored != nullptr && authored->object() != nullptr
                                            ? authored
                                            : defaults;
    DisclosureMotionSpec result;
    const runtime::Value* collapsed = field(declaration, "collapsedExtent");
    if ((collapsed == nullptr || collapsed->number() == nullptr) && declaration != defaults) {
        collapsed = field(defaults, "collapsedExtent");
    }
    if (collapsed != nullptr && collapsed->number() != nullptr &&
        std::isfinite(*collapsed->number()) && *collapsed->number() >= 0.0) {
        result.collapsed_extent = *collapsed->number();
    }
    result.expanded = declared_target(
        node,
        declaration,
        declaration != defaults ? declared_target(node, defaults, false) : false
    );
    const runtime::Value* duration = field(declaration, "durationNanos");
    if (duration == nullptr) duration = field(declaration, "timing");
    if (duration == nullptr) duration = field(declaration, "policy");
    if (duration == nullptr && declaration != defaults) {
        duration = field(defaults, "durationNanos");
        if (duration == nullptr) duration = field(defaults, "timing");
    }
    result.timing = timing(node, duration);
    return result;
}

bool motion_input_eligible(const RetainedNode& node) noexcept {
    for (const RetainedNode* current = &node; current != nullptr; current = current->parent()) {
        if (current->lifecycle() != RetainedLifecycle::attached) return false;
        const std::optional<DisclosureMotionSpec> disclosure = disclosure_motion(*current);
        if (current != &node && disclosure.has_value() && !disclosure->expanded) return false;
    }
    return true;
}

} // namespace strata::ui
