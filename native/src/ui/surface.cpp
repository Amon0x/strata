#include "ui/surface.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

#include "core/utf8.hpp"
#include "ui/text_geometry.hpp"
#include "ui/presentation_geometry.hpp"
#include "ui/widget/editor_geometry.hpp"
#include "ui/widget/inspection.hpp"

namespace strata::ui {

bool surface_detail::LazyConvergenceTracker::SignatureLess::operator()(
    const LazyRangeSignature& left,
    const LazyRangeSignature& right
) const noexcept {
    return std::lexicographical_compare(
        left.begin(),
        left.end(),
        right.begin(),
        right.end(),
        [](const LazyRangeState& left_state, const LazyRangeState& right_state) {
            return left_state < right_state;
        }
    );
}

surface_detail::LazyConvergenceStatus surface_detail::LazyConvergenceTracker::observe(
    LazyRangeSignature signature
) {
    std::sort(signature.begin(), signature.end());
    for (const LazyRangeState& state : signature) {
        producers_.emplace(state.structural_path, state.child_count);
    }
    const bool fixed = std::ranges::all_of(signature, [](const LazyRangeState& state) {
        return !state.stale_projection && state.materialized == state.visible;
    });
    if (!observed_.insert(std::move(signature)).second) {
        return LazyConvergenceStatus::cycle;
    }
    const std::size_t bound = known_state_bound();
    if (bound != std::numeric_limits<std::size_t>::max() && observed_.size() > bound) {
        throw std::logic_error("lazy convergence exceeded its canonical finite state bound");
    }
    if (fixed) return LazyConvergenceStatus::fixed_point;
    return LazyConvergenceStatus::progress;
}

std::size_t surface_detail::LazyConvergenceTracker::observed_state_count() const noexcept {
    return observed_.size();
}

std::size_t surface_detail::LazyConvergenceTracker::known_state_bound() const noexcept {
    const auto saturated_multiply = [](const std::size_t left, const std::size_t right) {
        return left != 0U && right > std::numeric_limits<std::size_t>::max() / left
            ? std::numeric_limits<std::size_t>::max()
            : left * right;
    };
    const auto saturated_add = [](const std::size_t left, const std::size_t right) {
        return right > std::numeric_limits<std::size_t>::max() - left
            ? std::numeric_limits<std::size_t>::max()
            : left + right;
    };
    std::size_t bound = 1U;
    for (const auto& [path, child_count] : producers_) {
        static_cast<void>(path);
        const std::size_t first = saturated_add(child_count, 1U);
        const std::size_t second = saturated_add(child_count, 2U);
        const std::size_t ranges = first % 2U == 0U
            ? saturated_multiply(first / 2U, second)
            : saturated_multiply(first, second / 2U);
        // A previously discovered producer is either absent from the current attached tree or
        // contributes one materialized/visible pair of canonical half-open ranges.
        const std::size_t component_states = saturated_add(
            saturated_multiply(ranges, ranges),
            1U
        );
        bound = saturated_multiply(bound, component_states);
    }
    return bound;
}

surface_detail::MaterializationPublicationClaim
surface_detail::MaterializationPublicationLedger::claim(
    const std::shared_ptr<const DescriptionMaterialization>& transaction
) {
    if (transaction == nullptr || transaction->transaction_id() == 0U) {
        return MaterializationPublicationClaim::invalid_transaction;
    }
    purge_expired();
    const std::uint64_t id = transaction->transaction_id();
    if (const auto found = published_.find(id); found != published_.end()) {
        if (const std::shared_ptr<const DescriptionMaterialization> live =
                found->second.lock();
            live != nullptr) {
            return live.get() == transaction.get()
                ? MaterializationPublicationClaim::already_published
                : MaterializationPublicationClaim::duplicate_live_identity;
        }
        published_.erase(found);
    }
    published_.emplace(id, transaction);
    return MaterializationPublicationClaim::publish;
}

void surface_detail::MaterializationPublicationLedger::purge_expired() {
    std::erase_if(published_, [](const auto& entry) { return entry.second.expired(); });
}

std::size_t surface_detail::MaterializationPublicationLedger::tracked_count() const noexcept {
    return published_.size();
}

namespace {

[[nodiscard]] bool blank(const std::string_view value) noexcept {
    return core::utf8_blankness(value) != core::Utf8Blankness::non_blank;
}

class RuntimeFrameGuard final {
public:
    explicit RuntimeFrameGuard(runtime::RuntimeServices& services)
        : services_(services), frame_index_(services_.begin_frame()) {}

    RuntimeFrameGuard(const RuntimeFrameGuard&) = delete;
    RuntimeFrameGuard& operator=(const RuntimeFrameGuard&) = delete;
    ~RuntimeFrameGuard() { services_.end_frame(); }

    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }

private:
    runtime::RuntimeServices& services_;
    std::uint64_t frame_index_;
};

class BooleanScope final {
public:
    explicit BooleanScope(bool& value) noexcept : value_(value) { value_ = true; }
    BooleanScope(const BooleanScope&) = delete;
    BooleanScope& operator=(const BooleanScope&) = delete;
    ~BooleanScope() { value_ = false; }

private:
    bool& value_;
};

[[nodiscard]] runtime::Value object(
    std::initializer_list<std::pair<std::string, runtime::Value>> fields
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>(fields));
}

[[nodiscard]] DescriptionNode::Properties surface_layout() {
    return {{
        "$layout",
        runtime::ExpressionValue(object({
            {"height", object({{"weight", runtime::Value(1.0)}})},
            {"kind", runtime::Value("OVERLAY")},
            {"width", object({{"weight", runtime::Value(1.0)}})},
        }))
    }};
}

[[nodiscard]] DescriptionNode::Properties surface_layer_properties(
    const std::optional<std::string>& transition
) {
    DescriptionNode::Properties properties = surface_layout();
    if (transition.has_value()) {
        properties.emplace(
            "transition",
            runtime::ExpressionValue(runtime::Value(*transition))
        );
    }
    return properties;
}

[[nodiscard]] std::shared_ptr<const DescriptionChildren> eager(
    std::vector<std::shared_ptr<const DescriptionNode>> children
) {
    return std::make_shared<const EagerDescriptionChildren>(std::move(children));
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> declaration_root(
    const std::shared_ptr<const DescriptionNode>& declaration
) {
    if (declaration == nullptr || declaration->children == nullptr ||
        declaration->children->size() != 1U) {
        throw std::logic_error("validated surface declaration must describe exactly one root node");
    }
    return declaration->children->at(0U);
}

[[nodiscard]] std::string layer_name(
    const std::string_view id,
    const std::string_view prefix
) {
    if (!id.starts_with(prefix) || id.size() == prefix.size()) {
        throw std::logic_error("declarative layer identity does not match its role");
    }
    return std::string(id.substr(prefix.size()));
}

[[nodiscard]] surface_detail::LazyRangeSignature lazy_range_signature(
    const RetainedTree& tree,
    const LayoutResult& layout
) {
    surface_detail::LazyRangeSignature result;
    if (tree.root() == nullptr) return result;
    const auto visit = [&result, &layout](const auto& self, const RetainedNode& node) -> void {
        if (node.lifecycle() != RetainedLifecycle::attached) return;
        if (node.description().materialization.has_value()) {
            const DescriptionNode& projected = node.description();
            const std::shared_ptr<const DescriptionNode>& source =
                projected.generated_source;
            const std::shared_ptr<const DescriptionChildren> provider =
                source != nullptr ? source->children : projected.children;
            const std::size_t child_count = provider->size();
            MaterializationRange materialized =
                node.realized_range().value_or(MaterializationRange{});
            materialized.start = std::min(materialized.start, child_count);
            materialized.end_exclusive = std::clamp(
                materialized.end_exclusive,
                materialized.start,
                child_count
            );
            MaterializationRange visible;
            if (const LayoutRecord* record = layout.find(node.identity());
                record != nullptr && record->visible_range.has_value()) {
                visible = MaterializationRange{
                    std::min(record->visible_range->start, child_count),
                    std::min(record->visible_range->end_exclusive, child_count),
                };
                visible.end_exclusive = std::max(visible.start, visible.end_exclusive);
            }
            result.push_back(surface_detail::LazyRangeState{
                std::string(node.structural_path()),
                child_count,
                materialized,
                visible,
                !node.realization_current(
                    provider,
                    projected.projected_theme,
                    projected.projected_theme_scope,
                    projected.projected_theme_generation,
                    materialized
                ),
            });
        }
        for (const auto& child : node.children()) self(self, *child);
    };
    visit(visit, *tree.root());
    std::sort(result.begin(), result.end());
    return result;
}

[[nodiscard]] MaterializationRange desired_realization_range(
    const RetainedNode& node,
    const LayoutResult& layout,
    const std::size_t child_count
) {
    MaterializationRange desired;
    const LayoutRecord* const record = layout.find(node.identity());
    if (record == nullptr) return desired;
    if (record->virtual_item_extents.has_value() &&
        record->virtual_axis.has_value() && record->viewport.has_value()) {
        const collection::VirtualItemExtents& extents = *record->virtual_item_extents;
        const LayoutAxis axis = *record->virtual_axis;
        double offset = axis == LayoutAxis::vertical
            ? record->scroll_offset.y
            : record->scroll_offset.x;
        if (const runtime::Value* retained = node.retained_value("strata.scroll.offset");
            retained != nullptr && retained->object() != nullptr) {
            const runtime::Value* value = retained->field(
                axis == LayoutAxis::vertical ? "y" : "x"
            );
            if (value != nullptr && value->number() != nullptr &&
                std::isfinite(*value->number())) {
                offset = *value->number();
            }
        }
        const double viewport_extent = axis == LayoutAxis::vertical
            ? record->viewport->height
            : record->viewport->width;
        const std::size_t item_count = std::min(child_count, extents.size());
        if (item_count == 0U || offset >= extents.total() || viewport_extent <= 0.0) {
            return desired;
        }
        offset = std::max(0.0, offset);
        const std::size_t start = std::min(
            item_count,
            extents.first_index_ending_after(offset)
        );
        const std::size_t end = std::min(
            item_count,
            extents.end_index_starting_before(
                std::min(extents.total(), offset + viewport_extent)
            )
        );
        const std::size_t overscan = record->virtual_overscan;
        return MaterializationRange{
            start > overscan ? start - overscan : 0U,
            std::min(item_count, end + std::min(overscan, item_count - end)),
        };
    }
    if (record->visible_range.has_value()) {
        desired = MaterializationRange{
            std::min(record->visible_range->start, child_count),
            std::min(record->visible_range->end_exclusive, child_count),
        };
        desired.end_exclusive = std::max(desired.start, desired.end_exclusive);
    }
    return desired;
}

[[nodiscard]] bool lazy_ranges_require_realization(
    const surface_detail::LazyRangeSignature& signature
) {
    return std::ranges::any_of(signature, [](const surface_detail::LazyRangeState& state) {
        return state.stale_projection || state.materialized != state.visible;
    });
}

void add_layout_operations(
    SurfaceOperationCounters& target,
    const LayoutOperationCounters& source
) {
    target.layout_measured_nodes += source.measured_nodes;
    target.layout_arranged_nodes += source.arranged_nodes;
    target.layout_translated_nodes += source.translated_nodes;
    target.layout_measurement_cache_hits += source.measurement_cache_hits;
}

void append_input(InputOperationResult& target, InputOperationResult source) {
    target.events.insert(
        target.events.end(),
        std::make_move_iterator(source.events.begin()),
        std::make_move_iterator(source.events.end())
    );
    target.action_outcomes.insert(
        target.action_outcomes.end(),
        std::make_move_iterator(source.action_outcomes.begin()),
        std::make_move_iterator(source.action_outcomes.end())
    );
    target.injected_events += source.injected_events;
    target.processed_events += source.processed_events;
}

[[nodiscard]] std::string resolved_viewport_class(const SurfaceEnvironment& environment) {
    const double content_width = std::max(
        0.0,
        environment.logical_width - environment.safe_insets.horizontal()
    );
    if (content_width < 520.0) return "compact";
    if (content_width < 900.0) return "regular";
    return "wide";
}

[[nodiscard]] std::string resolved_orientation(const SurfaceEnvironment& environment) {
    if (environment.logical_width > environment.logical_height * 1.05) return "landscape";
    if (environment.logical_height > environment.logical_width * 1.05) return "portrait";
    return "square";
}

[[nodiscard]] std::string pointer_precision_binding(const PointerPrecision precision) {
    switch (precision) {
    case PointerPrecision::none: return "NONE";
    case PointerPrecision::coarse: return "COARSE";
    case PointerPrecision::fine: return "FINE";
    }
    throw std::logic_error("unknown surface pointer precision");
}

[[nodiscard]] runtime::Value surface_environment_binding(
    const SurfaceEnvironment& environment
) {
    const double content_width = std::max(
        0.0,
        environment.logical_width - environment.safe_insets.horizontal()
    );
    const double content_height = std::max(
        0.0,
        environment.logical_height - environment.safe_insets.vertical()
    );
    const std::string viewport = resolved_viewport_class(environment);
    const bool compact = environment.density == SurfaceDensity::compact;
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
        {"width", runtime::Value(content_width)},
        {"height", runtime::Value(content_height)},
        {"framebufferWidth", runtime::Value(static_cast<double>(environment.framebuffer_width))},
        {"framebufferHeight", runtime::Value(static_cast<double>(environment.framebuffer_height))},
        {"scale", runtime::Value(environment.scale)},
        {"viewport", runtime::Value(viewport)},
        {"orientation", runtime::Value(resolved_orientation(environment))},
        {"compact", runtime::Value(viewport == "compact")},
        {"wide", runtime::Value(viewport == "wide")},
        {"controller", runtime::Value(environment.input.controller)},
        {"touch", runtime::Value(environment.input.touch)},
        {"pointer", runtime::Value(environment.input.pointer)},
        {"pointerPrecision", runtime::Value(pointer_precision_binding(environment.input.pointer_precision))},
        {"keyboard", runtime::Value(environment.input.keyboard)},
        {"ime", runtime::Value(environment.input.ime)},
        {"density", runtime::Value(std::string(surface_density_name(environment.density)))},
        {"reducedMotion", runtime::Value(environment.reduced_motion)},
        {"safeLeft", runtime::Value(environment.safe_insets.left)},
        {"safeTop", runtime::Value(environment.safe_insets.top)},
        {"safeRight", runtime::Value(environment.safe_insets.right)},
        {"safeBottom", runtime::Value(environment.safe_insets.bottom)},
        {"controlHeight", runtime::Value(compact ? 28.0 : 38.0)},
        {"contentPadding", runtime::Value(compact ? 6.0 : 10.0)},
        {"itemGap", runtime::Value(compact ? 4.0 : 8.0)},
    });
}

} // namespace

