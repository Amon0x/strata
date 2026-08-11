#pragma once

/**
 * Typed C++ authoring layer for native Strata extension packages.
 *
 * One package definition owns widget/behavior identity, typed `.strata` parameters, retained
 * fields, lifecycle hooks, and emitted action contracts. The same definition projects both the
 * runtime extension bundle consumed by `strata_surface_config::extensions` and the compiler schema
 * consumed by `--check-module`, so a widget name or parameter is never copied by hand.
 *
 * The layer is a thin projection over the C ABI in <strata/strata.h>: every builder field maps to
 * one descriptor field, and every facade call maps to one `strata_widget_*` entry point. It adds no
 * runtime of its own, allocates nothing after `Package::bundle()` is first taken, and keeps hooks
 * as plain function pointers so no per-callback state is captured.
 */

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <strata/extension_plugin.h>
#include <strata/strata.h>

namespace strata::extension {

using Rect = strata_rect;
struct Point final {
    double x = 0.0;
    double y = 0.0;
};
using Color = strata_color;
using Border = strata_border;

[[nodiscard]] constexpr Color rgba(const unsigned char red, const unsigned char green,
                                   const unsigned char blue,
                                   const unsigned char alpha = 255U) noexcept {
    return Color{red, green, blue, alpha};
}

[[nodiscard]] constexpr Border stroke(const double width, const Color color) noexcept {
    return Border{width, color};
}

using CornerRadii = strata_corner_radii;
using Edges = strata_edges;
using TextureRegion = strata_texture_region;
using MeshVertex = strata_mesh_vertex;

[[nodiscard]] constexpr CornerRadii corners(const double radius) noexcept {
    return CornerRadii{radius, radius, radius, radius};
}

/** Whole-image source region; narrow it to sample one sprite out of a raster atlas. */
[[nodiscard]] constexpr TextureRegion whole_texture() noexcept {
    return TextureRegion{0.0, 0.0, 1.0, 1.0};
}

/**
 * Non-owning geometry handed to one custom-mesh draw and copied by the engine. Vertex x and y are
 * normalized inside the draw bounds; indices must form triangles. A mesh that violates either is
 * dropped instead of failing the frame.
 */
struct Mesh final {
    std::span<const MeshVertex> vertices;
    std::span<const std::uint32_t> indices;
};

/**
 * Fixed-capacity custom geometry accumulator. Appended indices are rebased, and a failed append
 * leaves the batch unchanged. The batch owns no heap storage and its geometry view remains valid
 * until the next mutation.
 */
template <std::size_t VertexCapacity, std::size_t IndexCapacity> class MeshBatch final {
  public:
    static_assert(VertexCapacity <= UINT32_MAX);

    [[nodiscard]] bool append(const std::span<const MeshVertex> vertices,
                              const std::span<const std::uint32_t> indices) noexcept {
        if (vertices.size() > VertexCapacity - vertex_count_ ||
            indices.size() > IndexCapacity - index_count_) {
            return false;
        }
        for (const std::uint32_t index : indices) {
            if (index >= vertices.size())
                return false;
        }
        const auto base = static_cast<std::uint32_t>(vertex_count_);
        for (const MeshVertex& vertex : vertices)
            vertices_[vertex_count_++] = vertex;
        for (const std::uint32_t index : indices)
            indices_[index_count_++] = base + index;
        return true;
    }

    void clear() noexcept {
        vertex_count_ = 0U;
        index_count_ = 0U;
    }
    [[nodiscard]] std::size_t vertex_count() const noexcept {
        return vertex_count_;
    }
    [[nodiscard]] std::size_t index_count() const noexcept {
        return index_count_;
    }
    [[nodiscard]] Mesh geometry() const noexcept {
        return Mesh{
            std::span<const MeshVertex>(vertices_.data(), vertex_count_),
            std::span<const std::uint32_t>(indices_.data(), index_count_),
        };
    }

  private:
    std::array<MeshVertex, VertexCapacity> vertices_{};
    std::array<std::uint32_t, IndexCapacity> indices_{};
    std::size_t vertex_count_ = 0U;
    std::size_t index_count_ = 0U;
};

/**
 * Affine world-to-surface projection for canvas-like extension widgets. Scale is expressed in
 * surface pixels per world unit; origin is the world point at the viewport's leading/top edge.
 */
struct CanvasTransform final {
    Rect viewport{};
    Point origin{};
    Point scale{1.0, 1.0};

