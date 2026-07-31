#include "ui/widget/input_collection_common.hpp"

#include <algorithm>
#include <cmath>

#include "ui/collection/table_model.hpp"

namespace strata::ui::collection_input {
namespace {

constexpr std::string_view session_key = "strata.table.headerSession";
constexpr std::string_view preview_key = "strata.table.columnInsertion";
constexpr double drag_slop = 4.0;

[[nodiscard]] bool controlled(WidgetInputScope& scope, const std::string_view property) noexcept {
    const runtime::Value* value = scope.property(property);
    return value != nullptr && value->list() != nullptr;
}

[[nodiscard]] collection::TableGeometry geometry(WidgetInputScope& scope) {
    const LayoutRecord* layout = scope.layout();
    const Rect viewport = layout != nullptr
        ? layout->viewport.value_or(layout->bounds)
        : Rect{};
    return collection::table_geometry(
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
        layout != nullptr ? layout->scroll_offset.x : 0.0
    );
}

[[nodiscard]] runtime::Value header_session(
    const std::string& mode,
    const std::string& column,
    const double start_x,
    const double start_width,
    runtime::Value original_widths
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"columnId", runtime::Value(column)},
        {"mode", runtime::Value(mode)},
        {"moved", runtime::Value(false)},
        {"originalWidths", std::move(original_widths)},
        {"startWidth", runtime::Value(start_width)},
        {"startX", runtime::Value(start_x)},
    });
}

[[nodiscard]] std::vector<collection::TableColumn> columns(
    const collection::TableGeometry& value
) {
    std::vector<collection::TableColumn> result;
    result.reserve(value.tracks.size());
    for (const collection::TableTrack& track : value.tracks) result.push_back(track.column);
    return result;
}

[[nodiscard]] const runtime::Value* session(WidgetInputScope& scope) noexcept {
    const runtime::Value* value = scope.retained(session_key);
    return value != nullptr && value->object() != nullptr ? value : nullptr;
}

void clear_session(WidgetInputScope& scope) {
    scope.set_retained(std::string(session_key), runtime::Value{}, DirtyReason::input);
    scope.set_retained(std::string(preview_key), runtime::Value{}, DirtyReason::input);
}

void emit_sort(WidgetInputScope& scope, const std::string& column) {
    const std::string* current_column = text(scope.property("sortColumn"));
    const std::string* current_direction = text(scope.property("sortDirection"));
    std::string direction;
    if (current_column == nullptr || *current_column != column || current_direction == nullptr) {
        direction = "ASCENDING";
    } else if (*current_direction == "ASCENDING") {
        direction = "DESCENDING";
    } else {
        direction = "NONE";
    }
    scope.value_changed(
        "onSort",
        "table-sort-requested",
        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"columnId", runtime::Value(column)},
            {"direction", runtime::Value(direction)},
        })
    );
}

void cancel(WidgetInputScope& scope, const runtime::Value& active_session);

[[nodiscard]] std::size_t insertion_index(
    const collection::TableGeometry& value,
    const std::string& source_id,
    const double pointer_x
) {
    std::vector<collection::TableColumn> ordered = columns(value);
    const auto source = std::ranges::find(ordered, source_id, &collection::TableColumn::id);
    if (source == ordered.end()) return ordered.size();
    const bool pinned = source->pinned;
    const std::size_t source_index = static_cast<std::size_t>(source - ordered.begin());
    std::size_t raw = ordered.size();
    if (const collection::TableTrack* hit = value.column_at(pointer_x); hit != nullptr) {
        const auto found = std::ranges::find(ordered, hit->column.id, &collection::TableColumn::id);
        raw = static_cast<std::size_t>(found - ordered.begin()) +
            (pointer_x >= hit->visible_start + (hit->visible_end - hit->visible_start) * 0.5 ? 1U : 0U);
    } else if (pointer_x <= value.pinned_end) {
        raw = static_cast<std::size_t>(std::ranges::count(ordered, true, &collection::TableColumn::pinned));
    }
    std::vector<collection::TableColumn> remaining;
    for (const collection::TableColumn& column : ordered) {
        if (column.id != source_id) remaining.push_back(column);
    }
    std::size_t candidate = raw - (source_index < raw ? 1U : 0U);
    const std::size_t pinned_count = static_cast<std::size_t>(
        std::ranges::count(remaining, true, &collection::TableColumn::pinned)
    );
    candidate = pinned
        ? std::clamp(candidate, std::size_t{0}, pinned_count)
        : std::clamp(candidate, pinned_count, remaining.size());
    return candidate;
}

