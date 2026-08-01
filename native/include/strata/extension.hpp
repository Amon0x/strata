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
using Color = strata_color;
using Border = strata_border;

[[nodiscard]] constexpr Color rgba(
    const unsigned char red,
    const unsigned char green,
    const unsigned char blue,
    const unsigned char alpha = 255U
) noexcept {
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

/** Whole-texture sampling; narrow it to sample one sprite out of an atlas. */
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

/** Typed material parameter; build them with the number/boolean/text/color helpers below. */
using MaterialParameter = strata_material_parameter;

[[nodiscard]] inline MaterialParameter material_number(
    const std::string_view name,
    const double value
) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_NUMBER;
    parameter.number = value;
    return parameter;
}

[[nodiscard]] inline MaterialParameter material_boolean(
    const std::string_view name,
    const bool value
) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_BOOLEAN;
    parameter.boolean_value = value ? 1U : 0U;
    return parameter;
}

[[nodiscard]] inline MaterialParameter material_text(
    const std::string_view name,
    const std::string_view value
) noexcept {
    MaterialParameter parameter{};
    parameter.struct_size = sizeof(MaterialParameter);
    parameter.name = strata_string_view{name.data(), name.size()};
    parameter.kind = STRATA_MATERIAL_PARAMETER_TEXT;
    parameter.text = strata_string_view{value.data(), value.size()};
    return parameter;
}