    [[nodiscard]] Point project(const Point world) const noexcept {
        return Point{
            viewport.x + (world.x - origin.x) * valid_scale(scale.x),
            viewport.y + (world.y - origin.y) * valid_scale(scale.y),
        };
    }
    [[nodiscard]] Point unproject(const Point surface) const noexcept {
        return Point{
            origin.x + (surface.x - viewport.x) / valid_scale(scale.x),
            origin.y + (surface.y - viewport.y) / valid_scale(scale.y),
        };
    }
    [[nodiscard]] Rect project(const Rect world) const noexcept {
        const Point projected = project(Point{world.x, world.y});
        return Rect{
            projected.x,
            projected.y,
            world.width * valid_scale(scale.x),
            world.height * valid_scale(scale.y),
        };
    }
    [[nodiscard]] Rect visible_world() const noexcept {
        const Point start = unproject(Point{viewport.x, viewport.y});
        return Rect{
            start.x,
            start.y,
            viewport.width / valid_scale(scale.x),
            viewport.height / valid_scale(scale.y),
        };
    }
    void pan(const Point surface_delta) noexcept {
        origin.x -= surface_delta.x / valid_scale(scale.x);
        origin.y -= surface_delta.y / valid_scale(scale.y);
    }
    void zoom(const Point surface_anchor, const Point factor,
              const Point minimum_scale = Point{1.0e-4, 1.0e-4},
              const Point maximum_scale = Point{1.0e4, 1.0e4}) noexcept {
        const Point world_anchor = unproject(surface_anchor);
        const double factor_x = valid_factor(factor.x);
        const double factor_y = valid_factor(factor.y);
        scale.x = std::clamp(valid_scale(scale.x) * factor_x, valid_scale(minimum_scale.x),
                             valid_scale(maximum_scale.x));
        scale.y = std::clamp(valid_scale(scale.y) * factor_y, valid_scale(minimum_scale.y),
                             valid_scale(maximum_scale.y));
        origin.x = world_anchor.x - (surface_anchor.x - viewport.x) / scale.x;
        origin.y = world_anchor.y - (surface_anchor.y - viewport.y) / scale.y;
    }

  private:
    [[nodiscard]] static double valid_scale(const double value) noexcept {
        return std::isfinite(value) && value > 1.0e-9 ? value : 1.0;
    }
    [[nodiscard]] static double valid_factor(const double value) noexcept {
        return std::isfinite(value) && value > 0.0 ? value : 1.0;
    }
};

/** Typed material parameter; build them with the number/boolean/text/color helpers below. */
using MaterialParameter = strata_material_parameter;

[[nodiscard]] inline MaterialParameter material_number(const std::string_view name,
                                                       const double value) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_NUMBER;
    parameter.number = value;
    return parameter;
}

[[nodiscard]] inline MaterialParameter material_boolean(const std::string_view name,
                                                        const bool value) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_BOOLEAN;
    parameter.boolean_value = value ? 1U : 0U;
    return parameter;
}

[[nodiscard]] inline MaterialParameter material_text(const std::string_view name,
                                                     const std::string_view value) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_TEXT;
    parameter.text = strata_string_view{value.data(), value.size()};
    return parameter;
}

[[nodiscard]] inline MaterialParameter material_color(const std::string_view name,
                                                      const Color value) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_COLOR;
    parameter.color = value;
    return parameter;
}

/**
 * Material selected for one custom-mesh draw. The id must be an application material contract;
 * an unknown id or parameter is dropped from the packet with a diagnostic.
 */
struct Material final {
    std::string_view id;
    std::span<const MaterialParameter> parameters;
    std::string_view blend_mode;
    double opacity = 1.0;
};

/** Frame work invalidated when an extension writes one declared retained field. */
enum class Invalidation { properties, layout, style, text, semantics, paint, input };

/** Typed `.strata` parameter kinds understood by the compiler schema. */
struct number final {
    using value_type = double;
    static constexpr std::string_view schema_kind{"number"};
};
struct text final {
    using value_type = std::string_view;
    static constexpr std::string_view schema_kind{"string"};
};
struct boolean final {
    using value_type = bool;
    static constexpr std::string_view schema_kind{"boolean"};
};
struct color final {
    using value_type = std::monostate;
    static constexpr std::string_view schema_kind{"color"};
};
struct action final {
    using value_type = std::monostate;
    static constexpr std::string_view schema_kind{"action"};
};
struct any final {
    using value_type = std::monostate;
    static constexpr std::string_view schema_kind{"any"};
};

/**
 * Fixed-capacity, trivially-copyable widget state. The engine allocates node-local storage once;
 * equal-size writes reuse it, making this suitable for bounded compound-control collections.
 */
template <typename T>
    requires(std::is_trivially_copyable_v<T> && std::is_standard_layout_v<T>)
struct structured final {
    using value_type = T;
    static constexpr std::string_view schema_kind{"any"};
};

/**
 * Typed field declarations. A field is written once as a constexpr value naming itself, its type,
 * and its default; the widget consumes it and every hook reads it through the same handle, so a
 * name cannot be mistyped and a default cannot be restated at a call site.
 */
