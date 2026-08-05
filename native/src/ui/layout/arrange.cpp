#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"

namespace strata::ui {
using namespace layout_detail;
namespace {

[[nodiscard]] Rect translated_rect(const Rect value, const Point delta) noexcept {
    return Rect{value.x + delta.x, value.y + delta.y, value.width, value.height};
}

[[nodiscard]] std::optional<Rect> translated_rect(
    const std::optional<Rect> value,
    const Point delta
) noexcept {
    return value.has_value() ? std::optional<Rect>(translated_rect(*value, delta)) : std::nullopt;
}

[[nodiscard]] double arranged_axis_size(
    const LayoutStyle& style,
    const Size measured_size,
    const bool horizontal,
    const double available,
    const LayoutAlign alignment
) noexcept {
    const LayoutSize& authored = horizontal ? style.width : style.height;
    const double measured = horizontal ? measured_size.width : measured_size.height;
    // Fill is resolved and min/max-clamped during measurement. Stretching it back to the raw
    // parent slot here discards that result, leaving descendants constrained while the widget's
    // painted and hit bounds escape their authored maximum.
    double result = authored.kind == LayoutSize::Kind::fill
        ? std::min(measured, std::max(0.0, available))
        : cross_size(measured, available, alignment);
    const std::optional<LayoutSize>& maximum =
        horizontal ? style.max_width : style.max_height;
    if (maximum.has_value()) {
        const double margin = horizontal
            ? style.margin.horizontal()
            : style.margin.vertical();
        const double padding = horizontal
            ? style.padding.horizontal()
            : style.padding.vertical();
        const double border_available = std::max(0.0, available - margin);
        const double intrinsic_content = std::max(0.0, measured - margin - padding);
        const double maximum_content = resolve_content_size(
            *maximum,
            intrinsic_content,
            border_available,
            padding
        );
        result = std::min(result, maximum_content + margin + padding);
    }
    return result;
}

[[nodiscard]] LayoutAlign layered_axis_alignment(
    const LayoutJustify justify
) noexcept {
    switch (justify) {
    case LayoutJustify::center: return LayoutAlign::center;
    case LayoutJustify::end: return LayoutAlign::end;
    default: return LayoutAlign::start;
    }
}

[[nodiscard]] double placement_offset(
    const LayoutSize& position,
    const double available
) noexcept {
    if (position.kind == LayoutSize::Kind::percent) {
        return position.value * std::max(0.0, available);
    }
    return position.kind == LayoutSize::Kind::fixed ? position.value : 0.0;
}

[[nodiscard]] Rect apply_placement(
    Rect bounds,
    const Rect container,
    const std::optional<LayoutPlacement>& authored
) noexcept {
    if (!authored.has_value()) return bounds;
    const LayoutPlacement& placement = *authored;
    if (placement.x.has_value()) {
        bounds.x = container.x +
            placement_offset(*placement.x, container.width) -
            bounds.width * placement.anchor_x;
    }
    if (placement.y.has_value()) {
        bounds.y = container.y +
            placement_offset(*placement.y, container.height) -
            bounds.height * placement.anchor_y;
    }
    bounds.x += placement.offset_x;
    bounds.y += placement.offset_y;
    return bounds;
}

void translate_record(
    LayoutRecord& record,
    const Point delta,
    const LayoutEnvironment& environment
) {
    record.bounds = translated_rect(record.bounds, delta);
    record.snapped_bounds = snap_rectangle(record.bounds, environment);
    record.hit_bounds = translated_rect(record.hit_bounds, delta);
    record.content_bounds = translated_rect(record.content_bounds, delta);
    record.local_clip = translated_rect(record.local_clip, delta);
    record.scroll_frame = translated_rect(record.scroll_frame, delta);
    record.viewport = translated_rect(record.viewport, delta);
}

} // namespace

void LayoutEngine::arrange(
    const MeasuredNodePtr& measured_ptr,
    Rect bounds,
    std::optional<Rect> inherited_clip,
    PinContext pin_context,
    const LayoutEnvironment& environment,
    LayoutResult& result
) {
    const MeasuredNode& measured = *measured_ptr;
    const Rect arrangement_bounds = bounds;
    const std::uint64_t identity = measured.node->identity();
    const PinContext cache_pin_context{
        measured.subtree_pins_horizontal ? pin_context.horizontal_offset : std::nullopt,
        measured.subtree_pins_vertical ? pin_context.vertical_offset : std::nullopt,
    };
    const auto cached = arrangement_cache_.find(identity);
    const std::shared_ptr<const MeasuredNode> cached_measured =
        cached != arrangement_cache_.end() ? cached->second.measured.lock() : nullptr;
    if (!measured.subtree_portals &&
        cached != arrangement_cache_.end() && cached_measured == measured_ptr &&
        cached->second.bounds == arrangement_bounds &&
        cached->second.inherited_clip == inherited_clip &&
        cached->second.pin_context == cache_pin_context &&
        cached->second.node_arrangement_revision == measured.node->arrangement_revision() &&
        result_.records.contains(identity)) {
        current_arranged_subtree_roots_.insert(identity);
        return;
    }
    const Point translation{
        arrangement_bounds.x -
            (cached != arrangement_cache_.end() ? cached->second.bounds.x : arrangement_bounds.x),
        arrangement_bounds.y -
            (cached != arrangement_cache_.end() ? cached->second.bounds.y : arrangement_bounds.y),
    };
    const bool translated_cache_hit = !measured.subtree_portals &&
        cached != arrangement_cache_.end() &&
        cached_measured == measured_ptr && translation != Point{} &&
        cached->second.bounds.width == arrangement_bounds.width &&
        cached->second.bounds.height == arrangement_bounds.height &&
        cached->second.pin_context == cache_pin_context &&
        cached->second.node_arrangement_revision == measured.node->arrangement_revision() &&
        result_.records.contains(identity);
    if (translated_cache_hit) {
        const auto copy_translated_subtree =
            [this, &result, &environment, translation](
                const auto& self,
                const MeasuredNodePtr& node,
                const std::optional<Rect> current_inherited_clip
            ) -> void {
            const auto previous = result_.records.find(node->node->identity());
            if (previous == result_.records.end()) {
                throw std::logic_error("cached arrangement lost its retained layout record");
            }
            LayoutRecord record = previous->second;
            record.generation = result.generation;
            translate_record(record, translation, environment);
            record.render_generation = advance_render_generation();
            record.subtree_render_generation = advance_render_generation();
            record.translated_subtree = translation;
            translated_records_.push_back(record.identity);
            std::optional<Rect> child_clip = current_inherited_clip;
            if (record.local_clip.has_value()) {
                child_clip = intersect_optional(child_clip, *record.local_clip);
            }
            if (node->style.kind == LayoutKind::portal &&
                node->style.detach_from_parent_clip) {
                child_clip.reset();
            }
            record.clip = child_clip;
            result.records.insert_or_assign(record.identity, std::move(record));
            current_arranged_records_.insert(node->node->identity());
            if (auto entry = arrangement_cache_.find(node->node->identity());
                entry != arrangement_cache_.end()) {
                entry->second.bounds = translated_rect(entry->second.bounds, translation);
                entry->second.inherited_clip = current_inherited_clip;
            }
            for (const MeasuredNodePtr& child : node->children) {
                self(self, child, child_clip);
            }
        };
        copy_translated_subtree(copy_translated_subtree, measured_ptr, inherited_clip);
        return;
    }
    ++result.operations.arranged_nodes;
    const LayoutStyle& style = measured.style;
    const Point requested_scroll_offset = resolved_scroll_offset(
        *measured.node,
        style.scroll_offset
    );
    if (style.pin_horizontal && pin_context.horizontal_offset.has_value()) bounds.x += *pin_context.horizontal_offset;
    if (style.pin_vertical && pin_context.vertical_offset.has_value()) bounds.y += *pin_context.vertical_offset;
    const Rect border_bounds = bounds.deflate(style.margin);
    const Rect base_content_bounds = border_bounds.deflate(style.padding);
    const Rect scroll_viewport = style.kind == LayoutKind::scroll
                                     ? base_content_bounds.deflate(scroll_viewport_edges(style))
                                     : base_content_bounds;
    const Rect content_bounds = style.kind == LayoutKind::scroll
                                    ? scroll_viewport.deflate(style.scroll_content_padding)
                                    : base_content_bounds;
    const std::optional<Rect> self_hit = border_bounds;
    const bool clips = style.clip || style.kind == LayoutKind::scroll;
    const Rect clip_bounds = style.kind == LayoutKind::scroll ? scroll_viewport : content_bounds;
    const std::optional<Rect> local_clip = clips ? std::optional<Rect>(clip_bounds) : std::nullopt;
    std::optional<Rect> child_clip = inherited_clip;
    if (clips) child_clip = intersect_optional(inherited_clip, clip_bounds);
    if (style.kind == LayoutKind::portal && style.detach_from_parent_clip) child_clip.reset();

    LayoutRecord record;
    record.identity = measured.node->identity();
    record.generation = result.generation;
    record.kind = style.kind;
    record.measured_size = measured.measured_size;
    record.bounds = border_bounds;
    record.snapped_bounds = snap_rectangle(border_bounds, environment);
    record.hit_bounds = self_hit.value_or(Rect{});
    record.content_bounds = content_bounds;
    record.clip = child_clip;
    record.local_clip = local_clip;
    record.content_size = measured.content_size;
    record.requested_scroll_offset = requested_scroll_offset;
    record.scroll_horizontal = style.scroll_horizontal;
    record.scroll_vertical = style.scroll_vertical;
    record.pin_horizontal = style.pin_horizontal;
    record.pin_vertical = style.pin_vertical;
    record.z_index = style.z_index;
    record.portal_target = style.kind == LayoutKind::portal ? style.portal_target : std::string{};
    record.detached_from_parent_clip = style.kind == LayoutKind::portal && style.detach_from_parent_clip;
    record.content_motion_progress = measured.content_motion_progress;
    record.content_motion_running = measured.content_motion_running;
    record.content_motion_clip = measured.content_motion_clip;
    record.content_motion_snapped_by_reduced_motion =
        measured.content_motion_snapped_by_reduced_motion;
    record.content_motion_target_size = measured.content_motion_target_size;
    record.arranged_child_order.reserve(measured.children.size());
    record.materialized_child_indices.reserve(measured.children.size());
    for (const MeasuredNodePtr& child : measured.children) {
        record.arranged_child_order.push_back(child->node->identity());
        record.materialized_child_indices.push_back(child->node->source_index());
    }
    const std::span<const MeasuredNodePtr> flow_children{
        measured.children.data(),
        measured.flow_child_count,
    };

    if ((style.kind == LayoutKind::row || style.kind == LayoutKind::column) && style.wrap) {
        const bool horizontal = style.kind == LayoutKind::row;
        const LinearLayoutResolution& linear = *measured.linear;
        const double available_main = horizontal ? content_bounds.width : content_bounds.height;
        const double available_cross = horizontal ? content_bounds.height : content_bounds.width;
        const double main_gap = horizontal ? style.gap.x : style.gap.y;
        const double cross_gap = horizontal ? style.gap.y : style.gap.x;
        double used_cross = 0.0;
        for (const LayoutLine& line : linear.lines) used_cross += line.cross_size;
        if (!linear.lines.empty()) {
            used_cross += cross_gap * static_cast<double>(linear.lines.size() - 1U);
        }
        const Distribution line_distribution = distribution(
            used_cross, available_cross, linear.lines.size(), style.align_content
        );
        double cross_cursor = (horizontal ? content_bounds.y : content_bounds.x) +
                              line_distribution.start;
        for (const LayoutLine& line : linear.lines) {
            const Distribution item_distribution = distribution(
                line.main_size, available_main, line.children.size(), style.justify_content
            );
            double main_cursor = (horizontal ? content_bounds.x : content_bounds.y) +
                                 item_distribution.start;
            for (const std::size_t child_index : line.children) {
                const MeasuredNode& child = *measured.children[child_index];
                const LayoutAlign alignment = child.style.align_self.value_or(style.align_items);
                const double child_cross = arranged_axis_size(
                    child.style,
                    child.measured_size,
                    !horizontal,
                    line.cross_size,
                    alignment
                );
                const double child_cross_offset = cross_offset(
                    line.cross_size, child_cross, alignment
                );
                const Rect child_bounds = horizontal
                                              ? Rect{
                                                    main_cursor,
                                                    cross_cursor + child_cross_offset,
                                                    child.measured_size.width,
                                                    child_cross,
                                                }
                                              : Rect{
                                                    cross_cursor + child_cross_offset,
                                                    main_cursor,
                                                    child_cross,
                                                    child.measured_size.height,
                                                };
                arrange(
                    measured.children[child_index],
                    child_bounds,
                    child_clip,
                    pin_context,
                    environment,
                    result
                );
                main_cursor += (horizontal
                                    ? child.measured_size.width
                                    : child.measured_size.height) +
                               main_gap + item_distribution.between;
            }
            cross_cursor += line.cross_size + cross_gap + line_distribution.between;
        }
    } else if (style.kind == LayoutKind::row) {
        double used = 0.0;
        for (const MeasuredNodePtr& child : flow_children) {
            used += child->measured_size.width;
        }
        if (!flow_children.empty()) used += style.gap.x * static_cast<double>(flow_children.size() - 1U);
        const Distribution spacing = distribution(used, content_bounds.width, flow_children.size(), style.justify_content);
        double cursor = content_bounds.x + spacing.start;
        for (const MeasuredNodePtr& child : flow_children) {
            const LayoutAlign child_align = child->style.align_self.value_or(style.align_items);
            const double height = arranged_axis_size(
                child->style,
                child->measured_size,
                false,
                content_bounds.height,
                child_align
            );
            const double y = content_bounds.y + cross_offset(content_bounds.height, height, child_align);
            arrange(child, Rect{cursor, y, child->measured_size.width, height}, child_clip, pin_context, environment, result);
            cursor += child->measured_size.width + style.gap.x + spacing.between;
        }
    } else if (style.kind == LayoutKind::column) {
        double used = 0.0;
        for (const MeasuredNodePtr& child : flow_children) {
            used += child->measured_size.height;
        }
        if (!flow_children.empty()) used += style.gap.y * static_cast<double>(flow_children.size() - 1U);
        const Distribution spacing = distribution(used, content_bounds.height, flow_children.size(), style.justify_content);
        double cursor = content_bounds.y + spacing.start;
        for (const MeasuredNodePtr& child : flow_children) {
            const LayoutAlign child_align = child->style.align_self.value_or(style.align_items);
            const double width = arranged_axis_size(
                child->style,
                child->measured_size,
                true,
                content_bounds.width,
                child_align
            );
            const double x = content_bounds.x + cross_offset(content_bounds.width, width, child_align);
            arrange(child, Rect{x, cursor, width, child->measured_size.height}, child_clip, pin_context, environment, result);
            cursor += child->measured_size.height + style.gap.y + spacing.between;
        }
    } else if (style.kind == LayoutKind::grid) {
        if (!measured.grid.has_value()) {
            record.render_generation = advance_render_generation();
            result.records.insert_or_assign(record.identity, std::move(record));
            arrangement_cache_.insert_or_assign(identity, ArrangementCacheEntry{
                measured_ptr, arrangement_bounds, inherited_clip, cache_pin_context,
                measured.node->arrangement_revision(),
            });
            return;
        }
        const GridLayoutResolution& grid = *measured.grid;
        const std::vector<double> columns = resolve_tracks(
            grid.columns, content_bounds.width, style.gap.x
        );
        const std::vector<double> rows = resolve_tracks(
            grid.rows, content_bounds.height, style.gap.y
        );
        std::vector<const GridPlacement*> placements;
        placements.reserve(grid.placements.size());
        for (const GridPlacement& placement : grid.placements) placements.push_back(&placement);
        std::ranges::stable_sort(
            placements,
            {},
            [&measured](const GridPlacement* placement) {
                return measured.children[placement->child_index]->style.z_index;
            }
        );
        for (const GridPlacement* placement : placements) {
            const MeasuredNode& child = *measured.children[placement->child_index];
            const double cell_width = track_extent(
                columns, placement->column, placement->column_span, style.gap.x
            );
            const double cell_height = track_extent(
                rows, placement->row, placement->row_span, style.gap.y
            );
            const LayoutAlign horizontal = child.style.justify_self.value_or(
                LayoutAlign::stretch
            );
            const LayoutAlign vertical = child.style.align_self.value_or(
                LayoutAlign::stretch
            );
            const double width = arranged_axis_size(
                child.style,
                child.measured_size,
                true,
                cell_width,
                horizontal
            );
            const double height = arranged_axis_size(
                child.style,
                child.measured_size,
                false,
                cell_height,
                vertical
            );
            arrange(
                measured.children[placement->child_index],
                Rect{
                    content_bounds.x +
                        track_offset(columns, placement->column, style.gap.x) +
                        cross_offset(cell_width, width, horizontal),
                    content_bounds.y +
                        track_offset(rows, placement->row, style.gap.y) +
                        cross_offset(cell_height, height, vertical),
                    width,
                    height,
                },
                child_clip,
                pin_context,
                environment,
                result
            );
        }
    } else if (style.kind == LayoutKind::scroll) {
        std::optional<collection::VirtualExtentResolution> virtual_extents;
        Point resolved_scroll_offset = requested_scroll_offset;
        if (style.virtual_list.has_value()) {
            const VirtualListSpec& virtual_list = *style.virtual_list;
            std::vector<collection::VirtualMeasurement> measurements;
            if (virtual_list.measure_item_extents) {
                measurements.reserve(flow_children.size());
                for (const MeasuredNodePtr& child : flow_children) {
                    const std::optional<std::string>& materialization_key =
                        child->node->description().materialization_key;
                    measurements.push_back(collection::VirtualMeasurement{
                        child->node->source_index(),
                        materialization_key.value_or(
                            child->node->description().key.value_or(std::string{})
                        ),
                        virtual_list.axis == LayoutAxis::vertical
                            ? child->measured_size.height
                            : child->measured_size.width,
                    });
                }
            }
            const double requested_offset = virtual_list.axis == LayoutAxis::vertical
                                                ? requested_scroll_offset.y
                                                : requested_scroll_offset.x;
            virtual_extents = collection_virtualization_.resolve(
                measured.node->identity(),
                collection::VirtualExtentRequest{
                    virtual_list.items,
                    virtual_list.item_extent,
                    virtual_list.item_extents,
                    virtual_list.measure_item_extents,
                    virtual_list.reset_anchor_on_change,
                },
                requested_offset,
                measurements
            );
            if (virtual_list.axis == LayoutAxis::vertical) {
                resolved_scroll_offset.y = virtual_extents->offset;
            } else {
                resolved_scroll_offset.x = virtual_extents->offset;
            }
            record.virtual_items = virtual_list.items;
            record.virtual_item_members = virtual_list.item_members;
            record.virtual_item_extents = virtual_extents->extents;
            record.virtual_axis = virtual_list.axis;
            record.virtual_overscan = virtual_list.overscan;
        }
        Size scroll_content;
        for (const MeasuredNodePtr& child : flow_children) {
            scroll_content.width = std::max(scroll_content.width, child->measured_size.width);
            scroll_content.height += child->measured_size.height;
        }
        if (!flow_children.empty()) scroll_content.height += style.gap.y * static_cast<double>(flow_children.size() - 1U);
        if (style.virtual_list.has_value()) {
            const VirtualListSpec& virtual_list = *style.virtual_list;
            const double virtual_extent = virtual_extents->extents.total();
            if (virtual_list.axis == LayoutAxis::vertical) scroll_content.height = std::max(scroll_content.height, virtual_extent);
            else scroll_content.width = std::max(scroll_content.width, virtual_extent);
        }
        record.scroll_frame = base_content_bounds.deflate(style.scroll_viewport_insets);
        record.viewport = scroll_viewport;
        record.content_size = scroll_content;
        record.scroll_offset = Point{
            style.scroll_horizontal ? clamped_scroll(resolved_scroll_offset.x, scroll_content.width, content_bounds.width) : 0.0,
            style.scroll_vertical ? clamped_scroll(resolved_scroll_offset.y, scroll_content.height, content_bounds.height) : 0.0,
        };
        if (virtual_extents.has_value() && virtual_extents->anchor_changed) {
            collection_virtualization_.queue_anchor(collection::VirtualAnchorUpdate{
                measured.node->identity(),
                record.scroll_offset.x,
                record.scroll_offset.y,
            });
        }
        PinContext child_pin_context = pin_context;
        if (style.scroll_horizontal) child_pin_context.horizontal_offset = record.scroll_offset.x;
        if (style.scroll_vertical) child_pin_context.vertical_offset = record.scroll_offset.y;
        double cursor = content_bounds.y - record.scroll_offset.y;
        std::size_t first_visible = std::numeric_limits<std::size_t>::max();
        std::size_t last_visible = 0U;
        for (const MeasuredNodePtr& child : flow_children) {
            // A lazy provider can change count before Surface converges its retained realization.
            // Rows from the previous provider are transiently stale and must not index the new
            // extent table or participate in this layout pass.
            if (virtual_extents.has_value() &&
                child->node->source_index() >= virtual_extents->extents.size()) {
                continue;
            }
            const double x = content_bounds.x - record.scroll_offset.x;
            double child_x = x;
            double child_y = cursor;
            if (style.virtual_list.has_value()) {
                const VirtualListSpec& virtual_list = *style.virtual_list;
                const double position = virtual_extents->extents.start(child->node->source_index());
                if (virtual_list.axis == LayoutAxis::vertical) {
                    child_y = content_bounds.y + position - record.scroll_offset.y;
                } else if (virtual_list.axis == LayoutAxis::horizontal) {
                    child_x = content_bounds.x + position - record.scroll_offset.x;
                }
            }
            double child_width = child->measured_size.width;
            double child_height = child->measured_size.height;
            if (style.virtual_list.has_value()) {
                const VirtualListSpec& virtual_list = *style.virtual_list;
                const double child_extent = virtual_extents->extents.extent(
                    child->node->source_index()
                );
                if (virtual_list.axis == LayoutAxis::vertical) child_height = child_extent;
                else child_width = child_extent;
            }
            const Rect child_bounds{child_x, child_y, child_width, child_height};
            if (child_bounds.intersection(scroll_viewport).has_value()) {
                first_visible = std::min(first_visible, child->node->source_index());
                last_visible = std::max(last_visible, child->node->source_index() + 1U);
            }
            arrange(child, child_bounds, child_clip, child_pin_context, environment, result);
            cursor += child->measured_size.height + style.gap.y;
        }
        if (style.virtual_list.has_value()) {
            const VirtualListSpec& virtual_list = *style.virtual_list;
            const double offset = virtual_list.axis == LayoutAxis::vertical ? record.scroll_offset.y : record.scroll_offset.x;
            const double extent = virtual_list.axis == LayoutAxis::vertical ? scroll_viewport.height : scroll_viewport.width;
            const collection::VirtualItemExtents& resolved = virtual_extents->extents;
            const std::size_t item_count = virtual_list.item_count();
            if (item_count == 0U || offset >= resolved.total() || extent <= 0.0) {
                record.visible_range = VisibleRange{};
            } else {
                const std::size_t start_raw = resolved.first_index_ending_after(
                    std::max(0.0, offset)
                );
                const std::size_t end_raw = resolved.end_index_starting_before(
                    std::min(resolved.total(), offset + extent)
                );
                record.visible_range = VisibleRange{
                    start_raw > virtual_list.overscan
                        ? start_raw - virtual_list.overscan : 0U,
                    std::min(
                        item_count,
                        end_raw > item_count -
                            std::min(item_count, virtual_list.overscan)
                            ? item_count
                            : end_raw + virtual_list.overscan
                    ),
                };
            }
            record.virtual_item_keys.clear();
            record.virtual_item_key_start = record.visible_range->start;
            record.virtual_item_keys.reserve(
                record.visible_range->end_exclusive - record.visible_range->start
            );
            for (std::size_t index = record.visible_range->start;
                 index < record.visible_range->end_exclusive;
                 ++index) {
                record.virtual_item_keys.push_back(virtual_list.items->key_at(index));
            }
        } else if (first_visible != std::numeric_limits<std::size_t>::max()) {
            record.visible_range = VisibleRange{first_visible, last_visible};
        } else {
            record.visible_range = VisibleRange{};
        }
    } else if (style.kind != LayoutKind::spacer) {
        std::vector<MeasuredNodePtr> ordered(flow_children.begin(), flow_children.end());
        std::ranges::stable_sort(ordered, {}, [](const MeasuredNodePtr& child) {
            return child->style.z_index;
        });
        for (const MeasuredNodePtr& child : ordered) {
            const LayoutAlign horizontal = child->style.justify_self.value_or(style.align_items);
            const LayoutAlign vertical = child->style.align_self.value_or(
                style.justify_content_authored
                    ? layered_axis_alignment(style.justify_content)
                    : style.align_items
            );
            const double width = arranged_axis_size(
                child->style,
                child->measured_size,
                true,
                content_bounds.width,
                horizontal
            );
            const double height = arranged_axis_size(
                child->style,
                child->measured_size,
                false,
                content_bounds.height,
                vertical
            );
            Rect child_bounds{
                content_bounds.x + cross_offset(content_bounds.width, width, horizontal),
                content_bounds.y + cross_offset(content_bounds.height, height, vertical),
                width,
                height,
            };
            child_bounds = apply_placement(
                child_bounds,
                content_bounds,
                child->style.placement
            );
            arrange(
                child,
                child_bounds,
                child_clip,
                pin_context,
                environment,
                result
            );
        }
    }
    for (std::size_t index = measured.flow_child_count;
         index < measured.children.size();
         ++index) {
        const MeasuredNodePtr& child = measured.children[index];
        if (child->style.kind == LayoutKind::portal) {
            pending_portals_.push_back(PendingPortal{
                child,
                child_clip,
                pin_context,
            });
            continue;
        }
        const LayoutAlign horizontal =
            child->style.justify_self.value_or(style.align_items);
        const LayoutAlign vertical = child->style.align_self.value_or(
            style.justify_content_authored
                ? layered_axis_alignment(style.justify_content)
                : style.align_items
        );
        const double width = arranged_axis_size(
            child->style,
            child->measured_size,
            true,
            content_bounds.width,
            horizontal
        );
        const double height = arranged_axis_size(
            child->style,
            child->measured_size,
            false,
            content_bounds.height,
            vertical
        );
        Rect child_bounds{
            content_bounds.x + cross_offset(content_bounds.width, width, horizontal),
            content_bounds.y + cross_offset(content_bounds.height, height, vertical),
            width,
            height,
        };
        child_bounds = apply_placement(
            child_bounds,
            content_bounds,
            child->style.placement
        );
        pending_anchors_.push_back(PendingAnchor{
            child,
            child_bounds,
            content_bounds,
            child_clip,
            pin_context,
        });
    }
    record.render_generation = advance_render_generation();
    record.subtree_render_generation = advance_render_generation();
    result.records.insert_or_assign(record.identity, std::move(record));
    current_arranged_records_.insert(identity);
    arrangement_cache_.insert_or_assign(identity, ArrangementCacheEntry{
        measured_ptr, arrangement_bounds, inherited_clip, cache_pin_context,
        measured.node->arrangement_revision(),
    });
}
} // namespace strata::ui
