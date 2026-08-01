#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "data/json.hpp"

namespace strata::compiler {

enum class DeclaredTypeKind {
    unknown,
    any,
    unsafe_component_parameter,
    null_value,
    string,
    string_literal,
    number,
    duration,
    boolean,
    color,
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
    object,
    union_value,
    host_object,
    async_value,
    collection,
};

struct DeclaredType;
using DeclaredTypePtr = std::shared_ptr<const DeclaredType>;

struct DeclaredParameter final {
    std::string name;
    DeclaredTypePtr type;
    bool required = false;
    bool nullable = false;
    std::vector<std::string> aliases;
    std::optional<std::string> material_type;
};

struct DeclaredTypeField final {
    std::string name;
    DeclaredTypePtr type;
    bool required = false;
    bool nullable = false;
};

/** Typed, unresolved declaration. A non-empty reference selects a named catalog type. */
struct DeclaredType final {
    std::string reference;
    DeclaredTypeKind kind = DeclaredTypeKind::unknown;
    std::string label;
    std::string literal;
    std::vector<std::string> values;
    std::vector<DeclaredTypeField> fields;
    /** Component-template parameters retained for language-neutral tooling. */
    std::vector<DeclaredParameter> parameters;
    std::vector<DeclaredTypePtr> options;
    DeclaredTypePtr element;
    DeclaredTypePtr value;
    DeclaredTypePtr parameter;
    DeclaredTypePtr returns;
    std::optional<std::size_t> minimum_items;
    std::optional<std::size_t> maximum_items;
    bool element_nullable = false;
    bool value_nullable = false;
    bool allow_unknown_fields = false;
};

struct DeclaredNamedType final {
    std::string id;
    DeclaredTypePtr definition;
};

struct DeclaredProperty final {
    std::string name;
    DeclaredTypePtr type;
    bool nullable = false;
};

struct DeclaredWidgetBinding final {
    std::string shorthand_parameter;
    std::string value_parameter;
    std::string event_parameter;
};

struct DeclaredWidgetEvent final {
    std::string name;
    std::string callback_parameter;
    std::string description;
};

struct DeclaredRetainedState final {
    std::string name;
    std::string description;
};

struct DeclaredWidget final {
    std::string name;
    std::vector<DeclaredParameter> parameters;
    /** Stable authoring order after shared framework parameters are composed. */
    std::vector<std::string> parameter_order;
    std::vector<DeclaredWidgetBinding> bindings;
    std::vector<DeclaredWidgetEvent> events;
    std::vector<DeclaredRetainedState> retained_state;
    bool allows_children = false;
};

struct DeclaredAction final {
    std::string id;
    std::vector<DeclaredParameter> parameters;
    std::string dispatch_policy;
    std::string payload_contract;
    std::string summary;
};

struct DeclaredHelper final {
    std::string name;
    std::string implementation;
    std::vector<DeclaredParameter> parameters;
    DeclaredTypePtr return_type;
    DeclaredTypePtr vararg_type;
    std::vector<std::string> capabilities;
    bool allow_named_varargs = false;
};

struct DeclaredBehavior final {
    std::string id;
    DeclaredTypePtr options;
};

struct DeclaredMaterial final {
    std::string id;
    std::vector<DeclaredParameter> parameters;
};

struct DeclaredEffect final {
    std::string name;
    std::vector<DeclaredProperty> parameters;
};

/** Canonical immutable source for all built-in compiler/runtime/authoring declarations. */
struct BuiltinCatalog final {
    std::string id = "strata.builtins.v1";
    std::vector<DeclaredNamedType> types;
    std::vector<DeclaredProperty> layout_properties;
    std::vector<DeclaredProperty> style_properties;
    std::vector<DeclaredProperty> animation_properties;
    std::vector<DeclaredProperty> animation_timing_properties;
    std::vector<DeclaredParameter> framework_widget_parameters;
    std::vector<DeclaredWidget> widgets;
    std::vector<DeclaredAction> actions;
    std::vector<DeclaredHelper> helpers;
    std::vector<DeclaredProperty> component_parameter_types;
    std::vector<DeclaredBehavior> behaviors;
    std::vector<DeclaredMaterial> materials;
    std::vector<DeclaredEffect> effects;
};

[[nodiscard]] DeclaredTypePtr declared_type(DeclaredType value);
[[nodiscard]] DeclaredTypePtr declared_type_reference(std::string id);

/** Process-wide immutable catalog, validated before first use. */
[[nodiscard]] const BuiltinCatalog& builtin_catalog();

/** Framework parameters composed onto a widget without duplicating them in native declarations. */
[[nodiscard]] std::vector<DeclaredParameter>
composed_widget_parameters(const BuiltinCatalog& catalog, const DeclaredWidget& widget);

/** Language-neutral SDK/tooling projection. Core compiler/runtime code never consumes this JSON. */
[[nodiscard]] data::JsonValue export_builtin_registry(const BuiltinCatalog& catalog);
[[nodiscard]] std::string export_builtin_registry_json();

} // namespace strata::compiler
