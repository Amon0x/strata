#include "ui/widget/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "ui/input.hpp"
#include "ui/motion.hpp"
#include "ui/text_geometry.hpp"
#include "ui/text.hpp"
#include "ui/widget/editor_geometry.hpp"
#include "ui/widget/choice_model.hpp"
#include "ui/widget/icon_geometry.hpp"

namespace strata::ui {
namespace {

void icon_button_content(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(
            scope.layout().bounds,
            *scope.visual().background,
            scope.visual().border
        );
    }
    const std::string* texture = widget_image_value(scope.property("icon"));
    if (texture == nullptr) return;
    const double size = std::max(
        0.0,
        std::min(scope.layout().bounds.width, scope.layout().bounds.height) * 0.55
    );
    scope.image(
        Rect{
            scope.layout().bounds.x + (scope.layout().bounds.width - size) * 0.5,
            scope.layout().bounds.y + (scope.layout().bounds.height - size) * 0.5,
            size,
            size,
        },
        *texture,
        scope.visual().foreground,
        widget_texture_region(scope.property("source"))
    );
    scope.interaction(scope.layout().bounds);
    scope.focus(scope.layout().bounds);
}

void checkbox_content(WidgetRenderScope& scope) {
    const bool checked = scope.effective_boolean(
        "checked", "$checked", "defaultChecked", false
    );
    const double inset = scope.visual().indicator_inset.value_or(4.0);
    const double size = std::min(
        scope.visual().indicator_size.value_or(16.0),
        scope.layout().bounds.height
    );
    const Rect box{
        scope.layout().bounds.x + inset,
        scope.layout().bounds.y + (scope.layout().bounds.height - size) * 0.5,
        size,
        size,
    };
    scope.rounded_rect(box, scope.visual().track, scope.visual().border);
    if (checked) {
        const double mark = size * 0.25;
        scope.rounded_rect(
            Rect{box.x + mark, box.y + mark, box.width - mark * 2.0, box.height - mark * 2.0},
            scope.visual().fill
        );
    }
    scope.interaction(scope.layout().bounds);
    scope.focus(box);
    const std::optional<std::string_view> value = scope.node_text();
    if (!value.has_value() || scope.text_engine() == nullptr) return;
    const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *value);
    scope.text(
        *value,
        Point{
            scope.layout().content_bounds.x,
            scope.layout().bounds.y + (scope.layout().bounds.height - shaped.metrics.height) * 0.5,
        },
        scope.visual().foreground
    );
}

void toggle_content(WidgetRenderScope& scope) {
    const bool checked = scope.effective_boolean(
        "checked", "$checked", "defaultChecked", false
    );
    const double track_width = std::min(
        scope.visual().track_width.value_or(40.0), scope.layout().bounds.width
    );
    const double track_height = std::min(
        scope.visual().track_height.value_or(20.0), scope.layout().bounds.height
    );
    const Rect track{
        scope.layout().bounds.x + scope.visual().indicator_inset.value_or(4.0),
        scope.layout().bounds.y + (scope.layout().bounds.height - track_height) * 0.5,
        track_width,
        track_height,
    };
    const std::optional<double> animated_position = scope.visual().indicator_position;
    scope.rounded_rect(
        track,
        animated_position.has_value()
            ? scope.visual().track
            : checked ? scope.visual().fill : scope.visual().track,
        scope.visual().border,
        scope.visual().track_radius.value_or(track_height * 0.5)
    );
    constexpr double thumb_inset = 2.0;
    const double thumb_size = std::min(
        scope.visual().thumb_size.value_or(16.0),
        std::max(0.0, track.height - thumb_inset * 2.0)
    );
    const double travel = std::max(0.0, track.width - thumb_size - thumb_inset * 2.0);
    scope.rounded_rect(
        Rect{
            track.x + thumb_inset + travel *
                animated_position.value_or(checked ? 1.0 : 0.0),
            track.y + (track.height - thumb_size) * 0.5,
            thumb_size,
            thumb_size,
        },
        scope.visual().thumb,
        std::nullopt,
        scope.visual().thumb_radius.value_or(thumb_size * 0.5)
    );
    scope.interaction(scope.layout().bounds);
    scope.focus(track);
    const std::optional<std::string_view> value = scope.node_text();
    if (!value.has_value() || scope.text_engine() == nullptr) return;
    const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *value);
    scope.text(
        *value,
        Point{
            scope.layout().content_bounds.x,
            scope.layout().bounds.y + (scope.layout().bounds.height - shaped.metrics.height) * 0.5,
        },
        scope.visual().foreground
    );
}

