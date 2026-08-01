#include "ui/behavior/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "ui/behavior/registry.hpp"
#include "ui/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/reorder.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] bool hovered(
    const RetainedNode& node,
    const DescriptionBehavior&,
    const InputRouter& input
) {
    return input.hovered(node.identity());
}

void hover_overlay(const DescriptionBehavior&, WidgetRenderScope& scope) {
    scope.rounded_rect(
        scope.layout().bounds,
        RenderColor{255U, 255U, 255U, 18U},
        RenderBorder{1.0, RenderColor{255U, 255U, 255U, 46U}, true},
        5.0
    );
}

[[nodiscard]] double option_number(
    const runtime::Value* options,
    const std::string_view name,
    const double fallback
) noexcept {
    const runtime::Value* value = options != nullptr ? options->field(name) : nullptr;
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
        ? *value->number()
        : fallback;
}

[[nodiscard]] bool drag_preview_active(
    const RetainedNode& node,
    const DescriptionBehavior&,
    const InputRouter& input
) {
    return input.drag_preview(node.identity()).has_value();
}

void drag_preview_overlay(const DescriptionBehavior& attachment, WidgetRenderScope& scope) {
    const std::optional<DragPreviewPresentation> state =
        scope.input().drag_preview(scope.node().identity());
    if (!state.has_value()) return;
    const runtime::Value* preview_options = attachment.options.field("preview");
    const double max_width = std::max(1.0, option_number(preview_options, "maxWidth", 240.0));
    const double max_height = std::max(1.0, option_number(preview_options, "maxHeight", 160.0));
    const double opacity = std::clamp(
        option_number(preview_options, "opacity", 0.82), 0.0, 1.0
    );
    const double offset_x = std::max(0.0, option_number(preview_options, "offsetX", 14.0));
    const double offset_y = std::max(0.0, option_number(preview_options, "offsetY", 14.0));
    const double source_width = std::max(1.0, state->source_bounds.width);
    const double source_height = std::max(1.0, state->source_bounds.height);
    const double scale = std::min({1.0, max_width / source_width, max_height / source_height});
    const Rect preview{
        state->position.x + offset_x,
        state->position.y + offset_y,
        source_width * scale,
        source_height * scale,
    };
    const RenderColor accent = state->accepted
        ? RenderColor{91U, 226U, 154U, 255U}
        : RenderColor{239U, 103U, 112U, 255U};

    scope.shadow(preview, CornerRadii::all(6.0), RenderColor{0U, 0U, 0U, 150U}, 10.0, 1.0);
    scope.push_transform(scale, Point{
        preview.x - state->source_bounds.x * scale,
        preview.y - state->source_bounds.y * scale,
    });
    scope.push_clip(state->source_bounds);
    if (state->commands != nullptr) {
        for (const RenderCommand& command : *state->commands) scope.append(command, opacity);
    }
    scope.pop_clip();
    scope.pop_transform();
    if (state->commands == nullptr || state->commands->empty()) {
        scope.rounded_rect(
            preview,
            RenderColor{35U, 45U, 62U, static_cast<std::uint8_t>(255.0 * opacity)},
            std::nullopt,
            6.0
        );
    }
    scope.border(preview, RenderBorder{2.0, accent, true}, 6.0);
    const Rect badge{state->position.x + 5.0, state->position.y + 5.0, 12.0, 12.0};
    scope.rounded_rect(
        badge,
        accent,
        RenderBorder{1.0, RenderColor{245U, 249U, 255U, 235U}, true},
        6.0
    );
    if (!state->accepted) {
        scope.solid_rect(
            Rect{badge.x + 3.0, badge.y + 5.0, 6.0, 2.0},
            RenderColor{255U, 255U, 255U, 235U}
        );
    }
}

[[nodiscard]] bool drop_target_active(
    const RetainedNode& node,
    const DescriptionBehavior&,
    const InputRouter& input
) {
    return input.drop_target(node.identity()).has_value();
}