bool pointer(WidgetInputScope& scope) {
    const PointerInputEvent* event = scope.pointer();
    const LayoutRecord* layout = scope.layout();
    if (event == nullptr || layout == nullptr) return false;
    const collection::TableGeometry current_geometry = geometry(scope);
    const double header_height = scope.number("headerHeight", 32.0);
    const bool in_header = event->position.y >= current_geometry.viewport.y &&
        event->position.y <= current_geometry.viewport.y + header_height;
    const runtime::Value* active_session = session(scope);

    if (event->type == PointerEventType::press) {
        if (event->button != 0 || !in_header) return false;
        const collection::TableTrack* resize = current_geometry.resize_at(event->position.x);
        const collection::TableTrack* hit = current_geometry.column_at(event->position.x);
        const collection::TableTrack* chosen = resize != nullptr ? resize : hit;
        if (chosen == nullptr) return true;
        scope.set_retained(
            std::string(session_key),
            header_session(
                resize != nullptr ? "resize" : "reorder",
                chosen->column.id,
                event->position.x,
                chosen->column.width,
                collection::encode_widths(columns(current_geometry))
            ),
            DirtyReason::input
        );
        return true;
    }
    if (active_session == nullptr) return false;
    const std::string* mode = text(active_session->field("mode"));
    const std::string* column_id = text(active_session->field("columnId"));
    const double* start_x = active_session->field("startX") != nullptr
        ? active_session->field("startX")->number()
        : nullptr;
    const double* start_width = active_session->field("startWidth") != nullptr
        ? active_session->field("startWidth")->number()
        : nullptr;
    if (mode == nullptr || column_id == nullptr || start_x == nullptr || start_width == nullptr) {
        clear_session(scope);
        return true;
    }
    if (event->type == PointerEventType::cancel) {
        cancel(scope, *active_session);
        return true;
    }
    if (event->type == PointerEventType::move) {
        const double delta = event->position.x - *start_x;
        if (*mode == "resize") {
            std::vector<collection::TableColumn> resolved = columns(current_geometry);
            const auto changed = std::ranges::find(resolved, *column_id, &collection::TableColumn::id);
            if (changed == resolved.end()) return true;
            changed->width = std::clamp(
                *start_width + delta,
                changed->minimum,
                changed->maximum
            );
            if (!controlled(scope, "columnWidths")) {
                scope.set_retained(
                    "strata.table.columnWidths",
                    collection::encode_widths(resolved),
                    DirtyReason::layout
                );
            }
            scope.value_changed(
                "onColumnResize",
                "table-column-width-changed",
                runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"columnId", runtime::Value(*column_id)},
                    {"width", runtime::Value(changed->width)},
                })
            );
        } else if (std::abs(delta) >= drag_slop) {
            const std::size_t index = insertion_index(current_geometry, *column_id, event->position.x);
            scope.set_retained(
                std::string(preview_key),
                runtime::Value(static_cast<double>(index)),
                DirtyReason::input
            );
            std::vector<std::pair<std::string, runtime::Value>> updated = active_session->object()->fields;
            const auto moved = std::ranges::find_if(updated, [](const auto& field) {
                return field.first == "moved";
            });
            if (moved != updated.end()) moved->second = runtime::Value(true);
            scope.set_retained(std::string(session_key), runtime::Value(std::move(updated)), DirtyReason::input);
        }
        return true;
    }
    if (event->type != PointerEventType::release || event->button != 0) return true;
    const runtime::Value* moved_value = active_session->field("moved");
    const bool moved = moved_value != nullptr && moved_value->boolean() != nullptr && *moved_value->boolean();
    if (*mode == "reorder" && moved) {
        const runtime::Value* preview = scope.retained(preview_key);
        const std::size_t target = preview != nullptr && preview->number() != nullptr
            ? static_cast<std::size_t>(std::max(0.0, *preview->number()))
            : 0U;
        std::vector<collection::TableColumn> reordered = columns(current_geometry);
        const auto source_column = std::ranges::find(reordered, *column_id, &collection::TableColumn::id);
        if (source_column != reordered.end()) {
            collection::TableColumn moved_column = *source_column;
            reordered.erase(source_column);
            reordered.insert(
                reordered.begin() + static_cast<std::ptrdiff_t>(std::min(target, reordered.size())),
                std::move(moved_column)
            );
            if (!controlled(scope, "columnOrder")) {
                scope.set_retained(
                    "strata.table.columnOrder",
                    collection::encode_order(reordered),
                    DirtyReason::layout
                );
            }
            scope.value_changed(
                "onColumnReorder",
                "table-column-reordered",
                runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"columnId", runtime::Value(*column_id)},
                    {"targetIndex", runtime::Value(static_cast<double>(target))},
                })
            );
        }
    } else if (*mode == "reorder") {
        emit_sort(scope, *column_id);
    }
    clear_session(scope);
    return true;
}

