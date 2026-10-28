#include <algorithm>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "compiler/semantic/type_check.hpp"

namespace strata::compiler::semantic {
namespace {

/** The enumeration values a type accepts, through any unions it is made of. */
void collect_enumeration_values(const SemanticType& type, std::vector<std::string>& values) {
    if (type.kind == SemanticTypeKind::enumeration)
        values.insert(values.end(), type.values.begin(), type.values.end());
    for (const SemanticTypePtr& option : type.options) {
        if (option != nullptr) collect_enumeration_values(*option, values);
    }
}

/** A likely intended value for a mismatched string literal, or nothing. */
[[nodiscard]] std::string mismatch_hint(const SemanticType& expected, const SemanticType& actual) {
    if (actual.kind != SemanticTypeKind::string_literal) return {};
    if (expected.label == "layout size" && (actual.literal == "fill" || actual.literal == "FILL"))
        return " Use { weight: 1 } to fill the remaining space.";
    std::vector<std::string> values;
    collect_enumeration_values(expected, values);
    return did_you_mean(actual.literal, values);
}

/**
 * The declared object type an authored object must satisfy. A union offers several shapes, so the
 * authored keys select it: the one shape that declares every key present. No match means the
 * object fits nothing the type allows; several matches stay unchecked rather than guessed.
 */
void collect_object_options(const SemanticType& expected,
                            std::vector<const SemanticType*>& output) {
    if (expected.kind == SemanticTypeKind::map && !expected.fields.empty()) {
        output.push_back(&expected);
        return;
    }
    if (expected.kind != SemanticTypeKind::union_value) return;
    for (const SemanticTypePtr& option : expected.options) {
        if (option != nullptr) collect_object_options(*option, output);
    }
}

} // namespace

void TypeChecker::style_entries(const std::vector<LayoutProperty>& entries,
                                const std::vector<const Expression*>& values,
                                const std::string& component_path) {
    for (std::size_t index = 0U; index < entries.size(); ++index) {
        const LayoutProperty& entry = entries[index];
        const SchemaParameter* property = registry_.style_property(entry.name);
        if (property == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                   "Style property '" + entry.name + "' is not supported." +
                       did_you_mean(entry.name, registry_.style_property_names()),
                   entry.span, component_path + "." + entry.name,
                   expected_names(registry_.style_property_names()));
            static_cast<void>(infer(*values[index], component_path));
        } else {
            validate_expected(*values[index], *property->type, property->nullable,
                              component_path + "." + entry.name);
        }
    }
    check_layout_kind(analysis_, &oracle_, entries, component_path);
}

bool TypeChecker::validate_material(const Expression& expression,
                                    const std::string& component_path) {
    if (const std::string* id = string_literal_value(expression); id != nullptr) {
        if (!registry_.has_material(*id)) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL",
                   "Material '" + *id + "' is not a built-in material." +
                       did_you_mean(*id, registry_.material_ids()),
                   expression.span, component_path, expected_names(registry_.material_ids()));
        }
        return true;
    }
    const auto* call = std::get_if<CallExpression>(&expression.node);
    if (call == nullptr || call->target.qualified_name() != "material") return false;
    checked_[expression.id] = true;
    const ResolvedCall& resolved = *model_.call(expression);
    const std::string* id =
        !call->arguments.empty() ? string_literal_value(*call->arguments.front().value) : nullptr;
    if (id == nullptr) {
        report("STRATA.DSL.SEMANTIC_DYNAMIC_MATERIAL",
               "material(...) requires a static material id string.", expression.span,
               component_path, expected_names(registry_.material_ids()));
        for (const Argument& argument : call->arguments)
            static_cast<void>(infer(*argument.value, component_path));
        return true;
    }
    const MaterialSchema* material = resolved.material;
    if (material == nullptr) {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL",
               "Material '" + *id + "' is not a built-in material." +
                   did_you_mean(*id, registry_.material_ids()),
               call->arguments.front().span, component_path,
               expected_names(registry_.material_ids()));
    }
    for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
        const Argument& argument = call->arguments[index];
        if (!argument.name.has_value()) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT", "Material parameters must be named.",
                   argument.span, component_path, "named material parameter");
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        if (resolved.arguments[index].duplicate) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                   "Material parameter '" + *argument.name + "' is supplied more than once.",
                   argument.span, component_path + "." + *argument.name,
                   "unique material parameter");
        }
        const SchemaParameter* parameter = resolved.arguments[index].parameter;
        if (parameter == nullptr) {
            const std::vector<std::string> names =
                material != nullptr ? parameter_names(material->parameters)
                                    : std::vector<std::string>{};
            report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL_PARAMETER",
                   "Material '" + *id + "' does not define parameter '" + *argument.name +
                       "'." + did_you_mean(*argument.name, names),
                   argument.span, component_path + "." + *argument.name,
                   expected_names(names));
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        validate_expected(*argument.value, *parameter->type, false,
                          component_path + "." + parameter->name);
    }
    return true;
}

