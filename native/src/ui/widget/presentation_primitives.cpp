#include "ui/widget/presentation.hpp"

#include <algorithm>

#include "ui/input.hpp"
#include "ui/status.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/widget/icon_geometry.hpp"
#include "ui/widget/menu_model.hpp"

namespace strata::ui {
namespace {

void container_content(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background);
    }
}

void container_foreground(WidgetRenderScope& scope) {
    if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
    }
    scope.focus(scope.layout().bounds);
}

void static_text_selection(WidgetRenderScope& scope) {
    if (scope.text_engine() == nullptr)
        return;
    const std::optional<StaticTextSelectionSnapshot> selection =
        scope.input().static_text_selection_snapshot(scope.node().identity());
    if (!selection.has_value() || selection->selection_start == selection->selection_end ||
        selection->text.empty()) {
        return;
    }
    const TextLayout text_layout = scope.text_engine()->layout(scope.node(), selection->text);
    const Point origin{scope.layout().content_bounds.x, scope.layout().content_bounds.y};
    const std::size_t start =
        utf16_offset_for_utf8_byte(selection->text, selection->selection_start);
    const std::size_t end = utf16_offset_for_utf8_byte(selection->text, selection->selection_end);
    scope.push_clip(scope.layout().content_bounds);
    for (const Rect rect : text_layout_selection_rects(text_layout, origin, start, end)) {
        scope.solid_rect(rect, scope.visual().text_selection);
    }
    scope.pop_clip();
}

void section_content(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background,
                           scope.visual().border);
    }

    Rect header{
        scope.layout().bounds.x,
        scope.layout().bounds.y,
        scope.layout().bounds.width,
        std::min(std::max(24.0, scope.number("headerHeight", 36.0)), scope.layout().bounds.height),
    };
    for (const auto& child : scope.node().children()) {
        const auto marker = child->description().properties.find("$inputTransparent");
        const runtime::Value* value =
            marker != child->description().properties.end() ? marker->second.value() : nullptr;
        if (value == nullptr || value->boolean() == nullptr || !*value->boolean())
            continue;
        if (const LayoutRecord* record = scope.layout_result().find(child->identity());
            record != nullptr) {
            header = record->bounds;
        }
        break;
    }

    bool descendant_hovered = false;
    for (const auto& child : scope.node().children()) {
        descendant_hovered = descendant_hovered || scope.input().hovered(child->identity());
    }
    if (!descendant_hovered)
        scope.interaction(header);
    const bool expanded =
        scope.effective_boolean("expanded", "$expanded", "defaultExpanded", false);
    const double indicator_size =
        std::min(scope.visual().indicator_size.value_or(10.0), std::max(0.0, header.height - 12.0));
    if (indicator_size > 0.0) {
        scope.shape(
            Rect{
                header.x + 10.0,
                header.y + (header.height - indicator_size) * 0.5,
                indicator_size,
                indicator_size,
            },
            widget_chevron(expanded ? WidgetChevronDirection::down : WidgetChevronDirection::right,
                           scope.visual().foreground));
    }

    const std::string label = scope.string("label");
    if (scope.text_engine() != nullptr && !label.empty()) {
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), label);
        scope.text(label,
                   Point{
                       header.x + 10.0 + indicator_size + 7.0,
                       header.y + (header.height - shaped.metrics.height) * 0.5,
                   },
                   scope.visual().foreground);
    }
    scope.focus(header);
}

void text_content(WidgetRenderScope& scope) {
    static_text_selection(scope);
    scope.node_text(Point{scope.layout().content_bounds.x, scope.layout().content_bounds.y},
                    scope.visual().foreground);
}

void rich_text_content(WidgetRenderScope& scope) {
    static_text_selection(scope);
    scope.rich_text(Point{scope.layout().content_bounds.x, scope.layout().content_bounds.y},
                    scope.visual().foreground);
    scope.focus(scope.layout().bounds);
}

