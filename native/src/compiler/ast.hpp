#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "compiler/source.hpp"

namespace strata::compiler {

struct Expression;
struct Statement;
struct Block;
struct WidgetCall;

using ExpressionPtr = std::shared_ptr<const Expression>;
using StatementPtr = std::shared_ptr<const Statement>;
using BlockPtr = std::shared_ptr<const Block>;

struct TypeReference final {
    std::string name;
    std::vector<TypeReference> arguments;
    bool nullable;
    SourceSpan span;
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

struct IdentifierMapKey final {
    std::string name;
    SourceSpan span;
};

struct StringMapKey final {
    std::string value;
    SourceSpan span;
};

struct ExpressionMapKey final {
    ExpressionPtr expression;
    SourceSpan span;
};

using MapKey = std::variant<IdentifierMapKey, StringMapKey, ExpressionMapKey>;

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

struct LambdaExpression final {
    std::string parameter_name;
    ExpressionPtr body;
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
    ErrorExpression>;

struct Expression final {
    ExpressionNode node;
    SourceSpan span;
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
};

struct AnimationFrame final {
    AnimationFramePhase phase;
    std::vector<Property> properties;
    SourceSpan span;
};

struct AnimationFrameStatement final {
    AnimationFrame frame;
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
    ErrorStatement>;

struct Statement final {
    StatementNode node;
    SourceSpan span;
};

struct Block final {
    std::vector<StatementPtr> statements;
    SourceSpan span;
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
    AnimationDeclaration>;

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
    std::vector<SourceSpan> trivia;
    SourceSpan span;
};

} // namespace strata::compiler