bool TypeChecker::validate_effect(const Expression& expression,
                                  const std::string& component_path) {
    if (const std::string* name = string_literal_value(expression); name != nullptr) {
        if (registry_.effect(*name) == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_EFFECT",
                   "Effect '" + *name + "' is not declared." +
                       did_you_mean(*name, registry_.effect_names()),
                   expression.span, component_path, expected_names(registry_.effect_names()));
        }
        return true;
    }
    const auto* call = std::get_if<CallExpression>(&expression.node);
    if (call == nullptr || call->target.qualified_name() != "effect") return false;
    checked_[expression.id] = true;
    const ResolvedCall& resolved = *model_.call(expression);
    const std::string* name =
        !call->arguments.empty() ? string_literal_value(*call->arguments.front().value) : nullptr;
    if (name == nullptr) {
        report("STRATA.DSL.SEMANTIC_DYNAMIC_EFFECT",
               "effect(...) requires a static effect name string.", expression.span,
               component_path, expected_names(registry_.effect_names()));
    }
    const EffectSchema* effect = resolved.effect;
    if (name != nullptr && effect == nullptr) {
        report("STRATA.DSL.SEMANTIC_UNKNOWN_EFFECT",
               "Effect '" + *name + "' is not declared." +
                   did_you_mean(*name, registry_.effect_names()),
               call->arguments.front().span, component_path,
               expected_names(registry_.effect_names()));
    }
    std::set<std::string, std::less<>> seen;
    for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
        const Argument& argument = call->arguments[index];
        if (!argument.name.has_value()) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT", "Effect parameters must be named.",
                   argument.span, component_path, "named effect parameter");
            static_cast<void>(infer(*argument.value, component_path));
            continue;
        }
        seen.insert(*argument.name);
        if (resolved.arguments[index].duplicate) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                   "Effect parameter '" + *argument.name + "' is supplied more than once.",
                   argument.span, component_path, "unique effect parameter");
        }
        if (*argument.name == "backdropSource") {
            const std::string* literal = string_literal_value(*argument.value);
            if (literal == nullptr || (*literal != "CURRENT" && *literal != "SURFACE")) {
                report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                       "Effect backdropSource must be \"CURRENT\" or \"SURFACE\".",
                       argument.span, component_path + ".backdropSource", "CURRENT, SURFACE");
            } else if (effect != nullptr && effect->input != "BACKDROP") {
                report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                       "Effect backdropSource is available only to BACKDROP effects.",
                       argument.span, component_path + ".backdropSource", "BACKDROP effect");
            }
            continue;
        }
        if (*argument.name == "refreshRate") {
            const SemanticTypePtr actual =
                infer(*argument.value, component_path + ".refreshRate");
            const std::string* literal = string_literal_value(*argument.value);
            const std::optional<StaticNumber> number = static_number(*argument.value);
            const bool accepted_type = actual->kind == SemanticTypeKind::number ||
                                       (literal != nullptr && *literal == "UNBOUNDED");
            const bool accepted_number =
                !number.has_value() || (!number->unit.has_value() && number->value > 0.0);
            if (!accepted_type || !accepted_number) {
                report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                       "Effect refreshRate must be a positive number or \"UNBOUNDED\".",
                       argument.span, component_path + ".refreshRate", "number, UNBOUNDED");
            }
            continue;
        }
        const SchemaParameter* parameter = resolved.arguments[index].parameter;
        if (parameter == nullptr) {
            if (effect != nullptr) {
                std::vector<std::string> names;
                if (effect->input == "BACKDROP") names.push_back("backdropSource");
                names.push_back("refreshRate");
                for (const SchemaParameter& candidate : effect->parameters)
                    names.push_back(candidate.name);
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                       "Effect '" + *name + "' does not define parameter '" + *argument.name +
                           "'." + did_you_mean(*argument.name, names),
                       argument.span, component_path, expected_names(names));
            }
            static_cast<void>(infer(*argument.value, component_path));
        } else {
            validate_expected(*argument.value, *parameter->type, parameter->nullable,
                              component_path + "." + parameter->name);
        }
    }
    if (effect != nullptr) {
        for (const SchemaParameter& parameter : effect->parameters) {
            if (parameter.required && !seen.contains(parameter.name)) {
                report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                       "Effect '" + *name + "' requires parameter '" + parameter.name + "'.",
                       expression.span, component_path, parameter.name);
            }
        }
    }
    return true;
}