template <typename Kind> struct Retained final {
    std::string_view name;
    typename Kind::value_type fallback{};
    Invalidation invalidation = Invalidation::properties;
};

template <typename Kind> struct Parameter final {
    std::string_view name;
    std::optional<typename Kind::value_type> value{};
    bool required = false;
};

template <typename Kind>
[[nodiscard]] constexpr Retained<Kind>
retained(const std::string_view name, const typename Kind::value_type fallback = {},
         const Invalidation invalidation = Invalidation::properties) noexcept {
    return Retained<Kind>{name, fallback, invalidation};
}

template <typename Kind>
[[nodiscard]] constexpr Parameter<Kind> parameter(const std::string_view name) noexcept {
    return Parameter<Kind>{name, std::nullopt, false};
}

template <typename Kind>
[[nodiscard]] constexpr Parameter<Kind> parameter(const std::string_view name,
                                                  const typename Kind::value_type value) noexcept {
    return Parameter<Kind>{name, value, false};
}

template <typename Kind>
[[nodiscard]] constexpr Parameter<Kind> required_parameter(const std::string_view name) noexcept {
    return Parameter<Kind>{name, std::nullopt, true};
}

/** Committed key press delivered to a focused extension widget. */
struct Key final {
    std::string_view name;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
};

struct Pointer final {
    enum class Phase { capture, target, bubble };
    enum class Kind { move, press, release, cancel };

    Kind kind = Kind::move;
    Phase phase = Phase::target;
    int button = 0;
    int pointer_id = 0;
    double x = 0.0;
    double y = 0.0;
    double local_x = 0.0;
    double local_y = 0.0;
    double delta_x = 0.0;
    double delta_y = 0.0;
    long long timestamp_nanoseconds = 0;
    bool on_target = false;
    bool has_local_position = false;
    bool shift = false;
    std::string_view subtarget_id;
    std::size_t subtarget_index = SIZE_MAX;
    bool control = false;
    bool alt = false;
    bool super = false;
};
struct Scroll final {
    Pointer::Phase phase = Pointer::Phase::target;
    double x = 0.0;
    double y = 0.0;
    double local_x = 0.0;
    double local_y = 0.0;
    double delta_x = 0.0;
    double delta_y = 0.0;
    bool on_target = false;
    bool shift = false;
    bool control = false;
    bool alt = false;
    bool super = false;
};

struct Size final {
    double width = 0.0;
    double height = 0.0;
};

enum class TextAlignment : std::uint32_t {
    start = STRATA_WIDGET_TEXT_ALIGN_START,
    center = STRATA_WIDGET_TEXT_ALIGN_CENTER,
    end = STRATA_WIDGET_TEXT_ALIGN_END,
};

enum class FrameCost : std::uint32_t {
    paint = STRATA_WIDGET_FRAME_PAINT,
    layout = STRATA_WIDGET_FRAME_LAYOUT,
};

struct Frame final {
    long long time_nanoseconds = 0;
    long long delta_nanoseconds = 0;
    bool reduced_motion = false;
};

/** Borrowed immutable parameter/style value; valid only during the current lifecycle callback. */
class ValueView final {
  public:
    enum class Kind {
        null_value,
        boolean,
        number,
        duration,
        text,
        color,
        image,
        key,
        theme_token,
        list,
        object,
    };

    constexpr ValueView() noexcept = default;
    explicit constexpr ValueView(const strata_widget_value* const value) noexcept : value_(value) {}

    [[nodiscard]] Kind kind() const noexcept {
        return static_cast<Kind>(strata_widget_value_get_kind(value_));
    }
    [[nodiscard]] bool boolean(bool fallback = false) const noexcept {
        return strata_widget_value_get_boolean(value_, fallback ? 1U : 0U) != 0U;
    }
    [[nodiscard]] double number(double fallback = 0.0) const noexcept {
        return strata_widget_value_get_number(value_, fallback);
    }
    [[nodiscard]] std::optional<Color> color() const noexcept {
        Color result{};
        return strata_widget_value_get_color(value_, &result) != 0U ? std::optional<Color>(result)
                                                                    : std::nullopt;
    }
    [[nodiscard]] std::optional<std::string_view> text() const noexcept {
        const Kind value_kind = kind();
        if (value_kind != Kind::text && value_kind != Kind::image && value_kind != Kind::key &&
            value_kind != Kind::theme_token) {
            return std::nullopt;
        }
        const strata_string_view result = strata_widget_value_get_text(value_);
        return std::string_view(result.data != nullptr ? result.data : "", result.size);
    }
    [[nodiscard]] std::size_t size() const noexcept {
        return strata_widget_value_list_size(value_);
    }
    [[nodiscard]] ValueView at(const std::size_t index) const noexcept {
        return ValueView(strata_widget_value_list_at(value_, index));
    }
    [[nodiscard]] ValueView field(const std::string_view name) const noexcept {
        return ValueView(
            strata_widget_value_object_field(value_, strata_string_view{name.data(), name.size()}));
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return value_ != nullptr;
    }

