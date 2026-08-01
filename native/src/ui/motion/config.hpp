#pragma once

#include <optional>
#include <string>
#include <vector>

#include "ui/motion/catalog.hpp"
#include "ui/tree.hpp"

namespace strata::ui::motion_detail {

struct TriggerBinding final {
    MotionTrigger trigger = MotionTrigger::animate;
    const CompiledMotion* animation = nullptr;
    MotionDirection active_direction = MotionDirection::forward;
    std::optional<MotionTrigger> continuity_from;
    bool cancel_on_detach = true;
};

struct TimelineBinding final {
    std::string id;
    const CompiledMotion* animation = nullptr;
    std::optional<MotionInteraction> interaction = std::nullopt;
    std::optional<bool> state_target = std::nullopt;
};

struct MotionTimelineBinding final {
    std::string id;
    MotionTimelineSpec spec;
};

struct TargetBinding final {
    std::string id;
    MotionProperty property = MotionProperty::opacity;
    MotionValue target = 0.0;
    std::string timing = "standard";
    bool resolved_style = false;
};

struct NodeMotionConfig final {
    std::vector<TriggerBinding> triggers;
    std::vector<TimelineBinding> timelines;
    std::vector<MotionTimelineBinding> motion_timelines;
    std::vector<TargetBinding> targets;
};

[[nodiscard]] NodeMotionConfig node_motion_config(
    const RetainedNode& node,
    MotionCatalog& catalog
);
[[nodiscard]] const runtime::Value* node_property(
    const RetainedNode& node,
    std::string_view name
) noexcept;
[[nodiscard]] const runtime::Value* node_style(
    const RetainedNode& node,
    std::string_view name
) noexcept;
[[nodiscard]] std::optional<MotionValue> resolved_motion_value(
    const RetainedNode& node,
    MotionProperty property
) noexcept;

} // namespace strata::ui::motion_detail
