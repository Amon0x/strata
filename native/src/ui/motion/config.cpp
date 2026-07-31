#include "ui/motion/config.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string_view>

namespace strata::ui::motion_detail {
namespace {

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    if (value->key() != nullptr) return &value->key()->value;
    return nullptr;
}

[[nodiscard]] std::optional<MotionValue> motion_value(const runtime::Value* value) noexcept {
    if (value == nullptr) return std::nullopt;
    if (value->number() != nullptr && std::isfinite(*value->number())) return *value->number();
    if (value->color() != nullptr) return *value->color();
    if (value->boolean() != nullptr) return *value->boolean();
    return std::nullopt;
}

[[nodiscard]] const CompiledMotion* referenced_motion(
    const runtime::Value* value,
    MotionCatalog& catalog
) {
    const std::string* name = text(value);
    return name != nullptr ? catalog.find(*name) : nullptr;
}

[[nodiscard]] const CompiledMotion* attached_motion(
    const runtime::Value* value,
    MotionCatalog& catalog
) {
    return referenced_motion(
        value != nullptr && value->object() != nullptr ? value->field("animation") : value,
        catalog
    );
}

[[nodiscard]] MotionDirection attachment_direction(const runtime::Value* value) noexcept {
    const std::string* direction = value != nullptr ? text(value->field("direction")) : nullptr;
    if (direction == nullptr || *direction == "FORWARD") return MotionDirection::forward;
    if (*direction == "REVERSE") return MotionDirection::reverse;
    if (*direction == "TO_TARGET") return MotionDirection::to_target;
    if (*direction == "EXPAND") return MotionDirection::expand;
    if (*direction == "COLLAPSE") return MotionDirection::collapse;
    return MotionDirection::forward;
}

[[nodiscard]] std::optional<MotionTrigger> attachment_continuity(
    const runtime::Value* value
) noexcept {
    const std::string* trigger = value != nullptr ? text(value->field("continuityTrigger")) : nullptr;
    return trigger != nullptr ? motion_trigger(*trigger) : std::nullopt;
}

[[nodiscard]] bool attachment_cancel_on_detach(const runtime::Value* value) noexcept {
    const runtime::Value* flag = value != nullptr ? value->field("cancelOnDetach") : nullptr;
    return flag == nullptr || flag->boolean() == nullptr || *flag->boolean();
}

void bind_trigger(
    std::map<MotionTrigger, TriggerBinding>& bindings,
    const MotionTrigger trigger,
    const CompiledMotion* animation,
    const MotionDirection direction = MotionDirection::forward,
    const std::optional<MotionTrigger> continuity_from = std::nullopt,
    const bool cancel_on_detach = true
) {
    if (animation == nullptr) return;
    bindings.insert_or_assign(
        trigger,
        TriggerBinding{trigger, animation, direction, continuity_from, cancel_on_detach}
    );
}

[[nodiscard]] std::optional<MotionInteraction> interaction(const std::string_view value) noexcept {
    if (value == "HOVER" || value == "hover") return MotionInteraction::hover;
    if (value == "PRESSED" || value == "pressed") return MotionInteraction::pressed;
    if (value == "FOCUS" || value == "focus") return MotionInteraction::focus;
    if (value == "FOCUS_VISIBLE" || value == "focus-visible" ||
        value == "focusVisible") {
        return MotionInteraction::focus_visible;
    }
    return std::nullopt;
}

} // namespace

const runtime::Value* node_property(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const auto found = node.description().properties.find(name);
    return found != node.description().properties.end() ? found->second.value() : nullptr;
}

const runtime::Value* node_style(
    const RetainedNode& node,
    const std::string_view name
) noexcept {
    const runtime::Value* layout = node_property(node, "$layout");
    if (layout != nullptr && layout->object() != nullptr) {
        if (const runtime::Value* value = layout->field(name); value != nullptr) return value;
    }
    return node_property(node, name);
}

std::optional<MotionValue> resolved_motion_value(
    const RetainedNode& node,
    const MotionProperty property
) noexcept {
    return motion_value(node_style(node, motion_property_name(property)));
}