  private:
    const strata_widget_value* value_ = nullptr;
};

/** Input capability of one widget activation or key press. */
struct Subtarget final {
    std::string_view id;
    std::size_t index = SIZE_MAX;
    Rect bounds{};
    int z_index = 0;
    bool enabled = true;
    bool semantic = false;
};

/** Projection of stable widget-owned hit regions from retained state and current layout. */
class Subtargets final {
  public:
    explicit Subtargets(strata_widget_subtargets_context* const context) noexcept
        : context_(context) {}

    [[nodiscard]] Rect bounds() const noexcept {
        return strata_widget_subtargets_bounds(context_);
    }
    template <typename T> [[nodiscard]] T get(const Retained<structured<T>>& field) const noexcept {
        T value = field.fallback;
        static_cast<void>(strata_widget_subtargets_retained_bytes(
            context_, strata_string_view{field.name.data(), field.name.size()}, &value,
            sizeof(value)));
        return value;
    }
    [[nodiscard]] ValueView get(const Parameter<any>& field) const noexcept {
        return ValueView(strata_widget_subtargets_property_value(
            context_, strata_string_view{field.name.data(), field.name.size()}));
    }
    [[nodiscard]] std::optional<Color> get(const Parameter<color>& field) const noexcept {
        return ValueView(strata_widget_subtargets_property_value(
                             context_, strata_string_view{field.name.data(), field.name.size()}))
            .color();
    }
    bool reserve(const std::size_t capacity) noexcept {
        return strata_widget_subtargets_reserve(context_, capacity).status == STRATA_STATUS_OK;
    }
    bool add(const Subtarget& target) noexcept {
        const strata_widget_subtarget descriptor{
            sizeof(strata_widget_subtarget),
            strata_string_view{target.id.data(), target.id.size()},
            target.bounds,
            target.z_index,
            target.enabled ? 1U : 0U,
            target.index,
            target.semantic ? STRATA_WIDGET_SUBTARGET_ITEM : STRATA_WIDGET_SUBTARGET_CONTROL,
            0U,
        };
        return strata_widget_subtargets_add(context_, &descriptor).status == STRATA_STATUS_OK;
    }

  private:
    strata_widget_subtargets_context* context_;
};

class Input final {
  public:
    explicit Input(strata_widget_input_context* const context) noexcept : context_(context) {}

    [[nodiscard]] double get(const Retained<number>& field) const noexcept;
    [[nodiscard]] bool get(const Retained<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Retained<text>& field) const;
    template <typename T> [[nodiscard]] T get(const Retained<structured<T>>& field) const noexcept {
        T value = field.fallback;
        static_cast<void>(strata_widget_input_retained_bytes(
            context_, strata_string_view{field.name.data(), field.name.size()}, &value,
            sizeof(value)));
        return value;
    }
    [[nodiscard]] double get(const Parameter<number>& field) const noexcept;
    [[nodiscard]] bool get(const Parameter<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Parameter<text>& field) const;
    template <typename Kind> [[nodiscard]] bool has(const Parameter<Kind>& field) const noexcept {
        return strata_widget_input_has_property(
                   context_, strata_string_view{field.name.data(), field.name.size()}) != 0U;
    }
    [[nodiscard]] Rect bounds() const noexcept;
    [[nodiscard]] double scale() const noexcept;
    [[nodiscard]] ValueView get(const Parameter<any>& field) const noexcept {
        return ValueView(strata_widget_input_property_value(
            context_, strata_string_view{field.name.data(), field.name.size()}));
    }
    [[nodiscard]] std::optional<Color> get(const Parameter<color>& field) const noexcept {
        return ValueView(strata_widget_input_property_value(
                             context_, strata_string_view{field.name.data(), field.name.size()}))
            .color();
    }

    /** Writes reach only fields this widget declared; the handle makes that a compile-time fact. */
    bool set(const Retained<number>& field, double value) noexcept;
    bool set(const Retained<boolean>& field, bool value) noexcept;
    bool set(const Retained<text>& field, std::string_view value) noexcept;
    template <typename T> bool set(const Retained<structured<T>>& field, const T& value) noexcept {
        return strata_widget_input_set_retained_bytes(
                   context_, strata_string_view{field.name.data(), field.name.size()}, &value,
                   sizeof(value))
                   .status == STRATA_STATUS_OK;
    }