[[nodiscard]] inline MaterialParameter material_color(
    const std::string_view name,
    const Color value
) noexcept {
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
enum class Invalidation { properties, layout, style, text, semantics };

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

/**
 * Typed field declarations. A field is written once as a constexpr value naming itself, its type,
 * and its default; the widget consumes it and every hook reads it through the same handle, so a
 * name cannot be mistyped and a default cannot be restated at a call site.
 */
template <typename Kind>
struct Retained final {
    std::string_view name;
    typename Kind::value_type fallback{};
    Invalidation invalidation = Invalidation::properties;
};

template <typename Kind>
struct Parameter final {
    std::string_view name;
    std::optional<typename Kind::value_type> value{};
    bool required = false;
};

template <typename Kind>
[[nodiscard]] constexpr Retained<Kind> retained(
    const std::string_view name,
    const typename Kind::value_type fallback = {},
    const Invalidation invalidation = Invalidation::properties
) noexcept {
    return Retained<Kind>{name, fallback, invalidation};
}

template <typename Kind>
[[nodiscard]] constexpr Parameter<Kind> parameter(const std::string_view name) noexcept {
    return Parameter<Kind>{name, std::nullopt, false};
}

template <typename Kind>
[[nodiscard]] constexpr Parameter<Kind> parameter(
    const std::string_view name,
    const typename Kind::value_type value
) noexcept {
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
    bool on_target = false;
};

struct Size final {
    double width = 0.0;
    double height = 0.0;
};

/** Input capability of one widget activation or key press. */
class Input final {
public:
    explicit Input(strata_widget_input_context* const context) noexcept : context_(context) {}

    [[nodiscard]] double get(const Retained<number>& field) const noexcept;
    [[nodiscard]] bool get(const Retained<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Retained<text>& field) const;
    [[nodiscard]] double get(const Parameter<number>& field) const noexcept;
    [[nodiscard]] bool get(const Parameter<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Parameter<text>& field) const;

    /** Writes reach only fields this widget declared; the handle makes that a compile-time fact. */
    bool set(const Retained<number>& field, double value) noexcept;
    bool set(const Retained<boolean>& field, bool value) noexcept;
    bool set(const Retained<text>& field, std::string_view value) noexcept;

    /** Dispatches one package-declared action through the ordinary action registry. */
    bool emit(
        std::string_view action_id,
        std::string_view payload_json = {},
        std::string_view event_kind = "activated",
        std::string_view event_value_json = {}
    ) noexcept;
    bool emit_event(std::string_view event_kind, std::string_view event_value_json = {}) noexcept;

private:
    strata_widget_input_context* context_;
};

/** Balances one clip push; the guard is the only way to clip from the authoring layer. */
class ClipScope final {
public:
    explicit ClipScope(strata_widget_render_context* const context) noexcept : context_(context) {}
    ClipScope(const ClipScope&) = delete;
    ClipScope& operator=(const ClipScope&) = delete;
    ClipScope(ClipScope&& other) noexcept : context_(other.context_) { other.context_ = nullptr; }
    ClipScope& operator=(ClipScope&&) = delete;
    ~ClipScope() {
        if (context_ != nullptr) strata_widget_render_pop_clip(context_);
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
    [[nodiscard]] double get(const Parameter<number>& field) const noexcept;
    [[nodiscard]] bool get(const Parameter<boolean>& field) const noexcept;
    [[nodiscard]] std::string get(const Parameter<text>& field) const;
    /** Measures one line through the shaping cache the paint path already uses. */
    [[nodiscard]] std::optional<Size> measure(std::string_view value) const noexcept;

    void rect(Rect bounds, Color fill);
    void rounded_rect(Rect bounds, double radius, Color fill);
    void rounded_rect(Rect bounds, double radius, Color fill, Border border);
    void border(Rect bounds, double radius, Border border);
    void text(std::string_view value, double x, double y, Color color);
    void image(
        Rect bounds,
        std::string_view texture,
        Color tint = rgba(255U, 255U, 255U),
        TextureRegion source = whole_texture()
    );
    void nine_patch(
        Rect bounds,
        std::string_view texture,
        Edges source_insets,
        Edges destination_insets,
        Color tint = rgba(255U, 255U, 255U),
        TextureRegion source = whole_texture()
    );
    /** Custom geometry under an application material; the engine copies vertices and indices. */
    void mesh(Rect bounds, std::string_view id, const Mesh& geometry);
    void mesh(Rect bounds, std::string_view id, const Mesh& geometry, std::string_view texture);
    void mesh(
        Rect bounds,
        std::string_view id,
        const Mesh& geometry,
        std::string_view texture,
        const Material& material
    );
    void blur(Rect bounds, double radius, unsigned int downsample = 1U);
    void shadow(Rect bounds, CornerRadii radii, Color color, double radius, double spread = 0.0);
    /** Clips every command emitted while the returned guard is alive. */
    [[nodiscard]] ClipScope clip(Rect bounds);

private:
    strata_widget_render_context* context_;
};

/** Accessibility projection of one extension widget. */
class Semantics final {
public:
    explicit Semantics(strata_widget_semantics_context* const context) noexcept : context_(context) {}

    [[nodiscard]] double get(const Retained<number>& field) const noexcept;
    [[nodiscard]] bool get(const Retained<boolean>& field) const noexcept;

    void name(std::string_view value) noexcept;
    void value_text(std::string_view value) noexcept;
    void add_action(std::string_view value) noexcept;
    void checked(bool value) noexcept;
    void expanded(bool value) noexcept;
    void selected(bool value) noexcept;

private:
    strata_widget_semantics_context* context_;
};

/** Inspection capability used to narrow the interactive area of one widget. */
class Inspect final {
public:
    explicit Inspect(strata_widget_inspection_context* const context) noexcept : context_(context) {}

    [[nodiscard]] Rect layout_bounds() const noexcept;

private:
    strata_widget_inspection_context* context_;
};

/** Input capability of one behavior pointer event. */
class BehaviorInput final {
public:
    explicit BehaviorInput(strata_behavior_input_context* const context) noexcept
        : context_(context) {}

    bool emit(
        std::string_view action_id,
        std::string_view payload_json = {},
        std::string_view event_kind = "activated",
        std::string_view event_value_json = {}
    ) noexcept;

private:
    strata_behavior_input_context* context_;
};

using ActivateHook = bool (*)(Input&);
using KeyHook = bool (*)(Input&, const Key&);
using PresentHook = void (*)(Present&);
using SemanticsHook = void (*)(Semantics&);
using HitBoundsHook = Rect (*)(Inspect&);
using PointerHook = bool (*)(BehaviorInput&, const Pointer&);

namespace detail {

/** Descriptor-owned hook table; one stable address per widget backs `user_data`. */
struct WidgetHooks final {
    ActivateHook activate = nullptr;
    KeyHook key = nullptr;
    SemanticsHook semantics = nullptr;
    PresentHook present = nullptr;
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
    template <typename Kind>
    Widget& parameter(const Parameter<Kind>& declaration) {
        std::optional<DefaultValue> value;
        if (declaration.value.has_value()) {
            if constexpr (std::is_same_v<typename Kind::value_type, std::string_view>) {
                value = DefaultValue(std::string(*declaration.value));
            } else if constexpr (!std::is_same_v<typename Kind::value_type, std::monostate>) {
                value = DefaultValue(*declaration.value);
            }
        }
        return declare_parameter(
            std::string(declaration.name),
            Kind::schema_kind,
            declaration.required,
            std::move(value)
        );
    }
    /** Adopts one typed retained declaration and its invalidation class. */
    template <typename Kind>
    Widget& retained(const Retained<Kind>& declaration) {
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
    Widget& on_semantics(SemanticsHook hook);
    Widget& present(PresentHook hook);
    Widget& overlay(PresentHook hook);
    /** Overlay painted outside this widget's clip, gated by one declared retained boolean. */
    Widget& detached_overlay(PresentHook hook, const Retained<boolean>& open);
    Widget& hit_bounds(HitBoundsHook hook);
    /** Presentation reads motion progress and must repaint while channels animate. */
    Widget& depends_on_motion();
    /** Presentation reads hover/press/focus feedback and must repaint when it changes. */
    Widget& depends_on_status();

    [[nodiscard]] const std::string& type() const noexcept { return type_; }
    [[nodiscard]] const std::vector<ActionContract>& actions() const noexcept { return actions_; }

private:
    friend class Package;

    using DefaultValue = std::variant<double, bool, std::string>;

    struct Parameter final {
        std::string name;
        std::string kind;
        bool required = false;
        std::optional<DefaultValue> default_value;
    };

    Widget& declare_parameter(
        std::string name,
        std::string_view kind,
        bool required,
        std::optional<DefaultValue> default_value
    );
    Widget& declare_retained(std::string name, Invalidation invalidation);
    /** Materializes descriptor-owned storage once; the package holds the widget in place. */
    [[nodiscard]] strata_widget_extension descriptor();
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

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
    [[nodiscard]] const std::vector<ActionContract>& actions() const noexcept { return actions_; }

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

    [[nodiscard]] const std::string& id() const noexcept { return id_; }
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
    std::vector<strata_behavior_extension> behavior_descriptors_;
    strata_surface_extension_bundle bundle_{};
    bool finalized_ = false;
};

[[nodiscard]] inline Widget widget(std::string type) { return Widget(std::move(type)); }
[[nodiscard]] inline Behavior behavior(std::string id) { return Behavior(std::move(id)); }
[[nodiscard]] inline std::unique_ptr<Package> package(std::string id) {
    return std::make_unique<Package>(std::move(id));
}

/** Entry-point support used by STRATA_EXTENSION_PACKAGE; not an application-facing callback. */
namespace detail {

using PackageFactory = std::unique_ptr<Package> (*)();

[[nodiscard]] strata_status query_plugin(
    PackageFactory factory,
    std::uint32_t requested_plugin_abi,
    strata_extension_plugin* output
) noexcept;

} // namespace detail

} // namespace strata::extension

/** Exports one package factory as the stable C extension-library entry point. */
#define STRATA_EXTENSION_PACKAGE(factory)                                                    \
    extern "C" STRATA_EXTENSION_PLUGIN_EXPORT strata_status strata_extension_plugin_query( \
        const std::uint32_t requested_plugin_abi,                                             \
        strata_extension_plugin* const output                                                 \
    ) noexcept {                                                                               \
        return ::strata::extension::detail::query_plugin(                                     \
            &(factory), requested_plugin_abi, output                                          \
        );                                                                                    \
    }
