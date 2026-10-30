#include "compiler/semantic.hpp"

#include <algorithm>
#include <utility>

#include "compiler/semantic/analysis.hpp"

namespace strata::compiler {

namespace semantic {
namespace {

/** The aliases the analysis in progress on this thread declared. */
thread_local const TypeAliases* active_type_aliases = nullptr;

} // namespace

TypeAliasScope::TypeAliasScope(const TypeAliases& aliases) : previous_(active_type_aliases) {
    active_type_aliases = &aliases;
}

TypeAliasScope::~TypeAliasScope() {
    active_type_aliases = previous_;
}

SemanticTypePtr resolve_type_alias(const std::string_view name, const SchemaRegistry& registry) {
    const TypeAliases* aliases = active_type_aliases;
    if (aliases == nullptr)
        return nullptr;
    const auto declared = aliases->declarations.find(name);
    if (declared == aliases->declarations.end())
        return nullptr;
    if (const auto known = aliases->resolved.find(name); known != aliases->resolved.end())
        return known->second;
    // A type defined through itself (a tree's children) reads as any value where it recurs.
    if (!aliases->resolving.insert(std::string(name)).second)
        return simple(SemanticTypeKind::any);
    SemanticTypePtr type = resolve_type_reference(declared->second->type, registry);
    aliases->resolving.erase(aliases->resolving.find(name));
    if (type->label.empty() || type->kind == SemanticTypeKind::map ||
        type->kind == SemanticTypeKind::enumeration ||
        type->kind == SemanticTypeKind::union_value) {
        auto named = std::make_shared<SemanticType>(*type);
        named->label = std::string(name);
        type = std::move(named);
    }
    aliases->resolved.emplace(std::string(name), type);
    return type;
}

} // namespace semantic

SemanticTypePtr resolve_type_reference(
    const TypeReference& reference,
    const SchemaRegistry& registry,
    std::vector<const TypeReference*>* const unresolved
) {
    const auto nullable = [&reference](SemanticTypePtr type) -> SemanticTypePtr {
        if (!reference.nullable || reference.kind == TypeReferenceKind::named)
            return type;
        auto either = std::make_shared<SemanticType>();
        either->kind = SemanticTypeKind::union_value;
        either->label = type->diagnostic_name() + "?";
        either->options = {std::move(type), semantic::simple(SemanticTypeKind::null_value)};
        return either;
    };
    switch (reference.kind) {
    case TypeReferenceKind::named:
        break;
    case TypeReferenceKind::string_literal:
        return nullable(semantic::literal_string(reference.name));
    case TypeReferenceKind::union_type: {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::union_value;
        // A union of strings only is an enumeration, which a `when` branches over.
        const bool strings = std::ranges::all_of(reference.arguments, [](const TypeReference& option) {
            return option.kind == TypeReferenceKind::string_literal;
        });
        std::string label;
        for (const TypeReference& option : reference.arguments) {
            SemanticTypePtr resolved = resolve_type_reference(option, registry, unresolved);
            if (!label.empty()) label += " | ";
            label += strings ? "\"" + option.name + "\"" : resolved->diagnostic_name();
            type->options.push_back(std::move(resolved));
            if (strings) type->values.push_back(option.name);
        }
        if (strings) {
            type->kind = SemanticTypeKind::enumeration;
            type->options.clear();
        }
        type->label = std::move(label);
        return nullable(std::move(type));
    }
    case TypeReferenceKind::record: {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::map;
        type->label = "record";
        for (const TypeField& field : reference.fields) {
            type->fields.push_back(ObjectField{
                field.name,
                resolve_type_reference(field.type, registry, unresolved),
                !field.optional,
                field.optional || field.type.nullable,
            });
        }
        return nullable(std::move(type));
    }
    case TypeReferenceKind::function: {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::function;
        type->returns = !reference.returns.empty()
                            ? resolve_type_reference(reference.returns.front(), registry, unresolved)
                            : semantic::simple(SemanticTypeKind::any);
        return nullable(std::move(type));
    }
    }
    const std::string name = semantic::lower(reference.name);
    const auto argument = [&](const std::size_t index) {
        return index < reference.arguments.size()
                   ? resolve_type_reference(reference.arguments[index], registry, unresolved)
                   : semantic::simple(SemanticTypeKind::unsafe_component_parameter);
    };
    if (name == "binding") {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::state_binding;
        type->value = argument(0U);
        return type;
    }
    if (name == "list") {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::list;
        type->element = argument(0U);
        type->element_nullable =
            !reference.arguments.empty() && reference.arguments.front().nullable;
        type->maximum_items = maximum_eager_loop_items;
        return type;
    }
    if (name == "map" || name == "record") {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::map;
        type->label = name;
        type->allow_unknown_fields = true;
        type->value = reference.arguments.empty() ? semantic::simple(SemanticTypeKind::any)
                                                  : argument(0U);
        return type;
    }
    if (const SemanticType* declared = registry.component_parameter_type(name); declared != nullptr)
        return std::make_shared<SemanticType>(*declared);
    if (const SemanticType* declared = registry.application_type(name); declared != nullptr)
        return std::make_shared<SemanticType>(*declared);
    if (SemanticTypePtr alias = semantic::resolve_type_alias(reference.name, registry)) {
        if (!reference.nullable) return alias;
        auto either = std::make_shared<SemanticType>();
        either->kind = SemanticTypeKind::union_value;
        either->label = reference.name + "?";
        either->options = {std::move(alias), semantic::simple(SemanticTypeKind::null_value)};
        return either;
    }
    if (unresolved != nullptr) unresolved->push_back(&reference);
    return semantic::simple(SemanticTypeKind::unknown);
}

SemanticTypePtr resolve_parameter_type(
    const Parameter& parameter,
    const SchemaRegistry& registry,
    std::vector<const TypeReference*>* const unresolved
) {
    return parameter.type_reference.has_value()
               ? resolve_type_reference(*parameter.type_reference, registry, unresolved)
               : semantic::simple(SemanticTypeKind::unsafe_component_parameter);
}

SemanticResult validate_semantics(
    const File& file,
    const SchemaRegistry& registry,
    const AnalysisInput input
) {
    SemanticResult result;
    semantic::Analysis analysis(file, registry, result.model);
    semantic::collect_type_aliases(analysis);
    const semantic::TypeAliasScope aliases(analysis.type_aliases);
    analysis.diagnostics.set_missing(semantic::MissingDeclarations{
        input == AnalysisInput::missing_imports,
        file.truncated,
        &file.unparsed_names,
    });
    semantic::collect_declarations(analysis);
    semantic::resolve_names(analysis);
    semantic::collect_row_template_slots(analysis);
    semantic::TypeOracle oracle(analysis);
    semantic::check_types(analysis, oracle);
    semantic::check_policies(analysis, oracle);
    semantic::assign_placements(analysis, oracle);
    result.diagnostics = analysis.diagnostics.take();
    result.animations = std::move(analysis.animations);
    return result;
}

} // namespace strata::compiler