bool TypeChecker::validate_style(const Expression& expression, const SemanticType& expected,
                                 const std::string& component_path) {
    if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
        const Symbol* symbol = model_.symbol(expression);
        if (symbol != nullptr && reads_value(symbol->kind)) return false;
        if (model_.styles.contains(identifier->name)) return true;
        unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_STYLE", identifier->name, true,
                   "Style '" + identifier->name + "' is not declared." +
                       did_you_mean(identifier->name, analysis_.style_names),
                   expression.span, component_path, expected_names(analysis_.style_names));
        return true;
    }
    if (const auto* style = std::get_if<MapExpression>(&expression.node)) {
        checked_[expression.id] = true;
        // The parser already reports a key written twice in a map literal.
        std::vector<LayoutProperty> entries;
        std::vector<const Expression*> values;
        for (const MapEntry& entry : style->entries) {
            const std::string* name = static_map_key(entry.key);
            entries.push_back(LayoutProperty{name != nullptr ? *name : std::string{},
                                             entry.value.get(), entry.span});
            values.push_back(entry.value.get());
        }
        style_entries(entries, values, component_path);
        return true;
    }
    const auto* call = std::get_if<CallExpression>(&expression.node);
    if (call == nullptr || call->target.qualified_name() != "style") return false;
    checked_[expression.id] = true;
    const ResolvedCall& resolved = *model_.call(expression);
    std::vector<LayoutProperty> layout_properties;
    for (std::size_t index = 0U; index < call->arguments.size(); ++index) {
        const Argument& argument = call->arguments[index];
        const ArgumentBinding& binding = resolved.arguments[index];
        if (!argument.name.has_value()) {
            if (binding.out_of_order) {
                report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                       "Positional style bases must appear before named property overrides.",
                       argument.span, component_path, "named style property");
            }
            validate_expected(*argument.value, expected, false, component_path + ".base");
            continue;
        }
        const std::string& name = *argument.name;
        layout_properties.push_back(LayoutProperty{name, argument.value.get(), argument.span});
        if (binding.duplicate) {
            report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                   "Style property '" + name + "' is provided more than once.", argument.span,
                   component_path + "." + name, "single value for '" + name + "'");
        }
        const SchemaParameter* property = binding.parameter;
        if (property == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                   "Style property '" + name + "' is not supported." +
                       did_you_mean(name, registry_.style_property_names()),
                   argument.span, component_path + "." + name,
                   expected_names(registry_.style_property_names()));
            static_cast<void>(infer(*argument.value, component_path));
        } else {
            validate_expected(*argument.value, *property->type, property->nullable,
                              component_path + "." + name);
        }
    }
    check_layout_kind(analysis_, &oracle_, layout_properties, component_path);
    return true;
}

bool TypeChecker::validate_animation(const Expression& expression,
                                     const std::string& component_path) {
    std::optional<std::string> name;
    if (const std::string* literal = string_literal_value(expression); literal != nullptr) {
        name = *literal;
    } else if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
        const Symbol* symbol = model_.symbol(expression);
        if (symbol == nullptr || !reads_value(symbol->kind)) name = identifier->name;
    }
    const std::vector<std::string>& animations = analysis_.animation_names;
    if (name.has_value()) {
        if (!std::ranges::binary_search(animations, *name)) {
            unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION", *name, true,
                       "Animation '" + *name + "' is not declared." +
                           did_you_mean(*name, animations),
                       expression.span, component_path, expected_names(animations));
        }
        return true;
    }
    const auto* call = std::get_if<CallExpression>(&expression.node);
    if (call == nullptr || call->target.qualified_name() != "animation") return false;
    checked_[expression.id] = true;
    if (const HelperSchema* helper = registry_.helper("animation"); helper != nullptr)
        helper_arguments(expression, *call, *helper, component_path);
    const std::string* animation_name =
        !call->arguments.empty() ? string_literal_value(*call->arguments.front().value) : nullptr;
    if (animation_name != nullptr && !std::ranges::binary_search(animations, *animation_name)) {
        unresolved("STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION", *animation_name, true,
                   "Animation '" + *animation_name + "' is not declared." +
                       did_you_mean(*animation_name, animations),
                   call->arguments.front().span, component_path, expected_names(animations));
    }
    return true;
}

