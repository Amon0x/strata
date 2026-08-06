#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "data/json.hpp"

namespace strata::compiler {

struct BuiltinCatalog;
struct DeclaredParameter;
struct DeclaredType;

enum class SemanticTypeKind {
    unknown,
    any,
    unsafe_component_parameter,
    state_binding,
    null_value,
    string,
    string_literal,
    number,
    duration,
    boolean,
    color,
    /** A vector outline authored in the compact command form; parsed at compile time. */
    path,
    image,
    key,
    style,
    layout,
    animation,
    effect,
    material,
    action,
    component,
    lambda,
    component_template,
    enumeration,
    list,
    map,
    union_value,
    host_object,
    async_value,
    collection,
};

struct SemanticType;
using SemanticTypePtr = std::shared_ptr<const SemanticType>;

struct ObjectField final {
    std::string name;
    SemanticTypePtr type;
    bool required = false;
    bool nullable = false;
};

struct SemanticType final {
    SemanticTypeKind kind = SemanticTypeKind::unknown;
    std::string schema_name;
    std::string label;
    std::string literal;
    std::vector<std::string> values;
    std::vector<ObjectField> fields;
    std::vector<SemanticTypePtr> options;
    SemanticTypePtr element;
    SemanticTypePtr value;
    SemanticTypePtr parameter;
    SemanticTypePtr returns;
    std::optional<std::size_t> minimum_items;
    std::optional<std::size_t> maximum_items;
    bool element_nullable = false;
    bool value_nullable = false;
    bool allow_unknown_fields = false;

    [[nodiscard]] std::string diagnostic_name() const;
    [[nodiscard]] bool accepts(const SemanticType& actual) const;
    [[nodiscard]] const ObjectField* find_field(std::string_view name) const noexcept;
};

struct SchemaParameter final {
    std::string name;
    SemanticTypePtr type;
    bool required = false;
    bool nullable = false;
    std::vector<std::string> aliases;
    /** GPU material packing contract; absent for ordinary DSL parameters. */
    std::optional<std::string> material_type;

    [[nodiscard]] bool accepts_name(std::string_view candidate) const noexcept;
};

struct WidgetBindingSchema final {
    std::string shorthand_parameter;
    std::string value_parameter;
    std::string event_parameter;
};

struct WidgetSchema final {
    std::string name;
    std::vector<SchemaParameter> parameters;
    std::vector<WidgetBindingSchema> bindings;
    bool allows_children = false;

    [[nodiscard]] const SchemaParameter* find_parameter(std::string_view name) const noexcept;
    [[nodiscard]] const WidgetBindingSchema* find_binding(std::string_view shorthand) const noexcept;
};

struct ActionSchema final {
    std::string id;
    std::vector<SchemaParameter> parameters;
    std::string dispatch_policy;
    std::string payload_contract;
    std::string summary;
};

struct HelperSchema final {
    std::string name;
    std::string implementation;
    std::vector<SchemaParameter> parameters;
    SemanticTypePtr return_type;
    SemanticTypePtr vararg_type;
    bool allow_named_varargs = false;
};

/** Draw-data floats one material parameter occupies. */
[[nodiscard]] std::size_t material_parameter_width(
    const std::optional<std::string>& material_type
) noexcept;

/** Backend-neutral shader sources of one authored material, keyed by backend id (`hlsl`, `glsl`). */
using MaterialShaderSources = std::vector<std::pair<std::string, std::string>>;

struct MaterialSchema final {
    std::string id;
    std::vector<SchemaParameter> parameters;
    /** Empty for the built-in materials, which every backend implements natively. */
    MaterialShaderSources shaders;
    /** Material drawn instead where no declared backend source applies. */
    std::string fallback;
    std::string blend_mode = "straight_alpha";

    [[nodiscard]] const SchemaParameter* find_parameter(std::string_view name) const noexcept;
    /** The draw-data float a parameter occupies, assigned in declaration order. */
    [[nodiscard]] std::optional<std::size_t> packing_slot(std::string_view name) const noexcept;
};

/**
 * Draw-data layout an authored material sees. Floats 0..7 stay the shape's own geometry (size,
 * edge softness, border width, then the four corner radii) so a material can mask itself to the
 * silhouette it was applied to, and the last two floats carry the draw mode and material opacity.
 * Authored parameters occupy the six floats in between, assigned in declaration order.
 */
inline constexpr std::size_t first_material_slot = 8U;
inline constexpr std::size_t maximum_material_slots = 6U;

struct EffectSchema final {
    std::string name;
    std::vector<SchemaParameter> parameters;
    /** BACKDROP samples the already-rendered scene; CONTENT isolates and filters the subtree. */
    std::string input = "BACKDROP";