void slider_content(WidgetRenderScope& scope) {
    const double minimum = scope.number("min", 0.0);
    const double maximum = scope.number("max", 1.0);
    const double value = scope.effective_number(
        "value", "$value", "defaultValue", minimum
    );
    const double percent = maximum > minimum
                               ? std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0)
                               : 0.0;
    const bool vertical = scope.string("axis") == "VERTICAL";
    const double thickness = scope.visual().track_height.value_or(4.0);
    const double inset = scope.visual().indicator_inset.value_or(7.0);
    const Rect track = vertical
                           ? Rect{
                                 scope.layout().bounds.x +
                                     (scope.layout().bounds.width - thickness) * 0.5,
                                 scope.layout().bounds.y + inset,
                                 thickness,
                                 std::max(0.0, scope.layout().bounds.height - inset * 2.0),
                             }
                           : Rect{
                                 scope.layout().bounds.x + inset,
                                 scope.layout().bounds.y +
                                     (scope.layout().bounds.height - thickness) * 0.5,
                                 std::max(0.0, scope.layout().bounds.width - inset * 2.0),
                                 thickness,
                             };
    scope.rounded_rect(
        track,
        scope.visual().track,
        std::nullopt,
        scope.visual().track_radius.value_or(thickness * 0.5)
    );
    const Rect fill = vertical
                          ? Rect{
                                track.x,
                                track.bottom() - track.height * percent,
                                track.width,
                                track.height * percent,
                            }
                          : Rect{track.x, track.y, track.width * percent, track.height};
    if (!fill.empty()) {
        scope.rounded_rect(
            fill,
            scope.visual().fill,
            std::nullopt,
            std::min(fill.width, fill.height) * 0.5
        );
    }
    const double thumb_size = scope.visual().thumb_size.value_or(14.0);
    const Point center = vertical
                             ? Point{
                                   track.x + track.width * 0.5,
                                   track.bottom() - track.height * percent,
                               }
                             : Point{
                                   track.x + track.width * percent,
                                   track.y + track.height * 0.5,
                               };
    const Rect thumb{
        center.x - thumb_size * 0.5,
        center.y - thumb_size * 0.5,
        thumb_size,
        thumb_size,
    };
    scope.rounded_rect(
        thumb,
        scope.visual().thumb,
        RenderBorder{1.0, RenderColor{0U, 0U, 0U, 90U}, true},
        scope.visual().thumb_radius.value_or(thumb_size * 0.5)
    );
    scope.interaction(scope.layout().bounds);
    scope.focus(thumb);
}

enum class TextInputMode { single_line, multi_line, number };