void TypeChecker::validate_layout(const MapExpression& layout, const std::string& component_path) {
    std::vector<LayoutProperty> layout_properties;
    for (const MapEntry& entry : layout.entries) {
        const std::string* key = static_map_key(entry.key);
        const std::string name = key != nullptr ? *key : std::string{};
        layout_properties.push_back(LayoutProperty{name, entry.value.get(), entry.span});
        const SchemaParameter* property = registry_.layout_property(name);
        if (property == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_LAYOUT_PROPERTY",
                   "Layout property '" + name + "' is not declared." +
                       did_you_mean(name, registry_.layout_property_names()),
                   entry.span, component_path + "." + name,
                   expected_names(registry_.layout_property_names()));
            static_cast<void>(infer(*entry.value, component_path + "." + name));
        } else {
            validate_expected(*entry.value, *property->type, property->nullable,
                              component_path + "." + property->name);
        }
    }
    check_layout_kind(analysis_, &oracle_, layout_properties, component_path);
}

void TypeChecker::validate_expected(const Expression& expression, const SemanticType& expected,
                                    const bool nullable, const std::string& component_path) {
    if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
        checked_[expression.id] = true;
        validate_expected(*grouping->expression, expected, nullable, component_path);
        return;
    }
    if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
        checked_[expression.id] = true;
        validate_expected(*conditional->condition, *simple(SemanticTypeKind::boolean), false,
                          component_path + ".condition");
        validate_expected(*conditional->then_expression, expected, nullable,
                          component_path + ".then");
        validate_expected(*conditional->else_expression, expected, nullable,
                          component_path + ".else");
        return;
    }
    if (const auto* binary = std::get_if<BinaryExpression>(&expression.node);
        binary != nullptr && binary->operation == BinaryOperator::coalesce) {
        checked_[expression.id] = true;
        validate_expected(*binary->left, expected, true, component_path + ".left");
        validate_expected(*binary->right, expected, nullable, component_path + ".right");
        return;
    }
    if (const auto* literal = std::get_if<LiteralExpression>(&expression.node);
        literal != nullptr && std::holds_alternative<NullLiteral>(literal->value)) {
        if (!nullable)
            type_mismatch(expected, *simple(SemanticTypeKind::null_value), expression,
                          component_path);
        return;
    }
    if (expected.kind == SemanticTypeKind::action) {
        if (const std::string* id = string_literal_value(expression); id != nullptr) {
            const ActionSchema* action = registry_.action(*id);
            if (action == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ACTION",
                       "Action '" + *id + "' is not registered." +
                           did_you_mean(*id, registry_.action_names()),
                       expression.span, component_path,
                       expected_names(registry_.action_names()));
            } else {
                for (const SchemaParameter& parameter : action->parameters) {
                    if (!parameter.required) continue;
                    report("STRATA.DSL.SEMANTIC_MISSING_ACTION_ARGUMENT",
                           "Action '" + *id + "' requires argument '" + parameter.name +
                               "'; use action(...) with named arguments.",
                           expression.span, component_path,
                           parameter.name + ": " + parameter.type->diagnostic_name());
                }
            }
            return;
        }
        if (const auto* call = std::get_if<CallExpression>(&expression.node);
            call != nullptr && call->target.qualified_name() == "action") {
            checked_[expression.id] = true;
            action_call(expression, *call, component_path);
            return;
        }
    }
    if (expected.kind == SemanticTypeKind::component_template) {
        component_template(expression, expected, component_path);
        return;
    }
    if (expected.kind == SemanticTypeKind::material && validate_material(expression, component_path))
        return;
    if (expected.kind == SemanticTypeKind::animation &&
        validate_animation(expression, component_path)) {
        return;
    }
    if (expected.kind == SemanticTypeKind::style &&
        validate_style(expression, expected, component_path)) {
        return;
    }
    if (expected.kind == SemanticTypeKind::effect && validate_effect(expression, component_path))
        return;
    if (expected.kind == SemanticTypeKind::layout) {
        if (const auto* layout = std::get_if<MapExpression>(&expression.node)) {
            checked_[expression.id] = true;
            validate_layout(*layout, component_path);
            return;
        }
    }
    if (const auto* list = std::get_if<ListExpression>(&expression.node);
        list != nullptr && expected.kind == SemanticTypeKind::list &&
        expected.element != nullptr) {
        checked_[expression.id] = true;
        for (std::size_t index = 0U; index < list->elements.size(); ++index) {
            validate_expected(*list->elements[index], *expected.element,
                              expected.element_nullable,
                              component_path + "[" + std::to_string(index) + "]");
        }
        if ((expected.minimum_items.has_value() &&
             list->elements.size() < *expected.minimum_items) ||
            (expected.maximum_items.has_value() &&
             list->elements.size() > *expected.maximum_items)) {
            report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                   "Expected " + expected.diagnostic_name() + ", but found " +
                       std::to_string(list->elements.size()) + " items.",
                   expression.span, component_path, expected.diagnostic_name());
        }
        return;
    }
    if (const auto* map = std::get_if<MapExpression>(&expression.node); map != nullptr) {
        std::vector<const SemanticType*> options;
        collect_object_options(expected, options);
        if (!options.empty()) {
            std::vector<std::string> keys;
            for (const MapEntry& entry : map->entries) {
                if (const std::string* key = static_map_key(entry.key)) keys.push_back(*key);
            }
            std::vector<const SemanticType*> matches;
            for (const SemanticType* option : options) {
                const bool declares_every_key =
                    std::ranges::all_of(keys, [option](const std::string& key) {
                        return option->allow_unknown_fields ||
                               option->find_field(key) != nullptr;
                    });
                if (declares_every_key) matches.push_back(option);
            }
            if (matches.size() == 1U) {
                checked_[expression.id] = true;
                object_fields(*matches.front(), *map, expression.span, component_path);
                return;
            }
            if (matches.empty()) {
                type_mismatch(expected, *infer(expression, component_path), expression,
                              component_path);
                return;
            }
        }
    }
    if (expected.kind == SemanticTypeKind::path) {
        if (const std::string* commands = string_literal_value(expression);
            commands != nullptr) {
            if (const std::optional<std::string> error = path_outline_error(*commands)) {
                report("STRATA.DSL.SEMANTIC_INVALID_PATH", "Path outline is invalid: " + *error,
                       expression.span, component_path,
                       "a path outline of M/L/H/V/Q/C/Z commands");
            }
            return;
        }
    }
    const SemanticTypePtr actual = infer(expression, component_path);
    if (!type_matches(expected, *actual, nullable))
        type_mismatch(expected, *actual, expression, component_path);
}

