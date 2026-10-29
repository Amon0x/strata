#pragma once

#include <initializer_list>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/semantic/analysis.hpp"

// Pass 3: type check. Every expression is checked once, in the slot it fills: its own problems
// (unknown names and fields, operators, calls) and whether its type fits what the slot expects.
// The checker's members are defined by concern: statements and widget calls (type_check.cpp),
// helper, action and component-template calls (type_check_calls.cpp), values against the kind a
// slot expects (type_check_slots.cpp), and expressions on their own (type_check_expressions.cpp).
namespace strata::compiler::semantic {

enum class BlockContext { component, ui, lazy_ui };

[[nodiscard]] inline BlockContext nested_context(const BlockContext context) {
    return context == BlockContext::component ? BlockContext::ui : context;
}

[[nodiscard]] inline std::vector<std::string> parameter_names(
    const std::vector<SchemaParameter>& parameters
) {
    std::vector<std::string> names;
    names.reserve(parameters.size());
    for (const SchemaParameter& parameter : parameters) names.push_back(parameter.name);
    return names;
}

class TypeChecker final {
public:
    TypeChecker(Analysis& analysis, TypeOracle& oracle)
        : analysis_(analysis),
          oracle_(oracle),
          registry_(analysis.registry),
          model_(analysis.model),
          checked_(analysis.file.node_id_limit, false) {}

    void run() {
        for (const Declaration& declaration : analysis_.file.declarations) declaration_check(declaration);
    }

private:
    void report(std::string code, std::string message, const SourceSpan& span, std::string path,
                std::string expected) {
        analysis_.diagnostics.report(std::move(code), std::move(message), span, std::move(path),
                                     std::move(expected));
    }

    /**
     * Reports a name that resolves to nothing, unless a declaration that failed to parse or load
     * could be what it names; `module_level` when a module declaration could declare it.
     */
    void unresolved(std::string code, const std::string_view name, const bool module_level,
                    std::string message, const SourceSpan& span, std::string path,
                    std::string expected) {
        analysis_.diagnostics.report_unresolved(std::move(code), name, module_level,
                                                std::move(message), span, std::move(path),
                                                std::move(expected));
    }

    void declaration_check(const Declaration& declaration);

    void style_property(const Property& property, const std::string& path);

    void block(const Block& block, const std::string& path, const std::size_t maximum_loop_items,
               const BlockContext context);

    void statement(const Statement& statement, const std::string& path,
                   const std::size_t maximum_loop_items, const BlockContext context);

    void for_statement(const ForStatement& loop, const std::string& path,
                       const std::size_t maximum_loop_items, const BlockContext context);

    void widget_defaults(const Property& property, const std::string& path);

    void call(const WidgetCall& call, const std::string& path, const std::size_t maximum_loop_items,
              const BlockContext context);

    /** Positional, named and variadic helper arguments against the helper's parameters. */
    void helper_arguments(const Expression& expression, const CallExpression& call,
                          const HelperSchema& helper, const std::string& component_path);

    void action_call(const Expression& expression, const CallExpression& call,
                     const std::string& component_path);

    /** An action's argument `name`; the first argument is always the action id. */
    [[nodiscard]] static const Expression* named_argument(const CallExpression& call,
                                                          const std::string_view name);

    /** The type a state can hold, rather than the literal it starts from. */
    [[nodiscard]] static SemanticTypePtr widened(const SemanticTypePtr& type);

    /** The innermost state a state action's target names where the action is written. */
    [[nodiscard]] SemanticTypePtr state_in_scope(const Expression& action,
                                                 const std::string& name);

    /**
     * A state action names its target with a string, which the compiler can check: the name must
     * be a declared state, and where that state is in scope its value must fit. Actions in a
     * component may target a state of whatever screen hosts it, so only the name is checked there
     * when the state is not local.
     */
    void state_action(const std::string& id, const Expression& expression,
                      const CallExpression& call, const std::string& component_path);

    [[nodiscard]] std::set<std::string, std::less<>> component_template_arguments(
        const Expression& expression, const CallExpression& call,
        const std::string& template_name, const ComponentInfo& component,
        const SemanticType* owner_contract, const std::string& component_path);

    void component_template(const Expression& expression, const SemanticType& expected,
                            const std::string& component_path);

    /** Style entries, checked property by property, then their layout kind. */
    void style_entries(const std::vector<LayoutProperty>& entries,
                       const std::vector<const Expression*>& values,
                       const std::string& component_path);

    [[nodiscard]] bool validate_material(const Expression& expression,
                                         const std::string& component_path);

    [[nodiscard]] bool validate_effect(const Expression& expression,
                                       const std::string& component_path);

    [[nodiscard]] bool validate_style(const Expression& expression, const SemanticType& expected,
                                      const std::string& component_path);

    [[nodiscard]] bool validate_animation(const Expression& expression,
                                          const std::string& component_path);

    void validate_layout(const MapExpression& layout, const std::string& component_path);

    void validate_expected(const Expression& expression, const SemanticType& expected,
                           const bool nullable, const std::string& component_path);

    /**
     * A `behaviors` list that names one behavior twice: a node attaches each behavior once, so the
     * second attachment is reported where it is written (behaviors whose `id` is computed are
     * checked when the node is built).
     */
    void behavior_attachments(const Expression& behaviors, const std::string& component_path);

    /**
     * Structural validation of a declared object type: every authored entry is checked against its
     * declared field type, unknown entries are rejected unless the type opts into them, and missing
     * required fields are reported at the object itself.
     */
    void object_fields(const SemanticType& expected, const MapExpression& map,
                       const SourceSpan& span, const std::string& component_path);

    void type_mismatch(const SemanticType& expected, const SemanticType& actual,
                       const Expression& expression, const std::string& component_path);

    /**
     * A name that resolves to nothing would read as a missing host binding at runtime and quietly
     * draw nothing. With the host roots declared it is certainly a mistake; without them it may be
     * an undeclared host root, so it is reported only when it is a near miss of a name in scope.
     */
    void unknown_identifier(const IdentifierExpression& identifier, const Expression& expression,
                            const std::string& component_path);

    /**
     * Reports a `theme.<name>` operand. A token stays symbolic until the surface resolves its
     * effective theme, after evaluation, so an operator never sees its colour or number.
     */
    [[nodiscard]] bool theme_token_operand(const Expression& expression,
                                           std::initializer_list<const Expression*> operands,
                                           const std::string& symbol,
                                           const std::string& component_path);

    /** Reports an operand the runtime cannot do arithmetic with. */
    void arithmetic(const Expression& expression, const std::string& symbol,
                    const SemanticType& left, const SemanticType& right,
                    const std::string& component_path);

    /** Checks an expression on its own and returns its type. */
    [[nodiscard]] SemanticTypePtr infer(const Expression& expression,
                                        const std::string& component_path);

    void check(const Expression& expression, const std::string& component_path);

    /**
     * A lambda's body, checked like any expression and, where a helper takes the lambda, against
     * the result its signature declares: a predicate decides with a boolean.
     */
    void lambda_body(const Expression& expression, const LambdaExpression& lambda,
                     const std::string& component_path);

    void property_access(const Expression& expression, const PropertyAccessExpression& property,
                         const std::string& component_path);

    void call_expression(const Expression& expression, const CallExpression& call,
                         const std::string& component_path);

    Analysis& analysis_;
    TypeOracle& oracle_;
    const SchemaRegistry& registry_;
    SemanticModel& model_;
    std::vector<bool> checked_;
};

} // namespace strata::compiler::semantic
