#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "data/json.hpp"
#include "data/json_view.hpp"
#include "runtime/action.hpp"
#include "runtime/host.hpp"
#include "runtime/symbol.hpp"
#include "runtime/value.hpp"

namespace strata::runtime {

/** Indexes into one Program's tables. */
using ExpressionId = std::uint32_t;
using BlockId = std::uint32_t;
using CallId = std::uint32_t;
using IdentityId = std::uint32_t;
inline constexpr std::uint32_t no_program_id = std::numeric_limits<std::uint32_t>::max();

enum class ExpressionKind : std::uint8_t {
    literal,
    variable,
    list,
    map,
    unary,
    binary,
    conditional,
    property,
    index,
    helper,
    action,
    component_template,
    lambda,
    material,
    /** Evaluates to null after reporting the portable kind it could not lower. */
    unknown,
};

/** How a variable that no scope binds resolves. */
enum class VariableFallback : std::uint8_t {
    /** Reported as a missing binding. */
    missing,
    /** A host root. */
    host,
    /** A style, animation or component declaration: the name itself. */
    name,
};

enum class UnaryOperator : std::uint8_t { negate, logical_not };

enum class BinaryOperator : std::uint8_t {
    add,
    subtract,
    multiply,
    divide,
    modulo,
    equal,
    not_equal,
    less,
    less_equal,
    greater,
    greater_equal,
    logical_and,
    logical_or,
    coalesce,
};

enum class HelperKind : std::uint8_t {
    filter,
    map,
    sort_by,
    distinct_by,
    group_by,
    flatten,
    take_while,
    window,
    page,
    persisted,
    sequence,
    parallel,
    choose,
    count,
    any,
    all,
    min,
    max,
    clamp,
    abs,
    floor,
    ceil,
    round,
    length,
    size,
    is_empty,
    join,
    lower,
    upper,
    trim,
    title,
    contains,
    starts_with,
    ends_with,
    format,
    format_number,
    rgb,
    rgba,
    animation,
    style,
    when_style,
    effect,
    unknown,
};

/** Whether a helper derives a collection view (and so is cached with its dependencies). */
[[nodiscard]] bool collection_helper(HelperKind helper) noexcept;

/** A named or positional argument of a helper, action, component template or material. */
struct ProgramArgument final {
    /** Empty for a positional argument. */
    Symbol name;
    bool named = false;
    ExpressionId value = no_program_id;
    /** The argument entry, for diagnostics. */
    data::JsonView source;
};

/** One step of a host path: a field known in advance, or an index evaluated each time. */
struct ProgramHostStep final {
    bool dynamic = false;
    HostPathSegment segment;
    ExpressionId index = no_program_id;
};

/** A property/index chain rooted in a host variable: its root and each step after it. */
struct ProgramHostPath final {
    Symbol root;
    std::vector<ProgramHostStep> steps;
    /** The whole path when no step is dynamic, built once. */
    std::optional<std::vector<HostPathSegment>> fixed;
};

/**
 * One lowered expression. Which fields matter depends on the kind:
 * - literal: `value`.
 * - variable: `name`, `fallback`, `state_binding`, `value` (the name, for a named fallback),
 *   `host` (for a host fallback).
 * - list: `arguments` (the elements, positional).
 * - map: `arguments` (the entries, named by their keys).
 * - unary / binary: `unary` / `binary`, operands in `first` (and `second`); a binary operator
 *   the runtime does not know keeps its name in `text`.
 * - conditional: `first` condition, `second` then, `third` else.
 * - property: `name` and `text` (the property), `first` receiver, `host` when host-rooted.
 * - index: `first` receiver, `second` index, `host` when host-rooted.
 * - helper: `helper`, `text` (its name), `arguments`.
 * - action: `text` (the action id), `arguments`, `origin`.
 * - component_template: `text` (the component), `arguments`.
 * - lambda: `name` (the parameter), `first` body, `free_variables`.
 * - material: `text` (the material id), `arguments` (its parameters, named), `material_call`.
 * - unknown: `text` (the portable kind).
 */
struct ProgramExpression final {
    ExpressionKind kind = ExpressionKind::unknown;
    UnaryOperator unary = UnaryOperator::negate;
    BinaryOperator binary = BinaryOperator::add;
    HelperKind helper = HelperKind::unknown;
    VariableFallback fallback = VariableFallback::missing;
    bool state_binding = false;
    bool material_call = false;
    Symbol name;
    std::string text;
    Value value;
    ExpressionId first = no_program_id;
    ExpressionId second = no_program_id;
    ExpressionId third = no_program_id;
    std::uint32_t arguments_begin = 0U;
    std::uint32_t arguments_count = 0U;
    std::uint32_t host = no_program_id;
    std::uint32_t origin = no_program_id;
    std::uint32_t free_variables_begin = 0U;
    std::uint32_t free_variables_count = 0U;
    /** The portable expression, for diagnostics and its authored range. */
    data::JsonView source;
};

/** What an action expression says about where it was authored. */
struct ProgramActionOrigin final {
    /** Everything but the component path, which depends on the scope it is evaluated in. */
    ActionOrigin origin;
    /** The component path to use when the scope names none under "/component/". */
    std::optional<std::string> fallback_component_path;
};

enum class StatementKind : std::uint8_t { state, derived, node, conditional, when, loop };

struct ProgramWhenBranch final {
    /** No id for the fallback branch. */
    ExpressionId match = no_program_id;
    BlockId block = no_program_id;
};

/**
 * One lowered statement:
 * - state: `name`, `expression` initializer (no id when null), `state_declaration`.
 * - derived: `name`, `expression`.
 * - node: `call`.
 * - conditional: `expression` condition, `block` then, `otherwise` else (no id when absent).
 * - when: `expression` subject, `branches`.
 * - loop: `expression` collection, `filter`, `name` item, `index_name` (empty when absent),
 *   `block` body, `identity` (a Repeater's root-key extractor, no id otherwise).
 */
struct ProgramStatement final {
    StatementKind kind = StatementKind::node;
    Symbol name;
    Symbol index_name;
    bool has_index = false;
    ExpressionId expression = no_program_id;
    ExpressionId filter = no_program_id;
    BlockId block = no_program_id;
    BlockId otherwise = no_program_id;
    CallId call = no_program_id;
    IdentityId identity = no_program_id;
    std::uint32_t branches_begin = 0U;
    std::uint32_t branches_count = 0U;
    /** Index into the unit's state declarations, for a state statement. */
    std::uint32_t state_declaration = no_program_id;
    data::JsonView source;
};

struct ProgramBlock final {
    std::uint32_t statements_begin = 0U;
    std::uint32_t statements_count = 0U;
};

/** A call argument, named by the parameter it fills. */
struct ProgramCallArgument final {
    Symbol name;
    ExpressionId value = no_program_id;
};

/** One entry of a literal `behaviors` list: each field evaluated on its own. */
struct ProgramBehavior final {
    ExpressionId id = no_program_id;
    ExpressionId enabled = no_program_id;
    ExpressionId options = no_program_id;
    ExpressionId action = no_program_id;
};

/** A `Slot` call among a component call's children: content projected into that slot. */
struct ProgramSlotFill final {
    /** The slot name, no id when the fill names none. */
    ExpressionId name = no_program_id;
    /** Its content, no id when it has none. */
    BlockId children = no_program_id;
};

struct ProgramCall final {
    bool component = false;
    std::string type;
    std::string path;
    /** The declaration a component call instantiates. */
    std::uint32_t component_index = no_program_id;
    std::uint32_t arguments_begin = 0U;
    std::uint32_t arguments_count = 0U;
    /** Whether the call has a `behaviors` argument; only a literal list describes any. */
    bool has_behaviors = false;
    std::uint32_t behaviors_begin = 0U;
    std::uint32_t behaviors_count = 0U;
    /** The index, among this call's arguments, of its `key` argument. */
    std::uint32_t key_argument = no_program_id;
    BlockId children = no_program_id;
    /** For a component call, per parameter: the argument (index into this call's) filling it. */
    std::uint32_t parameter_arguments_begin = 0U;
    /** For a component call with children: its Slot fills and the statements they are. */
    std::uint32_t slot_fills_begin = 0U;
    std::uint32_t slot_fills_count = 0U;
    std::uint32_t projected_statements_begin = 0U;
    std::uint32_t projected_statements_count = 0U;
    data::JsonView source;
};

struct ProgramParameter final {
    Symbol name;
    ExpressionId default_value = no_program_id;
    bool state_binding = false;
};

struct ProgramWidgetDefault final {
    std::string widget;
    ExpressionId style = no_program_id;
    ExpressionId variant = no_program_id;
};

struct ProgramComponent final {
    std::string name;
    std::string path;
    /** "component <name>", the scope its states are declared in. */
    std::string declaration_scope;
    std::uint32_t parameters_begin = 0U;
    std::uint32_t parameters_count = 0U;
    std::uint32_t widget_defaults_begin = 0U;
    std::uint32_t widget_defaults_count = 0U;
    BlockId body = no_program_id;
};

struct ProgramLayer final {
    std::string name;
    std::string path;
    /** "screen <name>" or "overlay <name>". */
    std::string declaration_scope;
    BlockId body = no_program_id;
};

struct ProgramStyle final {
    std::string name;
    std::vector<std::string> bases;
    std::vector<std::pair<std::string, ExpressionId>> properties;
};

enum class IdentityKind : std::uint8_t { key, block, conditional, when };

struct ProgramIdentityBranch final {
    ExpressionId match = no_program_id;
    IdentityId identity = no_program_id;
};

/**
 * A Repeater row's root-key extractor: `key` evaluates `expression`; `block` takes the one key
 * its `children` select; `conditional` picks `then_identity` or `else_identity` by `expression`;
 * `when` matches `expression` against its `branches`.
 */
struct ProgramIdentity final {
    IdentityKind kind = IdentityKind::key;
    ExpressionId expression = no_program_id;
    IdentityId then_identity = no_program_id;
    IdentityId else_identity = no_program_id;
    std::uint32_t children_begin = 0U;
    std::uint32_t children_count = 0U;
    std::uint32_t branches_begin = 0U;
    std::uint32_t branches_count = 0U;
};

/**
 * A unit's portable IR lowered once into typed, indexed nodes: kinds and operators are enums,
 * names are symbols, literals are values, host paths are split, and calls know the declaration
 * they instantiate and which argument fills which parameter. Evaluation reads nothing by name
 * from JSON. The portable IR stays the interchange form; each node keeps its JSON for
 * diagnostics, so a program lives no longer than the IR it was lowered from.
 */
class Program final : public std::enable_shared_from_this<Program> {
  public:
    /**
     * Lowers a validated unit. `state_declaration` finds a state statement's declaration, and
     * learns the expression its initializer lowered to (no id when it has none).
     */
    using StateDeclarationIndex = std::function<std::uint32_t(
        std::string_view scope, std::string_view name, ExpressionId initializer)>;
    [[nodiscard]] static std::shared_ptr<const Program>
    lower_unit(data::JsonView unit, const StateDeclarationIndex& state_declaration);
    /** Lowers one expression, which the program keeps its own copy of. */
    [[nodiscard]] static std::shared_ptr<const Program> lower_expression(data::JsonView expression);

