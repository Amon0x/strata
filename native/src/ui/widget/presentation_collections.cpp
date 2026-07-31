#include "ui/widget/presentation.hpp"

#include <algorithm>
#include <cmath>
#include <set>
#include <utility>

#include "ui/collection/table_model.hpp"
#include "ui/scroll_geometry.hpp"
#include "ui/text.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const std::string* collection_text(const runtime::Value* const value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] std::set<std::string, std::less<>> selected_keys(
    WidgetRenderScope& scope,
    const bool marquee_preview = false
) {
    const runtime::Value* value = scope.property("selectedKeys");
    if (marquee_preview) {
        const runtime::Value* marquee = scope.retained("strata.collection.marquee");
        const runtime::Value* preview = marquee != nullptr
            ? marquee->field("lastSelection")
            : nullptr;
        if (preview != nullptr && preview->list() != nullptr) value = preview;
    }
    if (value == nullptr || value->list() == nullptr) {
        value = scope.retained("strata.collection.selected");
    }
    if (value == nullptr || value->list() == nullptr) {
        value = scope.property("defaultSelectedKeys");
    }
    std::set<std::string, std::less<>> result;
    if (value != nullptr && value->list() != nullptr) {
        for (const runtime::Value& entry : value->list()->values) {
            if (const std::string* key = collection_text(&entry); key != nullptr) result.insert(*key);
        }
    }
    return result;
}

[[nodiscard]] RenderColor selection_overlay(WidgetRenderScope& scope) noexcept {
    RenderColor result = scope.visual().selection;
    result.alpha = std::min<std::uint8_t>(result.alpha, 92U);
    return result;
}

[[nodiscard]] const runtime::Value* row_property(
    const RetainedNode& row,
    const std::string_view name
) noexcept {
    const auto found = row.description().properties.find(name);
    return found != row.description().properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] bool row_boolean(
    const RetainedNode& row,
    const std::string_view name
) noexcept {
    const runtime::Value* value = row_property(row, name);
    return value != nullptr && value->boolean() != nullptr && *value->boolean();
}

void collection_content(WidgetRenderScope& scope) {
    if (scope.visual().background.has_value()) {
        scope.rounded_rect(scope.layout().bounds, *scope.visual().background);
    }
}

void scrollbars(WidgetRenderScope& scope) {
    const LayoutStyle style = layout_style(scope.node().description());
    for (const ScrollbarAxis axis : {ScrollbarAxis::vertical, ScrollbarAxis::horizontal}) {
        const std::optional<ScrollbarGeometry> geometry = scrollbar_geometry(
            scope.layout(), style, axis
        );
        if (!geometry.has_value()) continue;
        scope.rounded_rect(
            geometry->thumb_bounds,
            scope.visual().thumb,
            std::nullopt,
            2.0
        );
    }
}

void collection_frame(WidgetRenderScope& scope) {
    scrollbars(scope);
    if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
    }
    scope.focus(scope.layout().viewport.value_or(scope.layout().bounds));
}

void tree_foreground(WidgetRenderScope& scope) {
    scope.push_clip(scope.layout().bounds);
    for (const auto& child : scope.node().children()) {
        const LayoutRecord* child_layout = scope.layout_result().find(child->identity());
        if (child_layout == nullptr || !child->description().key.has_value()) continue;
        if (row_boolean(*child, "$treeSelected")) {
            scope.solid_rect(child_layout->bounds, selection_overlay(scope));
        }
        const runtime::Value* depth_value = row_property(*child, "$treeDepth");
        const std::size_t depth = depth_value != nullptr && depth_value->number() != nullptr &&
            *depth_value->number() >= 0.0
            ? static_cast<std::size_t>(*depth_value->number())
            : 0U;
        for (std::size_t guide = 0U; guide < depth; ++guide) {
            scope.solid_rect(
                Rect{
                    child_layout->bounds.x + 18.0 * static_cast<double>(guide) + 5.0,
                    child_layout->bounds.y,
                    1.0,
                    child_layout->bounds.height,
                },
                scope.visual().border.has_value()
                    ? scope.visual().border->color
                    : scope.visual().foreground
            );
        }
        if (!row_boolean(*child, "$treeExpandable")) continue;
        const double x = child_layout->bounds.x + 18.0 * static_cast<double>(depth) + 3.0;
        const double y = child_layout->bounds.y + (child_layout->bounds.height - 10.0) * 0.5;
        if (row_boolean(*child, "$treeExpanded")) {
            scope.solid_rect(Rect{x, y + 3.5, 10.0, 2.0}, scope.visual().foreground);
        } else {
            scope.solid_rect(Rect{x + 3.5, y, 2.0, 10.0}, scope.visual().foreground);
            scope.solid_rect(Rect{x, y + 4.5, 10.0, 2.0}, scope.visual().foreground);
        }
    }
    if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
    }
    scope.pop_clip();
    scope.focus(scope.layout().bounds);
}

