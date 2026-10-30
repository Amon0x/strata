#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "compiler/ast.hpp"
#include "compiler/schema.hpp"

namespace strata::compiler {

/**
 * The resolved, typed view of a module that semantic analysis produces. Everything later stages
 * need about a name, a call or an expression is decided here once and keyed by node id; portable
 * lowering and the source map project these tables and never resolve anything themselves.
 */

using SymbolId = std::uint32_t;
inline constexpr SymbolId no_symbol = std::numeric_limits<SymbolId>::max();

/** What a bare name denotes. Locals shadow host roots, which shadow the module's declarations. */
enum class SymbolKind : std::uint8_t {
    state,
    derived,
    loop_item,
    loop_index,
    lambda_parameter,
    parameter,
    /** A StrataScript `let` or `var`, or a `catch` binding. */
    local,
    /** A `fn` declaration, at the top of a file or in a block. */
    function,
    /** A parameter of a `fn` declaration. */
    function_parameter,
    host,
    component,
    style,
    animation,
    unresolved,
};

/** Whether a name of this kind reads a value at runtime rather than naming a declaration. */
[[nodiscard]] constexpr bool reads_value(const SymbolKind kind) noexcept {
    return kind != SymbolKind::component && kind != SymbolKind::style &&
           kind != SymbolKind::animation && kind != SymbolKind::unresolved;
}

struct ComponentInfo;

struct Symbol final {
    SymbolKind kind = SymbolKind::unresolved;
    std::string name;
    /** The statement declaring a state, derived value or loop variable. */
    const Statement* statement = nullptr;
    /** The lambda declaring a lambda parameter. */
    const Expression* lambda = nullptr;
    /** The function a function or function parameter belongs to. */
    const FunctionDeclaration* function = nullptr;
    /** A `var` local, which assignment may change. */
    bool mutable_binding = false;
    /** The component a parameter belongs to, or the component a name refers to. */
    const ComponentInfo* component = nullptr;
    /** A parameter's position in its component's, function's or lambda's parameter list. */
    std::size_t parameter_index = 0U;
    /**
     * The symbol a READY when branch narrows: inside the branch an async value reads as its loaded
     * value under the same name. With `non_null`, a test against null narrows it instead: past
     * `x != null` it is not null.
     */
    SymbolId narrows = no_symbol;
    bool non_null = false;
    /** The module a component, style or animation was declared in. */
    std::string source_id;
};

struct ComponentSlot final {
    std::string name;
    bool required = false;
    SourceSpan span;
};

/** A component's `defaults` entry for one widget, as lowering emits it. */
struct ComponentWidgetDefault final {
    std::string widget;
    const Expression* style = nullptr;
    const Expression* variant = nullptr;
};

struct ComponentInfo final {
    /** The declaration calls resolve to; a later one with the same name is a duplicate. */
    const ComponentDeclaration* declaration = nullptr;
    /** The module the component was declared in. */
    std::string source_id;
    std::vector<SchemaParameter> parameters;
    std::vector<ComponentSlot> slots;
    /** Default `style` per widget name, which layout checks merge into that widget's calls. */
    std::map<std::string, const Expression*, std::less<>> widget_default_styles;
    /** The widget defaults lowering emits, sorted by widget name. */
    std::vector<ComponentWidgetDefault> widget_defaults;