NodeMotionConfig node_motion_config(const RetainedNode& node, MotionCatalog& catalog) {
    NodeMotionConfig result;
    std::map<MotionTrigger, TriggerBinding> triggers;
    if (const CompiledMotion* animation = attached_motion(node_style(node, "animation"), catalog);
        animation != nullptr) {
        bind_trigger(triggers, animation->trigger, animation);
    }
    if (const CompiledMotion* transition = referenced_motion(node_style(node, "transition"), catalog);
        transition != nullptr) {
        const std::string* sequence = text(node_style(node, "$transitionSequence"));
        if (sequence != nullptr && *sequence == "OUT_IN") {
            const std::int64_t exit_duration = std::max<std::int64_t>(
                1, transition->timing.duration_nanos / 2
            );
            const std::int64_t enter_duration = std::max<std::int64_t>(
                1, transition->timing.duration_nanos - exit_duration
            );
            bind_trigger(
                triggers,
                MotionTrigger::enter,
                catalog.timed(
                    transition->name,
                    enter_duration,
                    transition->timing.delay_nanos + exit_duration
                )
            );
            bind_trigger(
                triggers,
                MotionTrigger::exit,
                catalog.timed(
                    transition->name,
                    exit_duration,
                    transition->timing.delay_nanos
                ),
                MotionDirection::reverse,
                MotionTrigger::enter
            );
        } else {
            bind_trigger(triggers, MotionTrigger::enter, transition);
            bind_trigger(
                triggers,
                MotionTrigger::exit,
                transition,
                MotionDirection::reverse,
                MotionTrigger::enter
            );
        }
    }
    const auto authored_trigger = [&node, &catalog, &triggers](
                                      const std::string_view property,
                                      const MotionTrigger trigger
                                  ) {
        const runtime::Value* value = node_style(node, property);
        bind_trigger(
            triggers,
            trigger,
            attached_motion(value, catalog),
            attachment_direction(value),
            attachment_continuity(value),
            attachment_cancel_on_detach(value)
        );
    };
    authored_trigger("enter", MotionTrigger::enter);
    authored_trigger("exit", MotionTrigger::exit);
    authored_trigger("move", MotionTrigger::move);
    authored_trigger("hover", MotionTrigger::hover);
    authored_trigger("pressed", MotionTrigger::pressed);
    authored_trigger("focus", MotionTrigger::focus);
    authored_trigger("focusVisible", MotionTrigger::focus_visible);
    authored_trigger("checked", MotionTrigger::checked);
    authored_trigger("animate", MotionTrigger::animate);
    result.triggers.reserve(triggers.size());
    for (const auto& [trigger, binding] : triggers) {
        static_cast<void>(trigger);
        result.triggers.push_back(binding);
    }

    const runtime::Value* motions = node_style(node, "motions");
    if (motions != nullptr && motions->list() != nullptr) {
        for (const runtime::Value& item : motions->list()->values) {
            if (item.object() == nullptr) continue;
            const std::string* id = text(item.field("id"));
            if (id == nullptr || id->empty()) continue;
            if (const std::string* property_name = text(item.field("property")); property_name != nullptr) {
                const std::optional<MotionProperty> property = motion_property(*property_name);
                const std::optional<MotionValue> target = motion_value(item.field("target"));
                if (!property.has_value() || !target.has_value()) continue;
                const std::string* timing = text(item.field("policy"));
                result.targets.push_back(TargetBinding{
                    *id,
                    *property,
                    *target,
                    timing != nullptr && !timing->empty() ? *timing : "standard",
                    false,
                });
                continue;
            }
            const CompiledMotion* animation = referenced_motion(item.field("animation"), catalog);
            if (animation == nullptr || animation->timing.repeat.kind != MotionRepeatKind::none) {
                continue;
            }
            TimelineBinding channel{*id, animation};
            if (const std::string* interaction_name = text(item.field("interaction"));
                interaction_name != nullptr) {
                channel.interaction = interaction(*interaction_name);
                if (!channel.interaction.has_value()) continue;
            } else if (const runtime::Value* target = item.field("target");
                       target != nullptr && target->boolean() != nullptr) {
                channel.state_target = *target->boolean();
            } else {
                continue;
            }
            result.timelines.push_back(std::move(channel));
        }
    }

    const runtime::Value* motion_timeline = node_style(node, "$timeline");
    if (motion_timeline != nullptr && motion_timeline->object() != nullptr) {
        const std::string* id = text(motion_timeline->field("id"));
        const runtime::Value* duration = motion_timeline->field("durationNanos");
        if (id != nullptr && !id->empty() && duration != nullptr &&
            duration->number() != nullptr && std::isfinite(*duration->number()) &&
            *duration->number() >= 1.0 &&
            *duration->number() <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
            const auto boolean = [motion_timeline](
                                     const std::string_view field,
                                     const bool fallback
                                 ) noexcept {
                const runtime::Value* value = motion_timeline->field(field);
                return value != nullptr && value->boolean() != nullptr
                           ? *value->boolean()
                           : fallback;
            };
            result.motion_timelines.push_back(MotionTimelineBinding{
                *id,
                MotionTimelineSpec{
                    static_cast<std::int64_t>(*duration->number()),
                    boolean("running", true),
                    boolean("loop", false),
                    boolean("affectsLayout", false),
                },
            });
        }
    }

    const runtime::Value* resolved = node_style(node, "animateChanges");
    if (resolved != nullptr && resolved->object() != nullptr) {
        const std::string* timing = text(resolved->field("policy"));
        const runtime::Value* properties = resolved->field("properties");
        if (properties != nullptr && properties->list() != nullptr) {
            for (const runtime::Value& property_value : properties->list()->values) {
                const std::string* property_name = text(&property_value);
                if (property_name == nullptr) continue;
                const std::optional<MotionProperty> property = motion_property(*property_name);
                if (!property.has_value()) continue;
                const std::optional<MotionValue> target = resolved_motion_value(node, *property);
                if (!target.has_value()) continue;
                result.targets.push_back(TargetBinding{
                    "strata.resolved." + std::string(motion_property_name(*property)),
                    *property,
                    *target,
                    timing != nullptr && !timing->empty() ? *timing : "standard",
                    true,
                });
            }
        }
    }
    return result;
}

} // namespace strata::ui::motion_detail
