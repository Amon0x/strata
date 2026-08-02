#include "ui/widget/presentation.hpp"

#include <algorithm>
#include <ranges>
#include <utility>

#include "ui/command.hpp"
#include "ui/input.hpp"
#include "ui/text.hpp"
#include "ui/widget/presentation_editor.hpp"
#include "ui/widget/editor_geometry.hpp"
#include "ui/widget/shell_model.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] bool highlighted(
    const font::ShapedGlyph& glyph,
    const std::vector<QuickPickMatchSpan>& spans
) noexcept {
    return std::ranges::any_of(spans, [&glyph](const QuickPickMatchSpan& span) {
        return glyph.text_end_offset > span.start &&
               glyph.text_start_offset < span.end_exclusive;
    });
}

void draw_text_layout(
    WidgetRenderScope& scope,
    const TextLayout& layout,
    const Point origin,
    const Rect clip,
    const RenderColor color,
    const std::vector<QuickPickMatchSpan>& highlights = {},
    const RenderColor highlight_color = RenderColor{114U, 170U, 255U, 255U}
) {
    const TextEngine* engine = scope.text_engine();
    if (engine == nullptr || clip.empty()) return;
    scope.push_clip(clip);
    std::vector<LogicalGlyph> run;
    std::optional<bool> run_highlighted;
    std::optional<double> run_pixel_size;
    std::optional<std::size_t> run_line;
    std::optional<FontRasterization> run_rasterization;
    const auto flush = [&] {
        if (run.empty()) return;
        scope.append(TextRunRenderCommand{
            origin,
            run_highlighted.value_or(false) ? highlight_color : color,
            run_pixel_size.value_or(engine->pixel_size(scope.node())),
            std::move(run),
            run_rasterization.value_or(engine->font_rasterization(scope.node())),
            clip,
        });
        run.clear();
    };
    std::size_t resolved_run_index = 0U;
    for (std::size_t index = 0U; index < layout.shaped.glyphs.size(); ++index) {
        const font::ShapedGlyph& glyph = layout.shaped.glyphs[index];
        if (glyph.glyph_id == 0U) continue;
        const bool glyph_highlighted = highlighted(glyph, highlights);
        const double pixel_size = index < layout.glyph_pixel_sizes.size()
            ? layout.glyph_pixel_sizes[index]
            : engine->pixel_size(scope.node());
        while (resolved_run_index + 1U < layout.resolved_runs.size() &&
               index >= layout.resolved_runs[resolved_run_index].glyph_end_index) {
            ++resolved_run_index;
        }
        const FontRasterization rasterization =
            resolved_run_index < layout.resolved_runs.size() &&
                index >= layout.resolved_runs[resolved_run_index].glyph_start_index &&
                index < layout.resolved_runs[resolved_run_index].glyph_end_index
            ? layout.resolved_runs[resolved_run_index].font_rasterization
            : engine->font_rasterization(scope.node());
        if ((!run.empty() && run_highlighted != glyph_highlighted) ||
            (!run.empty() && run_pixel_size != pixel_size) ||
            (!run.empty() && run_line != glyph.line_index) ||
            (!run.empty() && run_rasterization != rasterization)) {
            flush();
        }
        run_highlighted = glyph_highlighted;
        run_pixel_size = pixel_size;
        run_line = glyph.line_index;
        run_rasterization = rasterization;
        run.push_back(LogicalGlyph{
            index < layout.glyph_font_ids.size()
                ? layout.glyph_font_ids[index]
                : engine->font_id(scope.node()),
            glyph.glyph_id,
            glyph.code_point,
            glyph.text_start_offset,
            glyph.text_end_offset,
            glyph.x,
            glyph.baseline,
            glyph.advance,
            glyph.x_placement,
            glyph.y_placement,
            glyph.y_advance,
            glyph.font_style_flags,
        });
    }
    flush();
    scope.pop_clip();
}

void command_surface(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(
            scope.layout().bounds,
            *scope.visual().background,
            scope.visual().border
        );
    } else if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
    }
}

[[nodiscard]] bool indexed_popup_open(const WidgetRenderScope& scope) noexcept {
    if (scope.node().description().type == "Toolbar") {
        const runtime::Value* open = scope.retained("$toolbarOverflow");
        return open != nullptr && open->boolean() != nullptr && *open->boolean();
    }
    if (scope.node().description().type == "MenuBar") {
        const runtime::Value* category = scope.retained("$menuCategory");
        return category != nullptr && category->string() != nullptr &&
            !category->string()->empty();
    }
    return false;
}