void SurfaceEnvironment::validate() const {
    if (framebuffer_width < 0 || framebuffer_height < 0 ||
        !std::isfinite(logical_width) || logical_width < 0.0 ||
        !std::isfinite(logical_height) || logical_height < 0.0 ||
        !std::isfinite(scale) || scale <= 0.0) {
        throw std::invalid_argument("surface environment contains invalid framebuffer, logical size, or scale");
    }
    layout_environment(0).validate();
}

LayoutEnvironment SurfaceEnvironment::layout_environment(
    const std::int64_t frame_time_nanos
) const {
    return LayoutEnvironment{
        generation,
        Rect{0.0, 0.0, logical_width, logical_height},
        scale,
        safe_insets,
        point_snapping,
        rectangle_snapping,
        true,
        frame_time_nanos,
        reduced_motion,
    };
}

Surface::Surface(
    std::string id,
    runtime::ApplicationContext& application,
    const runtime::LayerRole root_role,
    std::string root_name,
    SurfaceEnvironment environment,
    std::shared_ptr<const TextEngine> text_engine,
    WidgetRegistry widget_registry,
    BehaviorRegistry behavior_registry,
    runtime::HostServices* const host_services,
    Theme initial_theme,
    std::string host_service_owner,
    std::shared_ptr<const resource::SvgImageRegistry> svg_images
) : id_(std::move(id)),
    profiler_(runtime::ProfilerScope::surface, id_),
    application_(application),
    root_role_(root_role),
    root_name_(std::move(root_name)),
    environment_(std::move(environment)),
    adopted_environment_generation_(environment_.generation),
    viewport_class_(resolved_viewport_class(environment_)),
    behaviors_(std::move(behavior_registry)),
    widgets_(std::move(widget_registry)),
    descriptions_(application_, widgets_),
    themes_(std::move(initial_theme)),
    text_engine_(std::move(text_engine)),
    svg_images_(std::move(svg_images)),
    layout_engine_(text_engine_ != nullptr
        ? LayoutEngine::IntrinsicMeasure(
              [engine = text_engine_](const RetainedNode& node, const Constraints& constraints) {
                  return engine->measure(node, constraints);
              }
          )
        : LayoutEngine::IntrinsicMeasure{}),
    status_feedback_([this] { invalidate_frame(); }),
    notifications_([this](const NotificationChange& change) { notifications_changed(change); }),
    input_(
        id_,
        host_service_owner.empty() ? id_ : std::move(host_service_owner),
        application_,
        widgets_,
        behaviors_,
        status_feedback_,
        notifications_,
        [this](const runtime::Action& action) { return execute_environment_action(action); },
        [this](const RetainedNode* const node, const std::string_view name) {
            bool inside_lazy_materialization = false;
            for (const RetainedNode* current = node;
                 current != nullptr;
                 current = current->parent()) {
                if (current->description().materialization_result != nullptr) {
                    inside_lazy_materialization = true;
                    break;
                }
            }
            const bool native_slider_value =
                node != nullptr && node->description().type == "Slider" &&
                name == "$value" &&
                !node->description().properties.contains("presentationTemplate") &&
                !inside_lazy_materialization;
            if (node == nullptr ||
                descriptions_.observes_retained_value(
                    *node,
                    name,
                    !native_slider_value
                )) {
                invalidate_description();
            } else {
                invalidate_frame();
            }
        },
        [this](const RetainedNode& node, const LayoutRecord& layout) {
            return widget_hit_bounds(*this, node, layout);
        },
        [this](
            const RetainedNode& node,
            const LayoutRecord& layout,
            const std::string_view text,
            const Point position
        ) -> std::optional<std::size_t> {
            if (text_engine_ == nullptr) return std::nullopt;
            const TextLayout text_layout = text_engine_->layout(node, text);
            const std::vector<WidgetSubtarget> targets = input_.subtargets(node.identity());
            const std::optional<Rect> viewport = editable_text_viewport(
                node, layout, targets
            );
            if (!viewport.has_value() || viewport->empty()) return std::nullopt;
            if (node.description().type == "ChipInput" ||
                node.description().type == "CommandPalette") {
                if (position.x < viewport->x || position.y < viewport->y ||
                    position.x > viewport->right() || position.y > viewport->bottom()) {
                    return std::nullopt;
                }
            }
            const Point origin = text_input_origin(
                *viewport, text_layout, node.description().type == "TextArea"
            );
            const std::size_t utf16 = text_layout_hit_offset(
                text_layout,
                Point{position.x - origin.x, position.y - origin.y}
            );
            return utf8_byte_for_utf16_offset(text, utf16);
        },
        [this](const RetainedNode& node, const std::string_view text) {
            return text_engine_ != nullptr
                ? text_engine_->shape(node, text).metrics.width
                : 0.0;
        },
        [this](
            const RetainedNode& node,
            const LayoutRecord& layout,
            const TextEditorSnapshot& editor
        ) -> std::optional<runtime::HostServiceRect> {
            if (text_engine_ == nullptr || !environment_.input.ime) return std::nullopt;
            const std::vector<WidgetSubtarget> targets = input_.subtargets(node.identity());
            const std::optional<Rect> viewport = editable_text_viewport(
                node, layout, targets
            );
            if (!viewport.has_value() || viewport->empty()) return std::nullopt;
            const TextLayout text_layout = text_engine_->layout(node, editor.text);
            const Point origin = text_input_origin(
                *viewport, text_layout, node.description().type == "TextArea"
            );
            Rect caret = text_layout_caret_rect(
                text_layout, origin, editor.text, editor.caret
            );
            MotionTransform transform;
            std::vector<const RetainedNode*> route;
            for (const RetainedNode* current = &node; current != nullptr;
                 current = current->parent()) {
                route.push_back(current);
            }
            for (auto current = route.rbegin(); current != route.rend(); ++current) {
                const LayoutRecord* current_layout =
                    layout_engine_.result().find((*current)->identity());
                if (current_layout == nullptr) continue;
                transform = concatenate_presentation_transform(
                    transform,
                    local_presentation_transform(
                        **current,
                        motion_,
                        current_layout->bounds
                    )
                );
            }
            caret = transform_presentation_bounds(caret, transform);
            return runtime::HostServiceRect{
                caret.x, caret.y, caret.width, caret.height,
            };
        },
        [this](const RetainedNode& node) {
            return render_engine_.snapshot_subtree(node);
        },
        {},
        host_services,
        [this](
            const RetainedNode& node,
            const std::string_view value,
            const TextLayoutOptions& options
        ) {
            return text_engine_ != nullptr
                ? text_engine_->layout(node, value, options)
                : TextLayout{};
        },
        [this](const std::string_view key) {
            if (!advancing_scroll_animation_) {
                const auto animation = scroll_animations_.find(key);
                if (animation != scroll_animations_.end()) scroll_animations_.erase(animation);
            }
        },
        [this] { invalidate_frame(); }
    ),
    commands_(widgets_, &application_, &application_.durability(), id_),
    semantics_(widgets_, behaviors_),
    material_registry_(
        application_.bundle()->schema_registry(),
        [this](runtime::RuntimeDiagnostic diagnostic) {
            application_.services().report(std::move(diagnostic));
        }
    ) {
    if (id_.empty()) throw std::invalid_argument("surface id must not be empty");
    if (root_name_.empty()) throw std::invalid_argument("surface root name must not be empty");
    environment_.validate();
    tree_.configure_persistence(
        [this](const std::string_view type) {
            const WidgetLifecycle* lifecycle = widgets_.find(type);
            return lifecycle != nullptr
                ? lifecycle->persistence.retained_fields
                : std::vector<std::string>{};
        },
        [this](
            const std::string_view type,
            const std::string_view key,
            const std::string_view field
        ) -> std::optional<runtime::Value> {
            const runtime::Value* value = application_.durability().widget_value(key, field);
            if (value == nullptr) return std::nullopt;
            const WidgetLifecycle* lifecycle = widgets_.find(type);
            if (lifecycle == nullptr || lifecycle->persistence.accepts == nullptr ||
                lifecycle->persistence.accepts(field, *value)) {
                return *value;
            }
            application_.services().report(runtime::RuntimeDiagnostic{
                "STRATA.DURABILITY.TYPE_MISMATCH",
                "Persisted widget field '" + std::string(field) + "' for '" +
                    std::string(key) + "' has an invalid value and was discarded.",
                id_,
                std::string(type),
                runtime::DiagnosticSeverity::warning,
                std::nullopt,
            });
            static_cast<void>(application_.durability().erase_widget_value(key, field));
            return std::nullopt;
        },
        [this](
            const std::string_view key,
            const std::string_view field,
            const runtime::Value& value
        ) {
            application_.durability().set_widget_value(
                std::string(key), std::string(field), value
            );
        }
    );
    motion_.set_supplemental(themes_.declared_animations());
}

