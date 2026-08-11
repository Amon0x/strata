#include "ui/inspection.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <charconv>
#include <cctype>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/action.hpp"
#include "runtime/expression.hpp"
#include "ui/layout.hpp"
#include "ui/motion.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/tree.hpp"
#include "ui/widget/inspection.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue point(const Point value) {
    return object({{"x", JsonValue(value.x)}, {"y", JsonValue(value.y)}});
}

[[nodiscard]] JsonValue rectangle(const Rect value) {
    return object({
        {"height", JsonValue(value.height)},
        {"width", JsonValue(value.width)},
        {"x", JsonValue(value.x)},
        {"y", JsonValue(value.y)},
    });
}

[[nodiscard]] std::string_view subtarget_kind_name(const WidgetSubtargetKind kind) noexcept {
    switch (kind) {
    case WidgetSubtargetKind::control: return "control";
    case WidgetSubtargetKind::link: return "link";
    case WidgetSubtargetKind::choice: return "choice";
    case WidgetSubtargetKind::command: return "command";
    case WidgetSubtargetKind::action: return "action";
    case WidgetSubtargetKind::dismiss: return "dismiss";
    case WidgetSubtargetKind::scrim: return "scrim";
    case WidgetSubtargetKind::token: return "token";
    case WidgetSubtargetKind::separator: return "separator";
    case WidgetSubtargetKind::notification: return "notification";
    }
    return "control";
}

[[nodiscard]] JsonValue subtargets(const Surface& surface, const RetainedNode& node) {
    std::vector<JsonValue> values;
    for (const WidgetSubtarget& target : surface.input().subtargets(node.identity())) {
        std::vector<JsonValue> path;
        path.reserve(target.path.size());
        for (const std::size_t index : target.path) {
            path.emplace_back(static_cast<std::int64_t>(index));
        }
        values.push_back(object({
            {"bounds", rectangle(target.bounds)},
            {"commandId", target.command_id.empty() ? JsonValue{} : JsonValue(target.command_id)},
            {"detached", JsonValue(target.detached)},
            {"enabled", JsonValue(target.enabled)},
            {"id", JsonValue(target.id)},
            {"index", JsonValue(static_cast<std::int64_t>(target.index))},
            {"kind", JsonValue(std::string(subtarget_kind_name(target.kind)))},
            {"label", JsonValue(target.label)},
            {"notificationId", target.notification_id.has_value()
                                   ? JsonValue(static_cast<std::int64_t>(*target.notification_id))
                                   : JsonValue{}},
            {"path", array(std::move(path))},
        }));
    }
    return array(std::move(values));
}

[[nodiscard]] std::string_view dirty_name(const DirtyReason reason) noexcept {
    switch (reason) {
    case DirtyReason::structure: return "structure";
    case DirtyReason::properties: return "properties";
    case DirtyReason::layout: return "layout";
    case DirtyReason::text: return "text";
    case DirtyReason::style: return "style";
    case DirtyReason::semantics: return "semantics";
    case DirtyReason::input: return "input";
    case DirtyReason::scale: return "scale";
    case DirtyReason::animation: return "animation";
    case DirtyReason::resource: return "resource";
    case DirtyReason::editor: return "editor";
    case DirtyReason::paint: return "paint";
    }
    return "properties";
}

constexpr DirtyReason dirty_reasons[] = {
    DirtyReason::structure,
    DirtyReason::properties,
    DirtyReason::layout,
    DirtyReason::text,
    DirtyReason::style,
    DirtyReason::semantics,
    DirtyReason::input,
    DirtyReason::scale,
    DirtyReason::animation,
    DirtyReason::resource,
    DirtyReason::editor,
    DirtyReason::paint,
};

[[nodiscard]] JsonValue dirty_set(const DirtySet& dirty) {
    std::vector<JsonValue> names;
    for (const DirtyReason reason : dirty_reasons) {
        if (dirty.contains(reason)) names.emplace_back(std::string(dirty_name(reason)));
    }
    std::ranges::sort(names, {}, [](const JsonValue& value) { return *value.string(); });
    return array(std::move(names));
}

void collect_descendant_dirty(const RetainedNode& node, DirtySet& result) {
    for (const auto& child : node.children()) {
        for (const DirtyReason reason : dirty_reasons) {
            if (child->dirty().contains(reason)) result.add(reason);
        }
        collect_descendant_dirty(*child, result);
    }
}

