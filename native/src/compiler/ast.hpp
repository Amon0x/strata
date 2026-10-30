#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/source.hpp"

namespace strata::compiler {

/**
 * Identifies one expression, statement, block or widget call of a compilation. The parser numbers nodes
 * densely from the id it is given, and module loading hands each module the next free range, so
 * ids are unique across the merged file and analysis can key its side tables by them. Recovery
 * nodes are numbered like any other.
 */
using NodeId = std::uint32_t;

struct Expression;
struct Statement;
struct Block;
struct WidgetCall;

using ExpressionPtr = std::shared_ptr<const Expression>;
using StatementPtr = std::shared_ptr<const Statement>;
using BlockPtr = std::shared_ptr<const Block>;

enum class TypeReferenceKind : std::uint8_t {
    /** `name` or `name<arguments>`. */
    named,
    /** `"text"`: the one string `name` holds. */
    string_literal,
    /** `A | B | ...`: any of `arguments`. */
    union_type,
    /** `{ name: T, other?: U }`: a record with `fields`. */
    record,
    /** `(T, U) => R`: a function of `arguments` returning `returns`. */
    function,
};

struct TypeField;

struct TypeReference final {
    std::string name;
    std::vector<TypeReference> arguments;
    bool nullable;
    SourceSpan span;
    TypeReferenceKind kind = TypeReferenceKind::named;
    std::vector<TypeField> fields = {};
    /** A function type's return type, alone. */
    std::vector<TypeReference> returns = {};
};

struct TypeField final {
    std::string name;
    TypeReference type;
    /** Written `name?:`: a record may leave it out. */
    bool optional = false;
};

struct Parameter final {
    std::string name;
    std::optional<TypeReference> type_reference;
    ExpressionPtr default_value;
    SourceSpan span;
};

struct StringLiteral final {
    std::string value;
};

struct NumberLiteral final {
    std::string raw;
    std::optional<std::string> unit;
};

struct ColorLiteral final {
    std::string raw;
};

struct BooleanLiteral final {
    bool value;
};

struct NullLiteral final {};

using LiteralValue =
    std::variant<StringLiteral, NumberLiteral, ColorLiteral, BooleanLiteral, NullLiteral>;

struct LiteralExpression final {
    LiteralValue value;
};

struct IdentifierExpression final {
    std::string name;
};

struct GroupingExpression final {
    ExpressionPtr expression;
};

struct ListExpression final {
    std::vector<ExpressionPtr> elements;
};

/** `...operand` inside a list literal: the operand's elements, in place. */
struct SpreadExpression final {
    ExpressionPtr operand;
};

struct IdentifierMapKey final {
    std::string name;
    SourceSpan span;
};

struct StringMapKey final {
    std::string value;
    SourceSpan span;
};

/** `...record` inside a record literal: the record's fields, overridden by later entries. */
struct SpreadMapKey final {
    SourceSpan span;
};

using MapKey = std::variant<IdentifierMapKey, StringMapKey, SpreadMapKey>;

struct MapEntry final {
    MapKey key;
    ExpressionPtr value;
    SourceSpan span;
};

struct MapExpression final {
    std::vector<MapEntry> entries;
};

enum class UnaryOperator {
    negate,
    logical_not,
};

struct UnaryExpression final {
    UnaryOperator operation;
    ExpressionPtr operand;
};

enum class BinaryOperator {
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

struct BinaryExpression final {
    ExpressionPtr left;
    BinaryOperator operation;
    ExpressionPtr right;
};

struct ConditionalExpression final {
    ExpressionPtr condition;
    ExpressionPtr then_expression;
    ExpressionPtr else_expression;
};

struct PropertyAccessExpression final {
    ExpressionPtr receiver;
    std::string property_name;
};

struct IndexExpression final {
    ExpressionPtr receiver;
    ExpressionPtr index;
};

struct Argument final {
    std::optional<std::string> name;
    ExpressionPtr value;
    SourceSpan span;
};

struct CallTarget final {
    std::vector<std::string> parts;
    SourceSpan span;