Surface::~Surface() {
    static_cast<void>(application_.async().retain_owners(id_ + ":", {}));
    application_.undo().clear(id_);
}

const std::string& Surface::id() const noexcept { return id_; }
runtime::ApplicationContext& Surface::application() const noexcept { return application_; }
const SurfaceEnvironment& Surface::environment() const noexcept { return environment_; }
runtime::Profiler& Surface::profiler() noexcept { return profiler_; }
const runtime::Profiler& Surface::profiler() const noexcept { return profiler_; }
std::string_view Surface::viewport_class() const noexcept { return viewport_class_; }

const Theme& Surface::theme() const noexcept { return *themes_.root(); }

const std::shared_ptr<const Theme>* Surface::registered_theme(
    const std::string_view name
) const noexcept {
    return themes_.find(name);
}

bool Surface::register_theme(Theme theme) {
    if (!themes_.register_theme(std::move(theme))) return false;
    motion_.set_supplemental(themes_.declared_animations());
    invalidate_description();
    return true;
}

bool Surface::set_theme(Theme theme) {
    if (!themes_.set_root(std::move(theme))) return false;
    motion_.set_supplemental(themes_.declared_animations());
    invalidate_description();
    return true;
}

bool Surface::unregister_theme(const std::string_view name) {
    if (!themes_.unregister_theme(name)) return false;
    motion_.set_supplemental(themes_.declared_animations());
    invalidate_description();
    return true;
}

bool Surface::set_scoped_theme(std::string node_key, Theme theme) {
    if (!themes_.set_scoped_theme(std::move(node_key), std::move(theme))) return false;
    motion_.set_supplemental(themes_.declared_animations());
    invalidate_description();
    return true;
}

bool Surface::clear_scoped_theme(const std::string_view node_key) {
    if (!themes_.clear_scoped_theme(node_key)) return false;
    motion_.set_supplemental(themes_.declared_animations());
    invalidate_description();
    return true;
}

bool Surface::animate_scroll_to(ScrollAnimationRequest request) {
    if (blank(request.key) || blank(request.timing) ||
        (!request.x.has_value() && !request.y.has_value())) {
        throw std::invalid_argument("scroll animation requires a non-blank key/timing and a target axis");
    }
    if ((request.x.has_value() && !std::isfinite(*request.x)) ||
        (request.y.has_value() && !std::isfinite(*request.y)) ||
        (request.duration_nanos.has_value() && *request.duration_nanos <= 0)) {
        throw std::invalid_argument("scroll animation targets must be finite and duration must be positive");
    }
    RetainedNode* node = tree_.find_key(request.key);
    const std::optional<Point> current = input_.scroll_offset(request.key);
    if (node == nullptr || !current.has_value()) {
        scroll_animations_.erase(request.key);
        return false;
    }
    Point requested = *current;
    if (request.x.has_value()) requested.x = *request.x;
    if (request.y.has_value()) requested.y = *request.y;
    const std::optional<Point> target = input_.constrained_scroll_target(request.key, requested);
    if (!target.has_value()) {
        scroll_animations_.erase(request.key);
        return false;
    }
    if (*target == *current) {
        scroll_animations_.erase(request.key);
        return false;
    }
    const bool known_timing = theme_motion_timing_defined(node->description(), request.timing);
    if (!known_timing) report_unknown_theme_timing(request.timing, node->description());
    MotionTiming timing = theme_motion_timing(node->description(), request.timing);
    if (request.duration_nanos.has_value()) timing.duration_nanos = *request.duration_nanos;
    if (theme_motion_reduced(node->description(), environment_.reduced_motion)) {
        scroll_animations_.erase(request.key);
        InputOperationResult result;
        const BooleanScope advancing(advancing_scroll_animation_);
        static_cast<void>(input_.scroll_to(request.key, *target, result));
        append_input(pending_lifecycle_input_, std::move(result));
        return true;
    }
    scroll_animations_.insert_or_assign(
        request.key,
        ScrollAnimation{node->identity(), *current, *target, std::move(timing), std::nullopt}
    );
    return true;
}

std::size_t Surface::active_scroll_animation_count() const noexcept {
    return scroll_animations_.size();
}

void Surface::advance_scroll_animations(const std::int64_t frame_time_nanos) {
    InputOperationResult result;
    for (auto animation = scroll_animations_.begin(); animation != scroll_animations_.end();) {
        RetainedNode* node = tree_.find_key(animation->first);
        if (node == nullptr || node->identity() != animation->second.identity) {
            animation = scroll_animations_.erase(animation);
            continue;
        }
        const std::optional<Point> current = input_.scroll_offset(animation->first);
        const std::optional<Point> constrained = input_.constrained_scroll_target(
            animation->first,
            animation->second.target
        );
        if (!current.has_value() || !constrained.has_value()) {
            animation = scroll_animations_.erase(animation);
            continue;
        }
        animation->second.target = *constrained;
        if (theme_motion_reduced(node->description(), environment_.reduced_motion)) {
            {
                const BooleanScope advancing(advancing_scroll_animation_);
                static_cast<void>(input_.scroll_to(animation->first, *constrained, result));
            }
            animation = scroll_animations_.erase(animation);
            continue;
        }
        if (!animation->second.started_nanos.has_value()) {
            animation->second.started_nanos = frame_time_nanos;
        }
        const double elapsed = std::max(
            0.0,
            static_cast<double>(frame_time_nanos - *animation->second.started_nanos) -
                static_cast<double>(animation->second.timing.delay_nanos)
        );
        const double linear = std::clamp(
            elapsed / static_cast<double>(animation->second.timing.duration_nanos),
            0.0,
            1.0
        );
        const double eased = motion_easing(animation->second.timing.easing, linear);
        const Point sample{
            animation->second.start.x +
                (animation->second.target.x - animation->second.start.x) * eased,
            animation->second.start.y +
                (animation->second.target.y - animation->second.start.y) * eased,
        };
        {
            const BooleanScope advancing(advancing_scroll_animation_);
            static_cast<void>(input_.scroll_to(animation->first, sample, result));
        }
        if (linear >= 1.0) animation = scroll_animations_.erase(animation);
        else ++animation;
    }
    append_input(pending_lifecycle_input_, std::move(result));
}

void Surface::retain_scroll_animations() {
    std::erase_if(scroll_animations_, [this](const auto& value) {
        const RetainedNode* node = tree_.find_key(value.first);
        return node == nullptr || node->identity() != value.second.identity;
    });
}

NotificationService& Surface::notifications() noexcept { return notifications_; }
const NotificationService& Surface::notifications() const noexcept { return notifications_; }

void Surface::notifications_changed(const NotificationChange& change) {
    const std::vector<RetainedNode*>* regions = tree_.find_type("ToastRegion");
    if (regions == nullptr) return;
    for (RetainedNode* region : *regions) {
        if (change.input || change.render) {
            static_cast<void>(tree_.mark(region->identity(), DirtyReason::input));
        }
        if (change.semantics) {
            static_cast<void>(tree_.mark(region->identity(), DirtyReason::semantics));
        }
    }
}

bool Surface::adopt_environment(SurfaceEnvironment environment) {
    environment.validate();
    if (environment.generation <= adopted_environment_generation_) return false;
    const bool scale_changed = environment_.framebuffer_width != environment.framebuffer_width ||
                               environment_.framebuffer_height != environment.framebuffer_height ||
                               environment_.logical_width != environment.logical_width ||
                               environment_.logical_height != environment.logical_height ||
                               environment_.scale != environment.scale ||
                               environment_.point_snapping != environment.point_snapping ||
                               environment_.rectangle_snapping != environment.rectangle_snapping;
    const bool preferences_changed = environment_.safe_insets != environment.safe_insets ||
                                     environment_.density != environment.density ||
                                     environment_.reduced_motion != environment.reduced_motion ||
                                     environment_.input != environment.input;
    if (environment_.generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("surface environment generation is exhausted");
    }
    adopted_environment_generation_ = environment.generation;
    environment.generation = std::max(environment.generation, environment_.generation + 1U);
    environment_ = std::move(environment);
    viewport_class_ = resolved_viewport_class(environment_);
    if (scale_changed) {
        input_.invalidate_host_geometry();
        if (scale_context_generation_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("surface scale-context generation exhausted");
        }
        ++scale_context_generation_;
    }
    if (tree_.root() != nullptr) {
        if (scale_changed) {
            static_cast<void>(tree_.mark(tree_.root()->identity(), DirtyReason::scale));
        }
        if (preferences_changed) {
            static_cast<void>(tree_.mark(tree_.root()->identity(), DirtyReason::input));
        }
    }
    invalidate_description();
    return true;
}