[[nodiscard]] std::string_view action_policy_name(
    const runtime::ActionDispatchPolicy policy
) noexcept {
    switch (policy) {
    case runtime::ActionDispatchPolicy::required: return "required";
    case runtime::ActionDispatchPolicy::optional: return "optional";
    case runtime::ActionDispatchPolicy::broadcast: return "broadcast";
    case runtime::ActionDispatchPolicy::forwarded: return "forwarded";
    case runtime::ActionDispatchPolicy::framework: return "framework";
    }
    return "required";
}

[[nodiscard]] JsonValue actions(const Surface& surface, const RetainedNode& node) {
    std::vector<std::shared_ptr<const runtime::Action>> values;
    const WidgetLifecycle* lifecycle = surface.widget_registry().find(node.description().type);
    const WidgetInputPhase* input = lifecycle != nullptr ? &lifecycle->input : nullptr;
    const bool binding_present = input != nullptr && !input->action_property.empty() &&
                                 node.description().properties.contains(input->action_property);
    bool action_expression_present = false;
    const bool exposes_capability = input != nullptr && !input->action_property.empty() &&
                                    (!input->action_capability_requires_binding || binding_present);
    if (exposes_capability) {
        const auto property = node.description().properties.find(input->action_property);
        if (property != node.description().properties.end() && property->second.action() != nullptr) {
            action_expression_present = true;
            const auto& binding = *property->second.action();
            if (binding != nullptr && binding->action != nullptr) values.push_back(binding->action);
        }
        if (values.empty() && !action_expression_present && !input->fallback_action.empty()) {
            const auto contract = surface.application().bundle()->action_registry().contract(
                input->fallback_action
            );
            if (contract != nullptr) values.push_back(std::make_shared<const runtime::Action>(contract));
        }
    }
    if (values.empty() && !action_expression_present) {
        const auto default_property = node.description().properties.find("$defaultAction");
        const runtime::Value* default_value = default_property != node.description().properties.end()
                                                  ? default_property->second.value()
                                                  : nullptr;
        if (default_value != nullptr && default_value->string() != nullptr) {
            if (const auto contract = surface.application().bundle()->action_registry().contract(
                    *default_value->string()
                ); contract != nullptr) {
                values.push_back(std::make_shared<const runtime::Action>(contract));
            }
        }
    }
    for (const DescriptionBehavior& behavior : node.description().behaviors) {
        if (behavior.action == nullptr) continue;
        const auto collect = [&values](const auto& self, const runtime::ActionValue& action) -> void {
            if (action.composition.has_value()) {
                for (const auto& child : action.children) {
                    if (child != nullptr) self(self, *child);
                }
            } else if (action.action != nullptr) {
                values.push_back(action.action);
            }
        };
        collect(collect, *behavior.action);
    }
    std::ranges::sort(values, {}, [](const auto& value) { return value->id(); });
    values.erase(std::unique(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left->id() == right->id();
    }), values.end());
    std::vector<JsonValue> encoded;
    encoded.reserve(values.size());
    for (const auto& action : values) {
        std::vector<JsonValue> owners;
        std::vector<std::string> action_owners =
            surface.application().actions().handler_owners(action->id());
        if (action->contract->dispatch_policy == runtime::ActionDispatchPolicy::framework) {
            if (action->id().starts_with("state.")) {
                action_owners.emplace_back("strata.surface.state");
            } else {
                action_owners.emplace_back("strata.surface.declarative");
            }
        }
        std::ranges::sort(action_owners);
        action_owners.erase(std::unique(action_owners.begin(), action_owners.end()), action_owners.end());
        for (std::string owner : action_owners) {
            owners.emplace_back(std::move(owner));
        }
        encoded.push_back(object({
            {"dispatchPolicy", JsonValue(std::string(action_policy_name(action->contract->dispatch_policy)))},
            {"dynamic", JsonValue(action->dynamic)},
            {"handlerOwners", array(std::move(owners))},
            {"id", JsonValue(action->id())},
            {"payloadContract", JsonValue(action->contract->payload_contract)},
        }));
    }
    return array(std::move(encoded));
}

[[nodiscard]] JsonValue behaviors(const RetainedNode& node) {
    std::vector<JsonValue> values;
    for (const DescriptionBehavior& behavior : node.description().behaviors) {
        if (behavior.enabled) values.emplace_back(behavior.id);
    }
    return array(std::move(values));
}