void indexed_focus(
    WidgetRenderScope& scope,
    const std::vector<WidgetSubtarget>& targets,
    const bool detached
) {
    if (!scope.input().focused(scope.node().identity())) return;
    std::vector<const WidgetSubtarget*> enabled;
    for (const WidgetSubtarget& target : targets) {
        if (target.detached == detached && target.enabled &&
            (target.kind == WidgetSubtargetKind::command ||
             target.kind == WidgetSubtargetKind::control)) {
            enabled.push_back(&target);
        }
    }
    if (enabled.empty()) {
        if (!detached) scope.focus(scope.layout().bounds);
        return;
    }
    const runtime::Value* retained = scope.retained("$shellIndex");
    const std::size_t index = retained != nullptr && retained->number() != nullptr &&
        *retained->number() >= 0.0
        ? std::min(static_cast<std::size_t>(*retained->number()), enabled.size() - 1U)
        : 0U;
    scope.focus(enabled[index]->bounds);
}

void menu_bar_content(WidgetRenderScope& scope) {
    command_surface(scope);
    const std::vector<WidgetSubtarget> targets = scope.input().subtargets(scope.node().identity());
    for (const WidgetSubtarget& target : targets) {
        if (target.detached || target.kind != WidgetSubtargetKind::control) continue;
        scope.interaction(target.bounds, target.id);
        if (scope.text_engine() == nullptr) continue;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), target.label);
        scope.text(
            target.label,
            Point{target.bounds.x,
                  target.bounds.y + (target.bounds.height - shaped.metrics.height) * 0.5},
            scope.visual().foreground,
            target.bounds.width,
            WidgetTextAlignment::center
        );
    }
    if (!indexed_popup_open(scope)) indexed_focus(scope, targets, false);
}

void indexed_command_overlay(WidgetRenderScope& scope) {
    const std::vector<WidgetSubtarget> targets = scope.input().subtargets(scope.node().identity());
    std::vector<WidgetSubtarget> rows;
    for (const WidgetSubtarget& target : targets) {
        if (target.detached && target.kind == WidgetSubtargetKind::command) rows.push_back(target);
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
        if (scope.text_engine() == nullptr) continue;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), row.label);
        scope.text(
            row.label,
            Point{row.bounds.x + 7.0,
                  row.bounds.y + (row.bounds.height - shaped.metrics.height) * 0.5},
            row.enabled ? scope.visual().foreground : RenderColor{160U, 168U, 178U, 180U}
        );
    }
    indexed_focus(scope, targets, true);
}

void toolbar_content(WidgetRenderScope& scope) {
    command_surface(scope);
    const std::vector<WidgetSubtarget> targets = scope.input().subtargets(scope.node().identity());
    for (const WidgetSubtarget& target : targets) {
        if ((target.kind != WidgetSubtargetKind::command &&
             target.kind != WidgetSubtargetKind::control) || target.detached) {
            continue;
        }
        scope.interaction(target.bounds, target.id);
        if (scope.text_engine() == nullptr) continue;
        const std::string label = target.id == "$overflow" ? "..." : target.label;
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), label);
        scope.text(
            label,
            Point{target.bounds.x,
                  target.bounds.y + (target.bounds.height - shaped.metrics.height) * 0.5},
            target.enabled
                ? scope.visual().foreground
                : RenderColor{160U, 168U, 178U, 220U},
            target.bounds.width,
            WidgetTextAlignment::center
        );
    }
    if (!indexed_popup_open(scope)) indexed_focus(scope, targets, false);
}

void chip_input_content(WidgetRenderScope& scope) {
    command_surface(scope);
    const runtime::Value* active_value = scope.retained("$activeToken");
    const std::optional<std::size_t> active = active_value != nullptr &&
        active_value->number() != nullptr && *active_value->number() >= 0.0
        ? std::optional<std::size_t>(static_cast<std::size_t>(*active_value->number()))
        : std::nullopt;
    std::optional<Rect> editor;
    bool active_visible = false;
    const std::vector<WidgetSubtarget> subtargets =
        scope.input().subtargets(scope.node().identity());
    for (const WidgetSubtarget& target : subtargets) {
        if (target.id == "$editor") {
            editor = target.bounds;
            scope.interaction(target.bounds, target.id);
            continue;
        }
        if (target.kind != WidgetSubtargetKind::token) continue;
        const bool selected = active == target.index;
        active_visible = active_visible || selected;
        scope.interaction(target.bounds, target.id);
        scope.rounded_rect(
            target.bounds,
            selected
                ? RenderColor{91U, 141U, 239U, 175U}
                : RenderColor{91U, 141U, 239U, 95U},
            selected
                ? std::optional(RenderBorder{
                      1.0, RenderColor{145U, 185U, 255U, 255U}, true
                  })
                : std::nullopt,
            target.bounds.height * 0.5
        );
        if (scope.text_engine() == nullptr) continue;
        const std::string label = target.label + " ×";
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), label);
        scope.text(
            label,
            Point{
                target.bounds.x + 7.0,
                target.bounds.y + (target.bounds.height - shaped.metrics.height) * 0.5,
            },
            scope.visual().foreground,
            std::max(0.0, target.bounds.width - 14.0),
            WidgetTextAlignment::center
        );
    }
    if (editor.has_value() && !active_visible) {
        const std::optional<Rect> viewport = editable_text_viewport(
            scope.node(), scope.layout(), subtargets
        );
        if (!viewport.has_value()) return;
        present_editable_text(scope, EditableTextPresentation{
            *viewport,
            {},
            scope.string("placeholder"),
            false,
            false,
        });
    }
    scope.focus(scope.layout().bounds);
}