void button_content(WidgetRenderScope& scope) {
    if (scope.property("presentationTemplate") != nullptr)
        return;
    const bool context_menu = scope.string("$authoringType") == "ContextMenu";
    const bool authored_menu =
        scope.node().description().type == "Menu" && scope.property("triggerTemplate") != nullptr;
    if (authored_menu) {
        scope.focus(scope.layout().bounds);
        return;
    }
    if (!context_menu) {
        if (scope.visual().background.has_value()) {
            scope.rounded_rect(scope.layout().bounds, *scope.visual().background,
                               scope.visual().border);
        } else if (scope.visual().border.has_value()) {
            scope.border(scope.layout().bounds, *scope.visual().border);
        }
    }
    if (scope.input().hovered(scope.node().identity()) &&
        scope.visual().hover_overlay.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().hover_overlay);
    }
    if (scope.input().active(scope.node().identity()) &&
        scope.visual().active_overlay.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().active_overlay);
    }
    scope.focus(scope.layout().bounds);
    if (context_menu || scope.text_engine() == nullptr)
        return;
    const std::optional<std::string_view> value = scope.node_text();
    if (!value.has_value())
        return;
    const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *value);
    scope.text(
        *value,
        Point{
            scope.layout().content_bounds.x,
            scope.layout().bounds.y + (scope.layout().bounds.height - shaped.metrics.height) * 0.5,
        },
        scope.visual().foreground, scope.layout().content_bounds.width,
        WidgetTextAlignment::center);
}

void image_content(WidgetRenderScope& scope) {
    const std::string* image = widget_image_value(scope.property("image"));
    if (image == nullptr)
        return;
    scope.image(scope.layout().bounds, *image,
                widget_color(scope.property("tint"), RenderColor{255U, 255U, 255U, 255U}),
                widget_texture_region(scope.property("source")));
}

/**
 * Draw projects authored vector shapes. Geometry is normalized to the widget's own bounds, so the
 * same drawing scales with layout, and a malformed shape is skipped rather than failing the frame.
 */
void draw_content(WidgetRenderScope& scope) {
    const runtime::ValueList* shapes = scope.list("shapes");
    if (shapes == nullptr)
        return;
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background);
    }
    for (const runtime::Value& value : shapes->values) {
        const std::optional<PathShape> shape = path_shape_from_value(&value);
        if (shape.has_value())
            scope.shape(scope.layout().bounds, *shape);
    }
}