void text_input_content(WidgetRenderScope& scope, const TextInputMode mode) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(
            scope.layout().bounds,
            *scope.visual().background,
            scope.visual().border
        );
    } else if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
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
    std::string value;
    const std::optional<TextEditorSnapshot> editor =
        scope.input().editor_snapshot(scope.node().identity());
    if (editor.has_value()) {
        value.assign(editor->text);
        if (editor->preedit.has_value() && !editor->preedit->empty()) {
            value.insert(std::min(editor->caret, value.size()), *editor->preedit);
        }
    } else if (mode == TextInputMode::number) {
        value = widget_number_text(scope.effective_number(
            "value", "$value", "defaultValue", 0.0
        ));
    } else {
        const runtime::Value* current = scope.property("text");
        if (widget_string_value(current) == nullptr) current = scope.retained("$text");
        if (const std::string* text = widget_string_value(current); text != nullptr) value = *text;
    }
    RenderColor color = scope.visual().foreground;
    if (value.empty()) {
        value = scope.string("hint");
        color = scope.visual().text_hint;
    }
    const std::vector<WidgetSubtarget> subtargets =
        scope.input().subtargets(scope.node().identity());
    const std::optional<Rect> viewport = editable_text_viewport(
        scope.node(), scope.layout(), subtargets
    );
    if (!viewport.has_value() || viewport->empty()) return;
    scope.push_clip(*viewport);
    if (!value.empty() && scope.text_engine() != nullptr) {
        const TextLayout text_layout = scope.text_engine()->layout(scope.node(), value);
        const Point origin = text_input_origin(
            *viewport, text_layout, mode == TextInputMode::multi_line
        );
        if (editor.has_value() && editor->selection_start != editor->selection_end) {
            const std::size_t start = utf16_offset_for_utf8_byte(editor->text, editor->selection_start);
            const std::size_t end = utf16_offset_for_utf8_byte(editor->text, editor->selection_end);
            for (const Rect rect : text_layout_selection_rects(
                     text_layout, origin, start, end
                 )) {
                scope.solid_rect(rect, scope.visual().selection);
            }
        }
        std::optional<std::pair<std::size_t, std::size_t>> composition_range;
        if (editor.has_value() && editor->preedit.has_value() && !editor->preedit->empty()) {
            const std::size_t composition_start = utf16_offset_for_utf8_byte(editor->text, editor->caret);
            const std::size_t composition_end = composition_start +
                utf16_offset_for_utf8_byte(*editor->preedit, editor->preedit->size());
            composition_range = std::pair(composition_start, composition_end);
            const std::size_t selection_start = composition_start + utf16_offset_for_utf8_byte(
                *editor->preedit, editor->preedit_selection_start
            );
            const std::size_t selection_end = composition_start + utf16_offset_for_utf8_byte(
                *editor->preedit, editor->preedit_selection_end
            );
            for (const Rect rect : text_layout_selection_rects(
                     text_layout, origin, selection_start, selection_end
                 )) {
                scope.solid_rect(rect, RenderColor{255U, 255U, 255U, 72U});
            }
        }
        scope.text(value, origin, color);
        if (composition_range.has_value()) {
            for (Rect rect : text_layout_selection_rects(
                     text_layout, origin,
                     composition_range->first, composition_range->second
                 )) {
                rect.y += rect.height - 1.0;
                rect.height = 1.0;
                scope.solid_rect(rect, RenderColor{255U, 255U, 255U, 160U});
            }
        }
        if (editor.has_value() && scope.input().focused(scope.node().identity())) {
            scope.solid_rect(
                text_layout_caret_rect(text_layout, origin, editor->text, editor->caret),
                scope.visual().caret
            );
        }
    }
    scope.pop_clip();
}

void single_line_text_content(WidgetRenderScope& scope) {
    text_input_content(scope, TextInputMode::single_line);
}

void multi_line_text_content(WidgetRenderScope& scope) {
    text_input_content(scope, TextInputMode::multi_line);
}

void number_text_content(WidgetRenderScope& scope) {
    text_input_content(scope, TextInputMode::number);
}

void progress_content(WidgetRenderScope& scope) {
    scope.rounded_rect(
        scope.layout().bounds,
        scope.visual().track,
        scope.visual().border
    );
    std::optional<Rect> fill;
    if (scope.boolean("indeterminate", false)) {
        const MotionComputedValues* computed = scope.motion_values();
        const double fallback = computed != nullptr ? computed->progress : 0.0;
        const double progress = scope.motion_progress(
            "strata.progress.indeterminate", fallback
        );
        const double segment_width = scope.layout().bounds.width * 0.3;
        fill = Rect{
            scope.layout().bounds.x +
                (scope.layout().bounds.width + segment_width) * progress - segment_width,
            scope.layout().bounds.y,
            std::min(segment_width, scope.layout().bounds.width),
            scope.layout().bounds.height,
        }.intersection(scope.layout().bounds);
    } else {
        const double minimum = scope.number("min", 0.0);
        const double maximum = scope.number("max", 1.0);
        const double value = scope.number("value", minimum);
        const double fraction = maximum > minimum
                                    ? std::clamp((value - minimum) / (maximum - minimum), 0.0, 1.0)
                                    : 0.0;
        fill = Rect{
            scope.layout().bounds.x,
            scope.layout().bounds.y,
            scope.layout().bounds.width * fraction,
            scope.layout().bounds.height,
        };
    }
    if (fill.has_value() && !fill->empty()) scope.rounded_rect(*fill, scope.visual().fill);
}