    [[nodiscard]] std::string qualified_name() const;
};

struct CallExpression final {
    CallTarget target;
    std::vector<Argument> arguments;
};

struct LambdaParameter final {
    std::string name;
    std::optional<TypeReference> type_reference;
    SourceSpan span;
};

/**
 * `n -> expr` (a helper's item function), or a StrataScript function value: `x => expr`,
 * `(a, b) => expr`, `() => { statements }`. Exactly one of `body` and `block` is set.
 */
struct LambdaExpression final {
    std::vector<LambdaParameter> parameters;
    ExpressionPtr body;
    BlockPtr block;
    /** Written with `=>` (or a parameter list): a script function rather than a helper's `->`. */
    bool script = false;
};

/** `await operand`: performs an action (or calls an async function) and waits for its answer. */
struct AwaitExpression final {
    ExpressionPtr operand;
};

struct ErrorExpression final {};

using ExpressionNode = std::variant<
    LiteralExpression,
    IdentifierExpression,
    GroupingExpression,
    ListExpression,
    MapExpression,
    UnaryExpression,
    BinaryExpression,
    ConditionalExpression,
    PropertyAccessExpression,
    IndexExpression,
    CallExpression,
    LambdaExpression,
    SpreadExpression,
    AwaitExpression,
    ErrorExpression>;

struct Expression final {
    ExpressionNode node;
    SourceSpan span;
    NodeId id = 0U;
};

struct Property final {
    std::string name;
    ExpressionPtr value;
    SourceSpan span;
};

struct WidgetCall final {
    std::string name;
    std::vector<Argument> arguments;
    BlockPtr body;
    SourceSpan span;
    NodeId id = 0U;
};

struct StateStatement final {
    std::string name;
    std::optional<TypeReference> type_reference;
    ExpressionPtr initializer;
};

struct DerivedStatement final {
    std::string name;
    ExpressionPtr expression;
};

struct PropertyStatement final {
    Property property;
};

struct WidgetStatement final {
    WidgetCall call;
};

struct RootStatement final {
    WidgetCall call;
};

struct IfStatement final {
    ExpressionPtr condition;
    BlockPtr then_block;
    BlockPtr else_block;
};

struct WhenBranch final {
    ExpressionPtr match;
    BlockPtr block;
    SourceSpan span;
};

struct WhenStatement final {
    ExpressionPtr subject;
    std::vector<WhenBranch> branches;
};

struct ForStatement final {
    std::string item_name;
    std::optional<std::string> index_name;
    ExpressionPtr collection;
    ExpressionPtr filter;
    BlockPtr block;
};

enum class AnimationFramePhase {
    from,
    to,
    /** A keyframe between them: `0.4 { ... }`, at `offset` of the way through. */
    at,
};

struct AnimationFrame final {
    AnimationFramePhase phase;
    std::vector<Property> properties;
    SourceSpan span;
    double offset = 0.0;
};

struct AnimationFrameStatement final {
    AnimationFrame frame;
};

/** `type Name = Type`: a name for a type, at the top of a module or in a component. */
struct TypeDeclaration final {
    std::string name;
    TypeReference type;
    SourceSpan name_span;
};

/** A type declared in a component body. */
struct TypeStatement final {
    TypeDeclaration declaration;
};

/** A StrataScript function: `fn name(params): Type { ... }` or `fn name(params) = expr`. */
struct FunctionDeclaration final {
    std::string name;
    std::vector<Parameter> parameters;
    std::optional<TypeReference> return_type;
    /** Exactly one of `expression_body` and `block_body` is set. */
    ExpressionPtr expression_body;
    BlockPtr block_body;
    bool asynchronous = false;
    SourceSpan name_span;
};

/** A function declared in a component body (or a script block). */
struct FunctionStatement final {
    FunctionDeclaration function;
};

/** `let name = value` (immutable) or `var name = value` (mutable). */
struct LetStatement final {
    std::string name;
    bool mutable_binding = false;
    std::optional<TypeReference> type_reference;
    ExpressionPtr initializer;
    SourceSpan name_span;
};

enum class AssignmentOperator {
    assign,
    add,
    subtract,
    multiply,
    divide,
    modulo,
};

/** `target op value`; the target is a name, or a property or index path from one. */
struct AssignmentStatement final {
    ExpressionPtr target;
    AssignmentOperator operation = AssignmentOperator::assign;
    ExpressionPtr value;
};

/** An expression evaluated for its effect: a call, or an action, which is performed. */
struct ExpressionStatement final {
    ExpressionPtr expression;
};

struct WhileStatement final {
    ExpressionPtr condition;
    BlockPtr block;
};

struct BreakStatement final {};
struct ContinueStatement final {};

struct ReturnStatement final {
    /** Null for a bare `return`. */
    ExpressionPtr value;
};

struct ThrowStatement final {
    ExpressionPtr value;
};

/** `try { ... } catch name { ... }`. */
struct TryStatement final {
    BlockPtr block;
    std::string catch_name;
    BlockPtr catch_block;
};

struct ErrorStatement final {};

using StatementNode = std::variant<
    StateStatement,
    DerivedStatement,
    PropertyStatement,
    WidgetStatement,
    RootStatement,
    IfStatement,
    WhenStatement,
    ForStatement,
    AnimationFrameStatement,
    FunctionStatement,
    LetStatement,
    AssignmentStatement,
    ExpressionStatement,
    WhileStatement,
    BreakStatement,
    ContinueStatement,
    ReturnStatement,
    ThrowStatement,
    TryStatement,
    TypeStatement,
    ErrorStatement>;

struct Statement final {
    StatementNode node;
    SourceSpan span;
    NodeId id = 0U;
};

struct Block final {
    std::vector<StatementPtr> statements;
    SourceSpan span;
    NodeId id = 0U;
};

struct ScreenDeclaration final {
    std::string name;
    BlockPtr body;
};

struct OverlayDeclaration final {
    std::string name;
    BlockPtr body;
};

struct ComponentDeclaration final {
    std::string name;
    std::vector<Parameter> parameters;
    BlockPtr body;
};

struct StyleBase final {
    std::string name;
    SourceSpan span;
};

struct StyleDeclaration final {
    std::string name;
    std::vector<StyleBase> bases;
    std::vector<Property> properties;
    BlockPtr body;
};

struct AnimationProperty final {
    Property property;
    SourceSpan span;
};

using AnimationEntry = std::variant<AnimationFrame, AnimationProperty>;

struct AnimationDeclaration final {
    std::string name;
    std::vector<AnimationEntry> entries;
    BlockPtr body;
};

using DeclarationNode = std::variant<
    ScreenDeclaration,
    OverlayDeclaration,
    ComponentDeclaration,
    StyleDeclaration,
    AnimationDeclaration,
    FunctionDeclaration,
    TypeDeclaration>;

struct Declaration final {
    DeclarationNode node;
    SourceSpan span;
};

struct Import final {
    std::string path;
    SourceSpan span;
};

struct File final {
    std::string source_id;
    std::vector<Import> imports;
    std::vector<Declaration> declarations;
    SourceSpan span;
    /** One past the highest node id the file's nodes use; side tables size themselves by it. */
    NodeId node_id_limit = 0U;
    /**
     * The names read in regions the parser could not turn into nodes (a declaration or statement
     * it abandoned, tokens it skipped to recover, an unreadable token), sorted and unique: what
     * failed to parse may have declared any of them.
     */
    std::vector<std::string> unparsed_names;
    /** Parsing stopped at a limit, so the source past it may declare anything. */
    bool truncated = false;
};

} // namespace strata::compiler
