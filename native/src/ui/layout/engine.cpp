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
    arrange(measured, root_bounds, std::nullopt, {}, environment, next);
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
    const auto fixed = [values](const MotionProperty property, LayoutSize& size) {
        if (const auto value = values->number(property); value.has_value()) {
            size = LayoutSize{LayoutSize::Kind::fixed, std::max(0.0, *value)};
        }
    };
    fixed(MotionProperty::width, style.width);
    fixed(MotionProperty::height, style.height);
    const auto optional_fixed = [values](
                                    const MotionProperty property,
                                    std::optional<LayoutSize>& size
                                ) {
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
