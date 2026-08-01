#include "ui/render.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>

#include "ui/input.hpp"
#include "ui/behavior/presentation.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/command.hpp"
#include "ui/motion.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/render/cache.hpp"
#include "ui/render/material_registry.hpp"
#include "ui/status.hpp"
#include "ui/text.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/subtarget.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] std::optional<MaterialState> material_state(const runtime::Value* value) {
    if (value == nullptr || value->object() == nullptr) return std::nullopt;
    const runtime::Value* id_value = value->field("id");
    if (id_value == nullptr || id_value->string() == nullptr || id_value->string()->empty()) {
        return std::nullopt;
    }
    MaterialState result;
    result.id = *id_value->string();
    if (const runtime::Value* blend_mode = value->field("blendMode");
        blend_mode != nullptr && blend_mode->string() != nullptr) {
        result.blend_mode = *blend_mode->string();
    }
    if (const runtime::Value* opacity = value->field("opacity");
        opacity != nullptr && opacity->number() != nullptr) {
        result.opacity = std::clamp(*opacity->number(), 0.0, 1.0);
    }
    if (const runtime::Value* parameters = value->field("parameters");
        parameters != nullptr && parameters->object() != nullptr) {
        result.parameters.reserve(parameters->object()->fields.size());
        for (const auto& [name, parameter] : parameters->object()->fields) {
            result.parameters.push_back(MaterialParameter{name, parameter});
        }
    }
    return result;
}

void append_fragment(
    RenderCommandBuffer& output,
    const std::vector<RenderCommand>& fragment,
    RenderOperationCounters& counters,
    const double opacity = 1.0
) {
    if (opacity == 1.0) {
        output.append(fragment);
        counters.commands_emitted += fragment.size();
        return;
    }
    for (const RenderCommand& command : fragment) {
        output.append(render_command_with_opacity(command, opacity));
    }
    counters.commands_emitted += fragment.size();
}

[[nodiscard]] std::optional<Rect> intersect_clip(
    const std::optional<Rect> first,
    const std::optional<Rect> second
) noexcept {
    if (!first.has_value()) return second;
    if (!second.has_value()) return first;
    return first->clip_intersection(*second);
}

[[nodiscard]] std::vector<RenderCommand> command_slice(
    const RenderCommandBuffer& commands,
    const std::size_t begin,
    const std::size_t end
) {
    const std::vector<RenderCommand>& source = commands.commands();
    return std::vector<RenderCommand>(
        source.begin() + static_cast<std::ptrdiff_t>(begin),
        source.begin() + static_cast<std::ptrdiff_t>(end)
    );
}

} // namespace

LogicalGlyphRun::LogicalGlyphRun()
    : glyphs_(std::make_shared<const std::vector<LogicalGlyph>>()) {}

LogicalGlyphRun::LogicalGlyphRun(std::vector<LogicalGlyph> glyphs)
    : glyphs_(std::make_shared<const std::vector<LogicalGlyph>>(std::move(glyphs))) {}

LogicalGlyphRun::LogicalGlyphRun(std::initializer_list<LogicalGlyph> glyphs)
    : LogicalGlyphRun(std::vector<LogicalGlyph>(glyphs)) {}

bool LogicalGlyphRun::empty() const noexcept { return glyphs_->empty(); }
std::size_t LogicalGlyphRun::size() const noexcept { return glyphs_->size(); }
const LogicalGlyph& LogicalGlyphRun::front() const { return glyphs_->front(); }
const LogicalGlyph& LogicalGlyphRun::operator[](const std::size_t index) const noexcept {
    return (*glyphs_)[index];
}
LogicalGlyphRun::const_iterator LogicalGlyphRun::begin() const noexcept {
    return glyphs_->begin();
}
LogicalGlyphRun::const_iterator LogicalGlyphRun::end() const noexcept {
    return glyphs_->end();
}
bool operator==(const LogicalGlyphRun& left, const LogicalGlyphRun& right) noexcept {
    return left.glyphs_ == right.glyphs_ || *left.glyphs_ == *right.glyphs_;
}