void Surface::advance_environment_generation() {
    if (environment_.generation == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("surface environment generation is exhausted");
    }
    ++environment_.generation;
    invalidate_description();
}

bool Surface::adopt_scale_context(const SurfaceEnvironment& source) {
    source.validate();
    if (environment_.framebuffer_width == source.framebuffer_width &&
        environment_.framebuffer_height == source.framebuffer_height &&
        environment_.logical_width == source.logical_width &&
        environment_.logical_height == source.logical_height &&
        environment_.scale == source.scale &&
        environment_.point_snapping == source.point_snapping &&
        environment_.rectangle_snapping == source.rectangle_snapping) {
        return false;
    }
    environment_.framebuffer_width = source.framebuffer_width;
    environment_.framebuffer_height = source.framebuffer_height;
    environment_.logical_width = source.logical_width;
    environment_.logical_height = source.logical_height;
    environment_.scale = source.scale;
    environment_.point_snapping = source.point_snapping;
    environment_.rectangle_snapping = source.rectangle_snapping;
    if (scale_context_generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("surface scale-context generation exhausted");
    }
    ++scale_context_generation_;
    input_.invalidate_host_geometry();
    advance_environment_generation();
    if (tree_.root() != nullptr) {
        static_cast<void>(tree_.mark(tree_.root()->identity(), DirtyReason::scale));
    }
    viewport_class_ = resolved_viewport_class(environment_);
    return true;
}

bool Surface::adopt_environment_preferences(const SurfaceEnvironment& source) {
    source.validate();
    if (environment_.safe_insets == source.safe_insets &&
        environment_.density == source.density &&
        environment_.reduced_motion == source.reduced_motion &&
        environment_.input == source.input) {
        return false;
    }
    environment_.safe_insets = source.safe_insets;
    environment_.density = source.density;
    environment_.reduced_motion = source.reduced_motion;
    environment_.input = source.input;
    advance_environment_generation();
    if (tree_.root() != nullptr) {
        static_cast<void>(tree_.mark(tree_.root()->identity(), DirtyReason::input));
    }
    return true;
}

void Surface::invalidate_resources() {
    queue_resource_invalidation();
}

void Surface::note_resource_reload_duration(const std::uint64_t duration_nanos) noexcept {
    pending_reload_duration_nanos_ =
        duration_nanos > std::numeric_limits<std::uint64_t>::max() - pending_reload_duration_nanos_
            ? std::numeric_limits<std::uint64_t>::max()
            : pending_reload_duration_nanos_ + duration_nanos;
}

SurfaceResourceReloadPlan Surface::prepare_resource_reload(
    std::shared_ptr<const TextEngine> text_engine,
    std::shared_ptr<const resource::SvgImageRegistry> svg_images
) const {
    LayoutEngine next_layout = text_engine != nullptr
        ? LayoutEngine(LayoutEngine::IntrinsicMeasure(
              [engine = text_engine](const RetainedNode& node, const Constraints& constraints) {
                  return engine->measure(node, constraints);
              }
          ))
        : LayoutEngine{};
    return SurfaceResourceReloadPlan{
        std::move(text_engine),
        std::move(svg_images),
        std::move(next_layout),
    };
}

void Surface::commit_resource_reload(SurfaceResourceReloadPlan plan) noexcept {
    static_assert(std::is_nothrow_move_assignable_v<std::shared_ptr<const TextEngine>>);
    static_assert(std::is_nothrow_move_assignable_v<
        std::shared_ptr<const resource::SvgImageRegistry>
    >);
    static_assert(std::is_nothrow_move_assignable_v<LayoutEngine>);
    text_engine_ = std::move(plan.text_engine);
    svg_images_ = std::move(plan.svg_images);
    layout_engine_ = std::move(plan.layout_engine);
    queue_resource_invalidation();
}

void Surface::replace_text_engine(std::shared_ptr<const TextEngine> text_engine) {
    commit_resource_reload(prepare_resource_reload(std::move(text_engine), svg_images_));
}

void Surface::queue_resource_invalidation() noexcept {
    if (queued_resource_invalidations_ != std::numeric_limits<std::size_t>::max()) {
        ++queued_resource_invalidations_;
    }
    resource_invalidation_pending_ = true;
    invalidate_description();
}

void Surface::apply_pending_resource_invalidation() {
    if (!resource_invalidation_pending_) return;
    // Marking may allocate. Keep the pending flag set until both throwing operations succeed so a
    // failed frame cannot expose the newly installed engine with stale resource generations.
    if (tree_.root() != nullptr) {
        static_cast<void>(tree_.mark(tree_.root()->identity(), DirtyReason::resource));
    }
    application_.services().bump_resource_reload_generations();
    pending_resource_reloads_ =
        queued_resource_invalidations_ >
                std::numeric_limits<std::size_t>::max() - pending_resource_reloads_
            ? std::numeric_limits<std::size_t>::max()
            : pending_resource_reloads_ + queued_resource_invalidations_;
    queued_resource_invalidations_ = 0U;
    resource_invalidation_pending_ = false;
    invalidate_description();
}

void Surface::invalidate() noexcept { invalidate_description(); }

void Surface::invalidate_frame() noexcept { frame_invalidated_ = true; }

void Surface::invalidate_description() noexcept {
    description_invalidated_ = true;
    frame_invalidated_ = true;
}

runtime::ActionDispatchOutcome Surface::execute_environment_action(
    const runtime::Action& action
) {
    const auto outcome = [&action](
                             const runtime::ActionDispatchStatus status,
                             std::optional<std::string> message = std::nullopt
                         ) {
        return runtime::ActionDispatchOutcome{
            status,
            action.id(),
            {"strata.surface.declarative"},
            std::move(message),
        };
    };
    if (action.id() != "environment.set") {
        return outcome(
            runtime::ActionDispatchStatus::failed,
            "Surface framework executor does not own action '" + action.id() + "'."
        );
    }
    SurfaceEnvironment next = environment_;
    if (const runtime::Value* density = action.payload.field("density");
        density != nullptr && density->string() != nullptr) {
        if (*density->string() == "COMPACT" || *density->string() == "compact") {
            next.density = SurfaceDensity::compact;
        } else if (*density->string() == "COMFORTABLE" || *density->string() == "comfortable") {
            next.density = SurfaceDensity::comfortable;
        } else {
            return outcome(
                runtime::ActionDispatchStatus::failed,
                "Unknown surface density '" + *density->string() + "'."
            );
        }
    }
    if (const runtime::Value* reduced = action.payload.field("reducedMotion");
        reduced != nullptr && reduced->boolean() != nullptr) {
        next.reduced_motion = *reduced->boolean();
    }
    const auto adopt_inset = [&action](const std::string_view name, double& destination) {
        const runtime::Value* value = action.payload.field(name);
        if (value != nullptr && value->number() != nullptr) destination = *value->number();
    };
    adopt_inset("safeLeft", next.safe_insets.left);
    adopt_inset("safeTop", next.safe_insets.top);
    adopt_inset("safeRight", next.safe_insets.right);
    adopt_inset("safeBottom", next.safe_insets.bottom);
    SurfaceEnvironment comparable = next;
    comparable.generation = environment_.generation;
    if (comparable == environment_) return outcome(runtime::ActionDispatchStatus::ignored);
    if (environment_.generation == std::numeric_limits<std::uint64_t>::max()) {
        return outcome(
            runtime::ActionDispatchStatus::failed,
            "Surface environment generation is exhausted."
        );
    }
    next.generation = environment_.generation + 1U;
    next.validate();
    environment_ = std::move(next);
    declarative_environment_pending_ = true;
    invalidate_description();
    return outcome(runtime::ActionDispatchStatus::handled);
}

std::shared_ptr<const DescriptionNode> Surface::describe(DescriptionBuildResult& result) {
    struct VisibleLayer final {
        std::string id;
        runtime::LayerRole declaration_role;
        std::string name;
        std::optional<std::string> transition;
    };
    const std::vector<runtime::LayerSnapshot> application_layers = application_.layers().snapshot();
    std::vector<VisibleLayer> visible;
    // The stack retains navigation state, but only its active screen is declarative UI. A
    // displaced screen may remain in the retained tree only through the ordinary EXITING
    // lifecycle; describing the whole stack would keep obsolete screens attached forever.
    const auto active_screen = std::ranges::find_if(
        application_layers.rbegin(),
        application_layers.rend(),
        [](const runtime::LayerSnapshot& layer) {
            return layer.role == runtime::LayerRole::screen;
        }
    );
    if (active_screen != application_layers.rend()) {
        const std::string name = layer_name(active_screen->id, "screen:");
        const runtime::LayerRole declaration_role =
            application_.active_unit()->screen(name)
                ? runtime::LayerRole::screen
                : runtime::LayerRole::overlay;
        visible.push_back(VisibleLayer{
            active_screen->id,
            declaration_role,
            name,
            active_screen->transition,
        });
    } else if (!application_.layers().root_replaced()) {
        visible.push_back(VisibleLayer{
            "root:" + id_, root_role_, root_name_, std::nullopt
        });
    }
    for (const runtime::LayerSnapshot& layer : application_layers) {
        if (layer.role == runtime::LayerRole::overlay) {
            visible.push_back(VisibleLayer{
                layer.id,
                runtime::LayerRole::overlay,
                layer_name(layer.id, "overlay:"),
                layer.transition,
            });
        }
    }
    std::vector<LayerDescriptionRequest> requests;
    requests.reserve(visible.size());
    for (const VisibleLayer& layer : visible) {
        requests.push_back(LayerDescriptionRequest{layer.declaration_role, layer.name});
    }
    descriptions_.set_retained_tree(&tree_);
    descriptions_.set_contextual_host_roots({{"env", surface_environment_binding(environment_)}});
    DescriptionLayersBuildResult built = descriptions_.build_layers(requests);
    for (std::size_t index = 0U; index < visible.size(); ++index) {
        state_scopes_by_layer_.insert_or_assign(
            visible[index].id,
            built.layer_state_scopes[index]
        );
    }
    std::set<std::string> attached_layers;
    if (!application_.layers().root_replaced()) attached_layers.insert("root:" + id_);
    for (const runtime::LayerSnapshot& layer : application_layers) attached_layers.insert(layer.id);
    std::erase_if(state_scopes_by_layer_, [&attached_layers](const auto& entry) {
        return !attached_layers.contains(entry.first);
    });
    std::vector<std::shared_ptr<const DescriptionNode>> layers;
    layers.reserve(visible.size());
    for (std::size_t index = 0U; index < visible.size(); ++index) {
        layers.push_back(DescriptionNode::create(
            "SurfaceLayer",
            "strata.layer." + visible[index].id,
            {},
            {},
            surface_layer_properties(visible[index].transition),
            eager({declaration_root(built.roots[index])})
        ));
    }
    result = DescriptionBuildResult{
        {},
        std::move(built.diagnostics),
        built.evaluated_expressions,
        built.described_nodes,
    };
    return DescriptionNode::create(
        "SurfaceLayers",
        "strata.surface.layers",
        {},
        {},
        surface_layout(),
        eager(std::move(layers))
    );
}