void drop_target_overlay(const DescriptionBehavior& attachment, WidgetRenderScope& scope) {
    const std::optional<DropTargetPresentation> state =
        scope.input().drop_target(scope.node().identity());
    if (!state.has_value()) return;
    const RenderColor color{91U, 226U, 154U, 255U};
    const RenderColor tint{91U, 226U, 154U, 42U};
    const runtime::Value* axis_value = attachment.options.field("insertionAxis");
    const std::string axis = axis_value != nullptr && axis_value->string() != nullptr
        ? *axis_value->string()
        : std::string{};
    const Rect bounds = scope.layout().bounds;
    if (state->placement == "on" || axis.empty()) {
        scope.rounded_rect(bounds, tint, RenderBorder{2.0, color, true}, 5.0);
        return;
    }
    const bool horizontal = axis == "HORIZONTAL" || axis == "horizontal";
    if (horizontal) {
        const bool before = state->placement == "before";
        const double x = before ? bounds.x : bounds.right();
        const double half = bounds.width * 0.5;
        scope.rounded_rect(
            Rect{before ? bounds.x : bounds.right() - half, bounds.y, half, bounds.height},
            tint,
            std::nullopt,
            4.0
        );
        const double inset = std::min(5.0, bounds.height * 0.12);
        const Rect line{x - 1.5, bounds.y + inset, 3.0, std::max(3.0, bounds.height - inset * 2.0)};
        scope.rounded_rect(line, color, std::nullopt, 1.5);
        scope.rounded_rect(Rect{x - 5.0, line.y, 10.0, 3.0}, color, std::nullopt, 1.5);
        scope.rounded_rect(
            Rect{x - 5.0, line.bottom() - 3.0, 10.0, 3.0}, color, std::nullopt, 1.5
        );
        return;
    }
    const bool before = state->placement == "before";
    const double y = before ? bounds.y : bounds.bottom();
    const double half = bounds.height * 0.5;
    scope.rounded_rect(
        Rect{bounds.x, before ? bounds.y : bounds.bottom() - half, bounds.width, half},
        tint,
        std::nullopt,
        4.0
    );
    const double inset = std::min(5.0, bounds.width * 0.12);
    const Rect line{bounds.x + inset, y - 1.5, std::max(3.0, bounds.width - inset * 2.0), 3.0};
    scope.rounded_rect(line, color, std::nullopt, 1.5);
    scope.rounded_rect(Rect{line.x, y - 5.0, 3.0, 10.0}, color, std::nullopt, 1.5);
    scope.rounded_rect(
        Rect{line.right() - 3.0, y - 5.0, 3.0, 10.0}, color, std::nullopt, 1.5
    );
}

void reorder_target_overlay(const DescriptionBehavior& attachment, WidgetRenderScope& scope) {
    const std::optional<DropTargetPresentation> state =
        scope.input().drop_target(scope.node().identity());
    if (!state.has_value()) return;
    const runtime::Value* axis_value = attachment.options.field("axis");
    const std::string axis = axis_value != nullptr && axis_value->string() != nullptr
        ? *axis_value->string()
        : "VERTICAL";
    const bool horizontal = axis == "HORIZONTAL" || axis == "horizontal";
    const ReorderInsertion resolved = resolve_reorder_insertion(
        scope.node(), scope.layout_result(), state->position, horizontal
    );
    const Rect bounds = scope.layout().bounds;
    const double insertion = resolved.coordinate;
    const RenderColor color{103U, 225U, 164U, 255U};
    scope.rounded_rect(
        horizontal
            ? Rect{insertion - 1.5, bounds.y + 2.0, 3.0, std::max(0.0, bounds.height - 4.0)}
            : Rect{bounds.x + 2.0, insertion - 1.5, std::max(0.0, bounds.width - 4.0), 3.0},
        color,
        std::nullopt,
        1.5
    );
}

} // namespace

void register_builtin_behavior_presenters(BehaviorRegistry& registry) {
    registry.register_present_phase(
        "strata.hoverable",
        BehaviorPresentPhase{hovered, hover_overlay, false}
    );
    registry.register_present_phase(
        "strata.drag-source",
        BehaviorPresentPhase{drag_preview_active, drag_preview_overlay, true}
    );
    registry.register_present_phase(
        "strata.drop-target",
        BehaviorPresentPhase{drop_target_active, drop_target_overlay, true}
    );
    registry.register_present_phase(
        "strata.reorder-target",
        BehaviorPresentPhase{drop_target_active, reorder_target_overlay, true}
    );
}

bool has_behavior_overlay(
    const BehaviorRegistry& registry,
    const RetainedNode& node,
    const InputRouter& input,
    const bool detached
) {
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* lifecycle = registry.find(attachment.id);
        if (lifecycle == nullptr || lifecycle->present.detached_overlay != detached ||
            lifecycle->present.has_overlay == nullptr || lifecycle->present.overlay == nullptr) {
            continue;
        }
        if (lifecycle->present.has_overlay(node, attachment, input)) return true;
    }
    return false;
}

void append_behavior_overlays(
    const BehaviorRegistry& registry,
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputRouter& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    const double inherited_opacity,
    const bool detached,
    std::vector<RenderCommand>& output
) {
    for (const DescriptionBehavior& attachment : node.description().behaviors) {
        if (!attachment.enabled) continue;
        const BehaviorLifecycle* lifecycle = registry.find(attachment.id);
        if (lifecycle == nullptr || lifecycle->present.detached_overlay != detached ||
            lifecycle->present.has_overlay == nullptr || lifecycle->present.overlay == nullptr ||
            !lifecycle->present.has_overlay(node, attachment, input)) {
            continue;
        }
        WidgetRenderScope scope(
            node,
            layout,
            layout_result,
            input,
            commands,
            text,
            svg_images,
            motion,
            inherited_opacity,
            WidgetVisualProfile{},
            output
        );
        lifecycle->present.overlay(attachment, scope);
    }
}

} // namespace strata::ui