void menu_overlay(WidgetRenderScope& scope) {
    if (!scope.effective_boolean("open", "$expanded", "defaultOpen", false))
        return;
    const bool authored_popup = scope.property("popupTemplate") != nullptr;
    const bool authored_items = scope.property("itemTemplate") != nullptr;
    if (authored_popup && authored_items)
        return;
    const MenuProjection projection =
        project_menu(scope.node(), scope.layout_result(), &scope.command_index());
    const Paint background = scope.visual().background.value_or(RenderColor{34U, 38U, 46U, 245U});
    if (!authored_popup) {
        for (const MenuPanelModel& panel : projection.panels) {
            scope.shadow(panel.bounds, CornerRadii::all(scope.visual().radius),
                         RenderColor{0U, 0U, 0U, 105U}, 8.0, 1.0);
            scope.rounded_rect(panel.bounds, background, scope.visual().border);
        }
    }
    if (authored_items)
        return;
    const std::vector<WidgetSubtarget> targets = scope.input().subtargets(scope.node().identity());
    for (const MenuRowModel& row : projection.rows()) {
        if (row.item == nullptr)
            continue;
        const MenuItemModel& item = *row.item;
        const std::string identity = menu_row_identity(row.path);
        const auto target =
            std::ranges::find(targets, std::string_view(identity), &WidgetSubtarget::id);
        const Rect bounds = target != targets.end() ? target->bounds : row.bounds;
        if (item.separator) {
            scope.solid_rect(Rect{bounds.x + 8.0, bounds.y + bounds.height * 0.5,
                                  std::max(0.0, bounds.width - 16.0), 1.0},
                             RenderColor{92U, 102U, 118U, 150U});
            continue;
        }
        const bool selected =
            projection.active_path.size() >= row.path.size() &&
            std::equal(row.path.begin(), row.path.end(), projection.active_path.begin());
        if (selected) {
            scope.rounded_rect(Rect{bounds.x + 3.0, bounds.y + 2.0,
                                    std::max(0.0, bounds.width - 6.0),
                                    std::max(0.0, bounds.height - 4.0)},
                               RenderColor{91U, 141U, 239U, 62U});
        }
        scope.interaction(bounds, identity);
        const RenderColor foreground =
            item.enabled ? scope.visual().foreground : RenderColor{160U, 168U, 178U, 135U};
        if (item.has_checked && item.checked) {
            scope.shape(
                Rect{
                    bounds.x + 8.0,
                    bounds.y + (bounds.height - 12.0) * 0.5,
                    12.0,
                    12.0,
                },
                widget_checkmark(foreground));
        }
        if (item.shortcut.empty() && !item.children.empty()) {
            scope.shape(
                Rect{
                    bounds.right() - 18.0,
                    bounds.y + (bounds.height - 10.0) * 0.5,
                    10.0,
                    10.0,
                },
                widget_chevron(WidgetChevronDirection::right, foreground));
        }
        if (scope.text_engine() == nullptr)
            continue;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), item.label);
        scope.text(item.label,
                   Point{bounds.x + 28.0, bounds.y + (bounds.height - shaped.metrics.height) * 0.5},
                   foreground, std::max(0.0, bounds.width - 60.0));
        if (!item.shortcut.empty()) {
            scope.text(item.shortcut, Point{bounds.x + 8.0, bounds.y + 5.0}, foreground,
                       std::max(0.0, bounds.width - 24.0), WidgetTextAlignment::end);
        }
    }
}

void status_content(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background,
                           scope.visual().border);
    } else if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
    }
    if (!scope.boolean("showCommandFeedback", true) || scope.text_engine() == nullptr)
        return;
    const std::optional<std::string_view> feedback = scope.input().status_feedback().snapshot();
    if (!feedback.has_value())
        return;
    const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *feedback);
    scope.text(
        *feedback,
        Point{
            scope.layout().bounds.x + 10.0,
            scope.layout().bounds.y + (scope.layout().bounds.height - shaped.metrics.height) * 0.5,
        },
        scope.visual().border.has_value() ? scope.visual().border->color
                                          : RenderColor{160U, 168U, 178U, 220U},
        std::max(0.0, scope.layout().bounds.width - 20.0), WidgetTextAlignment::end);
}

void add(WidgetRegistry& registry, std::string type, const WidgetPresentHook content,
         const WidgetPresentHook foreground = nullptr, const WidgetPresentHook overlay = nullptr,
         const bool detached_overlay = false, const WidgetVisualProfile visual = {}) {
    WidgetPresentPhase phase{content, foreground, overlay, nullptr, detached_overlay};
    phase.visual = visual;
    registry.register_present_phase(std::move(type), std::move(phase));
}

} // namespace

void register_primitive_widget_presenters(WidgetRegistry& registry) {
    add(registry, "Panel", &container_content, &container_foreground);
    add(registry, "Popup", &container_content, &container_foreground);
    add(registry, "List", &container_content, &container_foreground);
    add(registry, "Slot", &container_content, nullptr, nullptr, false, {true, false, false});
    add(registry, "Section", &section_content);
    add(registry, "Text", &text_content, nullptr, nullptr, false, {true, false, true});
    add(registry, "RichText", &rich_text_content, nullptr, nullptr, false, {true, false, true});
    add(registry, "Button", &button_content, nullptr, &command_tooltip_overlay, true);
    add(registry, "Menu", &button_content, nullptr, &menu_overlay, true);
    add(registry, "Image", &image_content);
    add(registry, "Draw", &draw_content);
    WidgetPresentPhase status;
    status.content = &status_content;
    status.depends_on_status_feedback = true;
    registry.register_present_phase("StatusBar", std::move(status));
}

} // namespace strata::ui