bool Surface::rebuild_tree(
    SurfaceFrame& frame,
    std::optional<std::string_view>& restore_focus
) {
    if (!description_invalidated_ && tree_.root() != nullptr &&
        application_.dirty_generation() == observed_application_generation_) {
        return false;
    }
    auto profile = profiler_.section("description");
    const std::vector<runtime::LayerSnapshot> layers = application_.layers().snapshot();
    std::string active_screen = "root:" + id_;
    for (const runtime::LayerSnapshot& layer : layers) {
        if (layer.role == runtime::LayerRole::screen) active_screen = layer.id;
    }
    if (observed_active_screen_.has_value() && *observed_active_screen_ != active_screen) {
        if (const std::optional<std::string_view> focused = input_.focused_key();
            focused.has_value()) {
            focus_by_screen_.insert_or_assign(*observed_active_screen_, std::string(*focused));
        }
        if (const auto found = focus_by_screen_.find(active_screen);
            found != focus_by_screen_.end()) {
            restore_focus = found->second;
        }
    }
    observed_active_screen_ = std::move(active_screen);
    input_.begin_tree_update();
    DescriptionBuildResult description;
    {
        auto build_profile = profiler_.section("build");
        raw_description_ = describe(description);
    }
    std::shared_ptr<const DescriptionNode> materialized;
    {
        auto materialize_profile = profiler_.section("materialize");
        materialized = project_description_theme();
    }
    {
        auto reconcile_profile = profiler_.section("reconcile");
        static_cast<void>(tree_.reconcile(
            std::move(materialized),
            [this](const RetainedNode& node) { return motion_.should_retain_for_exit(node); }
        ));
    }
    // Preserve the old viewport across an application/theme rebuild, but refresh its rows against
    // the newly authored provider before input or layout can observe stale source indices.
    if (tree_.root() != nullptr) {
        static_cast<void>(realize_virtual_children(layout_engine_.result()));
    }
    descriptions_.set_retained_tree(&tree_);
    retain_scroll_animations();
    frame.operations.rebuilds = 1U;
    frame.operations.described_nodes += description.described_nodes;
    frame.operations.evaluated_expressions += description.evaluated_expressions;
    for (runtime::RuntimeDiagnostic& diagnostic : description.diagnostics) {
        application_.services().report(std::move(diagnostic));
    }
    observed_application_generation_ = application_.dirty_generation();
    description_invalidated_ = false;
    record_profiler_counters(frame);
    return true;
}

std::shared_ptr<const DescriptionNode> Surface::project_description_theme() {
    return materialize_theme_tree(
        raw_description_,
        themes_,
        [this](const std::string_view type) { return widgets_.find(type) != nullptr; },
        [this](const std::string_view name, const DescriptionNode& node) {
            report_unknown_theme_timing(name, node);
        },
        &theme_materialization_cache_
    ).root;
}

ReconcileStats Surface::realize_virtual_children(const LayoutResult& layout) {
    ReconcileStats aggregate;
    std::vector<std::uint64_t> producers;
    producers.reserve(tree_.virtual_nodes().size());
    for (const RetainedNode* const producer : tree_.virtual_nodes()) {
        producers.push_back(producer->identity());
    }
    for (const std::uint64_t identity : producers) {
        RetainedNode* const parent = tree_.find_identity(identity);
        if (parent == nullptr || parent->lifecycle() != RetainedLifecycle::attached ||
            !parent->description().materialization.has_value()) {
            continue;
        }
        const DescriptionNode& projected = parent->description();
        const std::shared_ptr<const DescriptionNode>& source = projected.generated_source;
        const std::shared_ptr<const DescriptionChildren> provider =
            source != nullptr ? source->children : projected.children;
        const std::size_t child_count = provider->size();
        const MaterializationRange desired =
            desired_realization_range(*parent, layout, child_count);
        if (parent->realization_current(
                provider,
                projected.projected_theme,
                projected.projected_theme_scope,
                projected.projected_theme_generation,
                desired
            )) {
            continue;
        }
        if (projected.projected_theme == nullptr) {
            throw std::logic_error(
                "lazy collection is missing its inherited projected theme context"
            );
        }

        struct RowProjection final {
            std::size_t source_index = 0U;
            std::shared_ptr<const DescriptionNode> description;
            bool requires_projection = false;
        };
        std::vector<RowProjection> rows;
        rows.reserve(desired.end_exclusive - desired.start);
        for (std::size_t index = desired.start; index < desired.end_exclusive; ++index) {
            std::shared_ptr<const DescriptionNode> description =
                parent->realized_child_description(
                    index,
                    provider,
                    projected.projected_theme,
                    projected.projected_theme_scope,
                    projected.projected_theme_generation
                );
            const bool generated = description == nullptr;
            if (generated) description = provider->at(index);
            rows.push_back(RowProjection{index, std::move(description), generated});
        }

        std::vector<RealizedDescriptionChild> realized;
        realized.reserve(rows.size());
        for (RowProjection& row : rows) {
            if (row.requires_projection) {
                row.description = materialize_theme_subtree(
                    row.description,
                    themes_,
                    projected.projected_theme,
                    projected.projected_theme_scope,
                    [this](const std::string_view type) {
                        return widgets_.find(type) != nullptr;
                    },
                    [this](const std::string_view name, const DescriptionNode& node) {
                        report_unknown_theme_timing(name, node);
                    },
                    &theme_materialization_cache_
                ).root;
                if (row.description->materialization_result != nullptr) {
                    pending_lazy_materializations_.push_back(
                        row.description->materialization_result
                    );
                }
            }
            realized.push_back(RealizedDescriptionChild{
                row.source_index,
                std::move(row.description),
            });
        }

        ReconcileStats local = tree_.realize_children(
            identity,
            provider,
            projected.projected_theme,
            projected.projected_theme_scope,
            projected.projected_theme_generation,
            desired,
            std::move(realized),
            [this](const RetainedNode& node) {
                return motion_.should_retain_for_exit(node);
            }
        );
        aggregate.created += local.created;
        aggregate.reused += local.reused;
        aggregate.updated += local.updated;
        aggregate.detached += local.detached;
        aggregate.materialized += local.materialized;
        aggregate.generation = local.generation;
    }
    return aggregate;
}

void Surface::report_unknown_theme_timing(
    const std::string_view name,
    const DescriptionNode& node
) {
    constexpr std::size_t maximum_reported_unknown_timings = 128U;
    std::string key = node.source_path;
    key.push_back('\n');
    key.append(name);
    if (reported_theme_motion_diagnostics_.contains(key) ||
        reported_theme_motion_diagnostics_.size() >= maximum_reported_unknown_timings) {
        return;
    }
    reported_theme_motion_diagnostics_.insert(std::move(key));
    application_.services().report(runtime::RuntimeDiagnostic{
        "STRATA.ANIMATION.MOTION_POLICY_UNKNOWN",
        "Motion timing '" + std::string(name) + "' is not defined for " +
            (node.key.has_value() ? *node.key : node.type) + "; '" +
            std::string(default_motion_timing_name) + "' is used.",
        node.source_path,
        std::string(default_motion_timing_name),
        runtime::DiagnosticSeverity::warning,
        std::nullopt,
    });
}

void Surface::sample_motion(
    SurfaceFrame& frame,
    const std::int64_t frame_time_nanos,
    const bool temporal
) {
    const auto motion_started = std::chrono::steady_clock::now();
    auto motion_profile = profiler_.section("motion");
    const MotionFrameCounters motion = temporal
        ? motion_.advance(
              tree_, application_.active_unit(), input_, frame_time_nanos,
              environment_.reduced_motion
          )
        : motion_.discover(
              tree_, application_.active_unit(), input_, frame_time_nanos,
              environment_.reduced_motion
          );
    frame.operations.animation_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - motion_started
        ).count()
    );
    if (motion.evaluated_nodes != 0U) {
        frame.operations.motion_mutated_nodes += motion.mutated_nodes;
    }
    frame.operations.motion_running_players = motion.running_players;
    record_profiler_counters(frame);
    motion_profile.close();
}