void breadcrumbs_content(WidgetRenderScope& scope) {
    const runtime::ValueList* items = scope.list("items");
    if (items == nullptr || items->values.empty() || scope.text_engine() == nullptr) return;
    const std::vector<WidgetSubtarget> targets = scope.input().subtargets(scope.node().identity());
    const double item_width = scope.layout().bounds.width /
                              static_cast<double>(items->values.size());
    for (std::size_t index = 0U; index < items->values.size(); ++index) {
        const std::string* raw = widget_string_value(items->values[index].field("label"));
        if (raw == nullptr) continue;
        const std::string label = *raw +
                                  (index + 1U < items->values.size() ? "  ›" : "");
        const Rect bounds{
            scope.layout().bounds.x + item_width * static_cast<double>(index) + 6.0,
            scope.layout().bounds.y,
            item_width - 12.0,
            scope.layout().bounds.height,
        };
        if (index < targets.size()) scope.interaction(targets[index].bounds, targets[index].id);
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), label);
        scope.text(
            label,
            Point{bounds.x, bounds.y + (bounds.height - shaped.metrics.height) * 0.5},
            scope.visual().foreground,
            bounds.width,
            WidgetTextAlignment::center
        );
    }
    scope.focus(scope.layout().bounds);
}

void banner_content(WidgetRenderScope& scope) {
    const BannerProjection banner = project_banner(scope.node());
    if (!banner.active) return;
    RenderColor accent{96U, 165U, 250U, 255U};
    const std::string severity = scope.string("severity", "INFO");
    if (severity == "SUCCESS") accent = RenderColor{74U, 222U, 128U, 255U};
    else if (severity == "WARNING") accent = RenderColor{250U, 204U, 21U, 255U};
    else if (severity == "ERROR") accent = RenderColor{248U, 113U, 113U, 255U};
    RenderColor background = accent;
    background.alpha = 36U;
    scope.rounded_rect(
        scope.layout().bounds,
        background,
        RenderBorder{1.0, accent, true},
        5.0
    );
    if (scope.text_engine() == nullptr) return;
    const Rect bounds{
        scope.layout().bounds.x + 12.0,
        scope.layout().bounds.y,
        scope.layout().bounds.width - 24.0,
        scope.layout().bounds.height,
    };
    const std::string message = scope.string("message");
    if (!message.empty()) {
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), message);
        scope.text(
            message,
            Point{bounds.x, bounds.y + (bounds.height - shaped.metrics.height) * 0.5},
            scope.visual().foreground
        );
    }
    const std::string action = banner.has_action
        ? scope.string("actionLabel")
        : std::string{};
    if (!action.empty()) {
        const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), action);
        scope.text(
            action,
            Point{bounds.x, bounds.y + (bounds.height - shaped.metrics.height) * 0.5},
            accent,
            bounds.width,
            WidgetTextAlignment::end
        );
    }
    for (const WidgetSubtarget& target : scope.input().subtargets(scope.node().identity())) {
        scope.interaction(target.bounds, target.id);
    }
    scope.focus(scope.layout().bounds);
}

void modal_content(WidgetRenderScope& scope) {
    if (!scope.boolean("open", false)) return;
    scope.solid_rect(scope.layout().bounds, scope.visual().scrim);
    scope.focus(scope.layout().bounds);
}

