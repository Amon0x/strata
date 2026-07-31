#include "ui/layout.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"
#include "ui/theme.hpp"
#include "ui/motion.hpp"

namespace strata::ui {
using namespace layout_detail;
namespace {

[[nodiscard]] std::string layout_number(const double value) {
    char buffer[64];
    const auto encoded = std::to_chars(
        std::begin(buffer), std::end(buffer), value, std::chars_format::general, 16
    );
    if (encoded.ec != std::errc{}) return "0.0";
    std::string result(buffer, encoded.ptr);
    if (result.find_first_of(".eE") == std::string::npos) result += ".0";
    return result;
}

[[nodiscard]] std::string layout_size_description(const LayoutSize& size) {
    switch (size.kind) {
    case LayoutSize::Kind::automatic: return "Auto";
    case LayoutSize::Kind::content: return "Content";
    case LayoutSize::Kind::fixed: return "Fixed(value=" + layout_number(size.value) + ")";
    case LayoutSize::Kind::percent: return "Percent(value=" + layout_number(size.value) + ")";
    case LayoutSize::Kind::fill: return "Fill(weight=" + layout_number(size.value) + ")";
    case LayoutSize::Kind::clamp:
        return "Clamp(min=" +
               (size.minimum != nullptr ? layout_size_description(*size.minimum) : std::string("null")) +
               ", preferred=" +
               (size.preferred != nullptr ? layout_size_description(*size.preferred) : std::string("Auto")) +
               ", max=" +
               (size.maximum != nullptr ? layout_size_description(*size.maximum) : std::string("null")) +
               ")";
    }
    return "Auto";
}

[[nodiscard]] std::string node_description(const RetainedNode& node) {
    return node.description().type +
           (node.description().key.has_value() ? "#" + *node.description().key : std::string{});
}

} // namespace

LayoutEngine::MeasuredNodePtr LayoutEngine::measure(
    const RetainedNode& node,
    const Constraints& constraints,
    const LayoutEnvironment& environment,
    LayoutOperationCounters& operations
) {
    constraints.validate();
    const auto cached = measurement_cache_.find(node.identity());
    const DirtySet& dirty = node.dirty();
    const bool measure_dirty = dirty.contains(DirtyReason::structure) ||
                               dirty.contains(DirtyReason::layout) ||
                               dirty.contains(DirtyReason::text) ||
                               dirty.contains(DirtyReason::editor) ||
                               dirty.contains(DirtyReason::style) ||
                               dirty.contains(DirtyReason::scale);
    if (cached != measurement_cache_.end() && cached->second.constraints == constraints &&
        cached->second.node_revision == node.revision() &&
        cached->second.environment_generation == environment.generation &&
        cached->second.scale == environment.scale && !measure_dirty) {
        ++operations.measurement_cache_hits;
        return cached->second.measured;
    }
    ++operations.measured_nodes;
    MeasuredNode measured;
    measured.node = &node;
    measured.style = resolved_style(node);
    const Edges frame_edges{
        measured.style.margin.left + measured.style.padding.left,
        measured.style.margin.top + measured.style.padding.top,
        measured.style.margin.right + measured.style.padding.right,
        measured.style.margin.bottom + measured.style.padding.bottom,
    };
    Edges child_constraint_edges = frame_edges;
    if (measured.style.kind == LayoutKind::scroll) {
        child_constraint_edges = add_edges(child_constraint_edges, scroll_viewport_edges(measured.style));
        child_constraint_edges = add_edges(child_constraint_edges, measured.style.scroll_content_padding);
    }
    const Constraints framed = constraints.deflate(child_constraint_edges);
    const Constraints inner = framed.loosen();
    const double available_content_width = std::min(
        measurement_content_available(measured.style, constraints, inner, true),
        inner.max_width
    );
    const double available_content_height = std::min(
        measurement_content_available(measured.style, constraints, inner, false),
        inner.max_height
    );
    Constraints content_constraints = inner;
    content_constraints.max_width = available_content_width;
    content_constraints.max_height = available_content_height;
    std::vector<const RetainedNode*> retained_children;
    retained_children.reserve(node.children().size());
    for (const auto& child : node.children()) {
        if (resolved_style(*child).participates &&
            child->lifecycle() != RetainedLifecycle::exiting) {
            retained_children.push_back(child.get());
        }
    }
    measured.children.reserve(retained_children.size());

    if (measured.style.kind == LayoutKind::row || measured.style.kind == LayoutKind::column) {
        const bool horizontal = measured.style.kind == LayoutKind::row;
        const double maximum_main = horizontal
                                        ? available_content_width
                                        : available_content_height;
        const double minimum_main = horizontal ? framed.min_width : framed.min_height;
        const double available_main = std::isfinite(maximum_main)
                                          ? maximum_main
                                          : minimum_main > 0.0 ? minimum_main : infinity;
        const double gap = horizontal ? measured.style.gap.x : measured.style.gap.y;
        const double total_gap = gap * static_cast<double>(retained_children.empty() ? 0U : retained_children.size() - 1U);
        double used = total_gap;
        double total_weight = 0.0;
        std::vector<MeasuredNodePtr> staged(retained_children.size());
        for (std::size_t index = 0U; index < retained_children.size(); ++index) {
            const LayoutStyle child_style = resolved_style(*retained_children[index]);
            const LayoutSize& main_size = horizontal ? child_style.width : child_style.height;
            if (main_size.kind == LayoutSize::Kind::fill &&
                std::isfinite(available_main) && !measured.style.wrap) {
                total_weight += main_size.value > 0.0 ? main_size.value : 1.0;
                continue;
            }
            Constraints child_constraints = content_constraints;
            if (horizontal) child_constraints.max_width = main_size.kind == LayoutSize::Kind::percent ? available_main : infinity;
            else child_constraints.max_height = main_size.kind == LayoutSize::Kind::percent ? available_main : infinity;
            staged[index] = measure(*retained_children[index], child_constraints, environment, operations);
            used += horizontal
                ? staged[index]->measured_size.width
                : staged[index]->measured_size.height;
        }
        const double remaining = std::isfinite(available_main) ? std::max(0.0, available_main - used) : 0.0;
        for (std::size_t index = 0U; index < retained_children.size(); ++index) {
            if (staged[index] == nullptr) {
                const LayoutStyle child_style = resolved_style(*retained_children[index]);
                const LayoutSize& main_size = horizontal ? child_style.width : child_style.height;
                const double weight = main_size.value > 0.0 ? main_size.value : 1.0;
                const double allocated = total_weight > 0.0
                                             ? remaining * weight / total_weight
                                             : 0.0;
                Constraints child_constraints = content_constraints;
                if (horizontal) {
                    child_constraints.min_width = allocated;
                    child_constraints.max_width = allocated;
                } else {
                    child_constraints.min_height = allocated;
                    child_constraints.max_height = allocated;
                }
                staged[index] = measure(*retained_children[index], child_constraints, environment, operations);
            }
            measured.children.push_back(std::move(staged[index]));
        }
        if (measured.style.wrap && std::isfinite(available_main)) {
            std::vector<Size> natural_sizes;
            natural_sizes.reserve(measured.children.size());
            for (const MeasuredNodePtr& child : measured.children) {
                natural_sizes.push_back(child->measured_size);
            }
            const LinearLayoutResolution natural_lines = resolve_linear_layout(
                natural_sizes, horizontal, true, available_main, measured.style.gap
            );
            for (const LayoutLine& line : natural_lines.lines) {
                double line_fill_weight = 0.0;
                for (const std::size_t child_index : line.children) {
                    const LayoutSize& main_size = horizontal
                                                      ? measured.children[child_index]->style.width
                                                      : measured.children[child_index]->style.height;
                    if (main_size.kind == LayoutSize::Kind::fill) {
                        line_fill_weight += main_size.value > 0.0 ? main_size.value : 1.0;
                    }
                }
                if (line_fill_weight <= 0.0) continue;
                const double line_remaining = std::max(0.0, available_main - line.main_size);
                for (const std::size_t child_index : line.children) {
                    const LayoutSize& main_size = horizontal
                                                      ? measured.children[child_index]->style.width
                                                      : measured.children[child_index]->style.height;
                    if (main_size.kind != LayoutSize::Kind::fill) continue;
                    const double natural_main = horizontal
                                                    ? measured.children[child_index]->measured_size.width
                                                    : measured.children[child_index]->measured_size.height;
                    const double weight = main_size.value > 0.0 ? main_size.value : 1.0;
                    const double allocated = natural_main +
                                             line_remaining * weight / line_fill_weight;
                    Constraints child_constraints = content_constraints;
                    if (horizontal) {
                        child_constraints.min_width = allocated;
                        child_constraints.max_width = allocated;
                    } else {
                        child_constraints.min_height = allocated;
                        child_constraints.max_height = allocated;
                    }
                    measured.children[child_index] = measure(
                        *retained_children[child_index],
                        child_constraints,
                        environment,
                        operations
                    );
                }
            }
        }
        std::vector<Size> child_sizes;
        child_sizes.reserve(measured.children.size());
        for (const MeasuredNodePtr& child : measured.children) {
            child_sizes.push_back(child->measured_size);
        }
        measured.linear = resolve_linear_layout(
            child_sizes,
            horizontal,
            measured.style.wrap,
            available_main,
            measured.style.gap
        );
    } else if (measured.style.kind == LayoutKind::grid) {
        for (const RetainedNode* child : retained_children) {
            Constraints child_constraints = content_constraints;
            measured.children.push_back(measure(
                *child, child_constraints, environment, operations
            ));
        }
        const auto grid_metrics = [](const std::vector<MeasuredNodePtr>& children) {
            std::vector<GridItemMetrics> result;
            result.reserve(children.size());
            for (std::size_t index = 0U; index < children.size(); ++index) {
                const MeasuredNode& child = *children[index];
                result.push_back(GridItemMetrics{
                    index,
                    child.style.grid_column,
                    child.style.grid_row,
                    child.style.column_span,
                    child.style.row_span,
                    child.measured_size,
                });
            }
            return result;
        };
        GridLayoutResolution grid = resolve_grid_layout(
            measured.style, grid_metrics(measured.children)
        );
        const std::vector<double> columns = resolve_tracks(
            grid.columns, available_content_width, measured.style.gap.x
        );
        const std::vector<double> rows = resolve_tracks(
            grid.rows, available_content_height, measured.style.gap.y
        );
        std::vector<MeasuredNodePtr> constrained_children;
        constrained_children.reserve(retained_children.size());
        for (std::size_t index = 0U; index < retained_children.size(); ++index) {
            const auto placement = std::ranges::find(
                grid.placements, index, &GridPlacement::child_index
            );
            if (placement == grid.placements.end()) {
                constrained_children.push_back(std::move(measured.children[index]));
                continue;
            }
            const double cell_width = track_extent(
                columns, placement->column, placement->column_span, measured.style.gap.x
            );
            const double cell_height = track_extent(
                rows, placement->row, placement->row_span, measured.style.gap.y
            );
            const bool definite_height = grid_span_is_definite(
                grid.rows,
                placement->row,
                placement->row_span,
                available_content_height
            );
            const LayoutStyle& child_style = measured.children[index]->style;
            const LayoutAlign horizontal_alignment = child_style.justify_self.value_or(
                LayoutAlign::stretch
            );
            const LayoutAlign vertical_alignment = child_style.align_self.value_or(
                LayoutAlign::stretch
            );
            Constraints child_constraints = content_constraints;
            child_constraints.min_width = horizontal_alignment == LayoutAlign::stretch
                                              ? cell_width
                                              : 0.0;
            child_constraints.max_width = cell_width;
            child_constraints.min_height = definite_height &&
                                                   vertical_alignment == LayoutAlign::stretch
                                               ? cell_height
                                               : 0.0;
            child_constraints.max_height = definite_height
                                               ? cell_height
                                               : available_content_height;
            constrained_children.push_back(measure(
                *retained_children[index], child_constraints, environment, operations
            ));
        }
        measured.children = std::move(constrained_children);
        grid = resolve_grid_layout(measured.style, grid_metrics(measured.children));
        grid.intrinsic_size = resolved_grid_size(
            grid,
            available_content_width,
            available_content_height,
            measured.style.gap
        );
        measured.grid = std::move(grid);
    } else {
        for (const auto& child : retained_children) {
            Constraints child_constraints = content_constraints;
            if (measured.style.kind == LayoutKind::scroll) {
                const LayoutStyle child_style = resolved_style(*child);
                if (measured.style.scroll_horizontal &&
                    child_style.width.kind == LayoutSize::Kind::fill &&
                    std::isfinite(available_content_width)) {
                    child_constraints.min_width = available_content_width;
                }
                if (measured.style.scroll_vertical &&
                    child_style.height.kind == LayoutSize::Kind::fill &&
                    std::isfinite(available_content_height)) {
                    child_constraints.min_height = available_content_height;
                }
                if (measured.style.scroll_horizontal) child_constraints.max_width = infinity;
                if (measured.style.scroll_vertical) child_constraints.max_height = infinity;
            }
            measured.children.push_back(measure(*child, child_constraints, environment, operations));
        }
    }
    measured.subtree_pins_horizontal = measured.style.pin_horizontal ||
        std::ranges::any_of(measured.children, [](const MeasuredNodePtr& child) {
            return child->subtree_pins_horizontal;
        });
    measured.subtree_pins_vertical = measured.style.pin_vertical ||
        std::ranges::any_of(measured.children, [](const MeasuredNodePtr& child) {
            return child->subtree_pins_vertical;
        });

    const auto intrinsic_children = [
        &measured,
        available_content_width,
        available_content_height
    ](const bool attached_only) {
        if (measured.style.kind == LayoutKind::row ||
            measured.style.kind == LayoutKind::column) {
            if (!attached_only && measured.linear.has_value()) {
                return measured.linear->intrinsic_size;
            }
            std::vector<Size> child_sizes;
            child_sizes.reserve(measured.children.size());
            for (const MeasuredNodePtr& child : measured.children) {
                if (attached_only &&
                    child->node->lifecycle() == RetainedLifecycle::exiting) {
                    continue;
                }
                child_sizes.push_back(child->measured_size);
            }
            const bool horizontal = measured.style.kind == LayoutKind::row;
            return resolve_linear_layout(
                child_sizes,
                horizontal,
                measured.style.wrap,
                horizontal ? available_content_width : available_content_height,
                measured.style.gap
            ).intrinsic_size;
        }
        if (measured.style.kind == LayoutKind::grid && measured.grid.has_value()) {
            return measured.grid->intrinsic_size;
        }
        Size result;
        std::size_t count = 0U;
        for (const MeasuredNodePtr& child : measured.children) {
            if (attached_only && child->node->lifecycle() == RetainedLifecycle::exiting) continue;
            ++count;
            if (measured.style.kind == LayoutKind::scroll) {
                result.width = std::max(result.width, child->measured_size.width);
                result.height += child->measured_size.height;
            } else if (measured.style.kind != LayoutKind::spacer) {
                result.width = std::max(result.width, child->measured_size.width);
                result.height = std::max(result.height, child->measured_size.height);
            }
        }
        if (count != 0U) {
            if (measured.style.kind == LayoutKind::scroll) {
                result.height += measured.style.gap.y * static_cast<double>(count - 1U);
            }
        }
        return result;
    };
    const Size child_intrinsic = intrinsic_children(false);
    Size own_intrinsic = intrinsic_measure_(node, content_constraints);
    if (measured.style.intrinsic_size.has_value()) {
        own_intrinsic.width = std::max(own_intrinsic.width, measured.style.intrinsic_size->width);
        own_intrinsic.height = std::max(own_intrinsic.height, measured.style.intrinsic_size->height);
    }
    Size intrinsic{
        std::max(child_intrinsic.width, own_intrinsic.width),
        std::max(child_intrinsic.height, own_intrinsic.height),
    };
    measured.content_size = resolve_content_box(measured.style, intrinsic, constraints);
    if (std::optional<ContentSizeMotionSpec> motion = content_size_motion(node.description());
        motion.has_value()) {
        measured.content_motion_target_size = measured.content_size;
        const bool intrinsic_width = measured.style.width.kind == LayoutSize::Kind::automatic ||
                                     measured.style.width.kind == LayoutSize::Kind::content;
        const bool intrinsic_height = measured.style.height.kind == LayoutSize::Kind::automatic ||
                                      measured.style.height.kind == LayoutSize::Kind::content;
        std::vector<std::string> unsupported;
        if (motion->animate_width && !intrinsic_width) {
            unsupported.push_back("width=" + layout_size_description(measured.style.width));
        }
        if (motion->animate_height && !intrinsic_height) {
            unsupported.push_back("height=" + layout_size_description(measured.style.height));
        }
        if (!unsupported.empty()) {
            std::string joined;
            for (const std::string& axis : unsupported) {
                if (!joined.empty()) joined += ", ";
                joined += axis;
            }
            const std::string fingerprint = std::to_string(node.identity()) + ":" + joined;
            if (reported_diagnostics_.insert(fingerprint).second) {
                diagnostics_.push_back(runtime::RuntimeDiagnostic{
                    "STRATA.ANIMATION.CONTENT_SIZE_AXIS_FIXED",
                    "Content-size motion on " + node_description(node) + " cannot own " + joined +
                        " because those axes are externally sized; unsupported axes snap.",
                    {},
                    std::nullopt,
                    runtime::DiagnosticSeverity::warning,
                    std::nullopt,
                });
            }
        }
        motion->animate_width = motion->animate_width && intrinsic_width;
        motion->animate_height = motion->animate_height && intrinsic_height;
        if (!motion->animate_width && !motion->animate_height) {
            content_size_transitions_.remove(node.identity());
        } else {
            Size target = measured.content_size;
            const bool filters_exiting_children = measured.style.kind == LayoutKind::stack ||
                                                  measured.style.kind == LayoutKind::row ||
                                                  measured.style.kind == LayoutKind::column ||
                                                  measured.style.kind == LayoutKind::panel ||
                                                  measured.style.kind == LayoutKind::overlay ||
                                                  measured.style.kind == LayoutKind::portal;
            if (filters_exiting_children && std::ranges::any_of(
                    measured.children,
                    [](const MeasuredNodePtr& child) {
                        return child->node->lifecycle() == RetainedLifecycle::exiting;
                    }
                )) {
                const Size target_children = intrinsic_children(true);
                target = resolve_content_box(
                    measured.style,
                    Size{
                        std::max(target_children.width, own_intrinsic.width),
                        std::max(target_children.height, own_intrinsic.height),
                    },
                    constraints
                );
            }
            if (const std::optional<DisclosureMotionSpec> disclosure = disclosure_motion(node);
                disclosure.has_value() && !disclosure->expanded) {
                target.height = std::max(
                    0.0,
                    disclosure->collapsed_extent - frame_edges.vertical()
                );
            }
            const ContentSizeMotionSample sample = content_size_transitions_.retarget(
                node.identity(),
                target,
                environment.frame_time_nanos,
                *motion,
                theme_motion_reduced(node.description(), environment.reduced_motion)
            );
            measured.content_size = sample.size;
            measured.content_motion_clip = sample.clip;
            measured.content_motion_progress = sample.progress;
            measured.content_motion_running = sample.running;
            measured.content_motion_snapped_by_reduced_motion = sample.snapped_by_reduced_motion;
            measured.content_motion_target_size = sample.target;
        }
    } else {
        content_size_transitions_.remove(node.identity());
    }
    measured.measured_size = constraints.constrain(Size{
        measured.content_size.width + frame_edges.horizontal(),
        measured.content_size.height + frame_edges.vertical(),
    });
    MeasuredNodePtr result = std::make_shared<const MeasuredNode>(std::move(measured));
    measurement_cache_.insert_or_assign(node.identity(), MeasurementCacheEntry{
        constraints, node.revision(), environment.generation, environment.scale, result,
    });
    return result;
}
} // namespace strata::ui