    /** Unique to this program for the life of the process. */
    [[nodiscard]] std::uint64_t serial() const noexcept {
        return serial_;
    }
    /** The root expression of a program lowered from one expression. */
    [[nodiscard]] ExpressionId root() const noexcept {
        return root_;
    }

    [[nodiscard]] const ProgramExpression& expression(ExpressionId id) const {
        return expressions_[id];
    }
    [[nodiscard]] std::span<const ProgramArgument> arguments(const ProgramExpression& node) const {
        return {arguments_.data() + node.arguments_begin, node.arguments_count};
    }
    [[nodiscard]] std::span<const Symbol> free_variables(const ProgramExpression& node) const {
        return {free_variables_.data() + node.free_variables_begin, node.free_variables_count};
    }
    [[nodiscard]] const ProgramHostPath& host_path(std::uint32_t id) const {
        return host_paths_[id];
    }
    [[nodiscard]] const ProgramActionOrigin& action_origin(std::uint32_t id) const {
        return origins_[id];
    }
    [[nodiscard]] const ProgramBlock& block(BlockId id) const {
        return blocks_[id];
    }
    [[nodiscard]] std::span<const ProgramStatement> statements(const ProgramBlock& block) const {
        return {statements_.data() + block.statements_begin, block.statements_count};
    }
    [[nodiscard]] std::span<const ProgramWhenBranch>
    branches(const ProgramStatement& statement) const {
        return {branches_.data() + statement.branches_begin, statement.branches_count};
    }
    [[nodiscard]] const ProgramCall& call(CallId id) const {
        return calls_[id];
    }
    [[nodiscard]] std::span<const ProgramCallArgument> arguments(const ProgramCall& call) const {
        return {call_arguments_.data() + call.arguments_begin, call.arguments_count};
    }
    [[nodiscard]] std::span<const ProgramBehavior> behaviors(const ProgramCall& call) const {
        return {behaviors_.data() + call.behaviors_begin, call.behaviors_count};
    }
    /** Per parameter of the called component, the index of the argument filling it. */
    [[nodiscard]] std::span<const std::uint32_t> parameter_arguments(const ProgramCall& call) const;
    [[nodiscard]] std::span<const ProgramSlotFill> slot_fills(const ProgramCall& call) const {
        return {slot_fills_.data() + call.slot_fills_begin, call.slot_fills_count};
    }
    /** The statement indices of a component call's children that are Slot fills, ascending. */
    [[nodiscard]] std::span<const std::size_t> projected_statements(const ProgramCall& call) const {
        return {projected_statements_.data() + call.projected_statements_begin,
                call.projected_statements_count};
    }
    [[nodiscard]] const ProgramComponent& component(std::uint32_t index) const {
        return components_[index];
    }
    [[nodiscard]] std::optional<std::uint32_t> component_index(std::string_view name) const;
    [[nodiscard]] std::span<const ProgramParameter>
    parameters(const ProgramComponent& component) const {
        return {parameters_.data() + component.parameters_begin, component.parameters_count};
    }
    [[nodiscard]] std::span<const ProgramWidgetDefault>
    widget_defaults(const ProgramComponent& component) const {
        return {widget_defaults_.data() + component.widget_defaults_begin,
                component.widget_defaults_count};
    }
    [[nodiscard]] const ProgramLayer* screen(std::string_view name) const;
    [[nodiscard]] const ProgramLayer* overlay(std::string_view name) const;
    [[nodiscard]] const ProgramStyle* style(std::string_view name) const;
    [[nodiscard]] const ProgramIdentity& identity(IdentityId id) const {
        return identities_[id];
    }
    [[nodiscard]] std::span<const IdentityId> children(const ProgramIdentity& identity) const {
        return {identity_children_.data() + identity.children_begin, identity.children_count};
    }
    [[nodiscard]] std::span<const ProgramIdentityBranch>
    branches(const ProgramIdentity& identity) const {
        return {identity_branches_.data() + identity.branches_begin, identity.branches_count};
    }
    [[nodiscard]] std::size_t expression_count() const noexcept {
        return expressions_.size();
    }
    [[nodiscard]] std::size_t call_count() const noexcept {
        return calls_.size();
    }