void command_palette_overlay(WidgetRenderScope& scope) {
    if (!scope.effective_boolean(
            "open", "strata.palette.open", "defaultOpen", false
        )) {
        return;
    }
    const std::string* query = scope.input().edited_text(scope.node().identity());
    const PaletteProjection projection = project_command_palette(
        scope.node(),
        scope.layout_result(),
        &scope.command_index(),
        query != nullptr ? std::string_view(*query) : std::string_view{}
    );
    scope.solid_rect(scope.root_bounds(), scope.visual().scrim);
    scope.shadow(
        projection.bounds,
        CornerRadii::all(8.0),
        RenderColor{0U, 0U, 0U, 135U},
        14.0,
        2.0
    );
    scope.rounded_rect(
        projection.bounds,
        scope.visual().background.value_or(RenderColor{28U, 32U, 40U, 252U}),
        scope.visual().border,
        8.0
    );
    scope.rounded_rect(
        projection.input_bounds,
        RenderColor{15U, 19U, 26U, 235U},
        RenderBorder{1.0, RenderColor{91U, 141U, 239U, 175U}, true},
        5.0
    );
    // The scrim owns dismissal hit testing, not hover chrome. Painting the widget's generic
    // hover overlay across the root washes over the detached palette whenever the pointer leaves
    // the panel; the dedicated scrim above already provides the intended modal dimming.
    scope.interaction(projection.input_bounds, "$editor");
    const std::vector<WidgetSubtarget> subtargets =
        scope.input().subtargets(scope.node().identity());
    const std::optional<Rect> viewport = editable_text_viewport(
        scope.node(), scope.layout(), subtargets
    );
    if (!viewport.has_value()) return;
    present_editable_text(scope, EditableTextPresentation{
        *viewport,
        {},
        scope.string("placeholder", "Search commands…"),
        false,
        true,
    });
    if (projection.matches.empty() && scope.text_engine() != nullptr) {
        scope.text(
            "No matching commands",
            Point{projection.bounds.x + 16.0, projection.bounds.y + 70.0},
            scope.visual().text_hint
        );
    }
    for (std::size_t local = 0U; local < projection.visible_count(); ++local) {
        const std::size_t index = projection.window_start + local;
        const PaletteEntryModel& entry = projection.matches[index];
        const Rect row = projection.row_bounds(local);
        const std::string identity = "$palette/" + entry.id;
        if (index == projection.active_index) {
            scope.rounded_rect(
                Rect{row.x + 2.0, row.y + 1.0, std::max(0.0, row.width - 4.0),
                     std::max(0.0, row.height - 2.0)},
                RenderColor{91U, 141U, 239U, 82U},
                std::nullopt,
                4.0
            );
        }
        scope.interaction(row, identity);
        if (scope.text_engine() == nullptr) continue;
        const Rect label_bounds{
            row.x + 10.0,
            row.y,
            std::max(1.0, row.width * 0.55 - 10.0),
            row.height,
        };
        const TextLayoutOptions label_options{
            label_bounds.width,
            std::string("NONE"),
            std::string("ELLIPSIS"),
            1U,
            std::string("START"),
        };
        const TextLayout label_layout = scope.text_engine()->layout(
            scope.node(), entry.label, label_options
        );
        draw_text_layout(
            scope,
            label_layout,
            Point{
                label_bounds.x,
                label_bounds.y + std::max(
                    0.0,
                    (label_bounds.height - label_layout.shaped.metrics.height) * 0.5
                ),
            },
            label_bounds,
            scope.visual().foreground,
            entry.label_spans
        );
        if (!entry.detail.empty()) {
            const Rect detail_bounds{
                row.x + row.width * 0.55,
                row.y,
                std::max(1.0, row.width * 0.45 - 10.0),
                row.height,
            };
            TextLayoutOptions detail_options{
                detail_bounds.width,
                std::string("NONE"),
                std::string("ELLIPSIS"),
                1U,
                std::string("END"),
            };
            const TextLayout detail_layout = scope.text_engine()->layout(
                scope.node(), entry.detail, detail_options
            );
            draw_text_layout(
                scope,
                detail_layout,
                Point{
                    detail_bounds.x,
                    detail_bounds.y + std::max(
                        0.0,
                        (detail_bounds.height - detail_layout.shaped.metrics.height) * 0.5
                    ),
                },
                detail_bounds,
                scope.visual().text_hint
            );
        }
    }
    scope.focus(projection.input_bounds);
}

[[nodiscard]] RenderColor toast_accent(const NotificationSeverity severity) noexcept {
    switch (severity) {
    case NotificationSeverity::success: return RenderColor{74U, 222U, 128U, 255U};
    case NotificationSeverity::warning: return RenderColor{250U, 204U, 21U, 255U};
    case NotificationSeverity::error: return RenderColor{248U, 113U, 113U, 255U};
    case NotificationSeverity::info: return RenderColor{96U, 165U, 250U, 255U};
    }
    return RenderColor{96U, 165U, 250U, 255U};
}

