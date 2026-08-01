#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "ui/render.hpp"
#include "ui/motion/model.hpp"

namespace strata::ui {

/** Private fragment cache and its complete layout dependency snapshot. */
struct RenderEngine::Impl final {
    struct ChildLayoutSnapshot final {
        std::optional<std::string> key;
        std::string type;
        Rect bounds;

        [[nodiscard]] friend bool operator==(
            const ChildLayoutSnapshot&,
            const ChildLayoutSnapshot&
        ) = default;
    };

    // Fragment producers may consume both their own layout slot and the arranged slots of
    // immediate children. Keep that complete dependency beside the fragment instead of
    // invalidating the entire surface whenever any layout work occurred.
    struct LayoutSnapshot final {
        Size measured_size;
        Rect bounds;
        Rect snapped_bounds;
        Rect hit_bounds;
        Rect content_bounds;
        std::optional<Rect> clip;
        std::optional<Rect> local_clip;
        std::optional<Rect> scroll_frame;
        std::optional<Rect> viewport;
        Size content_size;
        Point requested_scroll_offset;
        Point scroll_offset;
        std::optional<VisibleRange> visible_range;
        int z_index = 0;
        std::vector<std::uint64_t> arranged_child_order;
        std::vector<std::size_t> materialized_child_indices;
        std::string portal_target;
        bool detached_from_parent_clip = false;
        std::vector<ChildLayoutSnapshot> children;

        [[nodiscard]] friend bool operator==(const LayoutSnapshot&, const LayoutSnapshot&) = default;
    };

    struct CachedFragment final {
        struct MotionSnapshot final {
            std::vector<std::pair<MotionProperty, MotionValue>> values;
            [[nodiscard]] friend bool operator==(
                const MotionSnapshot&,
                const MotionSnapshot&
            ) = default;
        };

        struct CompositionPlan final {
            RenderGenerationToken generations;
            std::uint64_t node_render_generation = 0U;
            std::uint64_t node_presentation_generation = 0U;
            std::uint64_t layout_render_generation = 0U;
            std::uint64_t external_generation = 0U;
            bool depends_on_status_feedback = false;
            bool focused = false;
            bool focus_visible = false;
            bool hovered = false;
            bool active = false;
            std::optional<Rect> inherited_render_clip;
            MotionTransform inherited_transform;
            double inherited_opacity = 1.0;
            bool render_portals = false;
            std::vector<RenderCommand> prefix;
            std::vector<RenderCommand> suffix;
            std::vector<const RetainedNode*> ordered_children;
            std::vector<std::uint64_t> ordered_child_layout_generations;
            std::optional<Rect> child_render_clip;
            MotionTransform effective_transform;
            double descendant_opacity = 1.0;
            bool local_overlay_rendered = false;
            std::uint64_t subtree_node_render_generation = 0U;
            std::uint64_t subtree_presentation_generation = 0U;
            std::uint64_t subtree_layout_render_generation = 0U;
            std::uint64_t subtree_status_generation = 0U;
            std::optional<std::vector<RenderCommand>> subtree_commands;
            std::size_t subtree_node_count = 1U;
            std::size_t subtree_overlay_count = 0U;
            LayoutSnapshot subtree_layout;
            Point subtree_translation;
            bool subtree_translation_safe = false;
            bool subtree_contains_clip = false;
        };

        /**
         * Presentation emitted after child composition has a different lifetime from widget
         * content. Scrolling commonly translates a large retained subtree while only its
         * collection root changes scrollbars or selection chrome. Retain these layers per node
         * instead of rebuilding every foreground and non-detached overlay during that traversal.
         */
        struct PresentationPlan final {
            RenderGenerationToken generations;
            DirtyGenerationSnapshot node_generations;
            MotionSnapshot motion;
            std::uint64_t node_presentation_generation = 0U;
            std::uint64_t external_generation = 0U;
            bool focused = false;
            bool focus_visible = false;
            bool hovered = false;
            bool active = false;
            double inherited_opacity = 1.0;
            LayoutSnapshot layout;
            std::vector<RenderCommand> commands;
            bool local_overlay_rendered = false;
        };