  private:
    friend class ProgramLowering;

    Program();

    std::uint64_t serial_ = 0U;
    /** A program lowered from one expression owns that expression. */
    std::shared_ptr<const data::JsonValue> owned_source_;
    ExpressionId root_ = no_program_id;
    std::vector<ProgramExpression> expressions_;
    std::vector<ProgramArgument> arguments_;
    std::vector<Symbol> free_variables_;
    std::vector<ProgramHostPath> host_paths_;
    std::vector<ProgramActionOrigin> origins_;
    std::vector<ProgramBlock> blocks_;
    std::vector<ProgramStatement> statements_;
    std::vector<ProgramWhenBranch> branches_;
    std::vector<ProgramCall> calls_;
    std::vector<ProgramCallArgument> call_arguments_;
    std::vector<ProgramBehavior> behaviors_;
    std::vector<std::uint32_t> parameter_arguments_;
    std::vector<ProgramSlotFill> slot_fills_;
    std::vector<std::size_t> projected_statements_;
    std::vector<ProgramComponent> components_;
    std::vector<ProgramParameter> parameters_;
    std::vector<ProgramWidgetDefault> widget_defaults_;
    std::vector<ProgramLayer> screens_;
    std::vector<ProgramLayer> overlays_;
    std::vector<ProgramStyle> styles_;
    std::vector<ProgramIdentity> identities_;
    std::vector<IdentityId> identity_children_;
    std::vector<ProgramIdentityBranch> identity_branches_;
    std::map<std::string, std::uint32_t, std::less<>> component_indexes_;
    std::map<std::string, std::uint32_t, std::less<>> screen_indexes_;
    std::map<std::string, std::uint32_t, std::less<>> overlay_indexes_;
    std::map<std::string, std::uint32_t, std::less<>> style_indexes_;
};

} // namespace strata::runtime