RenderCommand render_command_with_opacity(RenderCommand command, const double opacity) {
    std::visit(
        [opacity](auto& value) {
            using Command = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Command, SolidRectRenderCommand>) {
                value.fill.multiply_alpha(opacity);
            } else if constexpr (std::is_same_v<Command, ImageRenderCommand> ||
                                 std::is_same_v<Command, NinePatchRenderCommand>) {
                value.tint = widget_opacity(value.tint, opacity);
            } else if constexpr (std::is_same_v<Command, RoundedRectRenderCommand>) {
                value.fill.multiply_alpha(opacity);
                if (value.border.has_value()) {
                    value.border->color = widget_opacity(value.border->color, opacity);
                }
            } else if constexpr (std::is_same_v<Command, BorderRenderCommand>) {
                value.border.color = widget_opacity(value.border.color, opacity);
            } else if constexpr (std::is_same_v<Command, TextRunRenderCommand> ||
                                 std::is_same_v<Command, ShadowRenderCommand>) {
                value.color = widget_opacity(value.color, opacity);
            } else if constexpr (std::is_same_v<Command, PathRenderCommand>) {
                if (value.shape.fill.has_value()) value.shape.fill->multiply_alpha(opacity);
                if (value.shape.stroke.has_value()) value.shape.stroke->multiply_alpha(opacity);
            } else if constexpr (std::is_same_v<Command, CustomMeshRenderCommand>) {
                value.opacity *= opacity;
            } else if constexpr (std::is_same_v<Command, MaterialPushRenderCommand>) {
                value.material.opacity *= opacity;
            }
        },
        command
    );
    return command;
}

RenderEngine::RenderEngine() : implementation_(std::make_unique<Impl>()) {}
RenderEngine::~RenderEngine() = default;

bool RenderEngine::Impl::base_matches(
    const RetainedTree& tree,
    const LayoutResult& layout,
    const InputRouter& input,
    const MotionRuntime& motion,
    const RenderGenerationToken& generations
) const noexcept {
    return retained_base_generations.has_value() &&
        *retained_base_generations == generations && tree.dirty_count() == 0U &&
        motion.active_count() == 0U && retained_tree_generation == tree.generation() &&
        retained_layout_generation == layout.generation &&
        retained_presentation_generation ==
            (tree.root() != nullptr ? tree.root()->subtree_presentation_generation() : 0U) &&
        retained_status_generation == input.status_feedback().generation();
}

void RenderEngine::Impl::retain_base(
    const RetainedTree& tree,
    const LayoutResult& layout,
    const InputRouter& input,
    const RenderGenerationToken& generations,
    const RenderCommandBuffer& commands
) {
    retained_base_commands = commands;
    retained_base_generations = generations;
    retained_tree_generation = tree.generation();
    retained_layout_generation = layout.generation;
    retained_presentation_generation =
        tree.root() != nullptr ? tree.root()->subtree_presentation_generation() : 0U;
    retained_status_generation = input.status_feedback().generation();
}

std::vector<RenderCommand> RenderEngine::snapshot_subtree(const RetainedNode& node) const {
    std::vector<RenderCommand> result;
    const auto collect = [this, &result](const auto& self, const RetainedNode& current) -> void {
        const auto found = implementation_->fragments.find(current.identity());
        if (found != implementation_->fragments.end() &&
            Impl::fragment_generations_match(
                found->second.node_generations, current.dirty_generations()
            )) {
            if (found->second.presentation_translation != Point{}) {
                result.emplace_back(TransformPushRenderCommand{
                    1.0,
                    0.0,
                    found->second.presentation_translation.x,
                    0.0,
                    1.0,
                    found->second.presentation_translation.y,
                });
            }
            result.insert(
                result.end(), found->second.commands.begin(), found->second.commands.end()
            );
            if (found->second.presentation_translation != Point{}) {
                result.emplace_back(TransformPopRenderCommand{});
            }
        }
        for (const auto& child : current.children()) self(self, *child);
    };
    collect(collect, node);
    return result;
}