bool click(WidgetInputScope& scope) {
    const Model collection_model = model(scope);
    const bool handled = common_click(scope, collection_model);
    if (!handled || scope.pointer() == nullptr || scope.pointer()->button != 0) return handled;
    if (scope.string("focusMode", "ROW") == "CELL") {
        const collection::TableGeometry current = geometry(scope);
        if (const collection::TableTrack* hit = current.column_at(scope.pointer()->position.x);
            hit != nullptr) {
            scope.set_retained(
                "strata.table.activeColumn",
                runtime::Value(hit->column.id),
                DirtyReason::semantics
            );
        }
    }
    return true;
}

void cancel(WidgetInputScope& scope, const runtime::Value& active_session) {
    const std::string* mode = text(active_session.field("mode"));
    const std::string* column_id = text(active_session.field("columnId"));
    const double* start_width = active_session.field("startWidth") != nullptr
        ? active_session.field("startWidth")->number()
        : nullptr;
    if (mode != nullptr && *mode == "resize") {
        if (!controlled(scope, "columnWidths")) {
            const runtime::Value* original = active_session.field("originalWidths");
            if (original != nullptr) {
                scope.set_retained("strata.table.columnWidths", *original, DirtyReason::layout);
            }
        } else if (column_id != nullptr && start_width != nullptr) {
            scope.value_changed(
                "onColumnResize",
                "table-column-width-changed",
                runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                    {"columnId", runtime::Value(*column_id)},
                    {"width", runtime::Value(*start_width)},
                })
            );
        }
    }
    clear_session(scope);
}

bool key(WidgetInputScope& scope) {
    if (scope.key() == "escape") {
        if (const runtime::Value* active_session = session(scope); active_session != nullptr) {
            cancel(scope, *active_session);
            return true;
        }
    }
    const collection::TableGeometry current = geometry(scope);
    if (scope.string("focusMode", "ROW") == "CELL" &&
        (scope.key() == "left" || scope.key() == "right") && !current.tracks.empty()) {
        const std::string* active = text(scope.retained("strata.table.activeColumn"));
        auto found = active != nullptr
            ? std::ranges::find(current.tracks, *active, [](const collection::TableTrack& track) {
                  return std::string_view(track.column.id);
              })
            : current.tracks.begin();
        std::ptrdiff_t index = found != current.tracks.end() ? found - current.tracks.begin() : 0;
        index += scope.key() == "left" ? -1 : 1;
        index = std::clamp<std::ptrdiff_t>(
            index,
            0,
            static_cast<std::ptrdiff_t>(current.tracks.size() - 1U)
        );
        scope.set_retained(
            "strata.table.activeColumn",
            runtime::Value(current.tracks[static_cast<std::size_t>(index)].column.id),
            DirtyReason::semantics
        );
        return true;
    }
    return common_key(scope, model(scope), NavigationConfig{
        .item_extent = scope.number("rowHeight", 30.0),
        .leading_content_inset = scope.number("headerHeight", 32.0),
        .sticky_viewport_inset = scope.number("headerHeight", 32.0),
    });
}

} // namespace

void register_table_input(WidgetRegistry& registry) {
    WidgetInputPhase phase;
    phase.pointer = &pointer;
    phase.click = &click;
    phase.key = &key;
    phase.focusable = true;
    registry.register_input_phase("Table", std::move(phase));
}

} // namespace strata::ui::collection_input