[[nodiscard]] JsonValue commands(const Surface& surface, const RetainedNode& node) {
    std::vector<JsonValue> values;
    for (const CommandSnapshot* command : surface.commands().referenced_by(node)) {
        values.push_back(object({
            {"enabled", JsonValue(command->enabled)},
            {"id", JsonValue(command->id)},
            {"label", JsonValue(command->label)},
            {"owningScope", command->owning_scope.has_value()
                                ? JsonValue(*command->owning_scope)
                                : JsonValue{}},
        }));
    }
    return array(std::move(values));
}

[[nodiscard]] JsonValue visible_range(const VisibleRange& value) {
    return object({
        {"endIndexExclusive", JsonValue(static_cast<std::int64_t>(value.end_exclusive))},
        {"startIndex", JsonValue(static_cast<std::int64_t>(value.start))},
    });
}

[[nodiscard]] std::string motion_number(double value) {
    std::array<char, 64U> buffer{};
    const auto encoded = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general
    );
    std::string result(buffer.data(), encoded.ptr);
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

[[nodiscard]] JsonValue motion_channels(
    const Surface& surface,
    const RetainedNode& node,
    const LayoutRecord& layout
) {
    std::vector<JsonValue> channels;
    if (const auto* runtime_channels = surface.motion().inspection_channels(node.identity());
        runtime_channels != nullptr) {
        channels.reserve(runtime_channels->size() + 1U);
        for (const MotionInspectionChannel& channel : *runtime_channels) {
            std::vector<JsonValue> affected;
            affected.reserve(channel.affected_properties.size());
            for (const std::string& property : channel.affected_properties) {
                affected.emplace_back(property);
            }
            channels.push_back(object({
                {"affectedProperties", array(std::move(affected))},
                {"currentValue", channel.current_value.has_value()
                                     ? JsonValue(*channel.current_value)
                                     : JsonValue{}},
                {"direction", JsonValue(std::string(motion_direction_name(channel.direction)))},
                {"id", JsonValue(channel.id)},
                {"interaction", channel.interaction.has_value()
                                    ? JsonValue(std::string(motion_interaction_name(*channel.interaction)))
                                    : JsonValue{}},
                {"progress", JsonValue(channel.progress)},
                {"running", JsonValue(channel.running)},
                {"snappedByReducedMotion", JsonValue(channel.snapped_by_reduced_motion)},
                {"source", JsonValue(channel.source)},
                {"targetValue", channel.target_value.has_value()
                                    ? JsonValue(*channel.target_value)
                                    : JsonValue{}},
                {"trigger", channel.trigger.has_value()
                                ? JsonValue(std::string(motion_trigger_name(*channel.trigger)))
                                : JsonValue{}},
            }));
        }
    }
    const std::optional<DisclosureMotionSpec> disclosure = disclosure_motion(node);
    if (disclosure.has_value()) {
        const NormalizedMotionSample* target = surface.motion().disclosure_sample(node.identity());
        const double current = target != nullptr
                                   ? target->current
                                   : disclosure->expanded ? 1.0 : 0.0;
        channels.push_back(object({
            {"affectedProperties", array({
                JsonValue("height"), JsonValue("clip"), JsonValue("inputEligibility"),
            })},
            {"currentValue", JsonValue("Number(value=" + motion_number(current) + ")")},
            {"direction", JsonValue(disclosure->expanded ? "EXPAND" : "COLLAPSE")},
            {"id", JsonValue("strata.disclosure")},
            {"interaction", JsonValue{}},
            {"progress", JsonValue(layout.content_motion_progress)},
            {"running", JsonValue(
                layout.content_motion_running || (target != nullptr && target->running)
            )},
            {"snappedByReducedMotion", JsonValue(
                layout.content_motion_snapped_by_reduced_motion ||
                (target != nullptr && target->snapped_by_reduced_motion)
            )},
            {"source", JsonValue("disclosure/content-size")},
            {"targetValue", JsonValue(disclosure->expanded ? "true" : "false")},
            {"trigger", JsonValue{}},
        }));
    } else if (const std::optional<ContentSizeMotionSpec> content =
                   content_size_motion(node.description());
               content.has_value()) {
        std::vector<JsonValue> affected;
        if (content->animate_width) affected.emplace_back("width");
        if (content->animate_height) affected.emplace_back("height");
        if (content->clip) affected.emplace_back("clip");
        const std::string size = motion_number(layout.content_size.width) + "x" +
                                 motion_number(layout.content_size.height);
        const std::string target_size = motion_number(layout.content_motion_target_size.width) +
                                        "x" +
                                        motion_number(layout.content_motion_target_size.height);
        channels.push_back(object({
            {"affectedProperties", array(std::move(affected))},
            {"currentValue", JsonValue(size)},
            {"direction", JsonValue("TO_TARGET")},
            {"id", JsonValue("strata.content-size")},
            {"interaction", JsonValue{}},
            {"progress", JsonValue(layout.content_motion_progress)},
            {"running", JsonValue(layout.content_motion_running)},
            {"snappedByReducedMotion", JsonValue(
                layout.content_motion_snapped_by_reduced_motion
            )},
            {"source", JsonValue("measured-content-size")},
            {"targetValue", JsonValue(target_size)},
            {"trigger", JsonValue{}},
        }));
    }
    return array(std::move(channels));
}