RenderOperationCounters RenderEngine::render(
    const RetainedTree& tree,
    const LayoutResult& layout,
    const InputRouter& input,
    const CommandIndex& commands,
    const WidgetRegistry& widgets,
    const BehaviorRegistry& behaviors,
    const MotionRuntime& motion,
    const TextEngine* text,
    const MaterialRegistry& materials,
    const RenderGenerationToken& generations,
    RenderCommandBuffer& output
) {
    output.clear();
    RenderOperationCounters counters;
    const auto collect_overlays = [&]() {
        return detached_overlay_roots(
            tree,
            layout,
            [&widgets, &behaviors, &input](const RetainedNode& node) {
                const WidgetLifecycle* lifecycle = widgets.find(node.description().type);
                return (lifecycle != nullptr && lifecycle->present.overlay != nullptr &&
                        lifecycle->present.detached_overlay) ||
                       has_behavior_overlay(behaviors, node, input, true);
            }
        );
    };
    const auto append_detached_overlays = [&](
                                              const std::vector<DetachedOverlayRoot>& overlays
                                          ) {
        for (const DetachedOverlayRoot& overlay_root : overlays) {
            const RetainedNode* node = overlay_root.node;
            if (node == nullptr) continue;
            const LayoutRecord* record = layout.find(node->identity());
            if (record == nullptr) continue;
            std::vector<RenderCommand> overlay = build_widget_overlay(
                widgets, *node, *record, layout, input, commands, text, &motion, 1.0
            );
            append_behavior_overlays(
                behaviors,
                *node,
                *record,
                layout,
                input,
                commands,
                text,
                &motion,
                1.0,
                true,
                overlay
            );
            if (overlay.empty()) continue;
            append_fragment(output, overlay, counters);
            ++counters.overlays_rendered;
        }
    };

    if (implementation_->base_matches(tree, layout, input, motion, generations)) {
        output = implementation_->retained_base_commands;
        append_detached_overlays(collect_overlays());
        counters.commands_emitted = output.size();
        return counters;
    }

    if (implementation_->fragment_tree_generation != tree.generation()) {
        std::erase_if(implementation_->fragments, [&tree](const auto& entry) {
            return tree.find_identity(entry.first) == nullptr;
        });
        implementation_->fragment_tree_generation = tree.generation();
    }
    std::vector<const RetainedNode*> portals;
    const auto collect_portals = [&ports = portals, &layout](
                                     const auto& self,
                                     const RetainedNode& node,
                                     const bool inside_portal
                                 ) -> void {
        const LayoutRecord* record = layout.find(node.identity());
        if (record == nullptr) return;
        const bool portal = record->kind == LayoutKind::portal;
        if (portal && !inside_portal) ports.push_back(&node);
        for (const auto& child : node.children()) self(self, *child, inside_portal || portal);
    };
    if (tree.root() != nullptr) collect_portals(collect_portals, *tree.root(), false);
    const std::vector<DetachedOverlayRoot> overlays = collect_overlays();

    const auto visit = [&](
                           const auto& self,
                           const RetainedNode& node,
                           const bool render_portals,
                           const std::optional<Rect> inherited_render_clip,
                           const MotionTransform inherited_transform,
                           const double inherited_opacity
                       ) -> void {
        const LayoutRecord* record = layout.find(node.identity());
        if (record == nullptr) return;
        if (!render_portals && record->kind == LayoutKind::portal) return;
        ++counters.nodes_visited;
        const bool focused = input.focused(node.identity());
        const bool focus_visible = input.focus_visible(node.identity());
        const bool hovered = input.hovered(node.identity());
        const bool active = input.active(node.identity());
        auto found = implementation_->fragments.find(node.identity());
        if (found != implementation_->fragments.end() &&
            found->second.composition.has_value()) {
            Impl::CachedFragment::CompositionPlan& retained =
                *found->second.composition;
            const auto append_retained_subtree = [&output, &counters](
                                                     const auto& plan
                                                 ) {
                if (plan.subtree_translation != Point{}) {
                    output.append(TransformPushRenderCommand{
                        1.0,
                        0.0,
                        plan.subtree_translation.x,
                        0.0,
                        1.0,
                        plan.subtree_translation.y,
                    });
                    ++counters.commands_emitted;
                }
                append_fragment(output, *plan.subtree_commands, counters);
                if (plan.subtree_translation != Point{}) {
                    output.append(TransformPopRenderCommand{});
                    ++counters.commands_emitted;
                }
            };
            const bool subtree_generations_match =
                retained.generations == generations;
            const bool subtree_node_matches =
                retained.subtree_node_render_generation ==
                node.subtree_render_generation();
            const bool subtree_presentation_matches =
                retained.subtree_presentation_generation ==
                node.subtree_presentation_generation();
            const bool subtree_layout_matches =
                retained.subtree_layout_render_generation ==
                record->subtree_render_generation;
            const bool subtree_status_matches =
                retained.subtree_status_generation ==
                input.status_feedback().generation();
            const bool subtree_context_matches =
                retained.inherited_render_clip == inherited_render_clip &&
                retained.inherited_transform == inherited_transform &&
                retained.inherited_opacity == inherited_opacity &&
                retained.render_portals == render_portals;
            if (retained.subtree_commands.has_value() &&
                subtree_generations_match && subtree_node_matches &&
                subtree_presentation_matches && subtree_layout_matches &&
                subtree_status_matches && subtree_context_matches) {
                found->second.visited = true;
                counters.fragments_reused += retained.subtree_node_count;
                counters.overlays_rendered += retained.subtree_overlay_count;
                append_retained_subtree(retained);
                return;
            }
            // Clip rectangles are inverse-adjusted to cancel presentation transforms. A cached
            // translation beneath an inherited scale would therefore move drawing by the scaled
            // delta but move the logical clip by that same scaled delta instead of its layout
            // delta. The combined cached stream therefore cannot translate clipped subtrees
            // beneath a non-unit inherited scale.
            const bool clip_translation_context_safe =
                !retained.subtree_contains_clip ||
                (inherited_transform.scale_x == 1.0 &&
                 inherited_transform.scale_y == 1.0);
            const bool translation_candidate =
                retained.subtree_commands.has_value() &&
                record->translated_subtree.has_value() &&
                record->kind != LayoutKind::scroll &&
                record->kind != LayoutKind::portal &&
                !record->virtual_axis.has_value() &&
                !record->virtual_item_extents.has_value() &&
                !record->visible_range.has_value() &&
                retained.subtree_translation_safe &&
                clip_translation_context_safe &&
                subtree_generations_match && subtree_node_matches &&
                subtree_presentation_matches && subtree_status_matches &&
                subtree_context_matches;
            const std::optional<Point> retained_translation =
                translation_candidate
                ? Impl::uniform_translation(
                      retained.subtree_layout, node, *record, layout
                  )
                : std::nullopt;
            if (retained_translation.has_value() &&
                *retained_translation == *record->translated_subtree) {
                retained.subtree_translation.x += retained_translation->x;
                retained.subtree_translation.y += retained_translation->y;
                retained.layout_render_generation = record->render_generation;
                retained.subtree_layout_render_generation =
                    record->subtree_render_generation;
                retained.subtree_layout = Impl::snapshot(node, *record, layout);
                for (std::size_t index = 0U;
                     index < retained.ordered_children.size();
                     ++index) {
                    const LayoutRecord* child_record = layout.find(
                        retained.ordered_children[index]->identity()
                    );
                    retained.ordered_child_layout_generations[index] =
                        child_record != nullptr ? child_record->render_generation : 0U;
                }
                found->second.visited = true;
                counters.fragments_reused += retained.subtree_node_count;
                counters.overlays_rendered += retained.subtree_overlay_count;
                append_retained_subtree(retained);
                return;
            }
            const bool external_matches = !retained.depends_on_status_feedback ||
                retained.external_generation == input.status_feedback().generation();
            bool child_layout_matches =
                retained.ordered_children.size() ==
                retained.ordered_child_layout_generations.size();
            for (std::size_t index = 0U;
                 child_layout_matches && index < retained.ordered_children.size();
                 ++index) {
                const LayoutRecord* child_record = layout.find(
                    retained.ordered_children[index]->identity()
                );
                child_layout_matches = child_record != nullptr &&
                    child_record->render_generation ==
                        retained.ordered_child_layout_generations[index];
            }
            if (retained.generations == generations &&
                retained.node_render_generation == node.render_generation() &&
                retained.node_presentation_generation ==
                    node.presentation_generation() &&
                retained.layout_render_generation == record->render_generation &&
                external_matches && child_layout_matches && retained.focused == focused &&
                retained.focus_visible == focus_visible && retained.hovered == hovered &&
                retained.active == active &&
                retained.inherited_render_clip == inherited_render_clip &&
                retained.inherited_transform == inherited_transform &&
                retained.inherited_opacity == inherited_opacity &&
                retained.render_portals == render_portals &&
                retained.subtree_translation == Point{}) {
                found->second.visited = true;
                ++counters.fragments_reused;
                append_fragment(output, retained.prefix, counters);
                for (const RetainedNode* child : retained.ordered_children) {
                    self(
                        self,
                        *child,
                        render_portals,
                        retained.child_render_clip,
                        retained.effective_transform,
                        retained.descendant_opacity
                    );
                }
                append_fragment(output, retained.suffix, counters);
                if (retained.local_overlay_rendered) ++counters.overlays_rendered;
                return;
            }
        }
        const WidgetLifecycle* lifecycle = widgets.find(node.description().type);
        const bool depends_on_status_feedback =
            lifecycle != nullptr && lifecycle->present.depends_on_status_feedback;
        const std::uint64_t external_generation = depends_on_status_feedback
            ? input.status_feedback().generation()
            : 0U;
        const auto style_value = [&node](const std::string_view name) -> const runtime::Value* {
            const auto direct_property = node.description().properties.find(name);
            const runtime::Value* direct = direct_property != node.description().properties.end()
                                               ? direct_property->second.value()
                                               : nullptr;
            const auto style_property = node.description().properties.find("$layout");
            const runtime::Value* style = style_property != node.description().properties.end()
                                              ? style_property->second.value()
                                              : nullptr;
            const runtime::Value* nested = style != nullptr ? style->field(name) : nullptr;
            return nested != nullptr ? nested : direct;
        };
        const MotionComputedValues* computed = motion.computed_values(node.identity());
        const MotionTransform transform = local_presentation_transform(node, motion);
        const MotionTransform effective_transform = concatenate_presentation_transform(
            inherited_transform,
            transform
        );
        const double local_opacity = std::clamp(
            computed != nullptr
                ? computed->number(MotionProperty::opacity).value_or(
                      visual_number(node, "opacity").value_or(1.0)
                  )
                : visual_number(node, "opacity").value_or(1.0),
            0.0,
            1.0
        );
        const double descendant_opacity = inherited_opacity * local_opacity;
        const std::optional<MaterialState> authored_material = material_state(style_value("material"));
        const std::optional<MaterialState> local_material = authored_material.has_value()
            ? materials.sanitize_state(*authored_material)
            : std::nullopt;
        std::optional<Rect> scope_clip_rect = inherited_render_clip;
        if (!record->local_clip.has_value()) {
            scope_clip_rect = intersect_clip(scope_clip_rect, record->clip);
        }
        const DirtyGenerationSnapshot node_generations = node.dirty_generations();
        const Impl::CachedFragment::MotionSnapshot fragment_motion =
            Impl::fragment_motion_snapshot(computed);
        const bool retained_state_matches = found != implementation_->fragments.end() &&
            found->second.generations == generations &&
            Impl::fragment_generations_match(found->second.node_generations, node_generations) &&
            found->second.motion == fragment_motion &&
            (lifecycle == nullptr || !lifecycle->present.depends_on_motion_progress ||
             found->second.node_generations.animation == node_generations.animation) &&
            found->second.external_generation == external_generation &&
            found->second.focused == focused &&
            found->second.focus_visible == focus_visible &&
            found->second.hovered == hovered && found->second.active == active;
        const bool retained_layout_matches = retained_state_matches &&
            Impl::matches(found->second.layout, node, *record, layout);
        Point fragment_translation{};
        bool has_fragment_translation = false;
        if (retained_state_matches && !retained_layout_matches) {
            const std::optional<Point> translation = Impl::uniform_translation(
                found->second.layout,
                node,
                *record,
                layout
            );
            if (translation.has_value()) {
                fragment_translation = *translation;
                has_fragment_translation = true;
            }
        }
        const bool rebuild = !retained_layout_matches && !has_fragment_translation;
        if (rebuild) {
            std::vector<RenderCommand> fragment = build_widget_fragment(
                widgets, node, *record, layout, input, commands, text, &motion, 1.0, false
            );
            Impl::CachedFragment next{
                generations,
                node_generations,
                fragment_motion,
                external_generation,
                focused,
                focus_visible,
                hovered,
                active,
                Impl::snapshot(node, *record, layout),
                std::move(fragment),
                {},
                true,
            };
            found = implementation_->fragments.insert_or_assign(node.identity(), std::move(next)).first;
            ++counters.fragments_built;
        } else {
            found->second.presentation_translation = fragment_translation;
            found->second.visited = true;
            ++counters.fragments_reused;
        }
        const std::size_t composition_prefix_begin = output.size();
        const bool scope_clip_changed =
            scope_clip_rect.has_value() && scope_clip_rect != inherited_render_clip;
        if (scope_clip_changed) {
            output.append(ClipPushRenderCommand{
                inverse_presentation_bounds(*scope_clip_rect, inherited_transform),
            });
            ++counters.commands_emitted;
        }
        if (!transform.identity()) {
            output.append(TransformPushRenderCommand{
                transform.scale_x,
                0.0,
                transform.translate_x,
                0.0,
                transform.scale_y,
                transform.translate_y,
            });
            ++counters.commands_emitted;
        }
        if (local_material.has_value()) {
            output.append(MaterialPushRenderCommand{*local_material});
            ++counters.commands_emitted;
        }
        if (has_fragment_translation) {
            output.append(TransformPushRenderCommand{
                1.0,
                0.0,
                fragment_translation.x,
                0.0,
                1.0,
                fragment_translation.y,
            });
            ++counters.commands_emitted;
        }
        append_fragment(output, found->second.commands, counters, descendant_opacity);
        if (has_fragment_translation) {
            output.append(TransformPopRenderCommand{});
            ++counters.commands_emitted;
        }
        if (local_material.has_value()) {
            output.append(MaterialPopRenderCommand{});
            ++counters.commands_emitted;
        }

        const std::optional<Rect> lifecycle_clip = widget_descendant_clip(
            widgets, node, *record, layout, input, commands, text, &motion, inherited_opacity
        );
        std::optional<Rect> descendant_local_clip = lifecycle_clip;
        if (record->local_clip.has_value()) {
            descendant_local_clip = descendant_local_clip.has_value()
                                        ? std::optional<Rect>(
                                              descendant_local_clip->clip_intersection(
                                                  *record->local_clip
                                              )
                                          )
                                        : record->local_clip;
        }
        const std::optional<Rect> child_render_clip = descendant_local_clip.has_value()
            ? intersect_clip(scope_clip_rect, descendant_local_clip)
            : scope_clip_rect;
        // Retain the local clip rather than its current intersection with an outer viewport.
        // A translated cached subtree then moves this raw clip while the live outer clip stack
        // recomputes their intersection. Retaining the pre-intersected rectangle can preserve an
        // empty/offscreen clip after scrolling and blank newly visible content.
        if (descendant_local_clip.has_value()) {
            output.append(ClipPushRenderCommand{
                inverse_presentation_bounds(*descendant_local_clip, effective_transform),
            });
            ++counters.commands_emitted;
        }
        const std::size_t composition_prefix_end = output.size();
        const auto child_z_index = [&layout](const RetainedNode& child) {
            const LayoutRecord* child_layout = layout.find(child.identity());
            return child_layout != nullptr ? child_layout->z_index : 0;
        };
        const auto& retained_children = node.children();
        bool children_already_ordered = true;
        for (std::size_t index = 1U; index < retained_children.size(); ++index) {
            if (child_z_index(*retained_children[index - 1U]) >
                child_z_index(*retained_children[index])) {
                children_already_ordered = false;
                break;
            }
        }
        std::vector<const RetainedNode*> ordered_children;
        ordered_children.reserve(retained_children.size());
        for (const auto& child : retained_children) ordered_children.push_back(child.get());
        if (!children_already_ordered) {
            std::ranges::stable_sort(
                ordered_children,
                [&child_z_index](const RetainedNode* left, const RetainedNode* right) {
                    return child_z_index(*left) < child_z_index(*right);
                }
            );
        }
        for (const RetainedNode* child : ordered_children) {
            self(
                self,
                *child,
                render_portals,
                child_render_clip,
                effective_transform,
                descendant_opacity
            );
        }
        std::vector<std::uint64_t> ordered_child_layout_generations;
        ordered_child_layout_generations.reserve(ordered_children.size());
        for (const RetainedNode* child : ordered_children) {
            const LayoutRecord* child_record = layout.find(child->identity());
            ordered_child_layout_generations.push_back(
                child_record != nullptr ? child_record->render_generation : 0U
            );
        }
        const std::size_t composition_suffix_begin = output.size();
        if (descendant_local_clip.has_value()) {
            output.append(ClipPopRenderCommand{});
            ++counters.commands_emitted;
        }

        found = implementation_->fragments.find(node.identity());
        if (found == implementation_->fragments.end()) {
            throw std::logic_error("render fragment disappeared before presentation");
        }
        Impl::CachedFragment& cached_fragment = found->second;
        const auto& retained_presentation = cached_fragment.presentation;
        const bool retained_presentation_state_matches =
            retained_presentation.has_value() &&
            retained_presentation->generations == generations &&
            Impl::fragment_generations_match(
                retained_presentation->node_generations, node_generations
            ) &&
            retained_presentation->motion == fragment_motion &&
            (lifecycle == nullptr || !lifecycle->present.depends_on_motion_progress ||
             retained_presentation->node_generations.animation ==
                 node_generations.animation) &&
            retained_presentation->node_presentation_generation ==
                node.presentation_generation() &&
            retained_presentation->external_generation == external_generation &&
            retained_presentation->focused == focused &&
            retained_presentation->focus_visible == focus_visible &&
            retained_presentation->hovered == hovered &&
            retained_presentation->active == active &&
            retained_presentation->inherited_opacity == inherited_opacity;
        const bool retained_presentation_layout_matches =
            retained_presentation_state_matches &&
            Impl::matches(retained_presentation->layout, node, *record, layout);
        const std::optional<Point> presentation_translation =
            retained_presentation_state_matches &&
                !retained_presentation_layout_matches
            ? Impl::uniform_translation(
                  retained_presentation->layout, node, *record, layout
              )
            : std::nullopt;
        if (!retained_presentation_layout_matches &&
            !presentation_translation.has_value()) {
            std::vector<RenderCommand> presentation;
            append_widget_foreground(
                widgets, node, *record, layout, input, commands, text, &motion,
                inherited_opacity, presentation
            );
            bool local_overlay_rendered = false;
            if (lifecycle != nullptr && lifecycle->present.overlay != nullptr &&
                !lifecycle->present.detached_overlay) {
                const std::size_t before = presentation.size();
                std::vector<RenderCommand> overlay = build_widget_overlay(
                    widgets, node, *record, layout, input, commands, text, &motion,
                    inherited_opacity
                );
                presentation.insert(
                    presentation.end(),
                    std::make_move_iterator(overlay.begin()),
                    std::make_move_iterator(overlay.end())
                );
                local_overlay_rendered = presentation.size() != before;
            }
            const std::size_t before_behavior = presentation.size();
            append_behavior_overlays(
                behaviors,
                node,
                *record,
                layout,
                input,
                commands,
                text,
                &motion,
                inherited_opacity,
                false,
                presentation
            );
            local_overlay_rendered =
                local_overlay_rendered || presentation.size() != before_behavior;
            cached_fragment.presentation =
                Impl::CachedFragment::PresentationPlan{
                    generations,
                    node_generations,
                    fragment_motion,
                    node.presentation_generation(),
                    external_generation,
                    focused,
                    focus_visible,
                    hovered,
                    active,
                    inherited_opacity,
                    Impl::snapshot(node, *record, layout),
                    std::move(presentation),
                    local_overlay_rendered,
                };
        }
        const Impl::CachedFragment::PresentationPlan& presentation =
            *cached_fragment.presentation;
        if (presentation_translation.has_value()) {
            output.append(TransformPushRenderCommand{
                1.0,
                0.0,
                presentation_translation->x,
                0.0,
                1.0,
                presentation_translation->y,
            });
            ++counters.commands_emitted;
        }
        append_fragment(output, presentation.commands, counters);
        if (presentation_translation.has_value()) {
            output.append(TransformPopRenderCommand{});
            ++counters.commands_emitted;
        }
        const bool local_overlay_rendered = presentation.local_overlay_rendered;
        if (local_overlay_rendered) ++counters.overlays_rendered;
        if (!transform.identity()) {
            output.append(TransformPopRenderCommand{});
            ++counters.commands_emitted;
        }
        if (scope_clip_changed) {
            output.append(ClipPopRenderCommand{});
            ++counters.commands_emitted;
        }
        const std::size_t composition_suffix_end = output.size();
        found = implementation_->fragments.find(node.identity());
        if (found == implementation_->fragments.end()) {
            throw std::logic_error("render fragment disappeared during retained composition");
        }
        found->second.composition = Impl::CachedFragment::CompositionPlan{
            generations,
            node.render_generation(),
            node.presentation_generation(),
            record->render_generation,
            external_generation,
            depends_on_status_feedback,
            focused,
            focus_visible,
            hovered,
            active,
            inherited_render_clip,
            inherited_transform,
            inherited_opacity,
            render_portals,
            command_slice(
                output, composition_prefix_begin, composition_prefix_end
            ),
            command_slice(
                output, composition_suffix_begin, composition_suffix_end
            ),
            std::move(ordered_children),
            std::move(ordered_child_layout_generations),
            child_render_clip,
            effective_transform,
            descendant_opacity,
            local_overlay_rendered,
        };
        Impl::CachedFragment::CompositionPlan& composition =
            *found->second.composition;
        composition.subtree_node_render_generation =
            node.subtree_render_generation();
        composition.subtree_presentation_generation =
            node.subtree_presentation_generation();
        composition.subtree_layout_render_generation =
            record->subtree_render_generation;
        composition.subtree_status_generation =
            input.status_feedback().generation();
        composition.subtree_overlay_count = local_overlay_rendered ? 1U : 0U;
        composition.subtree_layout = Impl::snapshot(node, *record, layout);
        composition.subtree_translation_safe =
            record->kind != LayoutKind::scroll &&
            record->kind != LayoutKind::portal &&
            !record->virtual_axis.has_value() &&
            !record->virtual_item_extents.has_value() &&
            !record->visible_range.has_value();
        composition.subtree_contains_clip =
            scope_clip_changed || descendant_local_clip.has_value();
        for (const RetainedNode* child : composition.ordered_children) {
            const auto child_fragment = implementation_->fragments.find(child->identity());
            if (child_fragment == implementation_->fragments.end() ||
                !child_fragment->second.composition.has_value()) {
                composition.subtree_translation_safe = false;
                continue;
            }
            composition.subtree_translation_safe =
                composition.subtree_translation_safe &&
                child_fragment->second.composition->subtree_translation_safe;
            composition.subtree_contains_clip =
                composition.subtree_contains_clip ||
                child_fragment->second.composition->subtree_contains_clip;
            composition.subtree_node_count +=
                child_fragment->second.composition->subtree_node_count;
            composition.subtree_overlay_count +=
                child_fragment->second.composition->subtree_overlay_count;
        }
        constexpr std::size_t maximum_retained_subtree_commands = 256U;
        if (composition_suffix_end - composition_prefix_begin <=
            maximum_retained_subtree_commands) {
            composition.subtree_commands = command_slice(
                output,
                composition_prefix_begin,
                composition_suffix_end
            );
        }
    };
    if (tree.root() != nullptr) {
        visit(
            visit, *tree.root(), false, std::nullopt, MotionTransform{}, 1.0
        );
    }
    // Portals remain retained content. Detached overlays occupy the final shared z-plane.
    for (const RetainedNode* portal : portals) {
        visit(visit, *portal, true, std::nullopt, MotionTransform{}, 1.0);
        ++counters.portals_rendered;
    }
    implementation_->retain_base(tree, layout, input, generations, output);
    append_detached_overlays(overlays);
    counters.commands_emitted = output.size();
    return counters;
}

void RenderEngine::clear() {
    implementation_->fragments.clear();
    implementation_->retained_base_commands.clear();
    implementation_->retained_base_generations.reset();
    implementation_->fragment_tree_generation = 0U;
}

} // namespace strata::ui
