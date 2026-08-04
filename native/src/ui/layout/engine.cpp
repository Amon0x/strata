#include "ui/layout.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "ui/layout/detail_algorithms.hpp"
#include "ui/motion.hpp"

namespace strata::ui {
using namespace layout_detail;
namespace {

[[nodiscard]] const RetainedNode* anchored_node(
    const RetainedTree& tree,
    const RetainedNode& portal,
    const std::string_view target
) noexcept {
    if (target == "parent") return portal.parent();
    if (target == "root") return tree.root();
    return target.empty() ? nullptr : tree.find_key(target);
}

[[nodiscard]] const RetainedNode* ordinary_anchor_node(
    const RetainedTree& tree,
    const RetainedNode& anchored,
    const std::string_view target
) {
    const RetainedNode* candidate = nullptr;
    if (target == "parent") {
        candidate = anchored.parent();
    } else if (target == "root") {
        candidate = anchored.parent() == tree.root() ? tree.root() : nullptr;
    } else if (!target.empty()) {
        candidate = tree.find_key(target);
        if (candidate != nullptr && candidate->parent() != anchored.parent()) {
            candidate = nullptr;
        }
    }
    if (candidate == nullptr || candidate->lifecycle() != RetainedLifecycle::attached) {
        return nullptr;
    }
    const LayoutStyle candidate_style = layout_style(candidate->description());
    return candidate_style.participates &&
        candidate_style.kind != LayoutKind::portal
        ? candidate
        : nullptr;
}

[[nodiscard]] Rect anchor_placement(
    const Rect anchor,
    const Rect viewport,
    const Size size,
    const LayoutStyle& style
) noexcept {
    const auto aligned = [alignment = style.anchor_align](
                             const double start,
                             const double extent,
                             const double popup_extent
                         ) {
        if (alignment == LayoutAnchorAlign::center) {
            return start + (extent - popup_extent) * 0.5;
        }
        if (alignment == LayoutAnchorAlign::end) {
            return start + extent - popup_extent;
        }
        return start;
    };
    LayoutAnchorSide side = style.anchor_side;
    if (style.anchor_flip) {
        if (side == LayoutAnchorSide::bottom) {
            const double following = viewport.bottom() - anchor.bottom() - style.anchor_gap;
            const double preceding = anchor.y - viewport.y - style.anchor_gap;
            if (size.height > following && preceding > following) side = LayoutAnchorSide::top;
        } else if (side == LayoutAnchorSide::top) {
            const double preceding = anchor.y - viewport.y - style.anchor_gap;
            const double following = viewport.bottom() - anchor.bottom() - style.anchor_gap;
            if (size.height > preceding && following > preceding) side = LayoutAnchorSide::bottom;
        } else if (side == LayoutAnchorSide::right) {
            const double following = viewport.right() - anchor.right() - style.anchor_gap;
            const double preceding = anchor.x - viewport.x - style.anchor_gap;
            if (size.width > following && preceding > following) side = LayoutAnchorSide::left;
        } else {
            const double preceding = anchor.x - viewport.x - style.anchor_gap;
            const double following = viewport.right() - anchor.right() - style.anchor_gap;
            if (size.width > preceding && following > preceding) side = LayoutAnchorSide::right;
        }
    }

    double x = anchor.x;
    double y = anchor.y;
    if (side == LayoutAnchorSide::bottom || side == LayoutAnchorSide::top) {
        x = aligned(anchor.x, anchor.width, size.width);
        y = side == LayoutAnchorSide::bottom
            ? anchor.bottom() + style.anchor_gap
            : anchor.y - style.anchor_gap - size.height;
    } else {
        x = side == LayoutAnchorSide::right
            ? anchor.right() + style.anchor_gap
            : anchor.x - style.anchor_gap - size.width;
        y = aligned(anchor.y, anchor.height, size.height);
    }
    if (style.anchor_shift) {
        x = std::clamp(x, viewport.x, std::max(viewport.x, viewport.right() - size.width));
        y = std::clamp(y, viewport.y, std::max(viewport.y, viewport.bottom() - size.height));
    }
    return Rect{x, y, size.width, size.height};
}

} // namespace

LayoutEngine::LayoutEngine(IntrinsicMeasure intrinsic_measure)
    : intrinsic_measure_(intrinsic_measure ? std::move(intrinsic_measure) : IntrinsicMeasure(default_intrinsic)) {}

const LayoutResult& LayoutEngine::result() const noexcept { return result_; }
std::size_t LayoutEngine::active_transition_count() const noexcept {
    return content_size_transitions_.active_count();
}

std::uint64_t LayoutEngine::advance_render_generation() {
    if (next_render_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("layout render generation exhausted");
    }
    return ++next_render_generation_;
}

bool LayoutEngine::requires_layout(
    const RetainedTree& tree,
    const LayoutEnvironment& environment
) const {
    environment.validate();
    return last_tree_ != &tree ||
        last_invalidation_generation_ != tree.layout_invalidation_generation() ||
        !last_environment_.has_value() ||
        !stable_environment_equal(*last_environment_, environment) ||
        content_size_transitions_.active_count() != 0U;
}

std::vector<runtime::RuntimeDiagnostic> LayoutEngine::take_diagnostics() {
    std::vector<runtime::RuntimeDiagnostic> result = std::move(diagnostics_);
    diagnostics_.clear();
    return result;
}

void LayoutEngine::clear_diagnostics() noexcept {
    diagnostics_.clear();
    reported_diagnostics_.clear();
}

const LayoutResult& LayoutEngine::layout(
    RetainedTree& tree,
    const LayoutEnvironment& environment,
    const MotionRuntime* const motion,
    const bool consume_dirty
) {
    environment.validate();
    motion_ = motion;
    const RetainedNode* root = tree.root();
    if (root == nullptr) throw std::invalid_argument("layout requires a retained root");
    if (!requires_layout(tree, environment)) {
        result_.operations = {};
        return result_;
    }
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("layout generation exhausted");
    }
    LayoutResult next;
    next.generation = ++next_generation_;
    next.root_identity = root->identity();
    if (content_size_transitions_.active_count() != 0U) {
        std::set<std::uint64_t> transition_frontier;
        for (const std::uint64_t identity : content_size_transitions_.active_identities()) {
            for (const RetainedNode* node = tree.find_identity(identity);
                 node != nullptr;
                 node = node->parent()) {
                transition_frontier.insert(node->identity());
            }
        }
        for (const std::uint64_t identity : transition_frontier) {
            measurement_cache_.erase(identity);
        }
    }
    Rect root_bounds = environment.apply_safe_insets
                           ? environment.viewport.deflate(environment.safe_insets)
                           : environment.viewport;
    const MeasuredNodePtr measured = measure(
        *root,
        Constraints::fixed(root_bounds.width, root_bounds.height),
        environment,
        next.operations
    );
    pending_portals_.clear();
    pending_anchors_.clear();
    arrange(measured, root_bounds, std::nullopt, {}, environment, next);
    while (!pending_anchors_.empty()) {
        std::optional<std::size_t> ready;
        bool missing_target = false;
        for (std::size_t index = 0U; index < pending_anchors_.size(); ++index) {
            const PendingAnchor& pending = pending_anchors_[index];
            const RetainedNode* target = ordinary_anchor_node(
                tree,
                *pending.measured->node,
                pending.measured->style.anchor_target
            );
            if (target == nullptr) {
                ready = index;
                missing_target = true;
                break;
            }
            if (next.find(target->identity()) != nullptr) {
                ready = index;
                break;
            }
        }
        if (!ready.has_value()) {
            std::vector<PendingAnchor> unresolved = std::move(pending_anchors_);
            pending_anchors_.clear();
            for (PendingAnchor& pending : unresolved) {
                const std::string fingerprint =
                    pending.measured->node->description().source_path + "\n" +
                    pending.measured->style.anchor_target;
                if (reported_diagnostics_.insert(fingerprint).second) {
                    diagnostics_.push_back(runtime::RuntimeDiagnostic{
                        "STRATA.UI.LAYOUT_ANCHOR_CYCLE",
                        "Sibling anchor '" + pending.measured->style.anchor_target +
                            "' cannot be resolved because its anchor chain is cyclic.",
                        pending.measured->node->description().source_path,
                        std::string("an already arranged sibling"),
                        runtime::DiagnosticSeverity::error,
                        std::nullopt,
                    });
                }
                arrange(
                    pending.measured,
                    pending.fallback_bounds,
                    pending.inherited_clip,
                    pending.pin_context,
                    environment,
                    next
                );
            }
            continue;
        }
        PendingAnchor pending = std::move(pending_anchors_[*ready]);
        pending_anchors_.erase(
            pending_anchors_.begin() + static_cast<std::ptrdiff_t>(*ready)
        );
        const LayoutStyle& style = pending.measured->style;
        const RetainedNode* target = ordinary_anchor_node(
            tree,
            *pending.measured->node,
            style.anchor_target
        );
        const LayoutRecord* anchor_record = target != nullptr
            ? next.find(target->identity())
            : nullptr;
        if (missing_target || anchor_record == nullptr) {
            const std::string fingerprint =
                pending.measured->node->description().source_path + "\n" +
                style.anchor_target;
            if (reported_diagnostics_.insert(fingerprint).second) {
                diagnostics_.push_back(runtime::RuntimeDiagnostic{
                    "STRATA.UI.LAYOUT_ANCHOR_MISSING",
                    "Sibling anchor '" + style.anchor_target +
                        "' is not an attached non-portal sibling.",
                    pending.measured->node->description().source_path,
                    std::string("an attached non-portal sibling key or parent"),
                    runtime::DiagnosticSeverity::error,
                    std::nullopt,
                });
            }
            arrange(
                pending.measured,
                pending.fallback_bounds,
                pending.inherited_clip,
                pending.pin_context,
                environment,
                next
            );
            continue;
        }
        if (style.match_anchor_width &&
            anchor_record->bounds.width != pending.measured->measured_size.width) {
            pending.measured = measure(
                *pending.measured->node,
                Constraints{
                    anchor_record->bounds.width,
                    anchor_record->bounds.width,
                    0.0,
                    pending.containing_bounds.height,
                },
                environment,
                next.operations
            );
        }
        arrange(
            pending.measured,
            anchor_placement(
                anchor_record->bounds,
                pending.containing_bounds,
                pending.measured->measured_size,
                style
            ),
            pending.inherited_clip,
            pending.pin_context,
            environment,
            next
        );
    }
    for (std::size_t index = 0U; index < pending_portals_.size(); ++index) {
        PendingPortal pending = pending_portals_[index];
        const LayoutStyle& style = pending.measured->style;
        const RetainedNode* anchor_node = anchored_node(
            tree,
            *pending.measured->node,
            style.anchor_target
        );
        const LayoutRecord* anchor_record = anchor_node != nullptr
            ? next.find(anchor_node->identity())
            : nullptr;
        const std::optional<Rect> point_anchor = style.anchor_point.has_value()
            ? std::optional<Rect>(Rect{
                  style.anchor_point->x,
                  style.anchor_point->y,
                  0.0,
                  0.0,
              })
            : std::nullopt;
        const RetainedNode* parent = pending.measured->node->parent();
        const LayoutRecord* parent_record = parent != nullptr
            ? next.find(parent->identity())
            : nullptr;
        const LayoutRecord* root_record = next.find(next.root_identity);
        const Rect viewport = root_record != nullptr ? root_record->bounds : root_bounds;
        if (pending.measured->measured_size.width > viewport.width ||
            pending.measured->measured_size.height > viewport.height) {
            pending.measured = measure(
                *pending.measured->node,
                Constraints{0.0, viewport.width, 0.0, viewport.height},
                environment,
                next.operations
            );
        }
        Rect portal_bounds{
            parent_record != nullptr ? parent_record->content_bounds.x : viewport.x,
            parent_record != nullptr ? parent_record->content_bounds.y : viewport.y,
            pending.measured->measured_size.width,
            pending.measured->measured_size.height,
        };
        if (anchor_record != nullptr || point_anchor.has_value()) {
            if (style.match_anchor_width &&
                anchor_record != nullptr &&
                anchor_record->bounds.width != pending.measured->measured_size.width) {
                pending.measured = measure(
                    *pending.measured->node,
                    Constraints{
                        anchor_record->bounds.width,
                        anchor_record->bounds.width,
                        0.0,
                        viewport.height,
                    },
                    environment,
                    next.operations
                );
            }
            const Rect anchor_bounds = point_anchor.has_value()
                ? *point_anchor
                : anchor_record->bounds;
            portal_bounds = anchor_placement(
                anchor_bounds,
                viewport,
                pending.measured->measured_size,
                style
            );
        }
        arrange(
            pending.measured,
            portal_bounds,
            pending.inherited_clip,
            pending.pin_context,
            environment,
            next
        );
    }
    pending_portals_.clear();
    const auto retain_exit_layout = [&](const auto& self, const RetainedNode& node) -> void {
        if (node.lifecycle() == RetainedLifecycle::exiting) {
            const auto retain_subtree = [&](const auto& retain, const RetainedNode& exiting) -> void {
                if (const LayoutRecord* previous = result_.find(exiting.identity());
                    previous != nullptr) {
                    LayoutRecord record = *previous;
                    record.generation = next.generation;
                    record.translated_subtree.reset();
                    next.records.insert_or_assign(exiting.identity(), std::move(record));
                }
                for (const auto& child : exiting.children()) retain(retain, *child);
            };
            retain_subtree(retain_subtree, node);
            return;
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    retain_exit_layout(retain_exit_layout, *root);
    const auto record_unarranged = [&](const auto& self, const RetainedNode& node) -> void {
        if (!next.records.contains(node.identity())) {
            const LayoutStyle style = resolved_style(node);
            LayoutRecord record;
            record.identity = node.identity();
            record.generation = next.generation;
            record.render_generation = advance_render_generation();
            record.kind = style.kind;
            record.scroll_horizontal = style.scroll_horizontal;
            record.scroll_vertical = style.scroll_vertical;
            record.pin_horizontal = style.pin_horizontal;
            record.pin_vertical = style.pin_vertical;
            if (style.virtual_list.has_value()) {
                record.virtual_axis = style.virtual_list->axis;
                record.virtual_overscan = style.virtual_list->overscan;
            }
            record.portal_target = style.kind == LayoutKind::portal ? style.portal_target : std::string{};
            record.detached_from_parent_clip = style.kind == LayoutKind::portal && style.detach_from_parent_clip;
            next.records.emplace(node.identity(), std::move(record));
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    record_unarranged(record_unarranged, *root);
    const auto finalize_subtree_render_generation =
        [this, &next](const auto& self, const RetainedNode& node) -> void {
        for (const auto& child : node.children()) self(self, *child);
        auto current = next.records.find(node.identity());
        if (current == next.records.end()) return;
        const LayoutRecord* previous = result_.find(node.identity());
        bool unchanged = previous != nullptr &&
            previous->render_generation == current->second.render_generation;
        for (const auto& child : node.children()) {
            const LayoutRecord* current_child = next.find(child->identity());
            const LayoutRecord* previous_child = result_.find(child->identity());
            if (current_child == nullptr || previous_child == nullptr ||
                current_child->subtree_render_generation !=
                    previous_child->subtree_render_generation) {
                unchanged = false;
                break;
            }
        }
        current->second.subtree_render_generation = unchanged
            ? previous->subtree_render_generation
            : advance_render_generation();
    };
    finalize_subtree_render_generation(
        finalize_subtree_render_generation,
        *root
    );
    for (const collection::VirtualAnchorUpdate& update :
         collection_virtualization_.take_anchor_updates()) {
        static_cast<void>(tree.set_arrangement_value(
            update.identity,
            "strata.scroll.offset",
            runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                {"x", runtime::Value(update.x)},
                {"y", runtime::Value(update.y)},
            })
        ));
    }
    if (consume_dirty) tree.consume_layout_dirty();
    std::erase_if(measurement_cache_, [&next](const auto& entry) {
        return !next.records.contains(entry.first);
    });
    std::erase_if(arrangement_cache_, [&next](const auto& entry) {
        return !next.records.contains(entry.first);
    });
    std::set<std::uint64_t> retained_identities;
    for (const auto& [identity, record] : next.records) {
        static_cast<void>(record);
        retained_identities.insert(identity);
    }
    collection_virtualization_.retain(retained_identities);
    content_size_transitions_.retain(next.records);
    result_ = std::move(next);
    last_tree_ = &tree;
    last_invalidation_generation_ = tree.layout_invalidation_generation();
    last_environment_ = environment;
    return result_;
}

LayoutStyle LayoutEngine::resolved_style(const RetainedNode& node) const {
    LayoutStyle style = layout_style(node.description());
    style.scroll_offset = resolved_scroll_offset(node, style.scroll_offset);
    if (const runtime::Value* retained = node.retained_value("strata.gesture.runtimeSize");
        retained != nullptr && retained->object() != nullptr) {
        const runtime::Value* width = retained->field("width");
        const runtime::Value* height = retained->field("height");
        if (width != nullptr && width->number() != nullptr && std::isfinite(*width->number())) {
            style.width = LayoutSize{LayoutSize::Kind::fixed, std::max(0.0, *width->number())};
        }
        if (height != nullptr && height->number() != nullptr && std::isfinite(*height->number())) {
            style.height = LayoutSize{LayoutSize::Kind::fixed, std::max(0.0, *height->number())};
        }
    }
    const MotionComputedValues* values = motion_ != nullptr
                                             ? motion_->computed_values(node.identity())
                                             : nullptr;
    if (values == nullptr) return style;
    const auto animated_size = [values](
                                   const MotionProperty property,
                                   LayoutSize& size
                               ) {
        if (const MotionLayoutValue* value = values->layout(property); value != nullptr) {
            const LayoutSize::Kind kind = value->unit == MotionLayoutUnit::percent
                ? LayoutSize::Kind::percent
                : value->unit == MotionLayoutUnit::fill
                    ? LayoutSize::Kind::fill
                    : LayoutSize::Kind::fixed;
            size = LayoutSize{kind, std::max(0.0, value->value)};
            return;
        }
        if (const auto value = values->number(property); value.has_value()) {
            size = LayoutSize{LayoutSize::Kind::fixed, std::max(0.0, *value)};
        }
    };
    animated_size(MotionProperty::width, style.width);
    animated_size(MotionProperty::height, style.height);
    const auto optional_fixed = [values](
                                    const MotionProperty property,
                                    std::optional<LayoutSize>& size
                                ) {
        if (const MotionLayoutValue* value = values->layout(property); value != nullptr) {
            const LayoutSize::Kind kind = value->unit == MotionLayoutUnit::percent
                ? LayoutSize::Kind::percent
                : value->unit == MotionLayoutUnit::fill
                    ? LayoutSize::Kind::fill
                    : LayoutSize::Kind::fixed;
            size = LayoutSize{kind, std::max(0.0, value->value)};
            return;
        }
        if (const auto value = values->number(property); value.has_value()) {
            size = LayoutSize{LayoutSize::Kind::fixed, std::max(0.0, *value)};
        }
    };
    optional_fixed(MotionProperty::min_width, style.min_width);
    optional_fixed(MotionProperty::min_height, style.min_height);
    optional_fixed(MotionProperty::max_width, style.max_width);
    optional_fixed(MotionProperty::max_height, style.max_height);
    const auto edge = [values](const MotionProperty property, double& destination) {
        if (const auto value = values->number(property); value.has_value()) {
            destination = std::max(0.0, *value);
        }
    };
    edge(MotionProperty::margin_left, style.margin.left);
    edge(MotionProperty::margin_top, style.margin.top);
    edge(MotionProperty::margin_right, style.margin.right);
    edge(MotionProperty::margin_bottom, style.margin.bottom);
    edge(MotionProperty::padding_left, style.padding.left);
    edge(MotionProperty::padding_top, style.padding.top);
    edge(MotionProperty::padding_right, style.padding.right);
    edge(MotionProperty::padding_bottom, style.padding.bottom);
    const auto placement = [values](
                               const MotionProperty property,
                               std::optional<LayoutSize>& destination
                           ) {
        if (const MotionLayoutValue* value = values->layout(property); value != nullptr) {
            const LayoutSize::Kind kind = value->unit == MotionLayoutUnit::percent
                ? LayoutSize::Kind::percent
                : value->unit == MotionLayoutUnit::fill
                    ? LayoutSize::Kind::fill
                    : LayoutSize::Kind::fixed;
            destination = LayoutSize{kind, std::max(0.0, value->value)};
        } else if (const auto sample = values->number(property); sample.has_value()) {
            destination = LayoutSize{
                LayoutSize::Kind::fixed,
                std::max(0.0, *sample),
            };
        }
    };
    if (style.placement.has_value() ||
        values->find(MotionProperty::placement_x) != nullptr ||
        values->find(MotionProperty::placement_y) != nullptr) {
        if (!style.placement.has_value()) style.placement.emplace();
        placement(MotionProperty::placement_x, style.placement->x);
        placement(MotionProperty::placement_y, style.placement->y);
    }
    return style;
}

Point LayoutEngine::resolved_scroll_offset(
    const RetainedNode& node,
    Point fallback
) noexcept {
    if (const runtime::Value* retained = node.retained_value("strata.scroll.offset");
        retained != nullptr && retained->object() != nullptr) {
        const runtime::Value* x = retained->field("x");
        const runtime::Value* y = retained->field("y");
        if (x != nullptr && x->number() != nullptr && std::isfinite(*x->number())) {
            fallback.x = *x->number();
        }
        if (y != nullptr && y->number() != nullptr && std::isfinite(*y->number())) {
            fallback.y = *y->number();
        }
    }
    return fallback;
}

void LayoutEngine::clear() {
    measurement_cache_.clear();
    arrangement_cache_.clear();
    collection_virtualization_.clear();
    result_ = {};
    last_tree_ = nullptr;
    last_invalidation_generation_ = 0U;
    last_environment_.reset();
    next_render_generation_ = 0U;
    content_size_transitions_.clear();
    motion_ = nullptr;
    diagnostics_.clear();
    reported_diagnostics_.clear();
}
} // namespace strata::ui