std::optional<Rect> table_descendant_clip(WidgetRenderScope& scope) {
    const Rect viewport = scope.layout().viewport.value_or(scope.layout().bounds);
    const double header = std::clamp(scope.number("headerHeight", 32.0), 0.0, viewport.height);
    return Rect{viewport.x, viewport.y + header, viewport.width, viewport.height - header};
}

void table_foreground(WidgetRenderScope& scope) {
    const Rect viewport = scope.layout().viewport.value_or(scope.layout().bounds);
    const double header_height = scope.number("headerHeight", 32.0);
    scope.push_clip(scope.layout().bounds);
    const std::set<std::string, std::less<>> selected = selected_keys(scope);
    for (const auto& child : scope.node().children()) {
        const LayoutRecord* row = scope.layout_result().find(child->identity());
        if (row == nullptr || row->bounds.bottom() < viewport.y || row->bounds.y > viewport.bottom()) {
            continue;
        }
        if (child->description().key.has_value() && selected.contains(*child->description().key)) {
            scope.solid_rect(row->bounds, selection_overlay(scope));
        }
        scope.solid_rect(
            Rect{viewport.x, std::max(viewport.y, row->bounds.bottom() - 1.0), viewport.width, 1.0},
            scope.visual().border.has_value()
                ? scope.visual().border->color
                : RenderColor{86U, 102U, 126U, 120U}
        );
    }
    scope.solid_rect(
        Rect{viewport.x, viewport.y, viewport.width, header_height},
        RenderColor{24U, 30U, 40U, 252U}
    );
    scope.solid_rect(
        Rect{viewport.x, viewport.y + header_height - 1.0, viewport.width, 1.0},
        RenderColor{96U, 112U, 136U, 180U}
    );
    if (scope.property("columns") != nullptr && scope.text_engine() != nullptr) {
        const collection::TableGeometry geometry = collection::table_geometry(
            collection::resolve_table_columns(
                scope.property("columns"),
                scope.property("columnWidths"),
                scope.retained("strata.table.columnWidths"),
                scope.property("defaultColumnWidths"),
                scope.property("columnOrder"),
                scope.retained("strata.table.columnOrder"),
                scope.property("defaultColumnOrder"),
                viewport.width
            ),
            viewport,
            scope.layout().scroll_offset.x
        );
        const std::string* sort_column = collection_text(scope.property("sortColumn"));
        const std::string* sort_direction = collection_text(scope.property("sortDirection"));
        const auto paint = [&scope, viewport, header_height, sort_column, sort_direction](
                               const collection::TableTrack& track
                           ) {
            if (track.column.pinned) {
                scope.solid_rect(
                    Rect{track.start, viewport.y, track.column.width, header_height},
                    RenderColor{28U, 36U, 48U, 255U}
                );
            }
            std::string header = track.column.header;
            if (sort_column != nullptr && *sort_column == track.column.id && sort_direction != nullptr) {
                header += *sort_direction == "ASCENDING" ? "  ↑" : "  ↓";
            }
            scope.text(
                header,
                Point{track.start + 8.0, viewport.y + 8.0},
                RenderColor{225U, 232U, 242U, 255U}
            );
            scope.solid_rect(
                Rect{track.end() - 1.0, viewport.y + 5.0, 1.0, header_height - 10.0},
                RenderColor{92U, 107U, 130U, 140U}
            );
        };
        for (const collection::TableTrack& track : geometry.tracks) {
            if (!track.column.pinned) paint(track);
        }
        for (const collection::TableTrack& track : geometry.tracks) {
            if (track.column.pinned) paint(track);
        }
        for (const collection::TableTrack& track : geometry.tracks) {
            if (!track.column.pinned) continue;
            scope.solid_rect(
                Rect{
                    track.end() - 1.0,
                    viewport.y + header_height,
                    1.0,
                    std::max(0.0, viewport.height - header_height),
                },
                RenderColor{86U, 102U, 126U, 150U}
            );
        }
        const runtime::Value* preview = scope.retained("strata.table.columnInsertion");
        if (preview != nullptr && preview->number() != nullptr) {
            const std::size_t index = static_cast<std::size_t>(std::max(0.0, *preview->number()));
            const double x = index < geometry.tracks.size()
                ? geometry.tracks[index].start
                : geometry.tracks.empty() ? viewport.x : geometry.tracks.back().end();
            scope.solid_rect(
                Rect{x - 1.5, viewport.y + 2.0, 3.0, std::max(0.0, header_height - 4.0)},
                RenderColor{103U, 225U, 164U, 255U}
            );
        }
    }
    scrollbars(scope);
    if (scope.visual().border.has_value()) {
        scope.border(scope.layout().bounds, *scope.visual().border);
    }
    scope.pop_clip();
}