    /** Wins arbitration for the active pressed pointer; release and cancellation end capture. */
    bool claim_gesture() noexcept;
    /** Stops an active claimed gesture without manufacturing a release event. */
    bool cancel_gesture() noexcept;
    /** Requests one downstream projection without changing retained state. */
    bool invalidate(Invalidation invalidation) noexcept;
    /** Requests one callback on the next surface frame; callbacks must opt into every successor. */
    bool request_frame(FrameCost cost = FrameCost::paint) noexcept;
    /** Cancels this widget's pending callback and forgets its frame-delta history. */
    void cancel_frame() noexcept;

    /** Dispatches one package-declared action through the ordinary action registry. */
    bool emit(std::string_view action_id, std::string_view payload_json = {},
              std::string_view event_kind = "activated",
              std::string_view event_value_json = {}) noexcept;
    bool emit_event(std::string_view event_kind, std::string_view event_value_json = {}) noexcept;
    bool emit_event(std::string_view event_kind, double value) noexcept;
    bool emit_event(std::string_view event_kind, bool value) noexcept;
    bool emit_text_event(std::string_view event_kind, std::string_view value) noexcept;

    /** Typed local feedback; these events do not dispatch a host action. */
    bool live(double value) noexcept;
    bool live(bool value) noexcept;
    bool live_text(std::string_view value) noexcept;
    bool commit(double value) noexcept;
    bool commit(bool value) noexcept;
    bool commit_text(std::string_view value) noexcept;

  private:
    strata_widget_input_context* context_;
};

/** Balances one clip push; the guard is the only way to clip from the authoring layer. */
class ClipScope final {
  public:
    explicit ClipScope(strata_widget_render_context* const context) noexcept : context_(context) {}
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;
    ClipScope(ClipScope&& other) noexcept : context_(other.context_) {
        other.context_ = nullptr;
    }
    ClipScope& operator=(ClipScope&&) = delete;
    ~ClipScope() {
        if (context_ != nullptr)
            strata_widget_render_pop_clip(context_);
    }

  private:
    strata_widget_render_context* context_;
};

/** Presentation capability of one widget content or overlay pass. */
class Present final {
  public:
    explicit Present(strata_widget_render_context* const context) noexcept : context_(context) {}

    [[nodiscard]] Rect bounds() const noexcept;
    [[nodiscard]] Rect root_bounds() const noexcept;
    /** Semantic focus, independent of whether pointer modality suppresses its indicator. */
    [[nodiscard]] bool focused() const noexcept;
    /** Whether this focused widget should currently paint keyboard/spatial focus treatment. */
    [[nodiscard]] bool focus_visible() const noexcept;
    [[nodiscard]] bool hovered() const noexcept;
    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] double motion(std::string_view channel, double fallback = 0.0) const noexcept;
    [[nodiscard]] double get(const Retained<number>& field) const noexcept;
    [[nodiscard]] bool get(const Retained<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Retained<text>& field) const;
    template <typename T> [[nodiscard]] T get(const Retained<structured<T>>& field) const noexcept {
        T value = field.fallback;
        static_cast<void>(strata_widget_render_retained_bytes(
            context_, strata_string_view{field.name.data(), field.name.size()}, &value,
            sizeof(value)));
        return value;
    }
    [[nodiscard]] double get(const Parameter<number>& field) const noexcept;
    [[nodiscard]] bool get(const Parameter<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Parameter<text>& field) const;
    template <typename Kind> [[nodiscard]] bool has(const Parameter<Kind>& field) const noexcept {
        return strata_widget_render_has_property(
                   context_, strata_string_view{field.name.data(), field.name.size()}) != 0U;
    }
    [[nodiscard]] double scale() const noexcept;
    /** Measures one line through the shaping cache the paint path already uses. */
    [[nodiscard]] std::optional<Size> measure(std::string_view value) const noexcept;
    [[nodiscard]] ValueView get(const Parameter<any>& field) const noexcept {
        return ValueView(strata_widget_render_property_value(
            context_, strata_string_view{field.name.data(), field.name.size()}));
    }
    [[nodiscard]] std::optional<Color> get(const Parameter<color>& field) const noexcept {
        return ValueView(strata_widget_render_property_value(
                             context_, strata_string_view{field.name.data(), field.name.size()}))
            .color();
    }
    [[nodiscard]] ValueView style(const std::string_view name) const noexcept {
        return ValueView(strata_widget_render_style_value(
            context_, strata_string_view{name.data(), name.size()}));
    }

    void rect(Rect bounds, Color fill);
    void rounded_rect(Rect bounds, double radius, Color fill);
    void rounded_rect(Rect bounds, double radius, Color fill, Border border);
    void border(Rect bounds, double radius, Border border);
    void text(std::string_view value, double x, double y, Color color);
    /** Aligns shaped text inside a logical rectangle using its measured line metrics. */
    void text(std::string_view value, Rect bounds, Color color, TextAlignment horizontal,
              TextAlignment vertical);
    void image(Rect bounds, std::string_view image, Color tint = rgba(255U, 255U, 255U),
               TextureRegion source = whole_texture());
    void nine_patch(

        Rect bounds, std::string_view texture, Edges source_insets, Edges destination_insets,
        Color tint = rgba(255U, 255U, 255U), TextureRegion source = whole_texture());
    /** Custom geometry under an application material; the engine copies vertices and indices. */
    void mesh(Rect bounds, std::string_view id, const Mesh& geometry);
    void mesh(Rect bounds, std::string_view id, const Mesh& geometry, std::string_view texture);
    void mesh(Rect bounds, std::string_view id, const Mesh& geometry, std::string_view texture,
              const Material& material);
    void blur(Rect bounds, double radius, unsigned int downsample = 1U);
    void shadow(Rect bounds, CornerRadii radii, Color color, double radius, double spread = 0.0);
    /** Clips every command emitted while the returned guard is alive. */
    [[nodiscard]] ClipScope clip(Rect bounds);

  private:
    strata_widget_render_context* context_;
};