[[nodiscard]] JsonValue authoring_type(const RetainedNode& node) {
    const std::string& type = node.description().type;
    if (node.description().source_path.empty() || type == "SurfaceLayers" ||
        type == "SurfaceLayer" || type == "AnimatedContent" ||
        type == "AnimatedContentItem" || type.starts_with('$')) {
        return JsonValue{};
    }
    const auto authored = node.description().properties.find("$authoringType");
    const runtime::Value* authored_value = authored != node.description().properties.end()
                                               ? authored->second.value()
                                               : nullptr;
    return authored_value != nullptr && authored_value->string() != nullptr
               ? JsonValue(*authored_value->string())
               : JsonValue(type);
}

[[nodiscard]] std::string component_type(const RetainedNode& node) {
    const std::string& type = node.description().type;
    constexpr std::string_view component_prefix = "$component:";
    if (type.starts_with(component_prefix)) return type.substr(component_prefix.size());
    return type;
}

[[nodiscard]] JsonValue node_inspection(
    const Surface& surface,
    const RetainedNode& node,
    const MotionTransform inherited_transform
) {
    const LayoutRecord* layout = surface.layout().find(node.identity());
    if (layout == nullptr) throw std::logic_error("retained inspection node has no layout record");
    const MotionTransform effective_transform = concatenate_presentation_transform(
        inherited_transform,
        local_presentation_transform(node, surface.motion(), layout->bounds)
    );
    std::vector<JsonValue> children;
    children.reserve(node.children().size());
    for (const auto& child : node.children()) {
        children.push_back(node_inspection(surface, *child, effective_transform));
    }
    DirtySet descendant_dirty;
    collect_descendant_dirty(node, descendant_dirty);
    WidgetInspectionScope inspection(surface, node, *layout);
    const WidgetLifecycle* lifecycle = surface.widget_registry().find(node.description().type);
    if (lifecycle != nullptr && lifecycle->inspection.derive != nullptr) {
        lifecycle->inspection.derive(inspection);
    }
    return object({
        {"actions", actions(surface, node)},
        {"authoringType", authoring_type(node)},
        {"behaviors", behaviors(node)},
        {"bounds", rectangle(layout->bounds)},
        {"children", array(std::move(children))},
        {"clip", layout->clip.has_value() ? rectangle(*layout->clip) : JsonValue{}},
        {"collection", inspection.collection()},
        {"commands", commands(surface, node)},
        {"componentType", JsonValue(component_type(node))},
        {"derivedCollection", inspection.derived_collection()},
        {"descendantLayoutDirtyReasons", dirty_set(descendant_dirty)},
        {"dirtyReasons", dirty_set(node.dirty())},
        {"hitBounds", rectangle(transform_presentation_bounds(
            inspection.hit_bounds(), effective_transform
        ))},
        {"interaction", object({
            {"active", JsonValue(surface.input().active(node.identity()))},
            {"focused", JsonValue(surface.input().focused(node.identity()))},
            {"focusVisible", JsonValue(surface.input().focus_visible(node.identity()))},
            {"hovered", JsonValue(surface.input().hovered(node.identity()))},
            {"movementOffset", point(Point{})},
            {"scrollOffset", point(layout->scroll_offset)},
        })},
        {"key", node.description().key.has_value() ? JsonValue(*node.description().key) : JsonValue{}},
        {"layoutKind", JsonValue(std::string(layout_kind_name(layout->kind)))},
        {"motionChannels", motion_channels(surface, node, *layout)},
        {"semantics", surface.semantics().find(node.identity()) != nullptr
                          ? *surface.semantics().find(node.identity())
                          : JsonValue{}},
        {"sourcePath", node.description().source_path.empty()
                           ? JsonValue{}
                           : JsonValue(node.description().source_path)},
        {"structuralPath", JsonValue(std::string(node.structural_path()))},
        {"subtargets", subtargets(surface, node)},
        {"visibleRange", layout->visible_range.has_value()
                             ? visible_range(*layout->visible_range)
                             : JsonValue{}},
    });
}

