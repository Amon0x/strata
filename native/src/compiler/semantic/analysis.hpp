#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/diagnostic.hpp"
#include "compiler/schema.hpp"
#include "compiler/semantic.hpp"
#include "compiler/semantic/model.hpp"

/**
 * Internals shared by the semantic passes. Analysis runs as a sequence of passes over one
 * `Analysis`: collect declarations, resolve names, type check, policy lints, then placement. Each
 * pass reads what the earlier ones recorded in the model and adds its own facts.
 */
namespace strata::compiler::semantic {

inline constexpr std::size_t maximum_lazy_loop_items = 100'000U;

[[nodiscard]] SemanticTypePtr simple(SemanticTypeKind kind);
[[nodiscard]] SemanticTypePtr literal_string(std::string value);
[[nodiscard]] std::string lower(std::string_view value);
[[nodiscard]] std::string expected_names(std::vector<std::string> names);
/** The candidate a misspelled name most likely meant, if any is close enough. */
[[nodiscard]] std::optional<std::string> nearest_name(
    std::string_view name,
    const std::vector<std::string>& candidates
);
/** " Did you mean 'x'?" for a likely misspelling, or nothing. */
[[nodiscard]] std::string did_you_mean(
    std::string_view name,
    const std::vector<std::string>& candidates
);
[[nodiscard]] const std::string* string_literal_value(const Expression& expression);
[[nodiscard]] std::optional<bool> boolean_literal_value(const Expression& expression);
[[nodiscard]] bool type_matches(const SemanticType& expected, const SemanticType& actual, bool nullable);
[[nodiscard]] const std::string* static_map_key(const MapKey& key);
[[nodiscard]] const SourceSpan& map_key_span(const MapKey& key);
[[nodiscard]] const Expression& ungrouped(const Expression& expression);

struct StaticNumber final {
    double value;
    std::optional<std::string> unit;
};

[[nodiscard]] std::optional<StaticNumber> static_number(const Expression& expression);
[[nodiscard]] const LiteralExpression* static_literal(const Expression& expression);
[[nodiscard]] std::string canonical_color(std::string raw);
/** The widget a widget or root statement calls, or none. */
[[nodiscard]] const WidgetCall* statement_call(const Statement& statement);

struct RootCardinality final {
    std::size_t minimum = 0U;
    std::size_t maximum = 0U;
    /** A statement the parser could not read stands in, so the count is a guess. */
    bool uncertain = false;
};

[[nodiscard]] RootCardinality root_cardinality(const Block& block);

/** The retained state or Binding<T> parameter `value` names directly, if it does. */
[[nodiscard]] SymbolId binding_target(const SemanticModel& model, const Expression& value);
[[nodiscard]] inline bool names_binding_target(const SemanticModel& model, const Expression& value) {
    return binding_target(model, value) != no_symbol;
}

/** Whether calls to `name` declare or fill component slots rather than draw a widget. */
[[nodiscard]] bool is_slot_widget(const SchemaRegistry& registry, std::string_view name);
/** Whether a widget generates its rows lazily from one keyed loop. */
[[nodiscard]] bool generates_lazy_rows(const WidgetSchema& widget);
/** The layout kind a widget lays its children out with when no style names one. */
[[nodiscard]] std::string_view implicit_layout_kind(const WidgetSchema& widget);

/** Why a path outline is malformed, or nothing when it parses; see path_outline.cpp. */
[[nodiscard]] std::optional<std::string> path_outline_error(const std::string& commands);

class Diagnostics final {
public:
    /**
     * Records an error. A problem is found once per context that checks it, a shared style read by
     * several widgets for one, so the same code, message and span is kept only the first time.
     */
    void report(
        std::string code,
        std::string message,
        const SourceSpan& span,
        std::string component_path,
        std::string expected
    );
    /** Drops reports of names that resolve to nothing; see `AnalysisInput::incomplete`. */
    void suppress_unresolved_names() noexcept { suppress_unresolved_names_ = true; }
    [[nodiscard]] std::size_t size() const noexcept { return diagnostics_.size(); }
    [[nodiscard]] std::vector<Diagnostic> take() { return std::move(diagnostics_); }

private:
    std::vector<Diagnostic> diagnostics_;
    bool suppress_unresolved_names_ = false;
    std::set<std::tuple<std::string, std::string, std::string, std::uint64_t, std::uint64_t>>
        reported_;
};

/** A layout-affecting property gathered from a style, layout object or widget default. */
struct LayoutProperty final {
    std::string name;
    const Expression* value = nullptr;
    SourceSpan span;
};

struct PersistenceOwner final {
    /** The key literal, which identifies one widget or state however often it is checked. */
    const Expression* key = nullptr;
    std::string path;
};

class TypeOracle;

/** Everything the passes share: the inputs, the model being built and the diagnostics. */
struct Analysis final {
    Analysis(const File& file, const SchemaRegistry& registry, SemanticModel& model);