void TypeChecker::object_fields(const SemanticType& expected, const MapExpression& map,
                                const SourceSpan& span, const std::string& component_path) {
    std::set<std::string, std::less<>> seen;
    for (const MapEntry& entry : map.entries) {
        const std::string* key = static_map_key(entry.key);
        const std::string name = key != nullptr ? *key : std::string{};
        const std::string path = component_path + "." + name;
        const ObjectField* field = expected.find_field(name);
        if (field == nullptr) {
            if (expected.allow_unknown_fields && expected.value != nullptr) {
                validate_expected(*entry.value, *expected.value, expected.value_nullable, path);
                continue;
            }
            if (expected.allow_unknown_fields) {
                static_cast<void>(infer(*entry.value, path));
                continue;
            }
            std::vector<std::string> names;
            names.reserve(expected.fields.size());
            for (const ObjectField& declared : expected.fields) names.push_back(declared.name);
            report("STRATA.DSL.SEMANTIC_UNKNOWN_FIELD",
                   "Field '" + name + "' is not part of " + expected.diagnostic_name() + "." +
                       did_you_mean(name, names),
                   entry.span, path, expected_names(names));
            static_cast<void>(infer(*entry.value, path));
            continue;
        }
        seen.insert(name);
        validate_expected(*entry.value, *field->type, field->nullable, path);
    }
    for (const ObjectField& field : expected.fields) {
        if (!field.required || seen.contains(field.name)) continue;
        report("STRATA.DSL.SEMANTIC_MISSING_FIELD",
               "Field '" + field.name + "' is required by " + expected.diagnostic_name() + ".",
               span, component_path + "." + field.name, field.type->diagnostic_name());
    }
}

void TypeChecker::type_mismatch(const SemanticType& expected, const SemanticType& actual,
                                const Expression& expression, const std::string& component_path) {
    report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
           "Expected " + expected.diagnostic_name() + ", but found " + actual.diagnostic_name() +
               "." + mismatch_hint(expected, actual),
           expression.span, component_path, expected.diagnostic_name());
}

} // namespace strata::compiler::semantic