void collect_editor_inspection(
    const Surface& surface,
    const RetainedNode& node,
    std::vector<JsonValue>& editors
) {
    if (const std::optional<TextEditorSnapshot> snapshot =
            surface.input().editor_snapshot(node.identity());
        snapshot.has_value()) {
        editors.push_back(object({
            {"caretUtf8", JsonValue(static_cast<std::int64_t>(snapshot->caret))},
            {"composition", snapshot->preedit.has_value()
                                ? JsonValue(std::string(*snapshot->preedit))
                                : JsonValue{}},
            {"compositionSelectionEndUtf8", JsonValue(static_cast<std::int64_t>(
                snapshot->preedit_selection_end
            ))},
            {"compositionSelectionStartUtf8", JsonValue(static_cast<std::int64_t>(
                snapshot->preedit_selection_start
            ))},
            {"key", node.description().key.has_value()
                        ? JsonValue(*node.description().key)
                        : JsonValue{}},
            {"selectionEndUtf8", JsonValue(static_cast<std::int64_t>(snapshot->selection_end))},
            {"selectionStartUtf8", JsonValue(static_cast<std::int64_t>(snapshot->selection_start))},
            {"structuralPath", JsonValue(std::string(node.structural_path()))},
            {"text", JsonValue(std::string(snapshot->text))},
        }));
    }
    for (const auto& child : node.children()) {
        collect_editor_inspection(surface, *child, editors);
    }
}

[[nodiscard]] std::string orientation(const SurfaceEnvironment& environment) {
    if (environment.logical_width > environment.logical_height * 1.05) return "landscape";
    if (environment.logical_height > environment.logical_width * 1.05) return "portrait";
    return "square";
}

} // namespace

JsonValue inspect_surface(const Surface& surface) {
    const SurfaceEnvironment& environment = surface.environment();
    const RetainedNode* root = surface.tree().root();
    std::vector<JsonValue> pending_navigation;
    for (std::string key : surface.input().pending_navigation_targets()) {
        pending_navigation.emplace_back(std::move(key));
    }
    std::vector<JsonValue> editors;
    if (root != nullptr) collect_editor_inspection(surface, *root, editors);
    const RetainedNode* selected = surface.inspected_node();
    return object({
        {"editors", array(std::move(editors))},
        {"environment", object({
            {"density", JsonValue(std::string(surface_density_name(environment.density)))},
            {"displayScale", JsonValue(environment.scale)},
            {"framebufferHeight", JsonValue(environment.framebuffer_height)},
            {"framebufferWidth", JsonValue(environment.framebuffer_width)},
            {"generation", JsonValue(static_cast<std::int64_t>(environment.generation))},
            {"input", object({
                {"controller", JsonValue(environment.input.controller)},
                {"ime", JsonValue(environment.input.ime)},
                {"keyboard", JsonValue(environment.input.keyboard)},
                {"pointer", JsonValue(environment.input.pointer)},
                {"pointerPrecision", JsonValue(std::string(pointer_precision_name(environment.input.pointer_precision)))},
                {"touch", JsonValue(environment.input.touch)},
            })},
            {"logicalHeight", JsonValue(environment.logical_height)},
            {"logicalWidth", JsonValue(environment.logical_width)},
            {"orientation", JsonValue(orientation(environment))},
            {"reducedMotion", JsonValue(environment.reduced_motion)},
            {"safeInsets", object({
                {"bottom", JsonValue(environment.safe_insets.bottom)},
                {"left", JsonValue(environment.safe_insets.left)},
                {"right", JsonValue(environment.safe_insets.right)},
                {"top", JsonValue(environment.safe_insets.top)},
            })},
            {"viewportClass", JsonValue(std::string(surface.viewport_class()))},
        })},
        {"pendingNavigationTargets", array(std::move(pending_navigation))},
        {"root", root != nullptr
                     ? node_inspection(surface, *root, MotionTransform{})
                     : JsonValue{}},
        {"selectedKey", selected != nullptr && selected->description().key.has_value()
                            ? JsonValue(*selected->description().key)
                            : JsonValue{}},
        {"selectedPath", selected != nullptr
                             ? JsonValue(std::string(selected->structural_path()))
                             : JsonValue{}},
        {"surfaceId", JsonValue(surface.id())},
    });
}