    const File& file;
    const SchemaRegistry& registry;
    SemanticModel& model;
    Diagnostics diagnostics;

    /** Declared names, sorted, for suggestions and expected-value lists. */
    std::vector<std::string> style_names;
    std::vector<std::string> animation_names;
    std::vector<std::string> component_names;
    /** Style declarations by name; the first of a duplicated name wins. */
    std::map<std::string, const StyleDeclaration*, std::less<>> style_declarations;
    /** Every state name declared anywhere, sorted: what a state action can target. */
    std::vector<std::string> state_names;

    /** Unknown type names a state declaration used, by statement id, for the type check. */
    std::map<NodeId, std::vector<const TypeReference*>> unresolved_state_types;
    /** Names visible where an unresolved identifier was read, by expression id. */
    std::map<NodeId, std::vector<std::string>> visible_names;
    /**
     * What the `name:` of a state action resolves to where the action is written, by action call
     * id: the state it targets when that state is in scope there.
     */
    std::map<NodeId, SymbolId> action_state_targets;
    /** The call and argument index whose value is a lambda, by lambda expression id. */
    std::map<NodeId, std::pair<const Expression*, std::size_t>> lambda_arguments;

    /** Layout values already reported, so one shared value is diagnosed once. */
    std::set<const Expression*> diagnosed_layout_properties;
    std::map<std::string, PersistenceOwner, std::less<>> persistence_keys;
    std::map<std::string, ValidatedAnimation, std::less<>> animations;

    [[nodiscard]] ExpressionFacts& facts(const Expression& expression);
    [[nodiscard]] SymbolId add_symbol(Symbol symbol);
};

/**
 * Pure, memoized types. A type depends only on the expression and the symbols its names resolved
 * to, never on where it is asked for, so each expression is typed once and reporting is left to
 * the checking passes.
 */
class TypeOracle final {
public:
    explicit TypeOracle(Analysis& analysis);

    [[nodiscard]] SemanticTypePtr type_of(const Expression& expression);
    [[nodiscard]] SemanticTypePtr symbol_type(SymbolId symbol);
    /** The argument of `call` that fills parameter `name`, by the recorded argument mapping. */
    [[nodiscard]] const Argument* argument_for(const Expression& call, std::string_view name) const;
    [[nodiscard]] SemanticTypePtr component_template_type(
        const ComponentInfo& component,
        const std::set<std::string, std::less<>>& bound = {}
    ) const;

private:
    [[nodiscard]] SemanticTypePtr compute(const Expression& expression);
    [[nodiscard]] SemanticTypePtr compute_call(
        const Expression& expression,
        const CallExpression& call
    );
    [[nodiscard]] SemanticTypePtr compute_symbol(SymbolId symbol);

    Analysis& analysis_;
    std::vector<SemanticTypePtr> symbol_types_;
    std::vector<bool> resolving_;
};

/** How an operand takes part in arithmetic, as the runtime evaluates it. */
enum class OperandClass { number, text, other, undetermined };

[[nodiscard]] OperandClass operand_class(const SemanticType& type);
[[nodiscard]] SemanticTypePtr theme_token_type(std::string_view name);
/** A state's type, given its declared type and its initializer's. */
[[nodiscard]] SemanticTypePtr retained_state_type(
    const StateStatement& state,
    SemanticTypePtr declared,
    const SemanticTypePtr& initializer
);

/** Checks layout properties against the layout kind they end up with. */
void check_layout_kind(
    Analysis& analysis,
    TypeOracle* oracle,
    const std::vector<LayoutProperty>& properties,
    const std::string& path,
    std::string_view implicit_kind = {}
);

/** Records `key_expression` as the owner of persistence key `key`, reporting a second owner. */
void claim_persistence_key(
    Analysis& analysis,
    const std::string& key,
    const Expression& key_expression,
    const std::string& owner_path,
    const std::string& component_path
);

void collect_declarations(Analysis& analysis);
void resolve_names(Analysis& analysis);
void check_types(Analysis& analysis, TypeOracle& oracle);
void check_policies(Analysis& analysis, TypeOracle& oracle);
/** Validates an animation declaration into its checked, lowerable form. */
void check_animation(
    Analysis& analysis,
    const AnimationDeclaration& animation,
    const SourceSpan& declaration_span,
    const std::function<void(const Expression&, const SemanticType&, bool, const std::string&)>&
        validate_expected,
    const std::function<void(const Expression&, const std::string&)>& check_value
);
void assign_placements(Analysis& analysis, TypeOracle& oracle);

} // namespace strata::compiler::semantic