        RenderGenerationToken generations;
        DirtyGenerationSnapshot node_generations;
        MotionSnapshot motion;
        std::uint64_t external_generation = 0U;
        bool focused = false;
        bool focus_visible = false;
        bool hovered = false;
        bool active = false;
        LayoutSnapshot layout;
        std::vector<RenderCommand> commands;
        Point presentation_translation;
        bool visited = false;
        std::optional<PresentationPlan> presentation;
        std::optional<CompositionPlan> composition;
    };

    std::unordered_map<std::uint64_t, CachedFragment> fragments;
    std::uint64_t fragment_tree_generation = 0U;
    RenderCommandBuffer retained_base_commands;
    std::optional<RenderGenerationToken> retained_base_generations;
    std::uint64_t retained_tree_generation = 0U;
    std::uint64_t retained_layout_generation = 0U;
    std::uint64_t retained_presentation_generation = 0U;
    std::uint64_t retained_status_generation = 0U;

    [[nodiscard]] bool base_matches(
        const RetainedTree& tree,
        const LayoutResult& layout,
        const InputRouter& input,
        const MotionRuntime& motion,
        const RenderGenerationToken& generations
    ) const noexcept;

    void retain_base(
        const RetainedTree& tree,
        const LayoutResult& layout,
        const InputRouter& input,
        const RenderGenerationToken& generations,
        const RenderCommandBuffer& commands
    );

    [[nodiscard]] static bool fragment_generations_match(
        const DirtyGenerationSnapshot& retained,
        const DirtyGenerationSnapshot& current
    ) noexcept {
        return retained.structure == current.structure &&
            retained.properties == current.properties && retained.text == current.text &&
            retained.style == current.style && retained.input == current.input &&
            retained.scale == current.scale && retained.resource == current.resource &&
            retained.editor == current.editor;
    }

    [[nodiscard]] static CachedFragment::MotionSnapshot fragment_motion_snapshot(
        const MotionComputedValues* computed
    ) {
        CachedFragment::MotionSnapshot result;
        if (computed == nullptr) return result;
        for (const auto& [property, value] : computed->values) {
            if (property >= MotionProperty::x && property <= MotionProperty::scale_y) continue;
            result.values.emplace_back(property, value);
        }
        return result;
    }

    [[nodiscard]] static bool matches(
        const LayoutSnapshot& snapshot,
        const RetainedNode& node,
        const LayoutRecord& record,
        const LayoutResult& layout
    ) {
        if (snapshot.measured_size != record.measured_size ||
            snapshot.bounds != record.bounds ||
            snapshot.snapped_bounds != record.snapped_bounds ||
            snapshot.hit_bounds != record.hit_bounds ||
            snapshot.content_bounds != record.content_bounds ||
            snapshot.clip != record.clip || snapshot.local_clip != record.local_clip ||
            snapshot.scroll_frame != record.scroll_frame || snapshot.viewport != record.viewport ||
            snapshot.content_size != record.content_size ||
            snapshot.requested_scroll_offset != record.requested_scroll_offset ||
            snapshot.scroll_offset != record.scroll_offset ||
            snapshot.visible_range != record.visible_range || snapshot.z_index != record.z_index ||
            snapshot.arranged_child_order != record.arranged_child_order ||
            snapshot.materialized_child_indices != record.materialized_child_indices ||
            snapshot.portal_target != record.portal_target ||
            snapshot.detached_from_parent_clip != record.detached_from_parent_clip) {
            return false;
        }
        std::size_t child_index = 0U;
        for (const auto& child : node.children()) {
            const LayoutRecord* child_layout = layout.find(child->identity());
            if (child_layout == nullptr) continue;
            if (child_index >= snapshot.children.size()) return false;
            const ChildLayoutSnapshot& retained = snapshot.children[child_index++];
            if (retained.key != child->description().key ||
                retained.type != child->description().type ||
                retained.bounds != child_layout->bounds) {
                return false;
            }
        }
        return child_index == snapshot.children.size();
    }