void Surface::layout_tree(SurfaceFrame& frame, const std::int64_t frame_time_nanos) {
    const auto layout_started = std::chrono::steady_clock::now();
    auto layout_profile = profiler_.section("layout");
    const auto finish_layout_profile = [this, &frame, &layout_profile, layout_started]() {
        {
            auto commit_profile = profiler_.section("commit-materialization");
            commit_lazy_materializations(frame);
        }
        frame.operations.layout_nanos += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - layout_started
            ).count()
        );
        if (text_engine_ != nullptr) frame.operations.text = text_engine_->operation_counters();
        record_profiler_counters(frame);
        layout_profile.close();
    };
    const LayoutEnvironment stable_environment = environment_.layout_environment(
        frame_time_nanos
    );
    if (tree_.dirty_count() == 0U && motion_.active_count() == 0U &&
        !layout_engine_.requires_layout(tree_, stable_environment)) {
        finish_layout_profile();
        return;
    }
    // Scroll offsets are retained before layout. The previous record already owns the immutable
    // virtual extent table and viewport geometry, so ordinary scrolling can predict and attach its
    // next window before the single required arrangement pass. Resizes or measured-extent changes
    // still converge through the checked post-layout path below.
    if (layout_engine_.result().root_identity != 0U) {
        auto prediction_profile = profiler_.section("prediction");
        const ReconcileStats predicted = realize_virtual_children(
            layout_engine_.result()
        );
        if (predicted.changed()) {
            auto prepare_profile = profiler_.section("input-prepare");
            append_input(frame.lifecycle_input, input_.prepare(tree_));
        }
    }
    const bool temporal_motion_sample = motion_sampled_frame_index_ != frame.frame_index;
    const auto layout_pass = [this, &frame, frame_time_nanos](
                                 const bool initial_pass,
                                 const bool evaluate_before_layout,
                                 const bool temporal_motion
                             ) -> const LayoutResult& {
        auto pass_profile = profiler_.section(
            initial_pass ? "initial-pass" : "convergence-pass"
        );
        const LayoutEnvironment environment = environment_.layout_environment(frame_time_nanos);
        if (evaluate_before_layout) {
            sample_motion(frame, frame_time_nanos, temporal_motion);
        }
        if (temporal_motion) {
            motion_sampled_frame_index_ = frame.frame_index;
            auto prune_profile = profiler_.section("prune-exiting");
            if (tree_.prune_exiting([this](const RetainedNode& node) {
                    return motion_.exit_finished(node);
                }) != 0U) {
                auto prepare_profile = profiler_.section("input-prepare");
                append_input(frame.lifecycle_input, input_.prepare(tree_));
            }
        }
        std::vector<MotionMoveOrigin> move_origins;
        if (!environment_.reduced_motion &&
            layout_engine_.requires_layout(tree_, environment)) {
            auto capture_profile = profiler_.section("move-capture");
            move_origins = motion_.capture_move_origins(tree_, layout_engine_.result());
        }
        const LayoutResult* result = nullptr;
        {
            auto engine_profile = profiler_.section("engine");
            result = &layout_engine_.layout(
                tree_,
                environment,
                &motion_,
                false,
                frame.frame_index
            );
            profiler_.record_external_timing("measure", result->measure_nanos);
            profiler_.record_external_timing("arrange", result->arrange_nanos);
            profiler_.record_external_timing(
                "maintenance",
                result->maintenance_nanos
            );
        }
        {
            auto apply_profile = profiler_.section("move-apply");
            motion_.apply_move_transitions(
                tree_, move_origins, *result, frame_time_nanos, environment_.reduced_motion
            );
        }
        return *result;
    };

    const auto report_nonconvergence = [this](
                                           const surface_detail::LazyRangeSignature& signature,
                                           const surface_detail::LazyConvergenceTracker& tracker,
                                           const std::string_view reason
                                       ) {
        constexpr std::size_t maximum_reported_convergence_failures = 128U;
        std::string fingerprint(reason);
        for (const surface_detail::LazyRangeState& state : signature) {
            fingerprint += "\n" + state.structural_path + ":" +
                std::to_string(state.child_count) + ":" +
                std::to_string(state.materialized.start) + "-" +
                std::to_string(state.materialized.end_exclusive) + ">" +
                std::to_string(state.visible.start) + "-" +
                std::to_string(state.visible.end_exclusive) +
                (state.stale_projection ? ":stale" : "");
        }
        if (reported_lazy_convergence_diagnostics_.size() >=
                maximum_reported_convergence_failures ||
            !reported_lazy_convergence_diagnostics_.insert(fingerprint).second) {
            return;
        }
        application_.services().publish_current_frame(runtime::RuntimeDiagnostic{
            "STRATA.UI.LAZY_REALIZATION_NONCONVERGENT",
            "Virtual realization stopped at a coherent layout boundary after " +
                std::string(reason) + "; observed " +
                std::to_string(tracker.observed_state_count()) +
                " distinct range states within a known state-space bound of " +
                (tracker.known_state_bound() == std::numeric_limits<std::size_t>::max()
                     ? std::string("at least size_t maximum")
                     : std::to_string(tracker.known_state_bound())) + ".",
            signature.empty() ? std::string{} : signature.front().structural_path,
            std::string("a fixed realized/visible range point"),
            runtime::DiagnosticSeverity::error,
            std::nullopt,
        });
    };

    // Realization/layout is a deterministic finite state machine over canonical ranges.
    // Every retry observes a new state; equality settles, while repetition proves a cycle. This
    // avoids a magic retry cap and never advances animation time during structural convergence.
    surface_detail::LazyConvergenceTracker convergence;
    bool evaluate_before_layout = true;
    bool initial_pass = true;
    for (;;) {
        const bool current_initial_pass = std::exchange(initial_pass, false);
        const bool initialize_motion = std::exchange(evaluate_before_layout, false);
        const LayoutResult& laid_out = layout_pass(
            current_initial_pass,
            initialize_motion,
            initialize_motion && temporal_motion_sample
        );
        add_layout_operations(frame.operations, laid_out.operations);
        if (raw_description_ == nullptr) break;
        surface_detail::LazyRangeSignature signature;
        {
            auto signature_profile = profiler_.section("range-signature");
            signature = lazy_range_signature(tree_, laid_out);
        }
        if (!lazy_ranges_require_realization(signature)) break;
        const surface_detail::LazyConvergenceStatus status = convergence.observe(signature);
        if (status == surface_detail::LazyConvergenceStatus::cycle) {
            report_nonconvergence(signature, convergence, "a repeated range-state cycle");
            break;
        }
        ReconcileStats lazy;
        {
            auto realization_profile = profiler_.section("realization");
            lazy = realize_virtual_children(laid_out);
        }
        if (!lazy.changed()) {
            if (status == surface_detail::LazyConvergenceStatus::fixed_point) break;
            surface_detail::LazyRangeSignature terminal;
            {
                auto signature_profile = profiler_.section("range-signature");
                terminal = lazy_range_signature(tree_, laid_out);
            }
            if (convergence.observe(terminal) !=
                surface_detail::LazyConvergenceStatus::fixed_point) {
                report_nonconvergence(
                    terminal,
                    convergence,
                    "reconciliation made no range progress"
                );
            }
            break;
        }
        {
            auto prepare_profile = profiler_.section("input-prepare");
            append_input(frame.lifecycle_input, input_.prepare(tree_));
        }
        sample_motion(frame, frame_time_nanos, false);
    }
    finish_layout_profile();
}

void Surface::commit_lazy_materializations(SurfaceFrame& frame) {
    for (const std::shared_ptr<const DescriptionMaterialization>& transaction :
         pending_lazy_materializations_) {
        const surface_detail::MaterializationPublicationClaim publication =
            materialization_publications_.claim(transaction);
        if (publication ==
            surface_detail::MaterializationPublicationClaim::duplicate_live_identity) {
            application_.services().publish_current_frame(runtime::RuntimeDiagnostic{
                "STRATA.UI.MATERIALIZATION_TRANSACTION_ID_DUPLICATE",
                "Two simultaneously live materialization transactions exposed identity " +
                    std::to_string(transaction->transaction_id()) +
                    "; the duplicate transaction was rejected.",
                {},
                std::string("one live owner per materialization transaction identity"),
                runtime::DiagnosticSeverity::error,
                std::nullopt,
            });
            continue;
        }
        if (publication != surface_detail::MaterializationPublicationClaim::publish) continue;
        frame.operations.evaluated_expressions += transaction->evaluated_expressions;
        frame.operations.described_nodes += transaction->described_nodes;
        for (const runtime::RuntimeDiagnostic& diagnostic : transaction->diagnostics) {
            application_.services().publish_current_frame(diagnostic);
        }
    }
    pending_lazy_materializations_.clear();
    materialization_publications_.purge_expired();

    runtime::StateScopeSet attached_state_scopes;
    for (const auto& [layer, scopes] : state_scopes_by_layer_) {
        static_cast<void>(layer);
        attached_state_scopes.insert(scopes.begin(), scopes.end());
    }
    const runtime::StateScopeSet lazy_scopes = attached_description_state_scopes(tree_);
    attached_state_scopes.insert(lazy_scopes.begin(), lazy_scopes.end());
    if (application_.state().retain_owned_scopes(attached_state_scopes) != 0U) {
        // Entries must never recreate state whose retained owner has detached.
        application_.undo().clear(id_);
    }

    std::set<std::string, std::less<>> async_owners;
    if (tree_.root() != nullptr) {
        const auto collect = [this, &async_owners](const auto& self, const RetainedNode& node) -> void {
            if (node.lifecycle() != RetainedLifecycle::attached) return;
            if (!node.description().state_scope.empty()) {
                async_owners.insert(id_ + ":state:" + node.description().state_scope);
            }
            if (node.description().key.has_value() && !node.description().key->empty()) {
                async_owners.insert(id_ + ":node:" + *node.description().key);
            }
            if (node.description().type == "TreeView") {
                const auto property = node.description().properties.find("expandedKeys");
                const runtime::Value* expanded = property != node.description().properties.end()
                    ? property->second.value() : nullptr;
                if (expanded == nullptr || expanded->list() == nullptr) {
                    expanded = node.retained_value("strata.tree.expanded");
                }
                if (expanded != nullptr && expanded->list() != nullptr) {
                    for (const runtime::Value& value : expanded->list()->values) {
                        const std::string* key = value.key() != nullptr
                            ? &value.key()->value : value.string();
                        if (key != nullptr && !key->empty()) {
                            async_owners.insert(id_ + ":item:" + *key);
                        }
                    }
                }
            }
            for (const auto& child : node.children()) self(self, *child);
        };
        collect(collect, *tree_.root());
    }
    static_cast<void>(application_.async().retain_owners(id_ + ":", async_owners));
}

void Surface::collect_input_profiler_counters(SurfaceFrame& frame) {
    const InputProfilerCounters input = input_.take_profiler_counters();
    frame.operations.input_dispatches += input.dispatches;
    frame.operations.input_coalesced_moves += input.coalesced_moves;
    frame.operations.behavior_dispatches += input.behavior_dispatches;
    frame.operations.pointer_geometry_rebuilds += input.pointer_geometry_rebuilds;
    frame.operations.input_events_processed = frame.lifecycle_input.processed_events;
    frame.operations.input_events_deferred = input_.queued_event_count();
}

void Surface::cancel_interactions() {
    scroll_animations_.clear();
    append_input(pending_lifecycle_input_, input_.cancel_interactions());
    invalidate_frame();
}

runtime::ActionDispatchOutcome Surface::dispatch_action(
    std::string action_id,
    runtime::Value payload,
    std::string event_kind,
    std::optional<std::string> source_key,
    runtime::Value event_value,
    const bool dynamic
) {
    InjectedActionResult injected = input_.dispatch_action(
        std::move(action_id),
        std::move(payload),
        std::move(event_kind),
        std::move(source_key),
        std::move(event_value),
        dynamic
    );
    runtime::ActionDispatchOutcome outcome = std::move(injected.outcome);
    append_input(pending_lifecycle_input_, std::move(injected.input));
    return outcome;
}

bool Surface::set_focus_containment(const std::optional<std::string_view> key) {
    InputOperationResult result;
    const bool contained = input_.set_focus_containment(key, result);
    append_input(pending_lifecycle_input_, std::move(result));
    invalidate_frame();
    return contained;
}

bool Surface::focus_contained() const noexcept { return input_.focus_contained(); }