    [[nodiscard]] const SchemaParameter* find_parameter(std::string_view name) const noexcept;
};

/** A StrataScript function: its declaration and its parameters as call arguments bind them. */
struct FunctionInfo final {
    const FunctionDeclaration* declaration = nullptr;
    /** The module a top-level function was declared in; empty for one declared in a block. */
    std::string source_id;
    std::vector<SchemaParameter> parameters;
};

/** A module-level style or animation declaration. */
struct NamedDeclaration final {
    const Declaration* declaration = nullptr;
    std::string source_id;
};

/** How one authored argument maps onto the callee's parameters. */
struct ArgumentBinding final {
    /** The parameter the argument fills; none when nothing accepts it. */
    const SchemaParameter* parameter = nullptr;
    /** A helper argument taken by the helper's variadic tail. */
    bool vararg = false;
    /** A positional argument written after a named one. */
    bool out_of_order = false;
    /** The parameter was filled by an earlier argument already. */
    bool duplicate = false;
};

/** `bind: state` shorthand, which lowering expands into the controlled value and its event. */
struct BindingExpansion final {
    const WidgetBindingSchema* binding = nullptr;
    const SchemaParameter* controlled = nullptr;
    /** The shorthand argument, by index. */
    std::size_t shorthand = 0U;
    /** The retained state or Binding<T> parameter the shorthand names. */
    SymbolId target = no_symbol;
    std::optional<std::size_t> undo_label;
    std::optional<std::size_t> undo_coalesce;
};

enum class WidgetCallTarget : std::uint8_t { widget, component, unknown };

struct ResolvedWidgetCall final {
    WidgetCallTarget target = WidgetCallTarget::unknown;
    const WidgetSchema* widget = nullptr;
    const ComponentInfo* component = nullptr;
    std::vector<ArgumentBinding> arguments;
    std::vector<BindingExpansion> bindings;
    /** The callee generates its rows lazily from one keyed loop, as a Repeater does. */
    bool lazy_rows = false;
};

enum class CallKind : std::uint8_t {
    action,
    material,
    effect,
    style,
    component_template,
    helper,
    unknown_helper,
    /** A StrataScript function, or a local holding a function value. */
    function,
};

struct ResolvedCall final {
    CallKind kind = CallKind::unknown_helper;
    const HelperSchema* helper = nullptr;
    const ActionSchema* action = nullptr;
    const MaterialSchema* material = nullptr;
    const EffectSchema* effect = nullptr;
    const ComponentInfo* component = nullptr;
    /** For a function call: what the callee name denotes, and the function when it is declared. */
    SymbolId callee = no_symbol;
    const FunctionInfo* function = nullptr;
    std::vector<ArgumentBinding> arguments;
};

/** How lowering reads an expression in the slot it fills. */
enum class ExpressionForm : std::uint8_t {
    value,
    theme_token,
    material,
    action,
    animation_name,
    style_reference,
    key_literal,
    image_literal,
    component_template,
};

/**
 * An expression read as an action or a material: the literal id it names and the named arguments
 * after the id that lower, with the parameter each fills.
 */
struct IdentifiedCall final {
    std::string id;
    const ActionSchema* action = nullptr;
    const MaterialSchema* material = nullptr;
    std::vector<std::pair<std::size_t, const SchemaParameter*>> arguments;
};

/** Where a node sits in the portable IR and in runtime component paths. */
struct Placement final {
    std::string path;
    std::string component_path;
};

struct ExpressionFacts final {
    /** The inferred type; empty for expressions analysis never reaches. */
    SemanticTypePtr type;
    /** What an identifier denotes. */
    SymbolId symbol = no_symbol;
    /** The call an expression call resolved to, by index into `SemanticModel::calls`. */
    std::uint32_t call = std::numeric_limits<std::uint32_t>::max();
    ExpressionForm form = ExpressionForm::value;
    /** An identifier passed where a Binding<T> parameter expects the state itself. */
    bool state_binding = false;
    /** A function given where an action is expected: it lowers wrapped, run with the event. */
    bool handler = false;
};

struct SemanticModel final {
    /** Per node id; expressions, statements, blocks and widget calls share one numbering. */
    std::vector<ExpressionFacts> expressions;
    std::vector<Placement> placements;
    /** Declaration placements, index-aligned with the analysed file's declarations. */
    std::vector<Placement> declaration_placements;
    std::vector<Symbol> symbols;
    std::vector<ResolvedCall> calls;
    std::map<NodeId, ResolvedWidgetCall> widget_calls;
    /** Expressions of the action and material forms, by expression id. */
    std::map<NodeId, IdentifiedCall> identified_calls;
    std::map<std::string, ComponentInfo, std::less<>> components;
    std::map<std::string, NamedDeclaration, std::less<>> styles;
    std::map<std::string, NamedDeclaration, std::less<>> animations;
    /** Every function, top-level and block-local, by its declaration. */
    std::map<const FunctionDeclaration*, FunctionInfo> functions;
    /** Top-level functions by name. */
    std::map<std::string, const FunctionInfo*, std::less<>> function_names;
    /** The type a typed state declares, by statement id. */
    std::map<NodeId, SemanticTypePtr> state_declared_types;
    /** The schema each state's slot is declared with, by statement id; empty when it has none. */
    std::map<NodeId, SemanticTypePtr> state_schemas;

    [[nodiscard]] const ExpressionFacts& facts(const Expression& expression) const;
    [[nodiscard]] const Symbol* symbol(const Expression& expression) const;
    [[nodiscard]] const ResolvedCall* call(const Expression& expression) const;
    [[nodiscard]] const ResolvedWidgetCall* widget_call(const WidgetCall& call) const;
    [[nodiscard]] const Placement& placement(NodeId id) const;
    /** The symbol a narrowed symbol stands for, or the symbol itself. */
    [[nodiscard]] const Symbol& origin(SymbolId id) const;
};

} // namespace strata::compiler