std::optional<Rect> item_grid_descendant_clip(WidgetRenderScope& scope) {
    const runtime::ValueList* entries = scope.list("entries");
    if (entries == nullptr) return std::nullopt;
    const bool has_header = std::ranges::any_of(entries->values, [](const runtime::Value& entry) {
        const std::string* kind = widget_string_value(entry.field("kind"));
        return kind != nullptr && *kind == "HEADER";
    });
    if (!has_header) return std::nullopt;
    const Rect viewport = scope.layout().viewport.value_or(scope.layout().bounds);
    const double inset = std::min(28.0, viewport.height);
    return Rect{viewport.x, viewport.y + inset, viewport.width, viewport.height - inset};
}

void item_grid_foreground(WidgetRenderScope& scope) {
    collection_frame(scope);
}

void item_grid_overlay(WidgetRenderScope& scope) {
    const std::set<std::string, std::less<>> selected = selected_keys(scope, true);
    const runtime::Value* marquee = scope.retained("strata.collection.marquee");
    const auto point = [](const runtime::Value* value) -> std::optional<Point> {
        const double* x = value != nullptr && value->field("x") != nullptr
            ? value->field("x")->number() : nullptr;
        const double* y = value != nullptr && value->field("y") != nullptr
            ? value->field("y")->number() : nullptr;
        return x != nullptr && y != nullptr ? std::optional<Point>(Point{*x, *y}) : std::nullopt;
    };
    const std::optional<Point> start = marquee != nullptr
        ? point(marquee->field("startViewport")) : std::nullopt;
    const std::optional<Point> current = marquee != nullptr
        ? point(marquee->field("currentViewport")) : std::nullopt;
    if (selected.empty() && (!start.has_value() || !current.has_value())) return;

    scope.push_clip(scope.layout().bounds);
    for (const auto& band : scope.node().children()) {
        for (const auto& item : band->children()) {
            if (!item->description().key.has_value() ||
                !selected.contains(*item->description().key)) {
                continue;
            }
            if (const LayoutRecord* layout = scope.layout_result().find(item->identity());
                layout != nullptr) {
                scope.solid_rect(layout->bounds, selection_overlay(scope));
            }
        }
    }
    if (start.has_value() && current.has_value()) {
        const Rect bounds{
            std::min(start->x, current->x),
            std::min(start->y, current->y),
            std::abs(start->x - current->x),
            std::abs(start->y - current->y),
        };
        scope.rounded_rect(
            bounds,
            RenderColor{70U, 137U, 230U, 50U},
            RenderBorder{1.0, RenderColor{104U, 169U, 255U, 220U}, true},
            2.0
        );
    }
    scope.pop_clip();
}

void add(
    WidgetRegistry& registry,
    std::string type,
    const WidgetPresentHook foreground,
    const WidgetClipHook descendant_clip = nullptr
) {
    registry.register_present_phase(
        std::move(type),
        WidgetPresentPhase{
            &collection_content,
            foreground,
            nullptr,
            descendant_clip,
            false,
        }
    );
}

} // namespace

void register_collection_widget_presenters(WidgetRegistry& registry) {
    add(registry, "Scroll", &collection_frame);
    add(registry, "VirtualList", &collection_frame);
    add(registry, "TreeView", &tree_foreground);
    add(registry, "Table", &table_foreground, &table_descendant_clip);
    WidgetPresentPhase item_grid{
        &collection_content,
        &item_grid_foreground,
        &item_grid_overlay,
        &item_grid_descendant_clip,
        true,
    };
    registry.register_present_phase("ItemGrid", std::move(item_grid));
}

} // namespace strata::ui
