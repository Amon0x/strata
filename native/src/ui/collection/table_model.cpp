#include "ui/collection/table_model.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <ranges>

namespace strata::ui::collection {
namespace {

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] bool boolean(const runtime::Value* value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] double number(
    const runtime::Value* value,
    const double fallback
) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
        ? *value->number()
        : fallback;
}

[[nodiscard]] std::map<std::string, double, std::less<>> widths(
    const runtime::Value* value
) {
    std::map<std::string, double, std::less<>> result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& entry : value->list()->values) {
        const std::string* id = text(entry.field("id"));
        const double* width = entry.field("width") != nullptr
            ? entry.field("width")->number()
            : nullptr;
        if (id != nullptr && !id->empty() && width != nullptr &&
            std::isfinite(*width) && *width >= 0.0) {
            result.insert_or_assign(*id, *width);
        }
    }
    return result;
}

[[nodiscard]] std::vector<std::string> order(const runtime::Value* value) {
    std::vector<std::string> result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& entry : value->list()->values) {
        const std::string* id = text(&entry);
        if (id != nullptr && !id->empty() && !std::ranges::contains(result, *id)) {
            result.push_back(*id);
        }
    }
    return result;
}

} // namespace

double TableTrack::end() const noexcept { return start + column.width; }

bool TableTrack::visible_contains(const double x) const noexcept {
    return visible_end > visible_start && x >= visible_start && x <= visible_end;
}

const TableTrack* TableGeometry::track(const std::string_view id) const noexcept {
    const auto found = std::ranges::find(tracks, id, [](const TableTrack& value) {
        return std::string_view(value.column.id);
    });
    return found != tracks.end() ? &*found : nullptr;
}

const TableTrack* TableGeometry::column_at(const double x) const noexcept {
    const auto found = std::ranges::find_if(tracks, [x](const TableTrack& track) {
        return track.visible_contains(x);
    });
    return found != tracks.end() ? &*found : nullptr;
}

const TableTrack* TableGeometry::resize_at(
    const double x,
    const double hit_width
) const noexcept {
    const TableTrack* result = nullptr;
    double distance = std::max(0.0, hit_width);
    for (const TableTrack& candidate : tracks) {
        if (!candidate.column.resizable || candidate.end() < viewport.x ||
            candidate.end() > viewport.right() ||
            (!candidate.column.pinned && candidate.end() < pinned_end)) {
            continue;
        }
        const double candidate_distance = std::abs(candidate.end() - x);
        if (candidate_distance <= distance) {
            distance = candidate_distance;
            result = &candidate;
        }
    }
    return result;
}

std::vector<TableColumn> resolve_table_columns(
    const runtime::Value* columns,
    const runtime::Value* controlled_widths,
    const runtime::Value* retained_widths,
    const runtime::Value* default_widths,
    const runtime::Value* controlled_order,
    const runtime::Value* retained_order,
    const runtime::Value* default_order,
    const double viewport_width
) {
    std::vector<TableColumn> authored;
    if (columns == nullptr || columns->list() == nullptr) return authored;
    const auto resolved_widths = [&]() {
        std::map<std::string, double, std::less<>> result = widths(controlled_widths);
        if (result.empty()) result = widths(retained_widths);
        if (result.empty()) result = widths(default_widths);
        return result;
    }();
    for (const runtime::Value& value : columns->list()->values) {
        const std::string* id = text(value.field("id"));
        if (id == nullptr || id->empty() || !boolean(value.field("visible"), true)) continue;
        const double minimum = std::max(0.0, number(value.field("minWidth"), 48.0));
        const double maximum = std::max(minimum, number(value.field("maxWidth"), 4096.0));
        const auto retained = resolved_widths.find(*id);
        const double explicit_width = retained != resolved_widths.end()
            ? retained->second
            : number(value.field("width"), -1.0);
        authored.push_back(TableColumn{
            &value,
            *id,
            text(value.field("header")) != nullptr ? *text(value.field("header")) : *id,
            explicit_width,
            minimum,
            maximum,
            boolean(value.field("pinned"), false),
            boolean(value.field("resizable"), true),
        });
    }
    std::vector<std::string> resolved_order = order(controlled_order);
    if (resolved_order.empty()) resolved_order = order(retained_order);
    if (resolved_order.empty()) resolved_order = order(default_order);
    if (!resolved_order.empty()) {
        std::ranges::stable_sort(authored, [&resolved_order](const TableColumn& left, const TableColumn& right) {
            const auto left_position = std::ranges::find(resolved_order, left.id);
            const auto right_position = std::ranges::find(resolved_order, right.id);
            const std::size_t left_index = left_position != resolved_order.end()
                ? static_cast<std::size_t>(left_position - resolved_order.begin())
                : resolved_order.size();
            const std::size_t right_index = right_position != resolved_order.end()
                ? static_cast<std::size_t>(right_position - resolved_order.begin())
                : resolved_order.size();
            return left_index < right_index;
        });
    }
    double fixed = 0.0;
    double total_weight = 0.0;
    for (const TableColumn& column : authored) {
        if (column.width >= 0.0) fixed += std::clamp(column.width, column.minimum, column.maximum);
        else total_weight += std::max(0.0, number(column.source->field("weight"), 1.0));
    }
    const double remaining = std::max(0.0, viewport_width - fixed);
    for (TableColumn& column : authored) {
        if (column.width < 0.0) {
            const double weight = std::max(0.0, number(column.source->field("weight"), 1.0));
            column.width = total_weight > 0.0 ? remaining * weight / total_weight : column.minimum;
        }
        column.width = std::clamp(column.width, column.minimum, column.maximum);
    }
    return authored;
}

TableGeometry table_geometry(
    std::vector<TableColumn> columns,
    const Rect viewport,
    const double scroll_x
) {
    double pinned_width = 0.0;
    for (const TableColumn& column : columns) {
        if (column.pinned) pinned_width += column.width;
    }
    const double pinned_end = std::min(
        viewport.right(),
        viewport.x + pinned_width
    );
    TableGeometry result{viewport, pinned_end, {}};
    result.tracks.reserve(columns.size());
    double scrolling = viewport.x - std::max(0.0, scroll_x);
    double pinned = viewport.x;
    for (TableColumn& column : columns) {
        const double start = column.pinned ? pinned : scrolling;
        if (column.pinned) pinned += column.width;
        scrolling += column.width;
        const double visible_start = std::max({
            start,
            viewport.x,
            column.pinned ? viewport.x : pinned_end,
        });
        const double visible_end = std::min({
            start + column.width,
            viewport.right(),
            column.pinned ? pinned_end : viewport.right(),
        });
        result.tracks.push_back(TableTrack{
            std::move(column), start, visible_start, visible_end,
        });
    }
    return result;
}

runtime::Value encode_widths(const std::vector<TableColumn>& columns) {
    std::vector<runtime::Value> result;
    result.reserve(columns.size());
    for (const TableColumn& column : columns) {
        result.emplace_back(std::vector<std::pair<std::string, runtime::Value>>{
            {"id", runtime::Value(column.id)},
            {"width", runtime::Value(column.width)},
        });
    }
    return runtime::Value(std::move(result));
}

runtime::Value encode_order(const std::vector<TableColumn>& columns) {
    std::vector<runtime::Value> result;
    result.reserve(columns.size());
    for (const TableColumn& column : columns) result.emplace_back(column.id);
    return runtime::Value(std::move(result));
}

} // namespace strata::ui::collection