struct SemanticChild final {
    std::size_t index = 0U;
    std::string_view role = "slider";
    std::string_view name;
    std::string_view value_text;
    std::optional<double> value;
    double minimum = 0.0;
    double maximum = 1.0;
    bool selected = false;
    bool disabled = false;
};
/** Accessibility projection of one extension widget. */
class Semantics final {
  public:
    explicit Semantics(strata_widget_semantics_context* const context) noexcept
        : context_(context) {}

    [[nodiscard]] double get(const Retained<number>& field) const noexcept;
    [[nodiscard]] std::string get(const Retained<text>& field) const;
    [[nodiscard]] bool get(const Retained<boolean>& field) const noexcept;
    template <typename T> [[nodiscard]] T get(const Retained<structured<T>>& field) const noexcept {
        T value = field.fallback;
        static_cast<void>(strata_widget_semantics_retained_bytes(
            context_, strata_string_view{field.name.data(), field.name.size()}, &value,
            sizeof(value)));
        return value;
    }

    [[nodiscard]] double get(const Parameter<number>& field) const noexcept;
    [[nodiscard]] std::string get(const Parameter<text>& field) const;
    [[nodiscard]] bool get(const Parameter<boolean>& field) const noexcept;
    template <typename Kind> [[nodiscard]] bool has(const Parameter<Kind>& field) const noexcept {
        return strata_widget_semantics_has_property(
                   context_, strata_string_view{field.name.data(), field.name.size()}) != 0U;
    }
    void name(std::string_view value) noexcept;
    void value_text(std::string_view value) noexcept;
    void add_action(std::string_view value) noexcept;
    [[nodiscard]] ValueView get(const Parameter<any>& field) const noexcept {
        return ValueView(strata_widget_semantics_property_value(
            context_, strata_string_view{field.name.data(), field.name.size()}));
    }
    [[nodiscard]] std::optional<Color> get(const Parameter<color>& field) const noexcept {
        return ValueView(strata_widget_semantics_property_value(
                             context_, strata_string_view{field.name.data(), field.name.size()}))
            .color();
    }
    bool child(const SemanticChild& value) noexcept {
        std::uint32_t flags = 0U;
        if (value.selected)
            flags |= STRATA_WIDGET_SEMANTIC_CHILD_SELECTED;
        if (value.disabled)
            flags |= STRATA_WIDGET_SEMANTIC_CHILD_DISABLED;
        if (value.value.has_value())
            flags |= STRATA_WIDGET_SEMANTIC_CHILD_VALUE_RANGE;
        const strata_widget_semantic_child descriptor{
            sizeof(strata_widget_semantic_child),
            value.index,
            strata_string_view{value.role.data(), value.role.size()},
            strata_string_view{value.name.data(), value.name.size()},
            strata_string_view{value.value_text.data(), value.value_text.size()},
            value.value.value_or(0.0),
            value.minimum,
            value.maximum,
            flags,
            0U,
        };
        return strata_widget_semantics_add_child(context_, &descriptor).status == STRATA_STATUS_OK;
    }
    void checked(bool value) noexcept;
    void expanded(bool value) noexcept;
    void value_range(double current, double minimum, double maximum) noexcept;
    void selected(bool value) noexcept;

  private:
    strata_widget_semantics_context* context_;
};

/** Inspection capability used to narrow the interactive area of one widget. */
class Inspect final {
  public:
    explicit Inspect(strata_widget_inspection_context* const context) noexcept
        : context_(context) {}

    [[nodiscard]] Rect layout_bounds() const noexcept;

  private:
    strata_widget_inspection_context* context_;
};