JsonValue inspect_operation_counters(const SurfaceFrame& frame) {
    const SurfaceOperationCounters& operations = frame.operations;
    return object({
        {"arrangedNodes", JsonValue(static_cast<std::int64_t>(operations.layout_arranged_nodes))},
        {"describedNodes", JsonValue(static_cast<std::int64_t>(operations.described_nodes))},
        {"evaluatedExpressions", JsonValue(static_cast<std::int64_t>(operations.evaluated_expressions))},
        {"glyphWork", JsonValue(std::int64_t{operations.text.cache_misses == 0U ? 0 : 1})},
        {"injectedEvents", JsonValue(static_cast<std::int64_t>(operations.injected_events))},
        {"inputEventsProcessed", JsonValue(static_cast<std::int64_t>(operations.input_events_processed))},
        {"measuredNodes", JsonValue(static_cast<std::int64_t>(operations.layout_measured_nodes))},
        {"layoutWork", JsonValue(std::int64_t{
            operations.layout_measured_nodes == 0U &&
                    operations.layout_measurement_cache_hits == 0U &&
                    operations.layout_arranged_nodes == 0U
                ? 0
                : 1
        })},
        {"motionMutatedNodes", JsonValue(static_cast<std::int64_t>(operations.motion_mutated_nodes))},
        {"motionRunningPlayers", JsonValue(static_cast<std::int64_t>(operations.motion_running_players))},
        {"rebuilds", JsonValue(static_cast<std::int64_t>(operations.rebuilds))},
        {"reusedMeasurements", JsonValue(static_cast<std::int64_t>(operations.layout_measurement_cache_hits))},
        {"render", object({
            {"commandsEmitted", JsonValue(static_cast<std::int64_t>(operations.render.commands_emitted))},
            {"fragmentsBuilt", JsonValue(static_cast<std::int64_t>(operations.render.fragments_built))},
            {"fragmentsReused", JsonValue(static_cast<std::int64_t>(operations.render.fragments_reused))},
            {"nodesVisited", JsonValue(static_cast<std::int64_t>(operations.render.nodes_visited))},
            {"overlaysRendered", JsonValue(static_cast<std::int64_t>(operations.render.overlays_rendered))},
            {"portalsRendered", JsonValue(static_cast<std::int64_t>(operations.render.portals_rendered))},
        })},
    });
}

JsonValue inspect_state(runtime::ApplicationContext& application) {
    std::vector<JsonValue> entries;
    for (const runtime::StateSnapshotEntry& entry : application.state().snapshot().entries) {
        entries.push_back(object({
            {"name", JsonValue(entry.address.name)},
            {"scope", JsonValue(entry.address.scope)},
            {"typeId", JsonValue(entry.type_id)},
            {"value", runtime::value_to_json(entry.value)},
        }));
    }
    return array(std::move(entries));
}

JsonValue inspect_selection(const Surface& surface) {
    const RetainedNode* node = surface.inspected_node();
    if (node == nullptr) return JsonValue{};
    const LayoutRecord* layout = surface.layout().find(node->identity());
    if (layout == nullptr) return JsonValue{};
    return object({
        {"actions", actions(surface, *node)},
        {"bounds", rectangle(layout->bounds)},
        {"componentType", JsonValue(component_type(*node))},
        {"key", node->description().key.has_value()
                    ? JsonValue(*node->description().key)
                    : JsonValue{}},
        {"motionChannels", motion_channels(surface, *node, *layout)},
        {"path", JsonValue(std::string(node->structural_path()))},
    });
}

} // namespace strata::ui