void tabs_content(WidgetRenderScope& scope) {
    const runtime::ValueList* tabs = scope.list("tabs");
    if (tabs == nullptr || tabs->values.empty()) return;

    const std::optional<EffectiveChoice> selected = effective_choice(scope.node());

    if (scope.visual().background.has_value()) {
        scope.rounded_rect(
            scope.layout().bounds,
            *scope.visual().background,
            scope.visual().border
        );
    }
    const double tab_width = scope.layout().bounds.width /
                             static_cast<double>(tabs->values.size());
    for (std::size_t index = 0U; index < tabs->values.size(); ++index) {
        const runtime::Value& tab = tabs->values[index];
        const Rect bounds{
            scope.layout().bounds.x + tab_width * static_cast<double>(index),
            scope.layout().bounds.y,
            tab_width,
            scope.layout().bounds.height,
        };
        const std::string* id = widget_string_value(tab.field("id"));
        if (id != nullptr && selected.has_value() && index == selected->index) {
            const double inset = scope.visual().indicator_inset.value_or(2.0);
            scope.rounded_rect(
                Rect{
                    bounds.x + inset,
                    bounds.y + inset,
                    std::max(0.0, bounds.width - inset * 2.0),
                    std::max(0.0, bounds.height - inset * 2.0),
                },
                scope.visual().fill
            );
        }
        if (id != nullptr) scope.interaction(bounds, *id);

        const std::string* label = widget_string_value(tab.field("label"));
        if (label == nullptr || scope.text_engine() == nullptr) continue;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *label);
        scope.text(
            *label,
            Point{
                bounds.x,
                bounds.y + (bounds.height - shaped.metrics.height) * 0.5,
            },
            scope.visual().foreground,
            bounds.width,
            WidgetTextAlignment::center
        );
    }
    scope.focus(scope.layout().bounds);
}

void select_content(WidgetRenderScope& scope) {
    if (scope.property("triggerTemplate") != nullptr) {
        scope.focus(scope.layout().bounds);
        return;
    }
    const runtime::ValueList* options = scope.list("options");
    if (options == nullptr || options->values.empty()) return;
    const std::optional<EffectiveChoice> selected = effective_choice(scope.node());
    if (!selected.has_value()) return;
    const runtime::Value* selected_option = &options->values[selected->index];
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(
            scope.layout().bounds,
            *scope.visual().background,
            scope.visual().border
        );
    }
    const double cap_width = std::min(
        scope.visual().indicator_size.value_or(scope.layout().bounds.height),
        scope.layout().bounds.width
    );
    const Rect cap{
        scope.layout().bounds.right() - cap_width,
        scope.layout().bounds.y,
        cap_width,
        scope.layout().bounds.height,
    };
    scope.rounded_rect(
        Rect{cap.x + 1.0, cap.y + 1.0, cap.width - 2.0, cap.height - 2.0},
        scope.visual().fill,
        std::nullopt,
        0.0
    );
    const std::string* label = widget_string_value(selected_option->field("label"));
    if (label != nullptr && scope.text_engine() != nullptr) {
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *label);
        scope.text(
            *label,
            Point{
                scope.layout().bounds.x + 5.0,
                scope.layout().bounds.y +
                    (scope.layout().bounds.height - shaped.metrics.height) * 0.5,
            },
            scope.visual().foreground
        );
    }
    const double icon_size = std::min(cap.width, cap.height) * 0.45;
    scope.shape(
        Rect{
            cap.x + (cap.width - icon_size) * 0.5,
            cap.y + (cap.height - icon_size) * 0.5,
            icon_size,
            icon_size,
        },
        widget_chevron(WidgetChevronDirection::down, scope.visual().foreground)
    );
    scope.interaction(scope.layout().bounds, "$control");
    scope.focus(scope.layout().bounds);
}