/** Input capability of one behavior pointer event. */
class BehaviorInput final {
  public:
    explicit BehaviorInput(strata_behavior_input_context* const context) noexcept
        : context_(context) {}

    bool emit(std::string_view action_id, std::string_view payload_json = {},
              std::string_view event_kind = "activated",
              std::string_view event_value_json = {}) noexcept;

  private:
    strata_behavior_input_context* context_;
};

using ActivateHook = bool (*)(Input&);
using KeyHook = bool (*)(Input&, const Key&);
using WidgetPointerHook = bool (*)(Input&, const Pointer&);
using FrameHook = void (*)(Input&, const Frame&);
using ScrollHook = bool (*)(Input&, const Scroll&);
using SubtargetsHook = void (*)(Subtargets&);
using PresentHook = void (*)(Present&);
using SemanticsHook = void (*)(Semantics&);
using HitBoundsHook = Rect (*)(Inspect&);
using PointerHook = bool (*)(BehaviorInput&, const Pointer&);

namespace detail {

/** Descriptor-owned hook table; one stable address per widget backs `user_data`. */
struct WidgetHooks final {
    ActivateHook activate = nullptr;
    KeyHook key = nullptr;
    WidgetPointerHook pointer = nullptr;
    ScrollHook scroll = nullptr;
    SemanticsHook semantics = nullptr;
    FrameHook frame = nullptr;
    PresentHook present = nullptr;
    SubtargetsHook subtargets = nullptr;
    PresentHook overlay = nullptr;
    HitBoundsHook hit_bounds = nullptr;
};

struct BehaviorHooks final {
    PointerHook pointer = nullptr;
};

} // namespace detail

/** One typed argument of a package-declared action contract. */
struct ActionArgument final {
    std::string name;
    std::string kind = "number";
    bool required = true;
    bool nullable = false;
};

struct ActionContract final {
    std::string id;
    std::string summary;
    std::string payload_contract = "no payload";
    std::string dispatch_policy = "optional";
    std::vector<ActionArgument> arguments;
};

struct Padding final {
    double top = 0.0;
    double right = 0.0;
    double bottom = 0.0;
    double left = 0.0;
};

/** Content-size disclosure motion supplied by the framework for expandable extension widgets. */
struct Disclosure final {
    Retained<boolean> expanded;
    double collapsed_extent = 0.0;
    long long duration_nanoseconds = 180000000;
};

class Package;

/** One extension widget: identity, `.strata` contract, retained state, and lifecycle hooks. */
class Widget final {
  public:
    explicit Widget(std::string type);

    /** Adopts one typed parameter declaration; its default reaches both schema and description. */
    template <typename Kind> Widget& parameter(const Parameter<Kind>& declaration) {
        std::optional<DefaultValue> value;
        if (declaration.value.has_value()) {
            if constexpr (std::is_same_v<typename Kind::value_type, std::string_view>) {
                value = DefaultValue(std::string(*declaration.value));
            } else if constexpr (!std::is_same_v<typename Kind::value_type, std::monostate>) {
                value = DefaultValue(*declaration.value);
            }
        }
        return declare_parameter(std::string(declaration.name), Kind::schema_kind,
                                 declaration.required, std::move(value));
    }
    /** Adopts one typed retained declaration and its invalidation class. */
    template <typename Kind> Widget& retained(const Retained<Kind>& declaration) {
        return declare_retained(std::string(declaration.name), declaration.invalidation);
    }

    Widget& children();
    Widget& no_children();
    Widget& focusable();
    Widget& intrinsic_size(double width, double height);
    Widget& padding(Padding value);
    Widget& clip();
    Widget& disclosure(Disclosure value);
    Widget& emits(ActionContract contract);
    Widget& semantics_role(std::string role);
    Widget& semantics_actions(std::vector<std::string> actions);

    Widget& on_activate(ActivateHook hook);
    Widget& on_key(KeyHook hook);
    Widget& on_pointer(WidgetPointerHook hook);
    Widget& on_scroll(ScrollHook hook);
    Widget& on_semantics(SemanticsHook hook);
    Widget& subtargets(SubtargetsHook hook);
    Widget& on_frame(FrameHook hook);
    Widget& present(PresentHook hook);
    Widget& overlay(PresentHook hook);
    /** Overlay painted outside this widget's clip, gated by one declared retained boolean. */
    Widget& detached_overlay(PresentHook hook, const Retained<boolean>& open);
    Widget& hit_bounds(HitBoundsHook hook);
    /** Presentation reads motion progress and must repaint while channels animate. */
    Widget& depends_on_motion();
    /** Presentation reads hover/press/focus feedback and must repaint when it changes. */
    Widget& depends_on_status();

    [[nodiscard]] const std::string& type() const noexcept {
        return type_;
    }
    [[nodiscard]] const std::vector<ActionContract>& actions() const noexcept {
        return actions_;
    }