    struct Pass final {
        std::string kind;
        std::optional<std::string> radius_parameter;
        double radius = 0.0;
        std::optional<std::string> downsample_parameter;
        std::uint32_t downsample = 1U;
        MaterialShaderSources shaders;
    };
    std::vector<Pass> passes;

    [[nodiscard]] const SchemaParameter* find_parameter(std::string_view parameter_name) const noexcept;
};

class SchemaRegistry final {
public:
    /** Creates a mutable application view over the immutable native built-in catalog. */
    [[nodiscard]] static SchemaRegistry builtins();

    [[nodiscard]] const WidgetSchema* widget(std::string_view name) const noexcept;
    [[nodiscard]] const ActionSchema* action(std::string_view id) const noexcept;
    [[nodiscard]] const HelperSchema* helper(std::string_view name) const noexcept;
    [[nodiscard]] const MaterialSchema* material(std::string_view id) const noexcept;
    [[nodiscard]] const EffectSchema* effect(std::string_view name) const noexcept;
    [[nodiscard]] const SemanticType* component_parameter_type(std::string_view name) const noexcept;
    [[nodiscard]] const SemanticType* application_type(std::string_view name) const noexcept;
    [[nodiscard]] const SchemaParameter* layout_property(std::string_view name) const noexcept;
    [[nodiscard]] const SchemaParameter* style_property(std::string_view name) const noexcept;
    [[nodiscard]] const SchemaParameter* animation_property(std::string_view name) const noexcept;
    [[nodiscard]] const SchemaParameter* animation_timing_property(std::string_view name) const noexcept;
    [[nodiscard]] std::vector<std::string> layout_property_names() const;
    [[nodiscard]] std::vector<std::string> style_property_names() const;
    [[nodiscard]] std::vector<std::string> animation_property_names() const;
    [[nodiscard]] std::vector<std::string> animation_timing_property_names() const;
    [[nodiscard]] bool has_material(std::string_view id) const noexcept;
    [[nodiscard]] std::vector<std::string> widget_names() const;
    [[nodiscard]] std::vector<std::string> action_names() const;
    [[nodiscard]] std::vector<std::string> material_ids() const;
    [[nodiscard]] std::vector<std::string> effect_names() const;

    void apply_scenario_declarations(const data::JsonValue& schemas);
    [[nodiscard]] const std::unordered_map<std::string, SemanticTypePtr>& host_types() const noexcept;

private:
    [[nodiscard]] static SchemaRegistry from_catalog(const BuiltinCatalog& catalog);

    /**
     * Application registries are sparse overlays over the immutable native catalog. Keeping the
     * catalog shared is important: constructing an application must not deep-copy every built-in
     * widget, helper, action, and semantic type before it can decode an already-compiled module.
     */
    std::shared_ptr<const SchemaRegistry> base_;
    std::unordered_map<std::string, WidgetSchema> widgets_;
    std::unordered_map<std::string, ActionSchema> actions_;
    std::unordered_map<std::string, HelperSchema> helpers_;
    std::unordered_map<std::string, MaterialSchema> materials_;
    std::unordered_map<std::string, EffectSchema> effects_;
    std::unordered_map<std::string, SemanticTypePtr> component_parameter_types_;
    std::unordered_map<std::string, SemanticTypePtr> host_types_;
    std::vector<SchemaParameter> framework_widget_parameters_;
    std::vector<SchemaParameter> layout_properties_;
    std::vector<SchemaParameter> style_properties_;
    std::vector<SchemaParameter> animation_properties_;
    std::vector<SchemaParameter> animation_timing_properties_;
    std::vector<std::string> material_ids_;
    std::unordered_map<std::string, std::shared_ptr<const DeclaredType>>
        declared_type_definitions_;
    std::unordered_map<std::string, data::JsonValue> application_type_definitions_;
    std::unordered_map<std::string, std::string> application_type_names_;
    std::unordered_map<std::string, SemanticTypePtr> resolved_types_;
    std::vector<std::string> resolving_types_;

    [[nodiscard]] SemanticTypePtr parse_type(const data::JsonValue& value);
    [[nodiscard]] SemanticTypePtr parse_type(
        const std::shared_ptr<const DeclaredType>& value
    );
    [[nodiscard]] SchemaParameter parse_parameter(const data::JsonValue& value);
    [[nodiscard]] SchemaParameter parse_parameter(const DeclaredParameter& value);
};

} // namespace strata::compiler