void toast_region_overlay(WidgetRenderScope& scope) {
    const ToastProjection projection = project_toasts(
        scope.node(),
        scope.layout_result(),
        scope.input().notifications(),
        scope.text_engine() != nullptr
            ? [&scope](
                  const RetainedNode& owner,
                  const std::string_view value,
                  const TextLayoutOptions& options
              ) {
                return scope.text_engine()->layout(owner, value, options);
            }
            : ToastTextLayoutResolver{}
    );
    for (const ToastCardModel& card : projection.cards) {
        const RenderColor accent = toast_accent(card.notification.request.severity);
        scope.shadow(
            card.bounds,
            CornerRadii::all(6.0),
            RenderColor{0U, 0U, 0U, 105U},
            9.0,
            1.0
        );
        scope.rounded_rect(
            card.bounds,
            RenderColor{27U, 31U, 39U, 248U},
            RenderBorder{1.0, accent, true},
            6.0
        );
        scope.solid_rect(Rect{card.bounds.x, card.bounds.y, 4.0, card.bounds.height}, accent);
        const std::string prefix = "$toast/" + std::to_string(card.notification.id);
        scope.interaction(card.bounds, prefix);
        scope.interaction(card.dismiss_bounds, prefix + "/dismiss");
        if (!card.action_bounds.empty()) {
            scope.interaction(card.action_bounds, prefix + "/action");
        }
        if (scope.text_engine() == nullptr) continue;
        draw_text_layout(
            scope,
            card.message_layout,
            card.message_origin,
            card.message_bounds,
            scope.visual().foreground
        );
        scope.text(
            "×",
            Point{card.dismiss_bounds.x, card.dismiss_bounds.y + 7.0},
            scope.visual().text_hint,
            card.dismiss_bounds.width,
            WidgetTextAlignment::center
        );
        if (!card.action_bounds.empty()) {
            draw_text_layout(
                scope,
                card.action_layout,
                card.action_origin,
                card.action_bounds,
                accent
            );
        }
    }
    if (projection.overflow_count != 0U && !projection.cards.empty() &&
        scope.text_engine() != nullptr) {
        const Rect& first = projection.cards.front().bounds;
        scope.text(
            "+" + std::to_string(projection.overflow_count) + " more",
            Point{first.x + 6.0, std::max(scope.root_bounds().y, first.y - 20.0)},
            scope.visual().text_hint
        );
    }
}

void tooltip_overlay(WidgetRenderScope& scope) {
    if (scope.property("contentTemplate") != nullptr) return;
    if (scope.text_engine() == nullptr) return;
    const std::optional<std::string_view> value = scope.node_text();
    if (!value.has_value() || value->empty()) return;
    const std::optional<TooltipProjection> projection = project_tooltip(
        scope.node(),
        scope.layout_result(),
        [&scope](
            const RetainedNode& owner,
            const std::string_view text,
            const TextLayoutOptions& options
        ) {
            return scope.text_engine()->layout(owner, text, options);
        }
    );
    if (!projection.has_value()) return;
    scope.rounded_rect(
        projection->bounds,
        scope.visual().background.value_or(RenderColor{24U, 24U, 42U, 240U}),
        scope.visual().border
    );
    scope.text(
        *value,
        projection->text_origin,
        scope.visual().foreground
    );
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetPresentHook content,
    const WidgetPresentHook overlay = nullptr,
    const bool detached_overlay = false,
    const WidgetVisualProfile visual = {}
) {
    WidgetPresentPhase phase{content, nullptr, overlay, nullptr, detached_overlay};
    phase.visual = visual;
    registry.register_present_phase(std::move(type), std::move(phase));
}

} // namespace

void register_shell_widget_presenters(WidgetRegistry& registry) {
    add(registry, "MenuBar", &menu_bar_content, &indexed_command_overlay, true);
    add(registry, "Toolbar", &toolbar_content, &indexed_command_overlay, true);
    add(registry, "CommandPalette", nullptr, &command_palette_overlay, true);
    add(registry, "ChipInput", &chip_input_content);
    add(registry, "Breadcrumbs", &breadcrumbs_content);
    add(registry, "Banner", &banner_content);
    add(registry, "ToastRegion", nullptr, &toast_region_overlay, true);
    add(registry, "Modal", &modal_content);
    add(registry, "Tooltip", nullptr, &tooltip_overlay, true, {false, true, false});
}

} // namespace strata::ui