void select_overlay(WidgetRenderScope& scope) {
    if (scope.property("popupTemplate") != nullptr &&
        scope.property("itemTemplate") != nullptr) {
        return;
    }
    const std::vector<WidgetSubtarget> targets = scope.input().subtargets(scope.node().identity());
    std::vector<WidgetSubtarget> rows;
    for (const WidgetSubtarget& target : targets) {
        if (target.detached && target.kind == WidgetSubtargetKind::choice) rows.push_back(target);
    }
    if (rows.empty()) return;
    Rect popup = rows.front().bounds;
    for (const WidgetSubtarget& row : rows) {
        const double left = std::min(popup.x, row.bounds.x);
        const double top = std::min(popup.y, row.bounds.y);
        const double right = std::max(popup.right(), row.bounds.right());
        const double bottom = std::max(popup.bottom(), row.bounds.bottom());
        popup = Rect{left, top, right - left, bottom - top};
    }
    scope.rounded_rect(
        popup,
        scope.visual().background.value_or(RenderColor{34U, 38U, 46U, 245U}),
        scope.visual().border
    );
    for (const WidgetSubtarget& row : rows) {
        scope.interaction(row.bounds, row.id);
        if (scope.text_engine() == nullptr || row.label.empty()) continue;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), row.label);
        scope.text(
            row.label,
            Point{row.bounds.x + 7.0, row.bounds.y + (row.bounds.height - shaped.metrics.height) * 0.5},
            row.enabled ? scope.visual().foreground : RenderColor{160U, 168U, 178U, 180U}
        );
    }
}

void radio_foreground(WidgetRenderScope& scope) {
    const runtime::ValueList* options = scope.list("options");
    if (options == nullptr) return;
    const bool group_enabled = scope.boolean("enabled", true);
    const std::optional<EffectiveChoice> selected = effective_choice(scope.node());
    for (std::size_t index = 0U;
         index < options->values.size() && index < scope.node().children().size();
         ++index) {
        const LayoutRecord* child = scope.layout_result().find(
            scope.node().children()[index]->identity()
        );
        if (child == nullptr) continue;
        const runtime::Value& option = options->values[index];
        const bool enabled = group_enabled && choice_option_enabled(option);
        const Rect ring{
            child->bounds.x + 6.0,
            child->bounds.y + child->bounds.height * 0.5 - 6.0,
            12.0,
            12.0,
        };
        scope.border(ring, RenderBorder{1.0, scope.visual().track.representative(), true}, 6.0);
        const std::string* id = widget_string_value(option.field("id"));
        if (id != nullptr && selected.has_value() && index == selected->index) {
            scope.rounded_rect(
                Rect{ring.x + 3.0, ring.y + 3.0, 6.0, 6.0},
                scope.visual().fill,
                std::nullopt,
                3.0
            );
        }
        const std::string* label = widget_string_value(option.field("label"));
        if (scope.text_engine() != nullptr && label != nullptr) {
            const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), *label);
            scope.text(
                *label,
                Point{
                    child->bounds.x + 28.0,
                    child->bounds.y + (child->bounds.height - shaped.metrics.height) * 0.5,
                },
                enabled ? scope.visual().foreground : RenderColor{160U, 168U, 178U, 220U}
            );
        }
        if (id != nullptr) scope.interaction(child->bounds, *id);
    }
    scope.focus(scope.layout().bounds);
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetPresentHook content,
    const WidgetPresentHook foreground = nullptr,
    const WidgetPresentHook overlay = nullptr,
    const bool detached_overlay = false,
    const WidgetVisualProfile visual = {},
    const bool depends_on_motion_progress = false
) {
    WidgetPresentPhase phase{content, foreground, overlay, nullptr, detached_overlay};
    phase.visual = visual;
    phase.depends_on_motion_progress = depends_on_motion_progress;
    registry.register_present_phase(std::move(type), std::move(phase));
}

} // namespace

void register_control_widget_presenters(WidgetRegistry& registry) {
    add(registry, "IconButton", &icon_button_content, nullptr, &command_tooltip_overlay, true);
    add(registry, "Checkbox", &checkbox_content);
    add(registry, "Toggle", &toggle_content);
    add(registry, "Switch", &toggle_content);
    add(registry, "Slider", &slider_content);
    add(registry, "TextBox", &single_line_text_content);
    add(registry, "TextArea", &multi_line_text_content);
    add(registry, "NumberField", &number_text_content);
    add(registry, "Progress", &progress_content, nullptr, nullptr, false, {}, true);
    add(registry, "Tabs", &tabs_content);
    add(registry, "Select", &select_content, nullptr, &select_overlay, true, {false, true, false});
    add(registry, "RadioGroup", nullptr, &radio_foreground, nullptr, false, {true, false, false});
    add(registry, "ComboBox", nullptr);
}

} // namespace strata::ui