SurfaceFrame Surface::frame(const std::int64_t frame_time_nanos) {
    if (last_frame_.frame_index != 0U && frame_time_nanos < last_frame_.frame_time_nanos) {
        throw std::invalid_argument("surface frame clock must be monotonic");
    }
    if (last_frame_.frame_index == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("surface frame index exhausted");
    }
    pending_lazy_materializations_.clear();
    apply_pending_resource_invalidation();
    auto profiler_frame = profiler_.frame(last_frame_.frame_index + 1U);
    if (pending_resource_reloads_ != 0U && !pending_reload_timing_recorded_) {
        profiler_.record_counters({
            {runtime::ProfilerCounter::resource_reloads, pending_resource_reloads_},
            {runtime::ProfilerCounter::reload_duration_nanos, pending_reload_duration_nanos_},
        });
        profiler_.record_external_timing(
            "resource-reload",
            static_cast<std::int64_t>(std::min<std::uint64_t>(
                pending_reload_duration_nanos_,
                static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
            ))
        );
        pending_reload_timing_recorded_ = true;
    }
    status_feedback_.advance_frame(frame_time_nanos);
    notifications_.advance_frame(frame_time_nanos);
    input_.publish_frame_time(frame_time_nanos);
    application_.async().advance(frame_time_nanos);
    if (declarative_environment_pending_) {
        const std::string resolved = resolved_viewport_class(environment_);
        if (resolved != viewport_class_) {
            if (environment_.generation == std::numeric_limits<std::uint64_t>::max()) {
                throw std::overflow_error("surface environment generation exhausted");
            }
            ++environment_.generation;
            viewport_class_ = resolved;
            invalidate_description();
        }
        declarative_environment_pending_ = false;
    }
    const runtime::RuntimeGenerationSnapshot service_generations =
        application_.services().generations();
    const bool pending_lifecycle = !pending_lifecycle_input_.events.empty() ||
        !pending_lifecycle_input_.action_outcomes.empty() ||
        pending_lifecycle_input_.injected_events != 0U ||
        pending_lifecycle_input_.processed_events != 0U;
    const bool requested_frame = std::exchange(frame_invalidated_, false);
    const bool settled_except_input = last_frame_.frame_index != 0U && !requested_frame &&
        !description_invalidated_ &&
        tree_.root() != nullptr &&
        application_.dirty_generation() == observed_application_generation_ &&
        service_generations == observed_service_generations_ && tree_.dirty_count() == 0U &&
        !input_.requires_frame_advance() && !pending_lifecycle &&
        scroll_animations_.empty() && motion_.active_count() == 0U &&
        layout_engine_.active_transition_count() == 0U &&
        !application_.services().has_pending_frame_work();
    if (settled_except_input && input_.queued_event_count() == 0U) {
        SurfaceFrame next;
        next.frame_index = last_frame_.frame_index + 1U;
        next.frame_time_nanos = frame_time_nanos;
        next.operations.resource_reloads = pending_resource_reloads_;
        next.operations.reload_duration_nanos = pending_reload_duration_nanos_;
        {
            auto services_profile = profiler_.section("runtime-services");
            RuntimeFrameGuard runtime_frame(application_.services());
            application_.flush_durable();
            next.diagnostics = application_.services().diagnostics_snapshot();
            record_profiler_counters(next);
        }
        record_profiler_counters(next);
        last_frame_ = std::move(next);
        pending_resource_reloads_ = 0U;
        pending_reload_duration_nanos_ = 0U;
        pending_reload_timing_recorded_ = false;
        return last_frame_;
    }
    SurfaceFrame next;
    if (text_engine_ != nullptr) {
        const runtime::RuntimeGenerationSnapshot generations =
            application_.services().generations();
        text_engine_->adopt_generations(
            scale_context_generation_,
            generations.style_resources,
            generations.font_resources
        );
        text_engine_->begin_frame();
    }
    next.frame_index = last_frame_.frame_index + 1U;
    next.frame_time_nanos = frame_time_nanos;
    next.operations.resource_reloads = pending_resource_reloads_;
    next.operations.reload_duration_nanos = pending_reload_duration_nanos_;
    RuntimeFrameGuard runtime_frame(application_.services());
    application_.flush_durable();
    {
        auto services_profile = profiler_.section("runtime-services");
        next.diagnostics = application_.services().diagnostics_snapshot();
        record_profiler_counters(next);
    }
    bool input_processed_before_layout = false;
    bool processed_queued_input = false;
    const auto process_queued_input = [this, &next, &processed_queued_input] {
        const auto input_started = std::chrono::steady_clock::now();
        const std::size_t dirty_before = tree_.dirty_count();
        auto input_profile = profiler_.section("input");
        InputOperationResult queued_input = input_.process_queued();
        processed_queued_input = queued_input.processed_events != 0U;
        append_input(next.lifecycle_input, std::move(queued_input));
        const std::size_t dirty_after = tree_.dirty_count();
        next.operations.input_mutated_nodes += dirty_after > dirty_before
            ? dirty_after - dirty_before
            : 0U;
        next.operations.input_nanos += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - input_started
            ).count()
        );
        collect_input_profiler_counters(next);
        record_profiler_counters(next);
        input_profile.close();
    };
    if (settled_except_input) {
        input_processed_before_layout = true;
        process_queued_input();
        if (processed_queued_input && !frame_invalidated_ && !description_invalidated_ &&
            tree_.dirty_count() == 0U &&
            application_.dirty_generation() == observed_application_generation_ &&
            application_.services().generations() == observed_service_generations_ &&
            !layout_engine_.requires_layout(
                tree_,
                environment_.layout_environment(frame_time_nanos)
            ) &&
            !input_.requires_frame_advance() && scroll_animations_.empty() &&
            motion_.active_count() == 0U && layout_engine_.active_transition_count() == 0U &&
            !application_.services().has_pending_frame_work()) {
            next.operations.injected_events = next.lifecycle_input.injected_events;
            next.operations.input_fast_path_frames = 1U;
            return complete_frame(std::move(next));
        }
    }
    advance_scroll_animations(frame_time_nanos);
    motion_.bind(application_.active_unit());
    std::optional<std::string_view> restore_focus;
    static_cast<void>(rebuild_tree(next, restore_focus));
    {
        const auto input_started = std::chrono::steady_clock::now();
        const std::size_t dirty_before = tree_.dirty_count();
        auto input_profile = profiler_.section("input");
        append_input(next.lifecycle_input, std::move(pending_lifecycle_input_));
        pending_lifecycle_input_ = InputOperationResult{};
        append_input(next.lifecycle_input, input_.prepare(tree_, restore_focus));
        append_input(next.lifecycle_input, input_.advance_frame());
        const std::size_t dirty_after = tree_.dirty_count();
        next.operations.input_mutated_nodes += dirty_after > dirty_before
            ? dirty_after - dirty_before
            : 0U;
        next.operations.input_nanos += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - input_started
            ).count()
        );
        collect_input_profiler_counters(next);
        record_profiler_counters(next);
        input_profile.close();
    }
    layout_tree(next, frame_time_nanos);
    input_.publish_layout(layout_engine_.result());
    const std::uint64_t pre_input_layout_generation = tree_.layout_invalidation_generation();
    if (!input_processed_before_layout) process_queued_input();

    std::optional<std::string_view> input_restore_focus;
    const bool input_rebuilt = rebuild_tree(next, input_restore_focus);
    if (input_rebuilt) {
        append_input(next.lifecycle_input, input_.prepare(tree_, input_restore_focus));
    }
    // Queued pointer/key processing can change interaction channels after the first layout pass.
    // Discover those targets at the current timestamp without advancing active animation clocks.
    if ((!input_processed_before_layout && processed_queued_input) || input_rebuilt) {
        sample_motion(next, frame_time_nanos, false);
    }
    if (input_rebuilt || (!input_processed_before_layout &&
        tree_.layout_invalidation_generation() != pre_input_layout_generation)) {
        layout_tree(next, frame_time_nanos);
    }
    tree_.consume_layout_dirty();
    const LayoutResult& laid_out = layout_engine_.result();
    input_.publish_motion(motion_);
    input_.publish_layout(laid_out);
    {
        const auto input_started = std::chrono::steady_clock::now();
        const std::size_t dirty_before = tree_.dirty_count();
        auto after_layout_profile = profiler_.section("input");
        append_input(next.lifecycle_input, input_.after_layout());
        const std::size_t dirty_after = tree_.dirty_count();
        next.operations.input_mutated_nodes += dirty_after > dirty_before
            ? dirty_after - dirty_before
            : 0U;
        next.operations.input_nanos += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - input_started
            ).count()
        );
        collect_input_profiler_counters(next);
        record_profiler_counters(next);
        after_layout_profile.close();
    }
    next.operations.injected_events = next.lifecycle_input.injected_events;
    {
        auto semantics_profile = profiler_.section("semantics");
        commands_.rebuild(tree_);
        input_.publish_commands(commands_);
        static_cast<void>(semantics_.update(tree_, commands_, input_, id_));
        record_profiler_counters(next);
    }
    {
        auto render_profile = profiler_.section("render");
        next.operations.render = render_engine_.render(
            tree_,
            laid_out,
            input_,
            commands_,
            widgets_,
            behaviors_,
            motion_,
            text_engine_.get(),
            svg_images_.get(),
            material_registry_,
            RenderGenerationToken{
                scale_context_generation_,
                application_.active_generation().value_or(0U),
                application_.services().generations()
            },
            render_commands_
        );
        if (const RetainedNode* inspected = inspected_node(); inspected != nullptr) {
            if (const LayoutRecord* record = laid_out.find(inspected->identity()); record != nullptr) {
                render_commands_.append(BorderRenderCommand{
                    record->bounds,
                    RenderBorder{2.0, RenderColor{255U, 191U, 76U, 255U}, true},
                    CornerRadii::all(3.0),
                });
                if (record->clip.has_value()) {
                    render_commands_.append(BorderRenderCommand{
                        *record->clip,
                        RenderBorder{1.0, RenderColor{99U, 179U, 255U, 210U}, true},
                        CornerRadii{},
                    });
                }
                MotionTransform transform;
                std::vector<const RetainedNode*> route;
                for (const RetainedNode* current = inspected; current != nullptr;
                     current = current->parent()) {
                    route.push_back(current);
                }
                for (auto current = route.rbegin(); current != route.rend(); ++current) {
                    const LayoutRecord* current_layout =
                        laid_out.find((*current)->identity());
                    if (current_layout == nullptr) continue;
                    transform = concatenate_presentation_transform(
                        transform,
                        local_presentation_transform(
                            **current,
                            motion_,
                            current_layout->bounds
                        )
                    );
                }
                const Rect hit_bounds = transform_presentation_bounds(
                    record->hit_bounds, transform
                );
                if (hit_bounds != record->bounds) {
                    render_commands_.append(BorderRenderCommand{
                        hit_bounds,
                        RenderBorder{1.0, RenderColor{232U, 121U, 249U, 220U}, true},
                        CornerRadii{},
                    });
                }
                next.operations.render.commands_emitted = render_commands_.size();
            }
        }
        if (text_engine_ != nullptr) next.operations.text = text_engine_->operation_counters();
        // Freeze the phase-relevant frame state before a render spike closes and snapshots it.
        record_profiler_counters(next);
    }
    return complete_frame(std::move(next));
}

