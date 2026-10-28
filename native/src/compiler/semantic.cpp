#include "compiler/semantic.hpp"

#include <utility>

#include "compiler/semantic/analysis.hpp"

namespace strata::compiler {

SemanticTypePtr resolve_type_reference(
    const TypeReference& reference,
    const SchemaRegistry& registry,
    std::vector<const TypeReference*>* const unresolved
) {
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
    analysis.diagnostics.set_missing(semantic::MissingDeclarations{
        input == AnalysisInput::missing_imports,
        file.truncated,
        &file.unparsed_names,
    });
    semantic::collect_declarations(analysis);
    semantic::resolve_names(analysis);
    semantic::TypeOracle oracle(analysis);
    semantic::check_types(analysis, oracle);
    semantic::check_policies(analysis, oracle);
    semantic::assign_placements(analysis, oracle);
    result.diagnostics = analysis.diagnostics.take();
    result.animations = std::move(analysis.animations);
    return result;
}

} // namespace strata::compiler