    [[nodiscard]] static std::optional<Point> uniform_translation(
        const LayoutSnapshot& snapshot,
        const RetainedNode& node,
        const LayoutRecord& record,
        const LayoutResult& layout
    ) {
        const Point delta{
            record.bounds.x - snapshot.bounds.x,
            record.bounds.y - snapshot.bounds.y,
        };
        if (delta == Point{} || snapshot.measured_size != record.measured_size ||
            snapshot.content_size != record.content_size ||
            snapshot.requested_scroll_offset != record.requested_scroll_offset ||
            snapshot.scroll_offset != record.scroll_offset ||
            snapshot.visible_range != record.visible_range || snapshot.z_index != record.z_index ||
            snapshot.arranged_child_order != record.arranged_child_order ||
            snapshot.materialized_child_indices != record.materialized_child_indices ||
            snapshot.portal_target != record.portal_target ||
            snapshot.detached_from_parent_clip != record.detached_from_parent_clip) {
            return std::nullopt;
        }
        const auto translated = [delta](const Rect& retained, const Rect& current) {
            constexpr double coordinate_tolerance = 1.0e-9;
            return retained.width == current.width && retained.height == current.height &&
                std::abs(retained.x + delta.x - current.x) <= coordinate_tolerance &&
                std::abs(retained.y + delta.y - current.y) <= coordinate_tolerance;
        };
        const auto translated_optional = [&translated](
                                             const std::optional<Rect>& retained,
                                             const std::optional<Rect>& current
                                         ) {
            return retained.has_value() == current.has_value() &&
                (!retained.has_value() || translated(*retained, *current));
        };
        if (!translated(snapshot.bounds, record.bounds)) {
            return std::nullopt;
        }
        // Widget fragments consume painted geometry. Snapped/hit bounds and the clip/scroll
        // records belong to inspection, input, and the fresh outer render traversal; requiring
        // those derived records to move with a fragment defeats reuse without protecting any
        // command stored in the fragment.
        if (!translated(snapshot.content_bounds, record.content_bounds)) {
            return std::nullopt;
        }
        if (!translated_optional(snapshot.viewport, record.viewport)) {
            return std::nullopt;
        }
        std::size_t child_index = 0U;
        for (const auto& child : node.children()) {
            const LayoutRecord* child_layout = layout.find(child->identity());
            if (child_layout == nullptr) continue;
            if (child_index >= snapshot.children.size()) return std::nullopt;
            const ChildLayoutSnapshot& retained = snapshot.children[child_index++];
            if (retained.key != child->description().key ||
                retained.type != child->description().type ||
                !translated(retained.bounds, child_layout->bounds)) {
                return std::nullopt;
            }
        }
        return child_index == snapshot.children.size()
            ? std::optional<Point>(delta)
            : std::nullopt;
    }

    [[nodiscard]] static LayoutSnapshot snapshot(
        const RetainedNode& node,
        const LayoutRecord& record,
        const LayoutResult& layout
    ) {
        LayoutSnapshot result{
            record.measured_size,
            record.bounds,
            record.snapped_bounds,
            record.hit_bounds,
            record.content_bounds,
            record.clip,
            record.local_clip,
            record.scroll_frame,
            record.viewport,
            record.content_size,
            record.requested_scroll_offset,
            record.scroll_offset,
            record.visible_range,
            record.z_index,
            record.arranged_child_order,
            record.materialized_child_indices,
            record.portal_target,
            record.detached_from_parent_clip,
            {},
        };
        result.children.reserve(node.children().size());
        for (const auto& child : node.children()) {
            if (const LayoutRecord* child_layout = layout.find(child->identity());
                child_layout != nullptr) {
                result.children.push_back(ChildLayoutSnapshot{
                    child->description().key,
                    child->description().type,
                    child_layout->bounds,
                });
            }
        }
        return result;
    }
};

} // namespace strata::ui