  private:
    friend class Package;

    using DefaultValue = std::variant<double, bool, std::string>;

    struct Parameter final {
        std::string name;
        std::string kind;
        bool required = false;
        std::optional<DefaultValue> default_value;
    };

    Widget& declare_parameter(std::string name, std::string_view kind, bool required,
                              std::optional<DefaultValue> default_value);
    Widget& declare_retained(std::string name, Invalidation invalidation);
    /** Materializes descriptor-owned storage once; the package holds the widget in place. */
    [[nodiscard]] strata_widget_extension descriptor();
    [[nodiscard]] std::optional<strata_widget_input_extension> input_descriptor();
    [[nodiscard]] std::optional<strata_widget_scroll_extension> scroll_descriptor();
    [[nodiscard]] std::string schema_json() const;

    std::string type_;
    std::vector<Parameter> parameters_;
    std::vector<std::pair<std::string, Invalidation>> retained_;
    std::vector<ActionContract> actions_;
    std::vector<strata_widget_retained_field> retained_fields_;
    std::string semantics_role_;
    std::vector<std::string> semantics_actions_;
    std::string popup_retained_;
    std::string description_json_;
    std::string layout_json_;
    std::optional<Size> intrinsic_size_;
    std::optional<Padding> padding_;
    std::optional<Disclosure> disclosure_;
    detail::WidgetHooks hooks_;
    bool allows_children_ = false;
    bool focusable_ = false;
    bool clip_ = false;
    bool detached_overlay_ = false;
    bool depends_on_motion_ = false;
    bool depends_on_status_ = false;
};

/** One extension behavior attachable from `.strata` through the `behaviors` parameter. */
class Behavior final {
  public:
    explicit Behavior(std::string id);

    Behavior& focusable();
    Behavior& on_pointer(PointerHook hook);
    Behavior& emits(ActionContract contract);

    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }
    [[nodiscard]] const std::vector<ActionContract>& actions() const noexcept {
        return actions_;
    }

  private:
    friend class Package;

    [[nodiscard]] strata_behavior_extension descriptor();

    std::string id_;
    std::vector<ActionContract> actions_;
    detail::BehaviorHooks hooks_;
    bool focusable_ = false;
};

/**
 * One versioned package. Hosts select packages by id; the compiler applies the same package's
 * projected schema, so registration and compilation cannot drift.
 */
class Package final {
  public:
    explicit Package(std::string id);
    Package(const Package&) = delete;
    Package& operator=(const Package&) = delete;

    Package& widget(Widget definition);
    Package& behavior(Behavior definition);

    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }
    /** Finalizes descriptor storage on first use and returns a bundle owned by this package. */
    [[nodiscard]] const strata_surface_extension_bundle& bundle();
    /** Compiler declarations for every widget, behavior, and emitted action in this package. */
    [[nodiscard]] std::string schema_json() const;
    [[nodiscard]] std::vector<std::string> widget_types() const;
    [[nodiscard]] std::vector<std::string> behavior_ids() const;

  private:
    void finalize();

    std::string id_;
    std::deque<Widget> widgets_;
    std::deque<Behavior> behaviors_;
    std::vector<strata_widget_extension> widget_descriptors_;
    std::vector<strata_widget_input_extension> widget_input_descriptors_;
    std::vector<strata_widget_scroll_extension> widget_scroll_descriptors_;
    std::vector<strata_behavior_extension> behavior_descriptors_;
    strata_surface_extension_bundle bundle_{};
    bool finalized_ = false;
};

[[nodiscard]] inline Widget widget(std::string type) {
    return Widget(std::move(type));
}
[[nodiscard]] inline Behavior behavior(std::string id) {
    return Behavior(std::move(id));
}
[[nodiscard]] inline std::unique_ptr<Package> package(std::string id) {
    return std::make_unique<Package>(std::move(id));
}

/** Entry-point support used by STRATA_EXTENSION_PACKAGE; not an application-facing callback. */
namespace detail {

using PackageFactory = std::unique_ptr<Package> (*)();

[[nodiscard]] strata_status query_plugin(PackageFactory factory, std::uint32_t requested_plugin_abi,
                                         strata_extension_plugin* output) noexcept;

} // namespace detail

} // namespace strata::extension

/** Exports one package factory as the stable C extension-library entry point. */
#define STRATA_EXTENSION_PACKAGE(factory)                                                          \
    extern "C" STRATA_EXTENSION_PLUGIN_EXPORT strata_status strata_extension_plugin_query(         \
        const std::uint32_t requested_plugin_abi,                                                  \
        strata_extension_plugin* const output) noexcept {                                          \
        return ::strata::extension::detail::query_plugin(&(factory), requested_plugin_abi,         \
                                                         output);                                  \
    }
