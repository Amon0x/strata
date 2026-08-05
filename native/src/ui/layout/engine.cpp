#include "ui/layout.hpp"

#include <algorithm>
#include <chrono>
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
    const bool consume_dirty,
    const std::uint64_t frame_index
) {
    environment.validate();
    motion_ = motion;
    const RetainedNode* root = tree.root();
    if (root == nullptr) throw std::invalid_argument("layout requires a retained root");
    if (frame_index == 0U || translation_frame_index_ != frame_index) {
        for (const std::uint64_t identity : translated_records_) {
            if (auto record = result_.records.find(identity);
                record != result_.records.end()) {
                record->second.translated_subtree.reset();
            }
        }
        translated_records_.clear();
        translation_frame_index_ = frame_index;
    }
    if (!requires_layout(tree, environment)) {
        result_.operations = {};
        result_.measure_nanos = 0;
        result_.arrange_nanos = 0;
        result_.maintenance_nanos = 0;
        return result_;
    }
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("layout generation exhausted");
    }
    current_arranged_records_.clear();
    current_arranged_subtree_roots_.clear();
    measurement_pass_.clear();
    measurement_frontier_.clear();
    structural_measurements_.clear();
    result_.generation = ++next_generation_;
    result_.root_identity = root->identity();
    result_.operations = {};
    result_.measure_nanos = 0;
    result_.arrange_nanos = 0;
    result_.maintenance_nanos = 0;
    LayoutResult& next = result_;
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
    const auto measure_started = std::chrono::steady_clock::now();
    // Measure dirty branches bottom-up. Shared ancestors enter the queue once after all deeper
    // siblings, avoiding both a full-root scan for one local edit and the old O(dirty * depth)
    // behavior which repeatedly measured the same repeater parents.
    std::map<
        std::pair<std::size_t, std::uint64_t>,
        const RetainedNode*,
        std::greater<>
    > pending_measurements;
    std::unordered_set<std::uint64_t> scheduled_measurements;
    const auto schedule_measurement =
        [&pending_measurements, &scheduled_measurements](const RetainedNode* node) {
            if (node == nullptr ||
                !scheduled_measurements.insert(node->identity()).second) {
                return;
            }
            std::size_t depth = 0U;
            for (const RetainedNode* current = node;
                 current != nullptr;
                 current = current->parent()) {
                ++depth;
            }
            pending_measurements.emplace(
                std::pair(depth, node->identity()),
                node
            );
        };
    for (RetainedNode* dirty : tree.dirty_nodes()) {
        const DirtySet& reasons = dirty->dirty();
        if (!reasons.contains(DirtyReason::structure) &&
            !reasons.contains(DirtyReason::layout) &&
            !reasons.contains(DirtyReason::text) &&
            !reasons.contains(DirtyReason::style) &&
            !reasons.contains(DirtyReason::scale) &&
            !reasons.contains(DirtyReason::resource) &&
            !reasons.contains(DirtyReason::editor)) {
            continue;
        }
        const RetainedNode* measurable = dirty;
        while (measurable != root &&
               !measurement_cache_.contains(measurable->identity())) {
            measurable = measurable->parent();
        }
        schedule_measurement(measurable);
    }
    if (measurement_cache_.empty() ||
        content_size_transitions_.active_count() != 0U) {
        schedule_measurement(root);
    }
    MeasuredNodePtr measured;
    while (!pending_measurements.empty()) {
        const auto pending = pending_measurements.begin();
        const RetainedNode* const current = pending->second;
        pending_measurements.erase(pending);
        const auto cached = measurement_cache_.find(current->identity());
        const Constraints constraints = current == root
            ? Constraints::fixed(root_bounds.width, root_bounds.height)
            : cached != measurement_cache_.end()
                ? cached->second.constraints
                : Constraints{};
        if (current != root && cached == measurement_cache_.end()) {
            schedule_measurement(current->parent());
            continue;
        }
        MeasuredNodePtr current_measured = measure(
            *current,
            constraints,
            environment,
            next.operations
        );
        if (current == root) measured = std::move(current_measured);
        if (!measurement_frontier_.contains(current->identity())) {
            schedule_measurement(current->parent());
        }
    }
    if (measured == nullptr) {
        const auto cached_root = measurement_cache_.find(root->identity());
        if (cached_root == measurement_cache_.end() ||
            cached_root->second.measured == nullptr) {
            measured = measure(
                *root,
                Constraints::fixed(root_bounds.width, root_bounds.height),
                environment,
                next.operations
            );
        } else {
            measured = cached_root->second.propagated != nullptr
                ? cached_root->second.propagated
                : cached_root->second.measured;
        }
    }
    if (!measurement_frontier_.empty()) {
        // A frontier deliberately propagates its old identity to stop parent layout work. Keep the
        // ancestors' canonical cached model current nevertheless, so a later resize/reflow cannot
        // resurrect stale descendants. Patch each shared ancestor once, deepest first.
        std::map<std::uint64_t, MeasuredNodePtr> replacements = measurement_frontier_;
        struct AffectedAncestor final {
            RetainedNode* node = nullptr;
            std::size_t depth = 0U;
        };
        std::map<std::uint64_t, AffectedAncestor> affected;
        for (const auto& [identity, frontier] : measurement_frontier_) {
            static_cast<void>(identity);
            std::size_t depth = 0U;
            for (const RetainedNode* current = frontier->node;
                 current != nullptr;
                 current = current->parent()) {
                ++depth;
            }
            for (RetainedNode* ancestor = frontier->node->parent();
                 ancestor != nullptr;
                 ancestor = ancestor->parent()) {
                --depth;
                affected.insert_or_assign(
                    ancestor->identity(),
                    AffectedAncestor{ancestor, depth}
                );
            }
        }
        std::vector<AffectedAncestor> ordered;
        ordered.reserve(affected.size());
        for (const auto& [identity, ancestor] : affected) {
            static_cast<void>(identity);
            ordered.push_back(ancestor);
        }
        std::ranges::sort(
            ordered,
            std::greater{},
            &AffectedAncestor::depth
        );
        for (const AffectedAncestor& ancestor : ordered) {
            auto cached = measurement_cache_.find(ancestor.node->identity());
            if (cached == measurement_cache_.end() ||
                cached->second.measured == nullptr) {
                continue;
            }
            MeasuredNode updated = *cached->second.measured;
            bool changed = false;
            for (MeasuredNodePtr& child : updated.children) {
                const auto replacement = replacements.find(child->node->identity());
                if (replacement == replacements.end() || child == replacement->second) {
                    continue;
                }
                child = replacement->second;
                changed = true;
            }
            if (!changed) continue;
            cached->second.measured =
                std::make_shared<const MeasuredNode>(std::move(updated));
            replacements.insert_or_assign(
                ancestor.node->identity(),
                cached->second.measured
            );
            if (auto frontier = measurement_frontier_.find(ancestor.node->identity());
                frontier != measurement_frontier_.end()) {
                frontier->second = cached->second.measured;
            }
            cached->second.node_revision = ancestor.node->revision();
        }
        std::erase_if(measurement_frontier_, [this](const auto& entry) {
            for (const RetainedNode* ancestor = entry.second->node->parent();
                 ancestor != nullptr;
                 ancestor = ancestor->parent()) {
                if (measurement_frontier_.contains(ancestor->identity())) return true;
            }
            return false;
        });
    }
    next.measure_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - measure_started
    ).count();
    const auto arrange_started = std::chrono::steady_clock::now();
    pending_portals_.clear();
    pending_anchors_.clear();
    const bool has_measurement_frontiers = !measurement_frontier_.empty();
    if (const auto root_frontier = measurement_frontier_.find(root->identity());
        root_frontier != measurement_frontier_.end()) {
        arrange(root_frontier->second, root_bounds, std::nullopt, {}, environment, next);
        measurement_frontier_.erase(root_frontier);
    } else if (!has_measurement_frontiers) {
        const auto current_root = measurement_cache_.find(root->identity());
        arrange(
            current_root != measurement_cache_.end() &&
                    current_root->second.measured != nullptr
                ? current_root->second.measured
                : measured,
            root_bounds,
            std::nullopt,
            {},
            environment,
            next
        );
    } else {
        arrange(measured, root_bounds, std::nullopt, {}, environment, next);
    }
    for (const auto& [identity, frontier] : measurement_frontier_) {
        const auto retained = arrangement_cache_.find(identity);
        if (retained == arrangement_cache_.end()) {
            throw std::logic_error(
                "incremental measurement frontier lost its arrangement cache"
            );
        }
        arrange(
            frontier,
            retained->second.bounds,
            retained->second.inherited_clip,
            retained->second.pin_context,
            environment,
            next
        );
    }
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
            if (arranged_in_current_pass(*target)) {
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
        const LayoutRecord* anchor_record = target != nullptr &&
                arranged_in_current_pass(*target)
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
        const LayoutRecord* anchor_record = anchor_node != nullptr &&
                arranged_in_current_pass(*anchor_node)
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
            if (next.records.contains(node.identity())) {
                current_arranged_subtree_roots_.insert(node.identity());
            }
            return;
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    retain_exit_layout(retain_exit_layout, *root);
    const auto record_unarranged = [&](const auto& self, const RetainedNode& node) -> void {
        if (!arranged_in_current_pass(node)) {
            const LayoutStyle style = resolved_style(node);
            LayoutRecord record;
            record.identity = node.identity();
            record.generation = next.generation;
            record.render_generation = advance_render_generation();
            record.subtree_render_generation = advance_render_generation();
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
            next.records.insert_or_assign(node.identity(), std::move(record));
            current_arranged_records_.insert(node.identity());
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    record_unarranged(record_unarranged, *root);
    next.arrange_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - arrange_started
    ).count();
    const auto maintenance_started = std::chrono::steady_clock::now();
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
    std::erase_if(next.records, [&tree](const auto& entry) {
        return tree.find_identity(entry.first) == nullptr;
    });
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
    next.maintenance_nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - maintenance_started
    ).count();
    last_tree_ = &tree;
    last_invalidation_generation_ = tree.layout_invalidation_generation();
    last_environment_ = environment;
    return result_;
}

bool LayoutEngine::arranged_in_current_pass(const RetainedNode& node) const noexcept {
    if (current_arranged_records_.contains(node.identity())) return true;
    for (const RetainedNode* ancestor = &node;
         ancestor != nullptr;
         ancestor = ancestor->parent()) {
        if (current_arranged_subtree_roots_.contains(ancestor->identity())) return true;
    }
    return false;
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
    measurement_frontier_.clear();
    measurement_pass_.clear();
    structural_measurements_.clear();
    arrangement_cache_.clear();
    current_arranged_records_.clear();
    current_arranged_subtree_roots_.clear();
    translated_records_.clear();
    translation_frame_index_ = 0U;
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