SurfaceFrame Surface::complete_frame(SurfaceFrame next) {
    application_.flush_durable();
    if (text_engine_ != nullptr) next.operations.text = text_engine_->operation_counters();
    if (text_engine_ != nullptr) {
        for (runtime::RuntimeDiagnostic& diagnostic : text_engine_->take_diagnostics()) {
            application_.services().report(std::move(diagnostic));
        }
    }
    for (runtime::RuntimeDiagnostic& diagnostic : layout_engine_.take_diagnostics()) {
        application_.services().report(std::move(diagnostic));
    }
    for (runtime::RuntimeDiagnostic& diagnostic : semantics_.take_diagnostics()) {
        application_.services().report(std::move(diagnostic));
    }
    for (runtime::RuntimeDiagnostic& diagnostic : input_.take_diagnostics()) {
        application_.services().report(std::move(diagnostic));
    }
    next.diagnostics = application_.services().diagnostics_snapshot();
    record_profiler_counters(next);
    tree_.clear_dirty();
    observed_service_generations_ = application_.services().generations();
    frame_invalidated_ = false;
    const std::size_t publication_count = std::max(
        next.lifecycle_input.events.size(),
        next.lifecycle_input.action_outcomes.size()
    );
    constexpr std::size_t maximum_published_events = 512U;
    for (std::size_t index = 0U; index < publication_count; ++index) {
        if (next_event_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
            throw std::overflow_error("surface event publication sequence exhausted");
        }
        published_events_.push_back(SurfaceEventRecord{
            next_event_sequence_++,
            next.frame_index,
            index < next.lifecycle_input.events.size()
                ? next.lifecycle_input.events[index]
                : data::JsonValue{},
            index < next.lifecycle_input.action_outcomes.size()
                ? next.lifecycle_input.action_outcomes[index]
                : data::JsonValue{},
        });
        if (published_events_.size() > maximum_published_events) {
            published_events_.pop_front();
            ++dropped_event_count_;
        }
    }
    last_frame_ = std::move(next);
    pending_resource_reloads_ = 0U;
    pending_reload_duration_nanos_ = 0U;
    pending_reload_timing_recorded_ = false;
    return last_frame_;
}

void Surface::record_profiler_counters(const SurfaceFrame& frame) {
    const SurfaceOperationCounters& value = frame.operations;
    profiler_.record_counters({
        {runtime::ProfilerCounter::described_nodes, value.described_nodes},
        {runtime::ProfilerCounter::evaluated_expressions, value.evaluated_expressions},
        {runtime::ProfilerCounter::rebuilds, value.rebuilds},
        {runtime::ProfilerCounter::input_nanos, value.input_nanos},
        {runtime::ProfilerCounter::input_events_processed, value.input_events_processed},
        {runtime::ProfilerCounter::input_events_deferred, value.input_events_deferred},
        {runtime::ProfilerCounter::input_dispatches, value.input_dispatches},
        {runtime::ProfilerCounter::input_mutated_nodes, value.input_mutated_nodes},
        {runtime::ProfilerCounter::input_coalesced_moves, value.input_coalesced_moves},
        {runtime::ProfilerCounter::ui_behavior_dispatches, value.behavior_dispatches},
        {runtime::ProfilerCounter::input_pointer_geometry_rebuilds,
         value.pointer_geometry_rebuilds},
        {runtime::ProfilerCounter::input_fast_path_frames, value.input_fast_path_frames},
        {runtime::ProfilerCounter::injected_events, value.injected_events},
        {runtime::ProfilerCounter::animation_nanos, value.animation_nanos},
        {runtime::ProfilerCounter::animation_mutated_nodes, value.motion_mutated_nodes},
        {runtime::ProfilerCounter::animation_running_players, value.motion_running_players},
        {runtime::ProfilerCounter::layout_nanos, value.layout_nanos},
        {runtime::ProfilerCounter::layout_measured_nodes, value.layout_measured_nodes},
        {runtime::ProfilerCounter::layout_arranged_nodes, value.layout_arranged_nodes},
        {runtime::ProfilerCounter::layout_translated_nodes,
         value.layout_translated_nodes},
        {runtime::ProfilerCounter::layout_reused_nodes, value.layout_measurement_cache_hits},
        {runtime::ProfilerCounter::resource_reloads, value.resource_reloads},
        {runtime::ProfilerCounter::reload_duration_nanos, value.reload_duration_nanos},
        {runtime::ProfilerCounter::text_layout_requests, value.text.requests},
        {runtime::ProfilerCounter::text_layout_cache_hits, value.text.cache_hits},
        {runtime::ProfilerCounter::text_layout_cache_misses, value.text.cache_misses},
        {runtime::ProfilerCounter::text_layout_cache_lookup_nanos, value.text.cache_lookup_nanos},
        {runtime::ProfilerCounter::text_layout_cache_restore_nanos, value.text.cache_restore_nanos},
        {runtime::ProfilerCounter::text_layout_cache_store_nanos, value.text.cache_store_nanos},
        {runtime::ProfilerCounter::text_shaping_nanos, value.text.shaping_nanos},
        {runtime::ProfilerCounter::text_font_resolution_nanos,
         value.text.font_resolution_nanos},
        {runtime::ProfilerCounter::text_opentype_nanos, value.text.opentype_nanos},
        {runtime::ProfilerCounter::text_line_assembly_nanos,
         value.text.line_assembly_nanos},
        {runtime::ProfilerCounter::render_commands, value.render.commands_emitted},
        {runtime::ProfilerCounter::render_fragments_built, value.render.fragments_built},
        {runtime::ProfilerCounter::render_fragments_reused, value.render.fragments_reused},
        {runtime::ProfilerCounter::render_nodes_visited, value.render.nodes_visited},
        {runtime::ProfilerCounter::render_overlays, value.render.overlays_rendered},
        {runtime::ProfilerCounter::render_portals, value.render.portals_rendered},
        {runtime::ProfilerCounter::render_retained_subtrees_reused,
         value.render.retained_subtrees_reused},
        {runtime::ProfilerCounter::render_retained_subtrees_translated,
         value.render.retained_subtrees_translated},
        {runtime::ProfilerCounter::diagnostics, frame.diagnostics.records.size()},
    });
}

SurfaceEventDrain Surface::drain_events() {
    SurfaceEventDrain result;
    result.records.reserve(published_events_.size());
    while (!published_events_.empty()) {
        result.records.push_back(std::move(published_events_.front()));
        published_events_.pop_front();
    }
    result.dropped_count = std::exchange(dropped_event_count_, 0U);
    return result;
}

void Surface::report_diagnostic(runtime::RuntimeDiagnostic diagnostic) {
    application_.services().report(std::move(diagnostic));
}

void Surface::clear_diagnostics() noexcept {
    input_.clear_diagnostics();
    layout_engine_.clear_diagnostics();
    semantics_.clear_diagnostics();
    material_registry_.clear_diagnostics();
    if (text_engine_ != nullptr) text_engine_->clear_diagnostics();
    reported_theme_motion_diagnostics_.clear();
    reported_lazy_convergence_diagnostics_.clear();
    last_frame_.diagnostics = application_.services().diagnostics_snapshot();
}

const RetainedTree& Surface::tree() const noexcept { return tree_; }
RetainedTree& Surface::tree() noexcept { return tree_; }
const LayoutResult& Surface::layout() const noexcept { return layout_engine_.result(); }
const SurfaceFrame& Surface::last_frame() const noexcept { return last_frame_; }
InputRouter& Surface::input() noexcept { return input_; }
const InputRouter& Surface::input() const noexcept { return input_; }
const SemanticsEngine& Surface::semantics() const noexcept { return semantics_; }
const CommandIndex& Surface::commands() const noexcept { return commands_; }
const WidgetRegistry& Surface::widget_registry() const noexcept { return widgets_; }
const BehaviorRegistry& Surface::behavior_registry() const noexcept { return behaviors_; }
const MotionRuntime& Surface::motion() const noexcept { return motion_; }
const TextEngine* Surface::text_engine() const noexcept { return text_engine_.get(); }
const RenderCommandBuffer& Surface::render_commands() const noexcept { return render_commands_; }

std::vector<runtime::LayerSnapshot> Surface::layer_snapshot() const {
    const std::vector<runtime::LayerSnapshot> application_layers = application_.layers().snapshot();
    std::vector<runtime::LayerSnapshot> result;
    if (!application_.layers().root_replaced()) {
        result.push_back(runtime::LayerSnapshot{
            "root:" + id_, runtime::LayerRole::screen, std::nullopt
        });
    }
    result.insert(result.end(), application_layers.begin(), application_layers.end());
    return result;
}

bool Surface::inspect_select(const std::string_view key) {
    const RetainedNode* node = tree_.find_key(key);
    if (node == nullptr) return false;
    const bool changed = inspected_identity_ != node->identity();
    inspected_identity_ = node->identity();
    if (changed) invalidate_frame();
    return true;
}

bool Surface::inspect_pick(const Point position) {
    const RetainedNode* node = input_.inspection_target(position);
    if (node == nullptr) return false;
    const bool changed = inspected_identity_ != node->identity();
    inspected_identity_ = node->identity();
    if (changed) invalidate_frame();
    return true;
}

void Surface::inspect_clear() noexcept {
    if (!inspected_identity_.has_value()) return;
    inspected_identity_.reset();
    invalidate_frame();
}

const RetainedNode* Surface::inspected_node() const noexcept {
    return inspected_identity_.has_value()
        ? tree_.find_identity(*inspected_identity_)
        : nullptr;
}

std::string_view surface_density_name(const SurfaceDensity value) noexcept {
    switch (value) {
    case SurfaceDensity::compact: return "compact";
    case SurfaceDensity::comfortable: return "comfortable";
    }
    return "comfortable";
}

std::string_view pointer_precision_name(const PointerPrecision value) noexcept {
    switch (value) {
    case PointerPrecision::none: return "none";
    case PointerPrecision::coarse: return "coarse";
    case PointerPrecision::fine: return "fine";
    }
    return "none";
}

} // namespace strata::ui
