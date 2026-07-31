#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <vector>

#include "ui/layout/detail_values.hpp"

namespace strata::ui::layout_detail {

[[nodiscard]] inline double resolve_content_size(
    const LayoutSize& size,
    const double intrinsic,
    const double border_available,
    const double padding
) noexcept {
    double result = intrinsic;
    switch (size.kind) {
    case LayoutSize::Kind::automatic:
    case LayoutSize::Kind::content: result = intrinsic; break;
    case LayoutSize::Kind::fixed: result = std::max(0.0, size.value - padding); break;
    case LayoutSize::Kind::percent:
        result = std::isfinite(border_available)
                     ? std::max(0.0, border_available * size.value - padding)
                     : intrinsic;
        break;
    case LayoutSize::Kind::fill:
        result = std::isfinite(border_available)
                     ? std::max(0.0, border_available - padding)
                     : intrinsic;
        break;
    case LayoutSize::Kind::clamp:
        result = size.preferred != nullptr
                     ? resolve_content_size(
                           *size.preferred, intrinsic, border_available, padding
                       )
                     : intrinsic;
        if (size.minimum != nullptr) {
            result = std::max(
                result,
                resolve_content_size(*size.minimum, 0.0, border_available, padding)
            );
        }
        if (size.maximum != nullptr) {
            result = std::min(
                result,
                resolve_content_size(*size.maximum, result, border_available, padding)
            );
        }
        break;
    }
    return std::max(0.0, finite_or(result, intrinsic));
}

[[nodiscard]] inline double clamp_optional_content_size(
    const double value,
    const std::optional<LayoutSize>& minimum,
    const std::optional<LayoutSize>& maximum,
    const double border_available,
    const double padding
) noexcept {
    double result = value;
    if (minimum.has_value()) {
        result = std::max(result, resolve_content_size(*minimum, 0.0, border_available, padding));
    }
    if (maximum.has_value()) {
        result = std::min(result, resolve_content_size(*maximum, result, border_available, padding));
    }
    return std::max(0.0, result);
}

[[nodiscard]] inline Size resolve_content_box(
    const LayoutStyle& style,
    const Size intrinsic,
    const Constraints& constraints
) noexcept {
    const double border_available_width = subtract_finite(
        constraints.max_width,
        style.margin.horizontal()
    );
    const double border_available_height = subtract_finite(
        constraints.max_height,
        style.margin.vertical()
    );
    Size result{
        resolve_content_size(
            style.width,
            intrinsic.width,
            border_available_width,
            style.padding.horizontal()
        ),
        resolve_content_size(
            style.height,
            intrinsic.height,
            border_available_height,
            style.padding.vertical()
        ),
    };
    result.width = clamp_optional_content_size(
        result.width,
        style.min_width,
        style.max_width,
        border_available_width,
        style.padding.horizontal()
    );
    result.height = clamp_optional_content_size(
        result.height,
        style.min_height,
        style.max_height,
        border_available_height,
        style.padding.vertical()
    );
    if (style.aspect_ratio.has_value()) {
        const bool width_explicit = style.width.kind != LayoutSize::Kind::automatic &&
                                    style.width.kind != LayoutSize::Kind::content;
        const bool height_explicit = style.height.kind != LayoutSize::Kind::automatic &&
                                     style.height.kind != LayoutSize::Kind::content;
        if (width_explicit && !height_explicit) {
            result.height = std::max(
                0.0,
                (result.width + style.padding.horizontal()) / *style.aspect_ratio -
                    style.padding.vertical()
            );
        } else if (height_explicit && !width_explicit) {
            result.width = std::max(
                0.0,
                (result.height + style.padding.vertical()) * *style.aspect_ratio -
                    style.padding.horizontal()
            );
        }
    }
    return result;
}

[[nodiscard]] inline double measurement_content_available(
    const LayoutStyle& style,
    const Constraints& constraints,
    const Constraints& loose_content_constraints,
    const bool horizontal
) noexcept {
    const LayoutSize& authored = horizontal ? style.width : style.height;
    const std::optional<LayoutSize>& minimum = horizontal ? style.min_width : style.min_height;
    const std::optional<LayoutSize>& maximum = horizontal ? style.max_width : style.max_height;
    const double constraint_maximum = horizontal ? constraints.max_width : constraints.max_height;
    const double margin = horizontal ? style.margin.horizontal() : style.margin.vertical();
    const double padding = horizontal ? style.padding.horizontal() : style.padding.vertical();
    const double loose_maximum = horizontal
                                     ? loose_content_constraints.max_width
                                     : loose_content_constraints.max_height;
    const double border_available = subtract_finite(constraint_maximum, margin);
    return clamp_optional_content_size(
        resolve_content_size(authored, loose_maximum, border_available, padding),
        minimum,
        maximum,
        border_available,
        padding
    );
}

[[nodiscard]] inline double cross_size(
    const double measured,
    const double available,
    const LayoutAlign alignment
) noexcept {
    return alignment == LayoutAlign::stretch ? std::max(0.0, available) : std::min(measured, std::max(0.0, available));
}

[[nodiscard]] inline double cross_offset(
    const double available,
    const double child,
    const LayoutAlign alignment
) noexcept {
    if (alignment == LayoutAlign::center) return std::max(0.0, (available - child) * 0.5);
    if (alignment == LayoutAlign::end) return std::max(0.0, available - child);
    return 0.0;
}

struct Distribution final {
    double start = 0.0;
    double between = 0.0;
};

[[nodiscard]] inline Distribution distribution(
    const double used,
    const double available,
    const std::size_t count,
    const LayoutJustify justification
) noexcept {
    const double extra = std::max(0.0, available - used);
    const double count_value = static_cast<double>(count);
    switch (justification) {
    case LayoutJustify::center: return {extra * 0.5, 0.0};
    case LayoutJustify::end: return {extra, 0.0};
    case LayoutJustify::space_between:
        return {0.0, count > 1U ? extra / static_cast<double>(count - 1U) : 0.0};
    case LayoutJustify::space_around:
        return count != 0U ? Distribution{extra / count_value * 0.5, extra / count_value} : Distribution{};
    case LayoutJustify::space_evenly:
        return {extra / (count_value + 1.0), extra / (count_value + 1.0)};
    case LayoutJustify::start: return {};
    }
    return {};
}

[[nodiscard]] inline LinearLayoutResolution resolve_linear_layout(
    const std::vector<Size>& children,
    const bool horizontal,
    const bool wrap,
    const double available_main,
    const Point gap
) {
    LinearLayoutResolution result;
    result.horizontal = horizontal;
    const double main_gap = horizontal ? gap.x : gap.y;
    const double cross_gap = horizontal ? gap.y : gap.x;
    const auto main_extent = [horizontal](const Size size) {
        return horizontal ? size.width : size.height;
    };
    const auto cross_extent = [horizontal](const Size size) {
        return horizontal ? size.height : size.width;
    };
    for (std::size_t index = 0U; index < children.size(); ++index) {
        const double child_main = main_extent(children[index]);
        const double child_cross = cross_extent(children[index]);
        const bool starts_line = result.lines.empty() || result.lines.back().children.empty();
        const double next_main = starts_line
                                     ? child_main
                                     : result.lines.back().main_size + main_gap + child_main;
        if (!starts_line && wrap && std::isfinite(available_main) && next_main > available_main) {
            result.lines.emplace_back();
        }
        if (result.lines.empty()) result.lines.emplace_back();
        LayoutLine& line = result.lines.back();
        if (!line.children.empty()) line.main_size += main_gap;
        line.children.push_back(index);
        line.main_size += child_main;
        line.cross_size = std::max(line.cross_size, child_cross);
    }
    double cross_total = 0.0;
    double largest_main = 0.0;
    for (const LayoutLine& line : result.lines) {
        largest_main = std::max(largest_main, line.main_size);
        cross_total += line.cross_size;
    }
    if (!result.lines.empty()) {
        cross_total += cross_gap * static_cast<double>(result.lines.size() - 1U);
    }
    result.intrinsic_size = horizontal
                                ? Size{largest_main, cross_total}
                                : Size{cross_total, largest_main};
    return result;
}

struct GridItemMetrics final {
    std::size_t child_index = 0U;
    std::optional<std::size_t> column;
    std::optional<std::size_t> row;
    std::size_t column_span = 1U;
    std::size_t row_span = 1U;
    Size measured_size;
};

[[nodiscard]] inline LayoutSize default_grid_track() noexcept {
    LayoutSize result;
    result.kind = LayoutSize::Kind::fill;
    result.value = 1.0;
    return result;
}

[[nodiscard]] inline double grid_track_base(
    const LayoutSize& track,
    const double contribution,
    const double available
) noexcept {
    double result = contribution;
    switch (track.kind) {
    case LayoutSize::Kind::fixed: result = std::max(0.0, track.value); break;
    case LayoutSize::Kind::percent:
        result = std::isfinite(available)
                     ? std::max(0.0, available * track.value)
                     : contribution;
        break;
    case LayoutSize::Kind::automatic:
    case LayoutSize::Kind::content: result = contribution; break;
    case LayoutSize::Kind::fill:
        // A weighted track is sized by its share of the free space, not by its content. Seeding it
        // with a filling child's own contribution would make every such track claim the whole axis
        // and push its neighbours off the grid.
        result = std::isfinite(available) ? 0.0 : contribution;
        break;
    case LayoutSize::Kind::clamp:
        result = track.preferred != nullptr
                     ? grid_track_base(*track.preferred, contribution, available)
                     : contribution;
        if (track.minimum != nullptr) {
            result = std::max(result, grid_track_base(*track.minimum, 0.0, available));
        }
        if (track.maximum != nullptr) {
            result = std::min(result, grid_track_base(*track.maximum, result, available));
        }
        break;
    }
    return std::max(0.0, finite_or(result, contribution));
}

[[nodiscard]] inline double grid_track_maximum(
    const LayoutSize& track,
    const double available
) noexcept {
    if (track.kind != LayoutSize::Kind::clamp || track.maximum == nullptr) return infinity;
    return grid_track_base(*track.maximum, infinity, available);
}

[[nodiscard]] inline bool grid_track_can_grow(
    const LayoutSize& track,
    const double available
) noexcept {
    if (track.kind == LayoutSize::Kind::fixed) return false;
    if (track.kind == LayoutSize::Kind::percent && std::isfinite(available)) return false;
    return true;
}

inline void grow_grid_tracks(
    std::vector<double>& sizes,
    const std::vector<LayoutSize>& tracks,
    const std::size_t start,
    const std::size_t span,
    double amount,
    const double available,
    const bool fill_only = false
) {
    const std::size_t end = std::min(sizes.size(), start + span);
    constexpr double epsilon = 0.000'000'1;
    while (amount > epsilon) {
        std::vector<std::size_t> growable;
        double total_weight = 0.0;
        for (std::size_t index = start; index < end; ++index) {
            const LayoutSize& track = tracks[index];
            const double maximum = grid_track_maximum(track, available);
            if ((fill_only && track.kind != LayoutSize::Kind::fill) ||
                !grid_track_can_grow(track, available) || sizes[index] >= maximum - epsilon) {
                continue;
            }
            growable.push_back(index);
            total_weight += track.kind == LayoutSize::Kind::fill && track.value > 0.0
                                ? track.value
                                : 1.0;
        }
        if (growable.empty() || total_weight <= 0.0) break;
        double consumed = 0.0;
        for (const std::size_t index : growable) {
            const LayoutSize& track = tracks[index];
            const double weight = track.kind == LayoutSize::Kind::fill && track.value > 0.0
                                      ? track.value
                                      : 1.0;
            const double share = amount * weight / total_weight;
            const double maximum = grid_track_maximum(track, available);
            const double next = std::min(maximum, sizes[index] + share);
            consumed += next - sizes[index];
            sizes[index] = next;
        }
        if (consumed <= epsilon) break;
        amount -= consumed;
    }
}

[[nodiscard]] inline std::vector<double> resolve_tracks(
    const GridAxisResolution& axis,
    const double available,
    const double gap
) {
    std::vector<double> result(axis.tracks.size(), 0.0);
    if (axis.tracks.empty()) return result;
    const double total_gap = gap * static_cast<double>(axis.tracks.size() - 1U);
    const double usable = subtract_finite(available, total_gap);
    for (std::size_t index = 0U; index < axis.tracks.size(); ++index) {
        result[index] = grid_track_base(
            axis.tracks[index],
            index < axis.contributions.size() ? axis.contributions[index] : 0.0,
            usable
        );
    }

    std::vector<GridSpanContribution> spanning = axis.spanning_contributions;
    std::ranges::stable_sort(spanning, {}, &GridSpanContribution::span);
    for (const GridSpanContribution& contribution : spanning) {
        const std::size_t end = std::min(result.size(), contribution.start + contribution.span);
        if (contribution.start >= end) continue;
        double current = 0.0;
        for (std::size_t index = contribution.start; index < end; ++index) current += result[index];
        const double internal_gap = gap * static_cast<double>(end - contribution.start - 1U);
        const double required_tracks = std::max(0.0, contribution.extent - internal_gap);
        grow_grid_tracks(
            result,
            axis.tracks,
            contribution.start,
            end - contribution.start,
            std::max(0.0, required_tracks - current),
            usable
        );
    }

    if (std::isfinite(usable)) {
        double used = 0.0;
        for (const double size : result) used += size;
        grow_grid_tracks(
            result,
            axis.tracks,
            0U,
            axis.tracks.size(),
            std::max(0.0, usable - used),
            usable,
            true
        );
    }
    return result;
}

[[nodiscard]] inline GridLayoutResolution resolve_grid_layout(
    const LayoutStyle& style,
    const std::vector<GridItemMetrics>& children
) {
    GridLayoutResolution result;
    const std::size_t child_count = children.size();
    const std::size_t column_count = !style.grid_columns.empty()
                                         ? style.grid_columns.size()
                                         : child_count == 0U
                                               ? 0U
                                               : child_count <= 1U
                                                     ? 1U
                                                     : static_cast<std::size_t>(std::ceil(
                                                           std::sqrt(static_cast<double>(child_count))
                                                       ));
    const std::size_t row_count = !style.grid_rows.empty()
                                      ? style.grid_rows.size()
                                      : column_count == 0U
                                            ? 0U
                                            : (child_count + column_count - 1U) / column_count;
    result.columns.tracks = !style.grid_columns.empty()
                                ? style.grid_columns
                                : std::vector<LayoutSize>(column_count, default_grid_track());
    result.rows.tracks = !style.grid_rows.empty()
                             ? style.grid_rows
                             : std::vector<LayoutSize>(row_count, default_grid_track());
    result.columns.contributions.assign(column_count, 0.0);
    result.rows.contributions.assign(row_count, 0.0);
    if (column_count == 0U || row_count == 0U) return result;

    std::vector<bool> occupied(column_count * row_count, false);
    const auto area_available = [&](const std::size_t column, const std::size_t row,
                                    const std::size_t column_span, const std::size_t row_span) {
        if (column >= column_count || row >= row_count ||
            column_span > column_count - column || row_span > row_count - row) {
            return false;
        }
        for (std::size_t y = row; y < row + row_span; ++y) {
            for (std::size_t x = column; x < column + column_span; ++x) {
                if (occupied[y * column_count + x]) return false;
            }
        }
        return true;
    };
    const auto mark_occupied = [&](const GridPlacement& placement) {
        const std::size_t end_column = std::min(column_count, placement.column + placement.column_span);
        const std::size_t end_row = std::min(row_count, placement.row + placement.row_span);
        for (std::size_t y = placement.row; y < end_row; ++y) {
            for (std::size_t x = placement.column; x < end_column; ++x) {
                occupied[y * column_count + x] = true;
            }
        }
    };

    std::size_t next_auto_index = 0U;
    result.placements.reserve(child_count);
    for (const GridItemMetrics& child : children) {
        const std::size_t requested_column_span = std::min(
            column_count, std::max<std::size_t>(1U, child.column_span)
        );
        const std::size_t requested_row_span = std::min(
            row_count, std::max<std::size_t>(1U, child.row_span)
        );
        GridPlacement placement;
        placement.child_index = child.child_index;
        if (child.column.has_value() || child.row.has_value()) {
            placement.column = std::min(child.column.value_or(0U), column_count - 1U);
            placement.row = std::min(child.row.value_or(0U), row_count - 1U);
        } else {
            bool found = false;
            for (std::size_t index = next_auto_index; index < occupied.size(); ++index) {
                const std::size_t column = index % column_count;
                const std::size_t row = index / column_count;
                if (!area_available(
                        column, row, requested_column_span, requested_row_span
                    )) {
                    continue;
                }
                placement.column = column;
                placement.row = row;
                next_auto_index = index + 1U;
                found = true;
                break;
            }
            if (!found) {
                const std::size_t fallback = child.child_index % occupied.size();
                placement.column = fallback % column_count;
                placement.row = fallback / column_count;
            }
        }
        placement.column_span = std::min(requested_column_span, column_count - placement.column);
        placement.row_span = std::min(requested_row_span, row_count - placement.row);
        result.placements.push_back(placement);
        mark_occupied(placement);

        if (placement.column_span == 1U) {
            result.columns.contributions[placement.column] = std::max(
                result.columns.contributions[placement.column], child.measured_size.width
            );
        } else {
            result.columns.spanning_contributions.push_back(GridSpanContribution{
                placement.column, placement.column_span, child.measured_size.width,
            });
        }
        if (placement.row_span == 1U) {
            result.rows.contributions[placement.row] = std::max(
                result.rows.contributions[placement.row], child.measured_size.height
            );
        } else {
            result.rows.spanning_contributions.push_back(GridSpanContribution{
                placement.row, placement.row_span, child.measured_size.height,
            });
        }
    }
    return result;
}

[[nodiscard]] inline Size resolved_grid_size(
    const GridLayoutResolution& grid,
    const double available_width,
    const double available_height,
    const Point gap
) {
    const std::vector<double> columns = resolve_tracks(grid.columns, available_width, gap.x);
    const std::vector<double> rows = resolve_tracks(grid.rows, available_height, gap.y);
    Size result;
    for (const double column : columns) result.width += column;
    for (const double row : rows) result.height += row;
    if (!columns.empty()) result.width += gap.x * static_cast<double>(columns.size() - 1U);
    if (!rows.empty()) result.height += gap.y * static_cast<double>(rows.size() - 1U);
    return result;
}

[[nodiscard]] inline bool grid_span_is_definite(
    const GridAxisResolution& axis,
    const std::size_t start,
    const std::size_t span,
    const double available
) noexcept {
    const std::size_t end = std::min(axis.tracks.size(), start + span);
    if (start >= end) return false;
    for (std::size_t index = start; index < end; ++index) {
        const LayoutSize& track = axis.tracks[index];
        if (track.kind == LayoutSize::Kind::fixed) continue;
        if (track.kind == LayoutSize::Kind::percent && std::isfinite(available)) continue;
        if (track.kind == LayoutSize::Kind::clamp && track.minimum != nullptr &&
            track.maximum != nullptr &&
            std::abs(
                grid_track_base(*track.minimum, 0.0, available) -
                grid_track_base(*track.maximum, 0.0, available)
            ) <= 0.000'000'1) {
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] inline double track_offset(
    const std::vector<double>& tracks,
    const std::size_t index,
    const double gap
) noexcept {
    double result = gap * static_cast<double>(index);
    for (std::size_t cursor = 0U; cursor < std::min(index, tracks.size()); ++cursor) {
        result += tracks[cursor];
    }
    return result;
}

[[nodiscard]] inline double track_extent(
    const std::vector<double>& tracks,
    const std::size_t index,
    const std::size_t span,
    const double gap
) noexcept {
    const std::size_t end = std::min(tracks.size(), index + span);
    double result = end > index ? gap * static_cast<double>(end - index - 1U) : 0.0;
    for (std::size_t cursor = index; cursor < end; ++cursor) result += tracks[cursor];
    return result;
}

[[nodiscard]] inline Size default_intrinsic(const RetainedNode& node, const Constraints& constraints) {
    static_cast<void>(constraints);
    const DescriptionNode& description = node.description();
    const runtime::Value* value = scalar_property(description, "text");
    if (value == nullptr) value = node.retained_value("$text");
    if (value == nullptr) value = scalar_property(description, "label");
    if (value != nullptr && value->string() != nullptr) {
        return Size{static_cast<double>(value->string()->size()) * 7.0, 14.0};
    }
    return {};
}

[[nodiscard]] inline std::optional<Rect> intersect_optional(
    const std::optional<Rect>& inherited,
    const Rect& local
) noexcept {
    return inherited.has_value()
               ? std::optional<Rect>(inherited->clip_intersection(local))
               : std::optional<Rect>(local);
}

[[nodiscard]] inline double clamped_scroll(const double requested, const double content, const double viewport) noexcept {
    return std::clamp(finite_or(requested, 0.0), 0.0, std::max(0.0, content - viewport));
}

[[nodiscard]] inline Edges add_edges(const Edges& left, const Edges& right) noexcept {
    return Edges{
        left.left + right.left,
        left.top + right.top,
        left.right + right.right,
        left.bottom + right.bottom,
    };
}

[[nodiscard]] inline Edges scroll_viewport_edges(const LayoutStyle& style) noexcept {
    Edges result = style.scroll_viewport_insets;
    if (style.scroll_vertical) result.right += style.scrollbar_gutter;
    if (style.scroll_horizontal) result.bottom += style.scrollbar_gutter;
    return result;
}

[[nodiscard]] inline bool stable_environment_equal(
    const LayoutEnvironment& left,
    const LayoutEnvironment& right
) noexcept {
    return left.generation == right.generation && left.viewport == right.viewport &&
           left.scale == right.scale && left.safe_insets == right.safe_insets &&
           left.point_snapping == right.point_snapping &&
           left.rectangle_snapping == right.rectangle_snapping &&
           left.apply_safe_insets == right.apply_safe_insets &&
           left.reduced_motion == right.reduced_motion;
}

} // namespace strata::ui::layout_detail
