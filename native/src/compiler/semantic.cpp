#include "compiler/semantic.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <variant>

#include "ui/path.hpp"

namespace strata::compiler {
namespace {

using Scope = std::unordered_map<std::string, SemanticTypePtr>;
using RetainedStateNames = std::set<std::string, std::less<>>;
using LocalNames = std::set<std::string, std::less<>>;

constexpr std::size_t maximum_eager_loop_items = 1'000U;
constexpr std::size_t maximum_lazy_loop_items = 100'000U;

[[nodiscard]] SemanticTypePtr simple(const SemanticTypeKind kind) {
    auto type = std::make_shared<SemanticType>();
    type->kind = kind;
    return type;
}

[[nodiscard]] SemanticTypePtr literal_string(std::string value) {
    auto type = std::make_shared<SemanticType>();
    type->kind = SemanticTypeKind::string_literal;
    type->literal = std::move(value);
    return type;
}

[[nodiscard]] std::string lower(std::string_view value) {
    std::string normalized(value);
    std::ranges::transform(normalized, normalized.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return normalized;
}

[[nodiscard]] std::string expected_names(std::vector<std::string> names) {
    std::ranges::sort(names);
    names.erase(std::ranges::unique(names).begin(), names.end());
    if (names.empty())
        return "none";
    std::string output;
    for (std::size_t index = 0U; index < names.size(); ++index) {
        if (index != 0U)
            output += ", ";
        output += names[index];
    }
    return output;
}

[[nodiscard]] const std::string* string_literal_value(const Expression& expression) {
    const auto* literal = std::get_if<LiteralExpression>(&expression.node);
    if (literal == nullptr)
        return nullptr;
    const auto* string = std::get_if<StringLiteral>(&literal->value);
    return string == nullptr ? nullptr : &string->value;
}

[[nodiscard]] std::optional<bool> boolean_literal_value(const Expression& expression) {
    const auto* literal = std::get_if<LiteralExpression>(&expression.node);
    if (literal == nullptr)
        return std::nullopt;
    const auto* boolean = std::get_if<BooleanLiteral>(&literal->value);
    return boolean == nullptr ? std::nullopt : std::optional<bool>(boolean->value);
}

[[nodiscard]] bool type_matches(const SemanticType& expected, const SemanticType& actual,
                                const bool nullable) {
    if (actual.kind == SemanticTypeKind::unknown)
        return true;
    if (actual.kind == SemanticTypeKind::null_value)
        return nullable;
    if (actual.kind == SemanticTypeKind::union_value) {
        return std::ranges::all_of(
            actual.options, [&expected, nullable](const SemanticTypePtr& option) {
                return option != nullptr && type_matches(expected, *option, nullable);
            });
    }
    if (expected.kind == SemanticTypeKind::unsafe_component_parameter)
        return false;
    if (expected.kind == SemanticTypeKind::state_binding) {
        const SemanticType* actual_value =
            actual.kind == SemanticTypeKind::state_binding ? actual.value.get() : &actual;
        return expected.value != nullptr && actual_value != nullptr &&
            type_matches(*expected.value, *actual_value, nullable);
    }
    if (actual.kind == SemanticTypeKind::state_binding) {
        return actual.value != nullptr && type_matches(expected, *actual.value, nullable);
    }
    return expected.accepts(actual);
}

struct ComponentSchema final {
    std::vector<SchemaParameter> parameters;
    struct Slot final {
        std::string name;
        bool required = false;
        SourceSpan span;
    };
    std::vector<Slot> slots;
    std::map<std::string, const Expression*, std::less<>> widget_default_styles;
};

enum class BlockContext {
    component,
    ui,
    lazy_ui,
};

[[nodiscard]] BlockContext nested_context(const BlockContext context) {
    return context == BlockContext::component ? BlockContext::ui : context;
}

struct RootCardinality final {
    std::size_t minimum = 0U;
    std::size_t maximum = 0U;
};

[[nodiscard]] std::size_t capped_root_sum(const std::size_t left, const std::size_t right) {
    return std::min<std::size_t>(2U, left + right);
}

[[nodiscard]] RootCardinality root_cardinality(const Block& block) {
    RootCardinality total;
    for (const StatementPtr& statement : block.statements) {
        RootCardinality contribution;
        if (std::holds_alternative<WidgetStatement>(statement->node) ||
            std::holds_alternative<RootStatement>(statement->node)) {
            contribution = {1U, 1U};
        } else if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
            const RootCardinality then_value = root_cardinality(*conditional->then_block);
            const RootCardinality else_value = conditional->else_block != nullptr
                                                   ? root_cardinality(*conditional->else_block)
                                                   : RootCardinality{};
            contribution = {
                std::min(then_value.minimum, else_value.minimum),
                std::max(then_value.maximum, else_value.maximum),
            };
        } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
            if (!when->branches.empty()) {
                contribution = root_cardinality(*when->branches.front().block);
                for (std::size_t index = 1U; index < when->branches.size(); ++index) {
                    const RootCardinality branch = root_cardinality(*when->branches[index].block);
                    contribution.minimum = std::min(contribution.minimum, branch.minimum);
                    contribution.maximum = std::max(contribution.maximum, branch.maximum);
                }
            }
        } else if (std::holds_alternative<ForStatement>(statement->node)) {
            contribution = {0U, 2U};
        }
        total.minimum = capped_root_sum(total.minimum, contribution.minimum);
        total.maximum = capped_root_sum(total.maximum, contribution.maximum);
    }
    return total;
}

[[nodiscard]] const std::string* static_map_key(const MapKey& key) {
    if (const auto* identifier = std::get_if<IdentifierMapKey>(&key))
        return &identifier->name;
    if (const auto* string = std::get_if<StringMapKey>(&key))
        return &string->value;
    return nullptr;
}

[[nodiscard]] const SourceSpan& map_key_span(const MapKey& key) {
    return std::visit([](const auto& value) -> const SourceSpan& { return value.span; }, key);
}

[[nodiscard]] const Expression& ungrouped(const Expression& expression) {
    const Expression* current = &expression;
    while (const auto* grouping = std::get_if<GroupingExpression>(&current->node)) {
        current = grouping->expression.get();
    }
    return *current;
}

struct StaticNumber final {
    double value;
    std::optional<std::string> unit;
};

[[nodiscard]] std::optional<StaticNumber> static_number(const Expression& expression) {
    const Expression& value = ungrouped(expression);
    bool negate = false;
    const Expression* literal_expression = &value;
    if (const auto* unary = std::get_if<UnaryExpression>(&value.node);
        unary != nullptr && unary->operation == UnaryOperator::negate) {
        negate = true;
        literal_expression = &ungrouped(*unary->operand);
    }
    const auto* literal = std::get_if<LiteralExpression>(&literal_expression->node);
    const auto* number = literal != nullptr ? std::get_if<NumberLiteral>(&literal->value) : nullptr;
    if (number == nullptr)
        return std::nullopt;
    std::string raw = number->raw;
    std::erase(raw, '_');
    double parsed = 0.0;
    const auto converted = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (converted.ec != std::errc{} || converted.ptr != raw.data() + raw.size() ||
        !std::isfinite(parsed)) {
        return std::nullopt;
    }
    return StaticNumber{negate ? -parsed : parsed, number->unit};
}

[[nodiscard]] const LiteralExpression* static_literal(const Expression& expression) {
    return std::get_if<LiteralExpression>(&ungrouped(expression).node);
}

[[nodiscard]] std::string canonical_color(std::string raw) {
    if (!raw.empty() && raw.front() == '#')
        raw.erase(raw.begin());
    std::ranges::transform(raw, raw.begin(), [](const unsigned char character) {
        return character >= 'A' && character <= 'F' ? static_cast<char>(character - 'A' + 'a')
                                                    : static_cast<char>(character);
    });
    if (raw.size() == 6U)
        raw += "ff";
    return raw;
}

class Validator final {
  public:
    Validator(const File& file, const SchemaRegistry& registry) : file_(file), registry_(registry) {
        std::set<std::pair<std::string, std::string>> declaration_names;
        for (const Declaration& declaration : file_.declarations) {
            const auto note_declaration = [&](std::string kind, const std::string& name) {
                if (!declaration_names.emplace(kind, name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_DECLARATION",
                           "The " + kind + " declaration '" + name +
                               "' is declared more than once.",
                           declaration.span, kind + " " + name, "unique " + kind + " name");
                }
            };
            if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
                note_declaration("component", component->name);
                ComponentSchema schema;
                std::set<std::string, std::less<>> parameter_names;
                for (const Parameter& parameter : component->parameters) {
                    if (!parameter_names.insert(parameter.name).second) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_PARAMETER",
                               "Component parameter '" + parameter.name +
                                   "' is declared more than once.",
                               parameter.span, "component " + component->name,
                               "unique component parameter");
                    }
                    SemanticTypePtr type =
                        parameter.type_reference.has_value()
                            ? resolve_type(*parameter.type_reference)
                            : simple(SemanticTypeKind::unsafe_component_parameter);
                    schema.parameters.push_back(SchemaParameter{
                        .name = parameter.name,
                        .type = std::move(type),
                        .required = parameter.default_value == nullptr,
                        .nullable = parameter.type_reference.has_value() &&
                                    parameter.type_reference->nullable,
                        .aliases = {},
                        .material_type = std::nullopt,
                    });
                }
                std::function<void(const Block&)> scan_slots;
                scan_slots = [&](const Block& block) {
                    for (const StatementPtr& statement : block.statements) {
                        const WidgetCall* call = nullptr;
                        if (const auto* widget = std::get_if<WidgetStatement>(&statement->node)) {
                            call = &widget->call;
                        } else if (const auto* root =
                                       std::get_if<RootStatement>(&statement->node)) {
                            call = &root->call;
                        }
                        if (call != nullptr) {
                            if (call->name == "Slot") {
                                const Expression* name_expression = nullptr;
                                const Expression* required_expression = nullptr;
                                for (const Argument& argument : call->arguments) {
                                    if ((argument.name == "name" || (!argument.name.has_value() &&
                                                                     name_expression == nullptr))) {
                                        name_expression = argument.value.get();
                                    } else if (argument.name == "required") {
                                        required_expression = argument.value.get();
                                    }
                                }
                                const std::string* name =
                                    name_expression != nullptr
                                        ? string_literal_value(*name_expression)
                                        : nullptr;
                                if (name == nullptr) {
                                    report(
                                        "STRATA.DSL.SEMANTIC_DYNAMIC_SLOT_DECLARATION",
                                        "Component slot declaration names must be string literals.",
                                        name_expression != nullptr ? name_expression->span
                                                                   : call->span,
                                        "component " + component->name,
                                        "Slot(name: \"static-name\")");
                                } else {
                                    const bool required =
                                        required_expression != nullptr
                                            ? boolean_literal_value(*required_expression)
                                                  .value_or(false)
                                            : false;
                                    const auto duplicate = std::ranges::find_if(
                                        schema.slots, [name](const ComponentSchema::Slot& slot) {
                                            return slot.name == *name;
                                        });
                                    if (duplicate != schema.slots.end()) {
                                        report("STRATA.DSL.SEMANTIC_DUPLICATE_SLOT_DECLARATION",
                                               "Component '" + component->name +
                                                   "' declares slot '" + *name +
                                                   "' more than once.",
                                               call->span, "component " + component->name,
                                               "unique slot name");
                                    } else {
                                        schema.slots.push_back(ComponentSchema::Slot{
                                            *name,
                                            required,
                                            call->span,
                                        });
                                    }
                                }
                            } else if (registry_.widget(call->name) != nullptr &&
                                       call->body != nullptr) {
                                scan_slots(*call->body);
                            }
                        }
                        if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
                            scan_slots(*conditional->then_block);
                            if (conditional->else_block != nullptr)
                                scan_slots(*conditional->else_block);
                        } else if (const auto* when =
                                       std::get_if<WhenStatement>(&statement->node)) {
                            for (const WhenBranch& branch : when->branches)
                                scan_slots(*branch.block);
                        } else if (const auto* loop = std::get_if<ForStatement>(&statement->node)) {
                            scan_slots(*loop->block);
                        }
                    }
                };
                scan_slots(*component->body);
                schema.widget_default_styles =
                    collect_widget_default_styles(*component->body);
                components_.emplace(component->name, std::move(schema));
            } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
                note_declaration("style", style->name);
                styles_.push_back(style->name);
                style_declarations_.try_emplace(style->name, style);
            } else if (const auto* animation =
                           std::get_if<AnimationDeclaration>(&declaration.node)) {
                note_declaration("animation", animation->name);
                animations_.push_back(animation->name);
            } else if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
                note_declaration("screen", screen->name);
            } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
                note_declaration("overlay", overlay->name);
            }
        }
        std::ranges::sort(styles_);
        std::ranges::sort(animations_);
    }

    [[nodiscard]] SemanticResult run() {
        validate_style_composition();
        for (const Declaration& declaration : file_.declarations)
            validate_declaration(declaration);
        return SemanticResult{
            std::move(diagnostics_),
            std::move(lowering_diagnostics_),
            std::move(validated_animations_),
        };
    }

  private:
    [[nodiscard]] static std::map<std::string, const Expression*, std::less<>>
    collect_widget_default_styles(const Block& body) {
        std::map<std::string, const Expression*, std::less<>> result;
        for (const StatementPtr& statement : body.statements) {
            const auto* property = std::get_if<PropertyStatement>(&statement->node);
            if (property == nullptr || property->property.name != "defaults") continue;
            const auto* defaults =
                std::get_if<MapExpression>(&property->property.value->node);
            if (defaults == nullptr) continue;
            for (const MapEntry& widget_entry : defaults->entries) {
                const std::string* widget_name = static_map_key(widget_entry.key);
                const auto* values =
                    std::get_if<MapExpression>(&widget_entry.value->node);
                if (widget_name == nullptr || values == nullptr) continue;
                for (const MapEntry& value : values->entries) {
                    const std::string* name = static_map_key(value.key);
                    if (name != nullptr && *name == "style") {
                        result.insert_or_assign(*widget_name, value.value.get());
                    }
                }
            }
        }
        return result;
    }

    struct LayoutPropertyRef final {
        std::string name;
        const Expression* value = nullptr;
        SourceSpan span;
    };
    using LayoutPropertyMap =
        std::map<std::string, LayoutPropertyRef, std::less<>>;

    void merge_named_style_properties(
        const std::string& name,
        LayoutPropertyMap& properties,
        std::set<std::string, std::less<>>& resolving
    ) const {
        const auto declaration = style_declarations_.find(name);
        if (declaration == style_declarations_.end() ||
            !resolving.insert(name).second) {
            return;
        }
        for (const StyleBase& base : declaration->second->bases) {
            merge_named_style_properties(base.name, properties, resolving);
        }
        for (const Property& property : declaration->second->properties) {
            properties.insert_or_assign(
                property.name,
                LayoutPropertyRef{
                    property.name,
                    property.value.get(),
                    property.span,
                }
            );
        }
        resolving.erase(name);
    }

    void merge_style_properties(
        const Expression& expression,
        LayoutPropertyMap& properties,
        std::set<std::string, std::less<>>& resolving
    ) const {
        const Expression& value = ungrouped(expression);
        if (const auto* identifier = std::get_if<IdentifierExpression>(&value.node)) {
            merge_named_style_properties(identifier->name, properties, resolving);
            return;
        }
        if (const auto* map = std::get_if<MapExpression>(&value.node)) {
            for (const MapEntry& entry : map->entries) {
                std::string name;
                if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key)) {
                    name = identifier->name;
                } else if (const auto* string = std::get_if<StringMapKey>(&entry.key)) {
                    name = string->value;
                }
                if (!name.empty()) {
                    properties.insert_or_assign(
                        name,
                        LayoutPropertyRef{name, entry.value.get(), entry.span}
                    );
                }
            }
            return;
        }
        const auto* call = std::get_if<CallExpression>(&value.node);
        if (call == nullptr || call->target.qualified_name() != "style") return;
        for (const Argument& argument : call->arguments) {
            if (!argument.name.has_value()) {
                merge_style_properties(*argument.value, properties, resolving);
                continue;
            }
            properties.insert_or_assign(
                *argument.name,
                LayoutPropertyRef{
                    *argument.name,
                    argument.value.get(),
                    argument.span,
                }
            );
        }
    }

    void merge_layout_properties(
        const Expression& expression,
        LayoutPropertyMap& properties
    ) const {
        const Expression& value = ungrouped(expression);
        const auto* map = std::get_if<MapExpression>(&value.node);
        if (map == nullptr) return;
        for (const MapEntry& entry : map->entries) {
            std::string name;
            if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key)) {
                name = identifier->name;
            } else if (const auto* string = std::get_if<StringMapKey>(&entry.key)) {
                name = string->value;
            }
            if (!name.empty()) {
                properties.insert_or_assign(
                    name,
                    LayoutPropertyRef{name, entry.value.get(), entry.span}
                );
            }
        }
    }

    void validate_widget_layout_properties(
        const WidgetCall& call,
        const std::vector<const SchemaParameter*>& parameters,
        const std::string& path,
        const Scope& scope
    ) {
        LayoutPropertyMap properties;
        std::set<std::string, std::less<>> resolving;
        const bool has_authored_style = std::ranges::any_of(
            parameters,
            [](const SchemaParameter* parameter) {
                return parameter != nullptr && parameter->name == "style";
            }
        );
        if (!has_authored_style) {
            for (auto defaults = active_widget_default_styles_.rbegin();
                 defaults != active_widget_default_styles_.rend();
                 ++defaults) {
                const auto style = (*defaults)->find(call.name);
                if (style == (*defaults)->end()) continue;
                merge_style_properties(*style->second, properties, resolving);
                break;
            }
        }
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const SchemaParameter* parameter = parameters[index];
            if (parameter != nullptr && parameter->name == "style") {
                merge_style_properties(
                    *call.arguments[index].value,
                    properties,
                    resolving
                );
            }
        }
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const SchemaParameter* parameter = parameters[index];
            if (parameter != nullptr && parameter->name == "layout") {
                merge_layout_properties(*call.arguments[index].value, properties);
            }
        }
        std::vector<LayoutPropertyRef> values;
        values.reserve(properties.size());
        for (const auto& [name, property] : properties) {
            static_cast<void>(name);
            values.push_back(property);
        }
        const std::string_view implicit_kind =
            call.name == "Panel" ? std::string_view("PANEL")
            : call.name == "Grid" ? std::string_view("GRID")
            : call.name == "Scroll" ? std::string_view("SCROLL")
                                    : std::string_view{};
        validate_layout_kind_properties(values, path, &scope, implicit_kind);
    }

    void validate_layout_kind_properties(
        const std::vector<LayoutPropertyRef>& properties,
        const std::string& path,
        const Scope* scope = nullptr,
        const std::string_view implicit_kind = {}
    ) {
        const auto find = [&properties](const std::string_view name)
            -> const LayoutPropertyRef* {
            const auto found = std::ranges::find(properties, name, &LayoutPropertyRef::name);
            return found != properties.end() ? &*found : nullptr;
        };
        const LayoutPropertyRef* kind_property = find("kind");
        const std::string* kind = kind_property != nullptr && kind_property->value != nullptr
            ? string_literal_value(*kind_property->value)
            : nullptr;
        const std::string implicit_kind_storage(implicit_kind);
        if (kind == nullptr && !implicit_kind_storage.empty()) {
            kind = &implicit_kind_storage;
        }
        if (kind == nullptr) return;
        const bool linear = *kind == "ROW" || *kind == "COLUMN";
        const bool layered = *kind == "PANEL" || *kind == "OVERLAY" ||
                             *kind == "PORTAL" || *kind == "STACK";
        const auto incompatible = [&](const std::string_view name,
                                      const std::string_view expected) {
            const LayoutPropertyRef* property = find(name);
            if (property == nullptr) return;
            if (property->value != nullptr &&
                !diagnosed_layout_properties_.insert(property->value).second) {
                return;
            }
            report(
                "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND",
                "Layout property '" + std::string(name) +
                    "' has no effect for layout kind '" + *kind + "'.",
                property->span,
                path + "." + std::string(name),
                std::string(expected)
            );
        };
        if (!linear) {
            incompatible("wrap", "ROW or COLUMN");
            incompatible("alignContent", "ROW or COLUMN");
        }
        if (*kind == "GRID" || *kind == "SCROLL" || *kind == "SPACER") {
            incompatible(
                "alignItems",
                "ROW, COLUMN, PANEL, OVERLAY, PORTAL, or STACK"
            );
            incompatible(
                "justifyContent",
                "ROW, COLUMN, PANEL, OVERLAY, PORTAL, or STACK"
            );
        }
        if (layered) {
            const LayoutPropertyRef* justify = find("justifyContent");
            const std::string* value = justify != nullptr && justify->value != nullptr
                ? string_literal_value(*justify->value)
                : nullptr;
            bool unsupported = value != nullptr && value->starts_with("SPACE_");
            if (!unsupported && value == nullptr && justify != nullptr &&
                justify->value != nullptr && scope != nullptr) {
                const SemanticTypePtr actual = infer(
                    *justify->value,
                    *scope,
                    path + ".justifyContent"
                );
                unsupported = actual->kind == SemanticTypeKind::enumeration &&
                    std::ranges::any_of(
                        actual->values,
                        [](const std::string& candidate) {
                            return candidate != "START" && candidate != "CENTER" &&
                                candidate != "END";
                        }
                    );
            }
            if (unsupported) {
                if (justify->value != nullptr &&
                    !diagnosed_layout_properties_.insert(justify->value).second) {
                    return;
                }
                report(
                    "STRATA.DSL.SEMANTIC_LAYOUT_PROPERTY_KIND",
                    value != nullptr
                        ? "Layered layout kind '" + *kind +
                            "' supports START, CENTER, or END justification, not '" +
                            *value + "'."
                        : "Layered layout kind '" + *kind +
                            "' requires a justification constrained to START, CENTER, or END.",
                    justify->span,
                    path + ".justifyContent",
                    value != nullptr
                        ? "START, CENTER, or END"
                        : "a layerJustify value"
                );
            }
        }
        if (layered || *kind == "SPACER") {
            incompatible("gap", "ROW, COLUMN, GRID, or SCROLL");
        }
        if (*kind != "PORTAL") {
            incompatible("anchorPoint", "PORTAL");
            incompatible("detachFromParentClip", "PORTAL");
            incompatible("portalTarget", "PORTAL");
        }
        // The remaining anchor presentation fields are also consumed from authored Select/Menu
        // popup roots and hoisted onto their synthesized portal, so they are not kind-local.
    }

    [[nodiscard]] SemanticTypePtr resolve_type(const TypeReference& reference) const {
        const std::string name = lower(reference.name);
        if (name == "binding") {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::state_binding;
            type->value = reference.arguments.size() == 1U
                              ? resolve_type(reference.arguments.front())
                              : simple(SemanticTypeKind::unsafe_component_parameter);
            return type;
        }
        if (name == "list") {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::list;
            type->element = reference.arguments.size() == 1U
                                ? resolve_type(reference.arguments.front())
                                : simple(SemanticTypeKind::unsafe_component_parameter);
            type->element_nullable =
                reference.arguments.size() == 1U && reference.arguments.front().nullable;
            type->maximum_items = maximum_eager_loop_items;
            return type;
        }
        if (name == "map" || name == "record") {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::map;
            type->label = name;
            type->allow_unknown_fields = true;
            type->value = reference.arguments.empty() ? simple(SemanticTypeKind::any)
                                                      : resolve_type(reference.arguments.front());
            return type;
        }
        if (const SemanticType* declared = registry_.component_parameter_type(name);
            declared != nullptr) {
            return std::make_shared<SemanticType>(*declared);
        }
        if (const SemanticType* declared = registry_.application_type(name);
            declared != nullptr) {
            return std::make_shared<SemanticType>(*declared);
        }
        return simple(SemanticTypeKind::unsafe_component_parameter);
    }

    void report(std::string code, std::string message, const SourceSpan& span,
                std::string component_path, std::string expected) {
        diagnostics_.push_back(Diagnostic{
            std::move(code),
            DiagnosticSeverity::error,
            std::move(message),
            span.range(),
            std::move(component_path),
            std::move(expected),
        });
    }

    void report_lowering(std::string code, std::string message, const SourceSpan& span,
                         std::string component_path, std::string expected) {
        lowering_diagnostics_.push_back(Diagnostic{
            std::move(code),
            DiagnosticSeverity::error,
            std::move(message),
            span.range(),
            std::move(component_path),
            std::move(expected),
        });
    }

    [[nodiscard]] std::optional<ValidatedAnimationValue>
    animation_frame_literal(const Property& property, const std::string& path) {
        if (const std::optional<StaticNumber> number = static_number(*property.value);
            number.has_value() && !number->unit.has_value()) {
            return ValidatedAnimationValue{
                ValidatedAnimationValueKind::number,
                number->value,
                false,
                {},
            };
        }
        const LiteralExpression* literal = static_literal(*property.value);
        if (literal != nullptr) {
            if (const auto* boolean = std::get_if<BooleanLiteral>(&literal->value)) {
                return ValidatedAnimationValue{
                    ValidatedAnimationValueKind::boolean,
                    0.0,
                    boolean->value,
                    {},
                };
            }
            if (const auto* color = std::get_if<ColorLiteral>(&literal->value)) {
                return ValidatedAnimationValue{
                    ValidatedAnimationValueKind::color,
                    0.0,
                    false,
                    canonical_color(color->raw),
                };
            }
        }
        report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
               "Animation frame property '" + property.name +
                   "' must be a number, boolean, or color literal.",
               property.value->span, path, "literal animation value");
        return std::nullopt;
    }

    [[nodiscard]] std::optional<std::int64_t>
    animation_duration(const Property& property, const std::string& path, const bool positive) {
        const std::optional<StaticNumber> number = static_number(*property.value);
        if (!number.has_value() || !number->unit.has_value()) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                   "Animation timing property '" + property.name + "' must be a duration literal.",
                   property.value->span, path, "duration literal");
            return std::nullopt;
        }
        const long double scale = *number->unit == "ms"  ? 1'000'000.0L
                                  : *number->unit == "s" ? 1'000'000'000.0L
                                                         : 0.0L;
        const long double nanos = static_cast<long double>(number->value) * scale;
        const long double rounded_nanos = std::round(nanos);
        const long double rounding_tolerance =
            std::numeric_limits<long double>::epsilon() * std::max(1.0L, std::abs(nanos)) * 8.0L;
        const bool in_range =
            scale != 0.0L && std::isfinite(nanos) &&
            rounded_nanos <= static_cast<long double>(std::numeric_limits<std::int64_t>::max()) &&
            rounded_nanos >= 0.0L && std::abs(nanos - rounded_nanos) <= rounding_tolerance &&
            (!positive || rounded_nanos >= 1.0L);
        if (!in_range) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_DURATION_BOUNDS",
                   "Animation timing property '" + property.name + "' must resolve to " +
                       (positive ? "a positive" : "a non-negative") +
                       " whole-nanosecond duration within int64 bounds.",
                   property.value->span, path,
                   positive ? "positive duration" : "non-negative duration");
            return std::nullopt;
        }
        return static_cast<std::int64_t>(rounded_nanos);
    }

    [[nodiscard]] std::optional<std::string>
    animation_string_literal(const Property& property, const std::string& path,
                             const std::set<std::string, std::less<>>& allowed) {
        const LiteralExpression* literal = static_literal(*property.value);
        const auto* string =
            literal != nullptr ? std::get_if<StringLiteral>(&literal->value) : nullptr;
        if (string == nullptr) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                   "Animation timing property '" + property.name + "' must be a string literal.",
                   property.value->span, path,
                   expected_names(std::vector<std::string>(allowed.begin(), allowed.end())));
            return std::nullopt;
        }
        if (!allowed.contains(string->value)) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_SCHEMA_VALUE",
                   "Animation timing property '" + property.name + "' has unsupported value '" +
                       string->value + "'.",
                   property.value->span, path,
                   expected_names(std::vector<std::string>(allowed.begin(), allowed.end())));
            return std::nullopt;
        }
        return string->value;
    }

    [[nodiscard]] std::optional<ValidatedAnimationRepeat>
    animation_repeat(const Property& property, const std::string& path) {
        const Expression& expression = ungrouped(*property.value);
        if (const LiteralExpression* literal = static_literal(expression); literal != nullptr) {
            if (const auto* string = std::get_if<StringLiteral>(&literal->value)) {
                if (string->value == "none")
                    return ValidatedAnimationRepeat{};
                if (string->value == "forever") {
                    return ValidatedAnimationRepeat{ValidatedAnimationRepeatKind::forever, 1U};
                }
                report("STRATA.DSL.SEMANTIC_ANIMATION_SCHEMA_VALUE",
                       "Animation repeat string must be exactly 'none' or 'forever'.",
                       property.value->span, path, "none, forever, or a positive integral count");
                return std::nullopt;
            }
        }

        const Expression* count_expression = &expression;
        if (const auto* map = std::get_if<MapExpression>(&expression.node); map != nullptr) {
            if (map->entries.size() != 1U || static_map_key(map->entries.front().key) == nullptr ||
                *static_map_key(map->entries.front().key) != "count") {
                report("STRATA.DSL.SEMANTIC_ANIMATION_REPEAT_COUNT",
                       "Animation repeat maps must contain exactly one literal 'count' field.",
                       property.value->span, path, "{ count: positive integer }");
                return std::nullopt;
            }
            count_expression = map->entries.front().value.get();
        }
        const std::optional<StaticNumber> count = static_number(*count_expression);
        constexpr double maximum = static_cast<double>(std::numeric_limits<std::uint32_t>::max());
        if (!count.has_value() || count->unit.has_value() || count->value <= 0.0 ||
            count->value > maximum || std::floor(count->value) != count->value) {
            report(count.has_value() ? "STRATA.DSL.SEMANTIC_ANIMATION_REPEAT_COUNT"
                                     : "STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                   count.has_value()
                       ? "Animation repeat count must be a positive integral uint32 value."
                       : "Animation repeat must be 'none', 'forever', a positive integer, or a "
                         "literal count map.",
                   count_expression->span, path, "positive integral uint32 repeat count");
            return std::nullopt;
        }
        return ValidatedAnimationRepeat{
            ValidatedAnimationRepeatKind::count,
            static_cast<std::uint32_t>(count->value),
        };
    }

    void validate_animation(const AnimationDeclaration& animation,
                            const SourceSpan& declaration_span, const Scope& scope) {
        const std::string path = "animation " + animation.name;
        const std::size_t diagnostic_start = diagnostics_.size();
        std::vector<const AnimationFrame*> from_frames;
        std::vector<const AnimationFrame*> to_frames;
        std::map<std::string, const Property*, std::less<>> timing;
        std::map<const Property*, std::optional<ValidatedAnimationValue>> frame_literals;

        for (const AnimationEntry& entry : animation.entries) {
            if (const auto* timing_entry = std::get_if<AnimationProperty>(&entry)) {
                const Property& property = timing_entry->property;
                if (!timing.emplace(property.name, &property).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Animation timing property '" + property.name +
                               "' is declared more than once.",
                           property.span, path, "unique animation timing property");
                }
                const SchemaParameter* schema = registry_.animation_timing_property(property.name);
                if (schema == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                           "Animation timing property '" + property.name + "' is not supported.",
                           property.span, path,
                           expected_names(registry_.animation_timing_property_names()));
                    static_cast<void>(infer(*property.value, scope, path + "." + property.name));
                } else {
                    validate_expected(*property.value, *schema->type, schema->nullable,
                                      path + "." + property.name, scope);
                }
                continue;
            }
            const AnimationFrame& frame = std::get<AnimationFrame>(entry);
            auto& frames = frame.phase == AnimationFramePhase::from ? from_frames : to_frames;
            frames.push_back(&frame);
            if (frames.size() > 1U) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ANIMATION_FRAME",
                       "Animation '" + animation.name + "' declares more than one " +
                           (frame.phase == AnimationFramePhase::from ? "from" : "to") + " frame.",
                       frame.span, path, "exactly one from frame and one to frame");
            }
            const std::string frame_path =
                path + (frame.phase == AnimationFramePhase::from ? "/from" : "/to");
            std::set<std::string, std::less<>> property_names;
            for (const Property& property : frame.properties) {
                if (!property_names.insert(property.name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Animation frame property '" + property.name +
                               "' is declared more than once.",
                           property.span, frame_path, "unique animation frame property");
                }
                const SchemaParameter* schema = registry_.animation_property(property.name);
                if (schema == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                           "Animation frame property '" + property.name + "' is not supported.",
                           property.span, frame_path,
                           expected_names(registry_.animation_property_names()));
                    static_cast<void>(
                        infer(*property.value, scope, frame_path + "." + property.name));
                } else {
                    validate_expected(*property.value, *schema->type, false,
                                      frame_path + "." + property.name, scope);
                    frame_literals.emplace(&property,
                                           animation_frame_literal(property, frame_path));
                }
            }
        }

        if (from_frames.empty() && to_frames.empty()) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_NO_KEYFRAMES",
                   "Animation '" + animation.name +
                       "' must declare one from and one to keyframe block.",
                   declaration_span, path, "one from and one to keyframe block");
        } else if (from_frames.size() != 1U || to_frames.size() != 1U) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_FRAME_PAIR",
                   "Animation '" + animation.name +
                       "' must declare exactly one from frame and exactly one to frame.",
                   declaration_span, path, "exactly one from frame and one to frame");
        }

        std::map<std::string, const Property*, std::less<>> from_properties;
        std::map<std::string, const Property*, std::less<>> to_properties;
        if (from_frames.size() == 1U) {
            for (const Property& property : from_frames.front()->properties) {
                from_properties.emplace(property.name, &property);
            }
        }
        if (to_frames.size() == 1U) {
            for (const Property& property : to_frames.front()->properties) {
                to_properties.emplace(property.name, &property);
            }
        }
        if (from_frames.size() == 1U && to_frames.size() == 1U && from_properties.empty() &&
            to_properties.empty()) {
            report("STRATA.DSL.SEMANTIC_ANIMATION_NO_TRACKS",
                   "Animation '" + animation.name + "' must animate at least one property.",
                   declaration_span, path, "matching non-empty frame property sets");
        } else if (from_frames.size() == 1U && to_frames.size() == 1U) {
            std::vector<std::string> from_names;
            std::vector<std::string> to_names;
            for (const auto& [name, property] : from_properties) {
                static_cast<void>(property);
                from_names.push_back(name);
            }
            for (const auto& [name, property] : to_properties) {
                static_cast<void>(property);
                to_names.push_back(name);
            }
            if (from_names != to_names) {
                report("STRATA.DSL.SEMANTIC_ANIMATION_FRAME_MISMATCH",
                       "Animation '" + animation.name +
                           "' must declare the same property set in its from and to frames.",
                       to_frames.front()->span, path + "/to",
                       expected_names(std::move(from_names)));
            }
        }

        ValidatedAnimation validated;
        validated.name = animation.name;
        const auto timing_property = [&timing](const std::string_view name) -> const Property* {
            const auto found = timing.find(name);
            return found != timing.end() ? found->second : nullptr;
        };
        if (const Property* property = timing_property("duration")) {
            if (const auto value = animation_duration(*property, path + ".duration", true)) {
                validated.duration_nanos = *value;
            }
        }
        if (const Property* property = timing_property("delay")) {
            if (const auto value = animation_duration(*property, path + ".delay", false)) {
                validated.delay_nanos = *value;
            }
        }
        if (const Property* property = timing_property("easing")) {
            if (const auto value =
                    animation_string_literal(*property, path + ".easing",
                                             {"cubic-in", "cubic-in-out", "cubic-out", "ease",
                                              "ease-in", "ease-out", "linear"})) {
                validated.easing = *value;
            }
        }
        if (const Property* property = timing_property("fillMode")) {
            if (const auto value = animation_string_literal(
                    *property, path + ".fillMode", {"BACKWARDS", "BOTH", "FORWARDS", "NONE"})) {
                validated.fill_mode = lower(*value);
            }
        }
        if (const Property* property = timing_property("trigger")) {
            if (const auto value =
                    animation_string_literal(*property, path + ".trigger",
                                             {"ANIMATE", "CHECKED", "ENTER", "EXIT", "FOCUS",
                                              "FOCUS_VISIBLE", "HOVER", "MOVE", "PRESSED"})) {
                validated.trigger = *value;
            }
        }
        if (const Property* property = timing_property("reverse")) {
            const LiteralExpression* literal = static_literal(*property->value);
            const auto* boolean =
                literal != nullptr ? std::get_if<BooleanLiteral>(&literal->value) : nullptr;
            if (boolean == nullptr) {
                report("STRATA.DSL.SEMANTIC_ANIMATION_LITERAL_REQUIRED",
                       "Animation timing property 'reverse' must be a boolean literal.",
                       property->value->span, path + ".reverse", "boolean literal");
            } else {
                validated.reverse = boolean->value;
            }
        }
        if (const Property* property = timing_property("repeat")) {
            if (const auto value = animation_repeat(*property, path + ".repeat")) {
                validated.repeat = *value;
            }
        }

        if (from_frames.size() == 1U && to_frames.size() == 1U &&
            from_properties.size() == to_properties.size()) {
            for (const Property& from : from_frames.front()->properties) {
                const auto matching = to_properties.find(from.name);
                if (matching == to_properties.end())
                    continue;
                const auto from_value = frame_literals.find(&from);
                const auto to_value = frame_literals.find(matching->second);
                if (from_value != frame_literals.end() && from_value->second.has_value() &&
                    to_value != frame_literals.end() && to_value->second.has_value()) {
                    validated.tracks.push_back(ValidatedAnimationTrack{
                        from.name,
                        *from_value->second,
                        *to_value->second,
                    });
                }
            }
        }
        if (diagnostics_.size() == diagnostic_start) {
            validated_animations_.try_emplace(animation.name, std::move(validated));
        }
    }

    void validate_declaration(const Declaration& declaration) {
        Scope scope = registry_.host_types();
        if (const auto* screen = std::get_if<ScreenDeclaration>(&declaration.node)) {
            validate_block(*screen->body, std::move(scope), "screen " + screen->name);
        } else if (const auto* overlay = std::get_if<OverlayDeclaration>(&declaration.node)) {
            validate_block(*overlay->body, std::move(scope), "overlay " + overlay->name);
        } else if (const auto* component = std::get_if<ComponentDeclaration>(&declaration.node)) {
            const ComponentSchema& schema = components_.at(component->name);
            for (std::size_t index = 0U; index < component->parameters.size(); ++index) {
                const Parameter& authored = component->parameters[index];
                const SchemaParameter& parameter = schema.parameters[index];
                if (authored.default_value != nullptr) {
                    if (parameter.type->kind == SemanticTypeKind::state_binding) {
                        report(
                            "STRATA.DSL.SEMANTIC_BINDING_TARGET",
                            "Binding component parameter '" + parameter.name +
                                "' cannot declare a default value.",
                            authored.default_value->span,
                            "component " + component->name + "(" + parameter.name + ")",
                            "required Binding<T> parameter"
                        );
                    } else {
                        validate_expected(
                            *authored.default_value,
                            *parameter.type,
                            parameter.nullable,
                            "component " + component->name + "(" + parameter.name + ")",
                            scope
                        );
                    }
                }
                scope.insert_or_assign(parameter.name, parameter.type);
            }
            const RootCardinality roots = root_cardinality(*component->body);
            if (roots.minimum != 1U || roots.maximum != 1U) {
                report("STRATA.DSL.SEMANTIC_COMPONENT_ROOT",
                       "Component '" + component->name +
                           "' must produce exactly one root node on every control-flow path.",
                       component->body->span, "component " + component->name, "single root node");
            }
            active_widget_default_styles_.push_back(&schema.widget_default_styles);
            validate_block(*component->body, std::move(scope), "component " + component->name, {},
                           {}, maximum_eager_loop_items, BlockContext::component);
            active_widget_default_styles_.pop_back();
        } else if (const auto* style = std::get_if<StyleDeclaration>(&declaration.node)) {
            const std::string path = "style " + style->name;
            std::set<std::string, std::less<>> seen;
            std::vector<LayoutPropertyRef> layout_properties;
            for (const Property& property : style->properties) {
                layout_properties.push_back(LayoutPropertyRef{
                    property.name,
                    property.value.get(),
                    property.span,
                });
                if (!seen.insert(property.name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Style property '" + property.name + "' is declared more than once.",
                           property.span, path, "unique style property");
                }
                const SchemaParameter* schema = registry_.style_property(property.name);
                if (schema == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                           "Style property '" + property.name + "' is not supported.",
                           property.span, path, expected_names(registry_.style_property_names()));
                    static_cast<void>(infer(*property.value, scope, path + "." + property.name));
                } else {
                    validate_expected(*property.value, *schema->type, schema->nullable,
                                      path + "." + property.name, scope);
                }
            }
            validate_layout_kind_properties(layout_properties, path, &scope);
            for (const StatementPtr& statement : style->body->statements) {
                if (!std::holds_alternative<PropertyStatement>(statement->node) &&
                    !std::holds_alternative<ErrorStatement>(statement->node)) {
                    report("STRATA.DSL.SEMANTIC_STATEMENT_NOT_ALLOWED",
                           "Style declarations may only contain property assignments.",
                           statement->span, path, "style property");
                }
            }
        } else if (const auto* animation = std::get_if<AnimationDeclaration>(&declaration.node)) {
            validate_animation(*animation, declaration.span, scope);
        }
    }

    void validate_style_composition() {
        for (const auto& [name, style] : style_declarations_) {
            static_cast<void>(name);
            std::set<std::string, std::less<>> seen_bases;
            for (const StyleBase& base : style->bases) {
                if (!seen_bases.insert(base.name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_STYLE_BASE",
                           "Style '" + style->name + "' extends '" + base.name +
                               "' more than once.",
                           base.span, "style " + style->name, "unique style base");
                }
                if (!style_declarations_.contains(base.name)) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_STYLE_BASE",
                           "Style '" + style->name + "' extends unknown style '" + base.name + "'.",
                           base.span, "style " + style->name, expected_names(styles_));
                }
            }
        }
        std::set<std::string, std::less<>> visiting;
        std::set<std::string, std::less<>> visited;
        const auto visit = [&](const auto& self, const std::string& name) -> void {
            if (visited.contains(name))
                return;
            if (!visiting.insert(name).second) {
                const StyleDeclaration* style = style_declarations_.at(name);
                report("STRATA.DSL.SEMANTIC_STYLE_CYCLE",
                       "Style composition cycle detected at '" + name + "'.", style->body->span,
                       "style " + name, "acyclic style composition");
                return;
            }
            const StyleDeclaration* style = style_declarations_.at(name);
            for (const StyleBase& base : style->bases) {
                if (style_declarations_.contains(base.name))
                    self(self, base.name);
            }
            visiting.erase(name);
            visited.insert(name);
        };
        for (const auto& [name, style] : style_declarations_) {
            static_cast<void>(style);
            visit(visit, name);
        }
        for (const auto& [name, style] : style_declarations_) {
            static_cast<void>(style);
            LayoutPropertyMap properties;
            std::set<std::string, std::less<>> resolving;
            merge_named_style_properties(name, properties, resolving);
            std::vector<LayoutPropertyRef> values;
            values.reserve(properties.size());
            for (const auto& [property_name, property] : properties) {
                static_cast<void>(property_name);
                values.push_back(property);
            }
            validate_layout_kind_properties(values, "style " + name);
        }
    }

    void collect_referenced_identifiers(const Expression& expression,
                                        std::set<std::string, std::less<>>& output,
                                        std::set<std::string, std::less<>> bound = {}) const {
        std::visit(
            [&](const auto& node) {
                using Node = std::decay_t<decltype(node)>;
                if constexpr (std::is_same_v<Node, IdentifierExpression>) {
                    if (!bound.contains(node.name))
                        output.insert(node.name);
                } else if constexpr (std::is_same_v<Node, GroupingExpression>) {
                    collect_referenced_identifiers(*node.expression, output, std::move(bound));
                } else if constexpr (std::is_same_v<Node, ListExpression>) {
                    for (const ExpressionPtr& element : node.elements) {
                        collect_referenced_identifiers(*element, output, bound);
                    }
                } else if constexpr (std::is_same_v<Node, MapExpression>) {
                    for (const MapEntry& entry : node.entries) {
                        if (const auto* key = std::get_if<ExpressionMapKey>(&entry.key)) {
                            collect_referenced_identifiers(*key->expression, output, bound);
                        }
                        collect_referenced_identifiers(*entry.value, output, bound);
                    }
                } else if constexpr (std::is_same_v<Node, UnaryExpression>) {
                    collect_referenced_identifiers(*node.operand, output, std::move(bound));
                } else if constexpr (std::is_same_v<Node, BinaryExpression>) {
                    collect_referenced_identifiers(*node.left, output, bound);
                    collect_referenced_identifiers(*node.right, output, std::move(bound));
                } else if constexpr (std::is_same_v<Node, ConditionalExpression>) {
                    collect_referenced_identifiers(*node.condition, output, bound);
                    collect_referenced_identifiers(*node.then_expression, output, bound);
                    collect_referenced_identifiers(*node.else_expression, output, std::move(bound));
                } else if constexpr (std::is_same_v<Node, PropertyAccessExpression>) {
                    collect_referenced_identifiers(*node.receiver, output, std::move(bound));
                } else if constexpr (std::is_same_v<Node, IndexExpression>) {
                    collect_referenced_identifiers(*node.receiver, output, bound);
                    collect_referenced_identifiers(*node.index, output, std::move(bound));
                } else if constexpr (std::is_same_v<Node, CallExpression>) {
                    for (const Argument& argument : node.arguments) {
                        collect_referenced_identifiers(*argument.value, output, bound);
                    }
                } else if constexpr (std::is_same_v<Node, LambdaExpression>) {
                    bound.insert(node.parameter_name);
                    collect_referenced_identifiers(*node.body, output, std::move(bound));
                }
            },
            expression.node);
    }

    void validate_derived_cycles(const Block& block, const std::string& path) {
        std::map<std::string, const Statement*, std::less<>> declarations;
        for (const StatementPtr& statement : block.statements) {
            if (const auto* derived = std::get_if<DerivedStatement>(&statement->node)) {
                declarations.try_emplace(derived->name, statement.get());
            }
        }
        if (declarations.empty())
            return;
        std::map<std::string, std::set<std::string, std::less<>>, std::less<>> dependencies;
        for (const auto& [name, statement] : declarations) {
            const auto& derived = std::get<DerivedStatement>(statement->node);
            std::set<std::string, std::less<>> referenced;
            collect_referenced_identifiers(*derived.expression, referenced);
            for (const std::string& candidate : referenced) {
                if (declarations.contains(candidate))
                    dependencies[name].insert(candidate);
            }
        }
        std::set<std::string, std::less<>> visiting;
        std::set<std::string, std::less<>> visited;
        std::set<std::string, std::less<>> reported;
        const auto visit = [&](const auto& self, const std::string& name) -> void {
            if (visited.contains(name))
                return;
            if (!visiting.insert(name).second) {
                if (reported.insert(name).second) {
                    report("STRATA.DSL.SEMANTIC_DERIVED_CYCLE",
                           "Derived value cycle detected at '" + name + "'.",
                           declarations.at(name)->span, path + "/" + name,
                           "acyclic derived values");
                }
                return;
            }
            for (const std::string& dependency : dependencies[name])
                self(self, dependency);
            visiting.erase(name);
            visited.insert(name);
        };
        for (const auto& [name, statement] : declarations) {
            static_cast<void>(statement);
            visit(visit, name);
        }
    }

    void validate_style_property(const Property& property, const Scope& scope,
                                 const std::string& path) {
        const SchemaParameter* schema = registry_.style_property(property.name);
        if (schema == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                   "Style property '" + property.name + "' is not supported.", property.span, path,
                   expected_names(registry_.style_property_names()));
            static_cast<void>(infer(*property.value, scope, path + "." + property.name));
            return;
        }
        validate_expected(*property.value, *schema->type, schema->nullable,
                          path + "." + property.name, scope);
    }

    void validate_widget_defaults(const Property& property, const Scope& scope,
                                  const std::string& path) {
        const auto* defaults = std::get_if<MapExpression>(&property.value->node);
        if (defaults == nullptr) {
            report("STRATA.DSL.SEMANTIC_INVALID_WIDGET_DEFAULTS",
                   "Component widget defaults must be an object keyed by widget name.",
                   property.value->span, path,
                   "{ Button: { style: StyleName, variant: \"primary\" } }");
            return;
        }
        std::set<std::string, std::less<>> seen_widgets;
        for (const MapEntry& entry : defaults->entries) {
            const std::string* widget_name = static_map_key(entry.key);
            if (widget_name == nullptr || registry_.widget(*widget_name) == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET_DEFAULT",
                       "Widget default '" +
                           (widget_name != nullptr ? *widget_name : std::string("<dynamic>")) +
                           "' does not name a registered widget.",
                       map_key_span(entry.key), path, expected_names(registry_.widget_names()));
                static_cast<void>(infer(*entry.value, scope, path + "/defaults"));
                continue;
            }
            if (!seen_widgets.insert(*widget_name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_WIDGET_DEFAULT",
                       "Widget default '" + *widget_name + "' is declared more than once.",
                       map_key_span(entry.key), path, "one default object per widget");
            }
            const auto* values = std::get_if<MapExpression>(&entry.value->node);
            if (values == nullptr) {
                report("STRATA.DSL.SEMANTIC_INVALID_WIDGET_DEFAULTS",
                       "Widget default '" + *widget_name + "' must be an object.",
                       entry.value->span, path, "style and/or variant properties");
                continue;
            }
            std::set<std::string, std::less<>> seen_properties;
            for (const MapEntry& value : values->entries) {
                const std::string* name = static_map_key(value.key);
                if (name == nullptr || (*name != "style" && *name != "variant")) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET_DEFAULT_PROPERTY",
                           "Widget defaults support only 'style' and 'variant', not '" +
                               (name != nullptr ? *name : std::string("<dynamic>")) + "'.",
                           map_key_span(value.key), path, "style, variant");
                    static_cast<void>(
                        infer(*value.value, scope, path + "/defaults/" + *widget_name));
                    continue;
                }
                if (!seen_properties.insert(*name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                           "Widget default property '" + *name + "' is declared more than once.",
                           map_key_span(value.key), path + "/defaults/" + *widget_name,
                           "unique widget default property");
                }
                validate_expected(
                    *value.value,
                    *simple(*name == "style" ? SemanticTypeKind::style : SemanticTypeKind::string),
                    false, path + "/defaults/" + *widget_name + "." + *name, scope);
            }
        }
    }

    void validate_block(const Block& block, Scope scope, const std::string& path,
                        RetainedStateNames retained_states = {}, LocalNames local_names = {},
                        const std::size_t maximum_loop_items = maximum_eager_loop_items,
                        const BlockContext context = BlockContext::ui) {
        validate_derived_cycles(block, path);
        for (const StatementPtr& statement : block.statements) {
            if (const auto* derived = std::get_if<DerivedStatement>(&statement->node)) {
                scope.try_emplace(derived->name, simple(SemanticTypeKind::unknown));
            }
        }
        for (const StatementPtr& statement : block.statements) {
            if (const auto* state = std::get_if<StateStatement>(&statement->node)) {
                if (!local_names.insert(state->name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_STATE",
                           "State or local value '" + state->name +
                               "' is already defined in this scope.",
                           statement->span, path + "/" + state->name,
                           "unique state or derived name");
                }
                const Expression* previous_persisted_owner = persisted_owner_;
                persisted_owner_ = state->initializer.get();
                SemanticTypePtr initializer =
                    infer(*state->initializer, scope, path + "/" + state->name);
                SemanticTypePtr declared = state->type_reference.has_value()
                                               ? resolve_type(*state->type_reference)
                                               : initializer;
                if (state->type_reference.has_value()) {
                    validate_expected(*state->initializer, *declared,
                                      state->type_reference->nullable, path + "/" + state->name,
                                      scope);
                }
                persisted_owner_ = previous_persisted_owner;
                scope.insert_or_assign(state->name, std::move(declared));
                retained_states.insert(state->name);
            } else if (const auto* derived = std::get_if<DerivedStatement>(&statement->node)) {
                if (!local_names.insert(derived->name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_STATE",
                           "State or local value '" + derived->name +
                               "' is already defined in this scope.",
                           statement->span, path + "/" + derived->name,
                           "unique state or derived name");
                }
                scope.insert_or_assign(
                    derived->name, infer(*derived->expression, scope, path + "/" + derived->name));
            } else if (const auto* property = std::get_if<PropertyStatement>(&statement->node)) {
                if (property->property.name == "defaults") {
                    if (context == BlockContext::component) {
                        validate_widget_defaults(property->property, scope, path);
                    } else {
                        report("STRATA.DSL.SEMANTIC_STATEMENT_NOT_ALLOWED",
                               "Widget defaults are only allowed at the top level of a component "
                               "declaration.",
                               statement->span, path, "component-level defaults");
                    }
                } else {
                    validate_style_property(property->property, scope, path);
                }
            } else if (const auto* widget = std::get_if<WidgetStatement>(&statement->node)) {
                validate_call(widget->call, scope, retained_states, path, maximum_loop_items,
                              context);
            } else if (const auto* root = std::get_if<RootStatement>(&statement->node)) {
                validate_call(root->call, scope, retained_states, path + "/root",
                              maximum_loop_items, context);
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
                validate_expected(*conditional->condition, *simple(SemanticTypeKind::boolean),
                                  false, path + "/if", scope);
                validate_block(*conditional->then_block, scope, path + "/then", retained_states,
                               local_names, maximum_loop_items, nested_context(context));
                if (conditional->else_block != nullptr) {
                    validate_block(*conditional->else_block, scope, path + "/else", retained_states,
                                   local_names, maximum_loop_items, nested_context(context));
                }
            } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
                SemanticTypePtr subject = infer(*when->subject, scope, path + "/when");
                const auto* status_access =
                    std::get_if<PropertyAccessExpression>(&when->subject->node);
                const auto* async_identifier =
                    status_access != nullptr && status_access->property_name == "status"
                        ? std::get_if<IdentifierExpression>(&status_access->receiver->node)
                        : nullptr;
                const SemanticTypePtr async_receiver =
                    status_access != nullptr
                        ? infer(*status_access->receiver, scope, path + "/when")
                        : SemanticTypePtr{};
                const bool async_when = async_identifier != nullptr && async_receiver != nullptr &&
                                        async_receiver->kind == SemanticTypeKind::async_value;
                std::set<std::string, std::less<>> async_matches;
                std::size_t async_match_count = 0U;
                std::size_t else_count = 0U;
                for (std::size_t index = 0U; index < when->branches.size(); ++index) {
                    const WhenBranch& branch = when->branches[index];
                    Scope branch_scope = scope;
                    if (branch.match != nullptr) {
                        validate_expected(*branch.match, *subject, false,
                                          path + "/when[" + std::to_string(index) + "]", scope);
                        if (async_when) {
                            if (const std::string* match = string_literal_value(*branch.match);
                                match != nullptr) {
                                std::string normalized = *match;
                                std::ranges::transform(
                                    normalized, normalized.begin(), [](const unsigned char value) {
                                        return static_cast<char>(std::toupper(value));
                                    });
                                async_matches.insert(normalized);
                                ++async_match_count;
                                if (normalized == "READY") {
                                    auto ready = std::make_shared<SemanticType>(*async_receiver);
                                    ready->kind = SemanticTypeKind::host_object;
                                    branch_scope.insert_or_assign(async_identifier->name,
                                                                  std::move(ready));
                                }
                            }
                        }
                    } else {
                        ++else_count;
                        if (index + 1U != when->branches.size()) {
                            report("STRATA.DSL.SEMANTIC_WHEN_ELSE_ORDER",
                                   "The else branch must be the final when branch.", branch.span,
                                   path + "/when", "else as final branch");
                        }
                    }
                    validate_block(*branch.block, branch_scope,
                                   path + "/when[" + std::to_string(index) + "]", retained_states,
                                   local_names, maximum_loop_items, nested_context(context));
                }
                const std::set<std::string, std::less<>> required_async{
                    "FAILED",
                    "IDLE",
                    "LOADING",
                    "READY",
                };
                if (async_when) {
                    if (async_matches != required_async ||
                        async_match_count != required_async.size() || else_count != 0U) {
                        report("STRATA.DSL.SEMANTIC_ASYNC_WHEN_NOT_EXHAUSTIVE",
                               "An async-state when must declare IDLE, LOADING, READY, and FAILED "
                               "branches exactly.",
                               statement->span, path + "/when", "IDLE, LOADING, READY, FAILED");
                    }
                } else if (else_count != 1U) {
                    report("STRATA.DSL.SEMANTIC_WHEN_NOT_EXHAUSTIVE",
                           "A when expression must declare exactly one else branch.",
                           statement->span, path + "/when", "one final else branch");
                }
            } else if (const auto* loop = std::get_if<ForStatement>(&statement->node)) {
                SemanticTypePtr collection = infer(*loop->collection, scope, path + "/for");
                Scope loop_scope = scope;
                SemanticTypePtr item = simple(SemanticTypeKind::any);
                if ((collection->kind == SemanticTypeKind::list ||
                     collection->kind == SemanticTypeKind::collection) &&
                    collection->element != nullptr) {
                    item = collection->element;
                } else {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_COLLECTION",
                           "Loop collection for '" + loop->item_name +
                               "' is not a known bounded collection.",
                           loop->collection->span, path + "/for", "bounded collection");
                }
                if (collection->kind == SemanticTypeKind::list ||
                    collection->kind == SemanticTypeKind::collection) {
                    if (!collection->maximum_items.has_value()) {
                        report("STRATA.DSL.SEMANTIC_UNBOUNDED_COLLECTION",
                               "Loop collection for '" + loop->item_name +
                                   "' does not declare a maximum item count.",
                               loop->collection->span, path + "/for",
                               "collection with a bounded maximum");
                    } else if (*collection->maximum_items > maximum_loop_items) {
                        report("STRATA.DSL.SEMANTIC_LOOP_BOUND_EXCEEDED",
                               "Loop collection for '" + loop->item_name + "' can contain " +
                                   std::to_string(*collection->maximum_items) +
                                   " items, exceeding the limit of " +
                                   std::to_string(maximum_loop_items) + ".",
                               loop->collection->span, path + "/for",
                               "collection with maxItems <= " + std::to_string(maximum_loop_items));
                    }
                }
                loop_scope.insert_or_assign(loop->item_name, std::move(item));
                if (loop->index_name.has_value()) {
                    loop_scope.insert_or_assign(*loop->index_name,
                                                simple(SemanticTypeKind::number));
                }
                if (loop->filter != nullptr) {
                    validate_expected(*loop->filter, *simple(SemanticTypeKind::boolean), false,
                                      path + "/for/filter", loop_scope);
                }
                validate_block(*loop->block, std::move(loop_scope), path + "/" + loop->item_name,
                               retained_states, local_names, maximum_loop_items,
                               nested_context(context));
            }
        }
    }

    [[nodiscard]] bool call_has_explicit_key(const WidgetCall& call) const {
        return std::ranges::any_of(call.arguments, [](const Argument& argument) {
            return argument.name == "key" || argument.name == "rowKey";
        });
    }

    [[nodiscard]] bool every_root_is_keyed(const Block& block) const {
        for (const StatementPtr& statement : block.statements) {
            if (const auto* widget = std::get_if<WidgetStatement>(&statement->node)) {
                if (!call_has_explicit_key(widget->call))
                    return false;
            } else if (const auto* root = std::get_if<RootStatement>(&statement->node)) {
                if (!call_has_explicit_key(root->call))
                    return false;
            } else if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
                if (!every_root_is_keyed(*conditional->then_block) ||
                    conditional->else_block == nullptr ||
                    !every_root_is_keyed(*conditional->else_block)) {
                    return false;
                }
            } else if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
                if (when->branches.empty() ||
                    !std::ranges::all_of(when->branches, [&](const WhenBranch& branch) {
                        return every_root_is_keyed(*branch.block);
                    })) {
                    return false;
                }
            } else if (std::holds_alternative<ForStatement>(statement->node)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool repeater_identity_is_extractable(const Block& block) const {
        for (const StatementPtr& statement : block.statements) {
            if (std::holds_alternative<WidgetStatement>(statement->node) ||
                std::holds_alternative<RootStatement>(statement->node)) {
                continue;
            }
            if (const auto* conditional = std::get_if<IfStatement>(&statement->node)) {
                if (!repeater_identity_is_extractable(*conditional->then_block) ||
                    conditional->else_block == nullptr ||
                    !repeater_identity_is_extractable(*conditional->else_block)) {
                    return false;
                }
                continue;
            }
            if (const auto* when = std::get_if<WhenStatement>(&statement->node)) {
                if (when->branches.empty() ||
                    !std::ranges::all_of(when->branches, [&](const WhenBranch& branch) {
                        return repeater_identity_is_extractable(*branch.block);
                    })) {
                    return false;
                }
                continue;
            }
            return false;
        }
        return true;
    }

    void validate_repeater_body(const WidgetCall& call, const std::string& call_path) {
        const ForStatement* loop = nullptr;
        if (call.body != nullptr && call.body->statements.size() == 1U) {
            loop = std::get_if<ForStatement>(&call.body->statements.front()->node);
        }
        if (loop == nullptr) {
            report("STRATA.DSL.SEMANTIC_REPEATER_BODY",
                   "Repeater requires exactly one keyed for statement so it can virtualize without "
                   "eager expansion.",
                   call.body != nullptr ? call.body->span : call.span, call_path,
                   "Repeater { for item in items { Panel(key: item.id) { ... } } }");
            return;
        }
        const RootCardinality roots = root_cardinality(*loop->block);
        if (roots.minimum != 1U || roots.maximum != 1U || !every_root_is_keyed(*loop->block)) {
            report("STRATA.DSL.SEMANTIC_REPEATER_ITEM_ROOT",
                   "Each Repeater iteration must produce exactly one explicitly keyed root node.",
                   loop->block->span, call_path, "one root widget with key: ...");
        } else if (!repeater_identity_is_extractable(*loop->block)) {
            report("STRATA.DSL.SEMANTIC_REPEATER_IDENTITY_EXTRACTOR",
                   "A Repeater root key must be selectable from the item/index and outer values "
                   "without executing row-local state, derivations, or nested loops.",
                   loop->block->span, call_path,
                   "direct keyed root or exhaustive keyed if/when roots");
        }
    }

    void validate_component_slot_fills(const WidgetCall& call, const ComponentSchema& component,
                                       const std::string& call_path) {
        std::set<std::string, std::less<>> occupied;
        bool has_raw_content = false;
        if (call.body != nullptr) {
            for (const StatementPtr& statement : call.body->statements) {
                const auto* widget = std::get_if<WidgetStatement>(&statement->node);
                if (widget == nullptr || widget->call.name != "Slot") {
                    if (!std::holds_alternative<StateStatement>(statement->node) &&
                        !std::holds_alternative<DerivedStatement>(statement->node)) {
                        has_raw_content = true;
                    }
                    continue;
                }
                const Expression* name_expression = nullptr;
                for (const Argument& argument : widget->call.arguments) {
                    if (argument.name == "name" ||
                        (!argument.name.has_value() && name_expression == nullptr)) {
                        name_expression = argument.value.get();
                    }
                }
                const std::string* name =
                    name_expression != nullptr ? string_literal_value(*name_expression) : nullptr;
                if (name == nullptr) {
                    report("STRATA.DSL.SEMANTIC_DYNAMIC_SLOT_NAME",
                           "Component slot fill names must be string literals.",
                           name_expression != nullptr ? name_expression->span : widget->call.span,
                           call_path, expected_names([&] {
                               std::vector<std::string> names;
                               for (const ComponentSchema::Slot& slot : component.slots) {
                                   names.push_back(slot.name);
                               }
                               return names;
                           }()));
                    continue;
                }
                const auto declared = std::ranges::find_if(
                    component.slots,
                    [name](const ComponentSchema::Slot& slot) { return slot.name == *name; });
                if (declared == component.slots.end()) {
                    std::vector<std::string> names;
                    for (const ComponentSchema::Slot& slot : component.slots)
                        names.push_back(slot.name);
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_SLOT",
                           "Component does not declare slot '" + *name + "'.", widget->call.span,
                           call_path, expected_names(std::move(names)));
                } else if (!occupied.insert(*name).second) {
                    report("STRATA.DSL.SEMANTIC_DUPLICATE_SLOT_FILL",
                           "Slot '" + *name + "' is filled more than once.", widget->call.span,
                           call_path, "at most one fill for '" + *name + "'");
                }
            }
        }
        if (has_raw_content) {
            const ComponentSchema::Slot* shorthand = nullptr;
            if (component.slots.size() == 1U) {
                shorthand = &component.slots.front();
            } else {
                const auto found =
                    std::ranges::find_if(component.slots, [](const ComponentSchema::Slot& slot) {
                        return slot.name == "content";
                    });
                if (found != component.slots.end())
                    shorthand = &*found;
            }
            if (shorthand == nullptr) {
                std::vector<std::string> names;
                for (const ComponentSchema::Slot& slot : component.slots)
                    names.push_back(slot.name);
                report("STRATA.DSL.SEMANTIC_AMBIGUOUS_SLOT_SHORTHAND",
                       "Raw child content is ambiguous for the component; fill a named Slot "
                       "explicitly.",
                       call.body != nullptr ? call.body->span : call.span, call_path,
                       expected_names(std::move(names)));
            } else if (!occupied.insert(shorthand->name).second) {
                report(
                    "STRATA.DSL.SEMANTIC_DUPLICATE_SLOT_FILL",
                    "Slot '" + shorthand->name + "' is filled by both shorthand and a named Slot.",
                    call.body != nullptr ? call.body->span : call.span, call_path, "one slot fill");
            }
        }
        for (const ComponentSchema::Slot& slot : component.slots) {
            if (slot.required && !occupied.contains(slot.name)) {
                report("STRATA.DSL.SEMANTIC_MISSING_REQUIRED_SLOT",
                       "Required slot '" + slot.name + "' is not filled.", call.span, call_path,
                       "Slot(name: \"" + slot.name + "\") { ... }");
            }
        }
    }

    void validate_call(const WidgetCall& call, const Scope& scope,
                       const RetainedStateNames& retained_states, const std::string& path,
                       const std::size_t maximum_loop_items, const BlockContext context) {
        const std::string call_path = path + "/" + call.name;
        const WidgetSchema* widget = registry_.widget(call.name);
        const auto component = components_.find(call.name);
        if (widget == nullptr && component == components_.end()) {
            std::vector<std::string> names = registry_.widget_names();
            for (const auto& [name, schema] : components_) {
                static_cast<void>(schema);
                names.push_back(name);
            }
            report("STRATA.DSL.SEMANTIC_UNKNOWN_WIDGET",
                   "Widget or component '" + call.name + "' is not declared.", call.span, call_path,
                   expected_names(std::move(names)));
            if (call.body != nullptr) {
                validate_block(*call.body, scope, call_path, retained_states, {},
                               maximum_loop_items, nested_context(context));
            }
            return;
        }

        const std::vector<SchemaParameter>& parameters =
            widget != nullptr ? widget->parameters : component->second.parameters;
        if (call.name == "Repeater")
            validate_repeater_body(call, call_path);
        if (component != components_.end()) {
            validate_component_slot_fills(call, component->second, call_path);
        }
        const bool allows_children = widget == nullptr || widget->allows_children;
        if (call.body != nullptr && !allows_children) {
            report("STRATA.DSL.SEMANTIC_CHILDREN_NOT_ALLOWED",
                   "Widget '" + call.name + "' does not accept child content.", call.body->span,
                   call_path, "no child block");
        }
        std::vector<const SchemaParameter*> resolved_parameters(call.arguments.size(), nullptr);
        std::set<std::string, std::less<>> consumed_parameters;
        std::size_t positional_index = 0U;
        bool seen_named = false;
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = nullptr;
            if (argument.name.has_value()) {
                seen_named = true;
                const auto found =
                    std::ranges::find_if(parameters, [&argument](const SchemaParameter& candidate) {
                        return candidate.accepts_name(*argument.name);
                    });
                if (found != parameters.end())
                    parameter = &*found;
            } else {
                if (seen_named) {
                    report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                           "Positional arguments must appear before named arguments.",
                           argument.span, call_path, "named argument");
                }
                if (positional_index < parameters.size())
                    parameter = &parameters[positional_index];
                ++positional_index;
            }
            resolved_parameters[index] = parameter;
            if (parameter != nullptr && parameter->name == "undoLabel") {
                const std::string* label = string_literal_value(*argument.value);
                if (label == nullptr || label->empty()) {
                    report("STRATA.DSL.SEMANTIC_UNDO_LABEL_STATIC",
                           "Undoable widget bindings require a non-empty string-literal undoLabel.",
                           argument.value->span, call_path + ".undoLabel",
                           "undoLabel: \"user-facing change\"");
                }
            }
            if (parameter != nullptr && parameter->name == "persistenceKey") {
                const std::string* key = string_literal_value(*argument.value);
                if (key == nullptr || key->empty()) {
                    report("STRATA.DSL.SEMANTIC_PERSISTENCE_KEY_STATIC",
                           "Widget persistenceKey must be a non-empty string literal.",
                           argument.value->span, call_path + ".persistenceKey",
                           "stable non-empty string literal");
                } else {
                    const auto [found, inserted] = persistence_keys_.try_emplace(*key, call_path);
                    if (!inserted && found->second != call_path) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_PERSISTENCE_KEY",
                               "Persistence key '" + *key + "' is already used by '" +
                                   found->second + "'.",
                               argument.value->span, call_path + ".persistenceKey",
                               "unique persistence key");
                    }
                }
            }
            if (parameter == nullptr) {
                std::vector<std::string> names;
                names.reserve(parameters.size());
                for (const SchemaParameter& candidate : parameters)
                    names.push_back(candidate.name);
                const std::string display_name = argument.name.has_value()
                                                     ? *argument.name
                                                     : "#" + std::to_string(positional_index);
                report(argument.name.has_value() ? "STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT"
                                                 : "STRATA.DSL.SEMANTIC_TOO_MANY_ARGUMENTS",
                       "Argument '" + display_name + "' is not accepted by '" + call.name + "'.",
                       argument.span, call_path, expected_names(std::move(names)));
                continue;
            }
            if (!consumed_parameters.insert(parameter->name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                       "Argument '" + parameter->name + "' is provided more than once.",
                       argument.span, call_path, "single value for '" + parameter->name + "'");
            }
        }
        if (widget != nullptr &&
            consumed_parameters.contains("presentationTemplate") &&
            !consumed_parameters.contains("key")) {
            report(
                "STRATA.DSL.SEMANTIC_PRESENTATION_KEY_REQUIRED",
                "Widget '" + call.name +
                    "' requires an explicit key when presentationTemplate is supplied.",
                call.span,
                call_path + ".key",
                "key: stableKey"
            );
        }
        if (widget != nullptr) {
            validate_widget_layout_properties(
                call,
                resolved_parameters,
                call_path + ".layout",
                scope
            );
        }
        for (std::size_t index = 0U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            const SchemaParameter* parameter = resolved_parameters[index];
            const std::string argument_name = argument.name.value_or("#" + std::to_string(index));
            if (parameter == nullptr) {
                static_cast<void>(infer(*argument.value, scope, call_path + "." + argument_name));
                continue;
            }
            const WidgetBindingSchema* binding =
                widget != nullptr ? widget->find_binding(parameter->name) : nullptr;
            if (binding != nullptr) {
                if (consumed_parameters.contains(binding->value_parameter) ||
                    consumed_parameters.contains(binding->event_parameter)) {
                    report("STRATA.DSL.SEMANTIC_BINDING_CONFLICT",
                           "Binding '" + binding->shorthand_parameter +
                               "' cannot be combined with explicit '" + binding->value_parameter +
                               "' or '" + binding->event_parameter + "'.",
                           argument.span, call_path + "." + binding->shorthand_parameter,
                           "binding shorthand or explicit controlled value/callback, not both");
                }
                const auto* identifier = std::get_if<IdentifierExpression>(&argument.value->node);
                const auto scoped =
                    identifier != nullptr ? scope.find(identifier->name) : scope.end();
                const bool forwarded_binding =
                    scoped != scope.end() && scoped->second != nullptr &&
                    scoped->second->kind == SemanticTypeKind::state_binding;
                if (identifier == nullptr ||
                    (!retained_states.contains(identifier->name) && !forwarded_binding)) {
                    report("STRATA.DSL.SEMANTIC_BINDING_TARGET",
                           "Binding '" + binding->shorthand_parameter +
                               "' must name retained state or a Binding<T> component parameter.",
                           argument.value->span, call_path + "." + binding->shorthand_parameter,
                           binding->shorthand_parameter + ": retainedState");
                }
                const SchemaParameter* controlled =
                    widget->find_parameter(binding->value_parameter);
                if (controlled != nullptr) {
                    validate_expected(*argument.value, *controlled->type, controlled->nullable,
                                      call_path + "." + binding->shorthand_parameter, scope);
                }
                consumed_parameters.insert(binding->value_parameter);
                consumed_parameters.insert(binding->event_parameter);
                continue;
            }
            if (parameter->type->kind == SemanticTypeKind::state_binding) {
                const auto* identifier =
                    std::get_if<IdentifierExpression>(&argument.value->node);
                const auto scoped =
                    identifier != nullptr ? scope.find(identifier->name) : scope.end();
                const bool forwarded_binding =
                    scoped != scope.end() && scoped->second != nullptr &&
                    scoped->second->kind == SemanticTypeKind::state_binding;
                if (identifier == nullptr ||
                    (!retained_states.contains(identifier->name) && !forwarded_binding)) {
                    report(
                        "STRATA.DSL.SEMANTIC_BINDING_TARGET",
                        "Binding component parameter '" + parameter->name +
                            "' must receive retained state directly.",
                        argument.value->span,
                        call_path + "." + parameter->name,
                        parameter->name + ": retainedState"
                    );
                }
                if (parameter->type->value != nullptr) {
                    validate_expected(
                        *argument.value,
                        *parameter->type->value,
                        parameter->nullable,
                        call_path + "." + parameter->name,
                        scope
                    );
                }
                continue;
            }
            validate_expected(*argument.value, *parameter->type, parameter->nullable,
                              call_path + "." + parameter->name, scope);
        }
        for (const SchemaParameter& parameter : parameters) {
            if (parameter.required && !consumed_parameters.contains(parameter.name)) {
                report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                       "Required argument '" + parameter.name + "' is missing for '" + call.name +
                           "'.",
                       call.span, call_path,
                       parameter.name + ": " + parameter.type->diagnostic_name());
            }
        }
        if (call.body != nullptr) {
            if (component != components_.end()) {
                active_widget_default_styles_.push_back(
                    &component->second.widget_default_styles
                );
            }
            validate_block(*call.body, scope, call_path, retained_states, {},
                           call.name == "Repeater" ? maximum_lazy_loop_items : maximum_loop_items,
                           call.name == "Repeater" ? BlockContext::lazy_ui
                                                   : nested_context(context));
            if (component != components_.end()) {
                active_widget_default_styles_.pop_back();
            }
        }
    }

    void validate_helper_arguments(const CallExpression& call, const HelperSchema& helper,
                                   const Scope& scope, const std::string& component_path) {
        std::set<std::string, std::less<>> consumed;
        std::size_t positional_index = 0U;
        bool seen_named = false;
        for (const Argument& argument : call.arguments) {
            const SchemaParameter* parameter = nullptr;
            if (argument.name.has_value()) {
                seen_named = true;
                const auto found = std::ranges::find_if(
                    helper.parameters, [&argument](const SchemaParameter& candidate) {
                        return candidate.accepts_name(*argument.name);
                    });
                if (found != helper.parameters.end())
                    parameter = &*found;
            } else {
                if (seen_named) {
                    report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                           "Positional helper arguments must appear before named arguments.",
                           argument.span, component_path, "named argument");
                }
                if (positional_index < helper.parameters.size()) {
                    parameter = &helper.parameters[positional_index];
                }
                ++positional_index;
            }
            const SemanticTypePtr expected =
                parameter != nullptr ? parameter->type : helper.vararg_type;
            const bool accepted_named_vararg = argument.name.has_value() && parameter == nullptr &&
                                               helper.allow_named_varargs && expected != nullptr;
            if (expected == nullptr ||
                (argument.name.has_value() && parameter == nullptr && !accepted_named_vararg)) {
                std::vector<std::string> names;
                names.reserve(helper.parameters.size());
                for (const SchemaParameter& candidate : helper.parameters) {
                    names.push_back(candidate.name);
                }
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                       "Helper '" + helper.name + "' does not accept argument '" +
                           argument.name.value_or("#" + std::to_string(positional_index)) + "'.",
                       argument.span, component_path, expected_names(std::move(names)));
                static_cast<void>(infer(*argument.value, scope, component_path));
                continue;
            }
            if (parameter != nullptr && !consumed.insert(parameter->name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                       "Helper argument '" + parameter->name + "' is provided more than once.",
                       argument.span, component_path, "single value for '" + parameter->name + "'");
            }
            validate_expected(
                *argument.value, *expected, parameter != nullptr && parameter->nullable,
                component_path + "." +
                    (parameter != nullptr ? parameter->name : argument.name.value_or("arg")),
                scope);
        }
        for (const SchemaParameter& parameter : helper.parameters) {
            if (parameter.required && !consumed.contains(parameter.name)) {
                report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                       "Required helper argument '" + parameter.name + "' is missing for '" +
                           helper.name + "'.",
                       call.target.span, component_path,
                       parameter.name + ": " + parameter.type->diagnostic_name());
            }
        }
    }

    void validate_action_call(const CallExpression& call, const Scope& scope,
                              const std::string& component_path) {
        if (call.arguments.empty()) {
            report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                   "Required action argument 'name' is missing for 'action'.", call.target.span,
                   component_path, "name: string");
            return;
        }
        const Argument& first = call.arguments.front();
        if (first.name.has_value() && *first.name != "name") {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                   "action(...) first argument must be positional or named 'name'.", first.span,
                   component_path, "name");
        }
        const std::string* id = string_literal_value(*first.value);
        if (id == nullptr) {
            report("STRATA.DSL.SEMANTIC_DYNAMIC_ACTION",
                   "action(...) requires a static action id string.", first.span, component_path,
                   expected_names(registry_.action_names()));
        }
        const ActionSchema* action = id != nullptr ? registry_.action(*id) : nullptr;
        if (id != nullptr && action == nullptr) {
            report("STRATA.DSL.SEMANTIC_UNKNOWN_ACTION", "Action '" + *id + "' is not registered.",
                   first.span, component_path, expected_names(registry_.action_names()));
        }
        std::set<std::string, std::less<>> consumed;
        for (std::size_t index = 1U; index < call.arguments.size(); ++index) {
            const Argument& argument = call.arguments[index];
            if (!argument.name.has_value()) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT", "Action arguments must be named.",
                       argument.span, component_path, "named action argument");
                static_cast<void>(infer(*argument.value, scope, component_path));
                continue;
            }
            const SchemaParameter* schema = nullptr;
            if (action != nullptr) {
                const auto parameter = std::ranges::find_if(
                    action->parameters, [&argument](const SchemaParameter& candidate) {
                        return candidate.accepts_name(*argument.name);
                    });
                if (parameter != action->parameters.end())
                    schema = &*parameter;
            }
            const std::string canonical_name = schema != nullptr ? schema->name : *argument.name;
            if (!consumed.insert(canonical_name).second) {
                report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                       "Action argument '" + canonical_name + "' is provided more than once.",
                       argument.span, component_path, "single value for '" + canonical_name + "'");
            }
            if (schema == nullptr) {
                if (action != nullptr) {
                    std::vector<std::string> names;
                    for (const SchemaParameter& candidate : action->parameters) {
                        names.push_back(candidate.name);
                    }
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_ACTION_ARGUMENT",
                           "Action '" + *id + "' does not define argument '" + *argument.name +
                               "'.",
                           argument.span, component_path, expected_names(std::move(names)));
                }
                static_cast<void>(infer(*argument.value, scope, component_path));
                continue;
            }
            validate_expected(*argument.value, *schema->type, schema->nullable,
                              component_path + "." + schema->name, scope);
            if (schema->name == "undoLabel") {
                const std::string* label = string_literal_value(*argument.value);
                if (label == nullptr || label->empty()) {
                    report("STRATA.DSL.SEMANTIC_UNDO_LABEL_STATIC",
                           "Undoable state actions require a non-empty string-literal undoLabel.",
                           argument.value->span, component_path + ".undoLabel",
                           "undoLabel: \"user-facing change\"");
                }
            }
        }
        if (action != nullptr) {
            for (const SchemaParameter& parameter : action->parameters) {
                if (parameter.required && !consumed.contains(parameter.name)) {
                    report("STRATA.DSL.SEMANTIC_MISSING_ACTION_ARGUMENT",
                           "Action '" + *id + "' requires argument '" + parameter.name + "'.",
                           call.target.span, component_path,
                           parameter.name + ": " + parameter.type->diagnostic_name());
                }
            }
        }
    }

    [[nodiscard]] SemanticTypePtr component_template_type(
        const ComponentSchema& component,
        const std::set<std::string, std::less<>>& bound = {}
    ) const {
        auto type = std::make_shared<SemanticType>();
        type->kind = SemanticTypeKind::component_template;
        type->fields.reserve(component.parameters.size());
        for (const SchemaParameter& parameter : component.parameters) {
            if (bound.contains(parameter.name)) continue;
            type->fields.push_back(ObjectField{
                parameter.name,
                parameter.type,
                parameter.required,
                parameter.nullable,
            });
        }
        return type;
    }

    [[nodiscard]] std::set<std::string, std::less<>>
    validate_component_template_arguments(
        const CallExpression& call,
        const std::string& template_name,
        const ComponentSchema& component,
        const SemanticType* owner_contract,
        const Scope& scope,
        const std::string& component_path
    ) {
        std::set<std::string, std::less<>> bound;
        std::size_t positional_index = 0U;
        bool seen_named = false;
        for (const Argument& argument : call.arguments) {
            const SchemaParameter* parameter = nullptr;
            if (argument.name.has_value()) {
                seen_named = true;
                const auto found = std::ranges::find_if(
                    component.parameters,
                    [&argument](const SchemaParameter& candidate) {
                        return candidate.accepts_name(*argument.name);
                    }
                );
                if (found != component.parameters.end()) parameter = &*found;
            } else {
                if (seen_named) {
                    report(
                        "STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                        "Positional arguments must appear before named arguments.",
                        argument.span,
                        component_path,
                        "named argument"
                    );
                }
                if (positional_index < component.parameters.size()) {
                    parameter = &component.parameters[positional_index];
                }
                ++positional_index;
            }
            if (parameter == nullptr) {
                std::vector<std::string> names;
                names.reserve(component.parameters.size());
                for (const SchemaParameter& candidate : component.parameters) {
                    names.push_back(candidate.name);
                }
                report(
                    argument.name.has_value()
                        ? "STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT"
                        : "STRATA.DSL.SEMANTIC_TOO_MANY_ARGUMENTS",
                    "Argument '" +
                        argument.name.value_or("#" + std::to_string(positional_index)) +
                        "' is not accepted by component template '" + template_name + "'.",
                    argument.span,
                    component_path,
                    expected_names(std::move(names))
                );
                static_cast<void>(infer(*argument.value, scope, component_path));
                continue;
            }
            if (!bound.insert(parameter->name).second) {
                report(
                    "STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                    "Argument '" + parameter->name + "' is provided more than once.",
                    argument.span,
                    component_path + "." + parameter->name,
                    "single value for '" + parameter->name + "'"
                );
            }
            if (owner_contract != nullptr &&
                owner_contract->find_field(parameter->name) != nullptr) {
                report(
                    "STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_BOUND_OWNER_PARAMETER",
                    "Component template parameter '" + parameter->name +
                        "' is supplied by the template owner and cannot be pre-bound.",
                    argument.span,
                    component_path + "." + parameter->name,
                    "bind an additional component parameter"
                );
            }
            validate_expected(
                *argument.value,
                *parameter->type,
                parameter->nullable,
                component_path + "." + parameter->name,
                scope
            );
        }
        return bound;
    }

    void validate_component_template(
        const Expression& expression,
        const SemanticType& expected,
        const std::string& component_path,
        const Scope& scope
    ) {
        const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
        const auto* partial = std::get_if<CallExpression>(&expression.node);
        const std::string template_name = identifier != nullptr
            ? identifier->name
            : partial != nullptr ? partial->target.qualified_name() : std::string{};
        if (template_name.empty() ||
            (identifier != nullptr && scope.contains(identifier->name))) {
            const SemanticTypePtr actual = infer(expression, scope, component_path);
            if (!type_matches(expected, *actual, false)) {
                type_mismatch(expected, *actual, expression, component_path);
            }
            return;
        }
        const auto component = components_.find(template_name);
        if (component == components_.end()) {
            std::vector<std::string> names;
            names.reserve(components_.size());
            for (const auto& [name, schema] : components_) {
                static_cast<void>(schema);
                names.push_back(name);
            }
            report(
                "STRATA.DSL.SEMANTIC_UNKNOWN_COMPONENT_TEMPLATE",
                "Component template '" + template_name + "' is not declared.",
                expression.span,
                component_path,
                expected_names(std::move(names))
            );
            return;
        }
        const std::set<std::string, std::less<>> bound = partial != nullptr
            ? validate_component_template_arguments(
                  *partial,
                  template_name,
                  component->second,
                  &expected,
                  scope,
                  component_path
              )
            : std::set<std::string, std::less<>>{};
        for (const SchemaParameter& parameter : component->second.parameters) {
            if (bound.contains(parameter.name)) continue;
            const ObjectField* provided = expected.find_field(parameter.name);
            if (provided == nullptr) {
                if (parameter.required) {
                    report(
                        "STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER",
                        "Component template '" + template_name +
                            "' requires parameter '" + parameter.name +
                            "', but the template owner does not provide it.",
                        expression.span,
                        component_path + "." + parameter.name,
                        "remove the parameter or give it a default value"
                    );
                }
                continue;
            }
            if (provided->nullable && !parameter.nullable) {
                report(
                    "STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER",
                    "Component template parameter '" + parameter.name +
                        "' must accept null values.",
                    expression.span,
                    component_path + "." + parameter.name,
                    parameter.name + ": " + parameter.type->diagnostic_name() + "?"
                );
                continue;
            }
            if (provided->type->kind != SemanticTypeKind::any &&
                !type_matches(*parameter.type, *provided->type, parameter.nullable)) {
                report(
                    "STRATA.DSL.SEMANTIC_COMPONENT_TEMPLATE_PARAMETER",
                    "Component template parameter '" + parameter.name + "' expects " +
                        parameter.type->diagnostic_name() + ", but the owner provides " +
                        provided->type->diagnostic_name() + ".",
                    expression.span,
                    component_path + "." + parameter.name,
                    parameter.name + ": " + provided->type->diagnostic_name()
                );
            }
        }
    }

    void validate_expected(const Expression& expression, const SemanticType& expected,
                           const bool nullable, const std::string& component_path,
                           const Scope& scope) {
        if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            validate_expected(*grouping->expression, expected, nullable, component_path, scope);
            return;
        }
        if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            validate_expected(*conditional->condition, *simple(SemanticTypeKind::boolean), false,
                              component_path + ".condition", scope);
            validate_expected(*conditional->then_expression, expected, nullable,
                              component_path + ".then", scope);
            validate_expected(*conditional->else_expression, expected, nullable,
                              component_path + ".else", scope);
            return;
        }
        if (const auto* binary = std::get_if<BinaryExpression>(&expression.node);
            binary != nullptr && binary->operation == BinaryOperator::coalesce) {
            validate_expected(*binary->left, expected, true, component_path + ".left", scope);
            validate_expected(*binary->right, expected, nullable, component_path + ".right", scope);
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
                           "Action '" + *id + "' is not registered.", expression.span,
                           component_path, expected_names(registry_.action_names()));
                } else {
                    for (const SchemaParameter& parameter : action->parameters) {
                        if (parameter.required) {
                            report("STRATA.DSL.SEMANTIC_MISSING_ACTION_ARGUMENT",
                                   "Action '" + *id + "' requires argument '" + parameter.name +
                                       "'; use action(...) with named arguments.",
                                   expression.span, component_path,
                                   parameter.name + ": " + parameter.type->diagnostic_name());
                        }
                    }
                }
                return;
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "action") {
                validate_action_call(*call, scope, component_path);
                return;
            }
        }
        if (expected.kind == SemanticTypeKind::component_template) {
            validate_component_template(expression, expected, component_path, scope);
            return;
        }
        if (expected.kind == SemanticTypeKind::material) {
            if (const std::string* id = string_literal_value(expression); id != nullptr) {
                if (!registry_.has_material(*id)) {
                    const std::string names = expected_names(registry_.material_ids());
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL",
                           "Material '" + *id + "' is not a built-in material.", expression.span,
                           component_path, names);
                    report_lowering("STRATA.DSL.COMPILE_UNKNOWN_MATERIAL",
                                    "Material '" + *id + "' is not a built-in material.",
                                    expression.span, component_path, names);
                }
                return;
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "material") {
                const std::string* id = !call->arguments.empty()
                                            ? string_literal_value(*call->arguments.front().value)
                                            : nullptr;
                if (id == nullptr) {
                    report("STRATA.DSL.SEMANTIC_DYNAMIC_MATERIAL",
                           "material(...) requires a static material id string.", expression.span,
                           component_path, expected_names(registry_.material_ids()));
                    for (const Argument& argument : call->arguments) {
                        static_cast<void>(infer(*argument.value, scope, component_path));
                    }
                    return;
                }
                const MaterialSchema* material = registry_.material(*id);
                if (material == nullptr) {
                    const std::string names = expected_names(registry_.material_ids());
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL",
                           "Material '" + *id + "' is not a built-in material.",
                           call->arguments.front().span, component_path, names);
                    report_lowering("STRATA.DSL.COMPILE_UNKNOWN_MATERIAL",
                                    "Material '" + *id + "' is not a built-in material.",
                                    call->arguments.front().span, component_path, names);
                }
                std::set<std::string, std::less<>> seen;
                for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
                    const Argument& argument = call->arguments[index];
                    if (!argument.name.has_value()) {
                        report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                               "Material parameters must be named.", argument.span, component_path,
                               "named material parameter");
                        static_cast<void>(infer(*argument.value, scope, component_path));
                        continue;
                    }
                    if (!seen.insert(*argument.name).second) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                               "Material parameter '" + *argument.name +
                                   "' is supplied more than once.",
                               argument.span, component_path + "." + *argument.name,
                               "unique material parameter");
                    }
                    const SchemaParameter* parameter =
                        material != nullptr ? material->find_parameter(*argument.name) : nullptr;
                    if (parameter == nullptr) {
                        std::vector<std::string> parameter_names;
                        if (material != nullptr) {
                            parameter_names.reserve(material->parameters.size());
                            for (const SchemaParameter& candidate : material->parameters) {
                                parameter_names.push_back(candidate.name);
                            }
                        }
                        report("STRATA.DSL.SEMANTIC_UNKNOWN_MATERIAL_PARAMETER",
                               "Material '" + *id + "' does not define parameter '" +
                                   *argument.name + "'.",
                               argument.span, component_path + "." + *argument.name,
                               expected_names(std::move(parameter_names)));
                        static_cast<void>(infer(*argument.value, scope, component_path));
                        continue;
                    }
                    validate_expected(*argument.value, *parameter->type, false,
                                      component_path + "." + parameter->name, scope);
                }
                return;
            }
        }
        if (expected.kind == SemanticTypeKind::animation) {
            std::optional<std::string> name;
            if (const std::string* literal = string_literal_value(expression); literal != nullptr) {
                name = *literal;
            } else if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
                       identifier != nullptr && scope.find(identifier->name) == scope.end()) {
                name = identifier->name;
            }
            if (name.has_value()) {
                if (std::ranges::find(animations_, *name) == animations_.end()) {
                    const std::string names = expected_names(animations_);
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION",
                           "Animation '" + *name + "' is not declared.", expression.span,
                           component_path, names);
                    report_lowering("STRATA.DSL.COMPILE_UNKNOWN_ANIMATION",
                                    "Animation '" + *name + "' is not declared.", expression.span,
                                    component_path, names);
                }
                return;
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "animation") {
                if (const HelperSchema* helper = registry_.helper("animation"); helper != nullptr) {
                    validate_helper_arguments(*call, *helper, scope, component_path);
                }
                const std::string* animation_name =
                    !call->arguments.empty() ? string_literal_value(*call->arguments.front().value)
                                             : nullptr;
                if (animation_name != nullptr &&
                    std::ranges::find(animations_, *animation_name) == animations_.end()) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_ANIMATION",
                           "Animation '" + *animation_name + "' is not declared.",
                           call->arguments.front().span, component_path,
                           expected_names(animations_));
                }
                return;
            }
        }
        if (expected.kind == SemanticTypeKind::style) {
            if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
                identifier != nullptr &&
                std::ranges::find(styles_, identifier->name) != styles_.end()) {
                return;
            }
            if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node);
                identifier != nullptr && scope.find(identifier->name) == scope.end()) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_STYLE",
                       "Style '" + identifier->name + "' is not declared.", expression.span,
                       component_path, expected_names(styles_));
                return;
            }
            if (const auto* style = std::get_if<MapExpression>(&expression.node)) {
                std::set<std::string, std::less<>> seen;
                std::vector<LayoutPropertyRef> layout_properties;
                for (const MapEntry& entry : style->entries) {
                    std::string name;
                    if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key)) {
                        name = identifier->name;
                    } else if (const auto* string = std::get_if<StringMapKey>(&entry.key)) {
                        name = string->value;
                    } else {
                        report("STRATA.DSL.SEMANTIC_DYNAMIC_STYLE_KEY",
                               "Style object keys must be static identifiers or strings.",
                               entry.span, component_path, "static style property name");
                        static_cast<void>(infer(*entry.value, scope, component_path));
                        continue;
                    }
                    if (!seen.insert(name).second) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                               "Style property '" + name + "' is declared more than once.",
                               entry.span, component_path + "." + name, "unique style property");
                    }
                    layout_properties.push_back(LayoutPropertyRef{
                        name,
                        entry.value.get(),
                        entry.span,
                    });
                    const SchemaParameter* property = registry_.style_property(name);
                    if (property == nullptr) {
                        report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                               "Style property '" + name + "' is not supported.", entry.span,
                               component_path + "." + name,
                               expected_names(registry_.style_property_names()));
                        static_cast<void>(infer(*entry.value, scope, component_path));
                    } else {
                        validate_expected(*entry.value, *property->type, property->nullable,
                                          component_path + "." + name, scope);
                    }
                }
                validate_layout_kind_properties(layout_properties, component_path, &scope);
                return;
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "style") {
                std::set<std::string, std::less<>> seen;
                bool seen_named = false;
                std::vector<LayoutPropertyRef> layout_properties;
                for (const Argument& argument : call->arguments) {
                    if (!argument.name.has_value()) {
                        if (seen_named) {
                            report("STRATA.DSL.SEMANTIC_POSITIONAL_ARGUMENT_ORDER",
                                   "Positional style bases must appear before named property "
                                   "overrides.",
                                   argument.span, component_path, "named style property");
                        }
                        validate_expected(*argument.value, expected, false,
                                          component_path + ".base", scope);
                        continue;
                    }
                    seen_named = true;
                    const std::string& name = *argument.name;
                    layout_properties.push_back(LayoutPropertyRef{
                        name,
                        argument.value.get(),
                        argument.span,
                    });
                    if (!seen.insert(name).second) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_PROPERTY",
                               "Style property '" + name + "' is provided more than once.",
                               argument.span, component_path + "." + name,
                               "single value for '" + name + "'");
                    }
                    const SchemaParameter* property = registry_.style_property(name);
                    if (property == nullptr) {
                        report("STRATA.DSL.SEMANTIC_UNKNOWN_PROPERTY",
                               "Style property '" + name + "' is not supported.", argument.span,
                               component_path + "." + name,
                               expected_names(registry_.style_property_names()));
                        static_cast<void>(infer(*argument.value, scope, component_path));
                    } else {
                        validate_expected(*argument.value, *property->type, property->nullable,
                                          component_path + "." + name, scope);
                    }
                }
                validate_layout_kind_properties(layout_properties, component_path, &scope);
                return;
            }
        }
        if (expected.kind == SemanticTypeKind::effect) {
            if (const std::string* name = string_literal_value(expression); name != nullptr) {
                if (registry_.effect(*name) == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_EFFECT",
                           "Effect '" + *name + "' is not declared.", expression.span,
                           component_path, expected_names(registry_.effect_names()));
                }
                return;
            }
            if (const auto* call = std::get_if<CallExpression>(&expression.node);
                call != nullptr && call->target.qualified_name() == "effect") {
                const std::string* name = !call->arguments.empty()
                                              ? string_literal_value(*call->arguments.front().value)
                                              : nullptr;
                if (name == nullptr) {
                    report("STRATA.DSL.SEMANTIC_DYNAMIC_EFFECT",
                           "effect(...) requires a static effect name string.", expression.span,
                           component_path, expected_names(registry_.effect_names()));
                }
                const EffectSchema* effect = name != nullptr ? registry_.effect(*name) : nullptr;
                if (name != nullptr && effect == nullptr) {
                    report("STRATA.DSL.SEMANTIC_UNKNOWN_EFFECT",
                           "Effect '" + *name + "' is not declared.", call->arguments.front().span,
                           component_path, expected_names(registry_.effect_names()));
                }
                std::set<std::string, std::less<>> seen;
                for (std::size_t index = 1U; index < call->arguments.size(); ++index) {
                    const Argument& argument = call->arguments[index];
                    if (!argument.name.has_value()) {
                        report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                               "Effect parameters must be named.", argument.span, component_path,
                               "named effect parameter");
                        static_cast<void>(infer(*argument.value, scope, component_path));
                        continue;
                    }
                    if (!seen.insert(*argument.name).second) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_ARGUMENT",
                               "Effect parameter '" + *argument.name +
                                   "' is supplied more than once.",
                               argument.span, component_path, "unique effect parameter");
                    }
                    if (*argument.name == "refreshRate") {
                        const SemanticTypePtr actual =
                            infer(*argument.value, scope, component_path + ".refreshRate");
                        const std::string* literal = string_literal_value(*argument.value);
                        const std::optional<StaticNumber> number = static_number(*argument.value);
                        const bool accepted_type = actual->kind == SemanticTypeKind::number ||
                                                   (literal != nullptr && *literal == "UNBOUNDED");
                        const bool accepted_number =
                            !number.has_value() ||
                            (!number->unit.has_value() && number->value > 0.0);
                        if (!accepted_type || !accepted_number) {
                            report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
                                   "Effect refreshRate must be a positive number or \"UNBOUNDED\".",
                                   argument.span, component_path + ".refreshRate",
                                   "number, UNBOUNDED");
                        }
                        continue;
                    }
                    const SchemaParameter* parameter =
                        effect != nullptr ? effect->find_parameter(*argument.name) : nullptr;
                    if (parameter == nullptr) {
                        std::vector<std::string> names;
                        if (effect != nullptr) {
                            names.push_back("refreshRate");
                            for (const SchemaParameter& candidate : effect->parameters) {
                                names.push_back(candidate.name);
                            }
                            report("STRATA.DSL.SEMANTIC_UNKNOWN_ARGUMENT",
                                   "Effect '" + *name + "' does not define parameter '" +
                                       *argument.name + "'.",
                                   argument.span, component_path, expected_names(std::move(names)));
                        }
                        static_cast<void>(infer(*argument.value, scope, component_path));
                    } else {
                        validate_expected(*argument.value, *parameter->type, parameter->nullable,
                                          component_path + "." + parameter->name, scope);
                    }
                }
                if (effect != nullptr) {
                    for (const SchemaParameter& parameter : effect->parameters) {
                        if (parameter.required && !seen.contains(parameter.name)) {
                            report("STRATA.DSL.SEMANTIC_MISSING_ARGUMENT",
                                   "Effect '" + *name + "' requires parameter '" + parameter.name +
                                       "'.",
                                   expression.span, component_path, parameter.name);
                        }
                    }
                }
                return;
            }
        }
        if (expected.kind == SemanticTypeKind::layout) {
            if (const auto* layout = std::get_if<MapExpression>(&expression.node)) {
                std::vector<LayoutPropertyRef> layout_properties;
                for (const MapEntry& entry : layout->entries) {
                    std::string name;
                    if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key)) {
                        name = identifier->name;
                    } else if (const auto* string = std::get_if<StringMapKey>(&entry.key)) {
                        name = string->value;
                    }
                    layout_properties.push_back(LayoutPropertyRef{
                        name,
                        entry.value.get(),
                        entry.span,
                    });
                    const SchemaParameter* property = registry_.layout_property(name);
                    if (property == nullptr) {
                        report("STRATA.DSL.SEMANTIC_UNKNOWN_LAYOUT_PROPERTY",
                               "Layout property '" + name + "' is not declared.", entry.span,
                               component_path + "." + name,
                               expected_names(registry_.layout_property_names()));
                        static_cast<void>(infer(*entry.value, scope, component_path + "." + name));
                    } else {
                        validate_expected(*entry.value, *property->type, property->nullable,
                                          component_path + "." + property->name, scope);
                    }
                }
                validate_layout_kind_properties(layout_properties, component_path, &scope);
                return;
            }
        }
        if (const auto* list = std::get_if<ListExpression>(&expression.node);
            list != nullptr && expected.kind == SemanticTypeKind::list &&
            expected.element != nullptr) {
            for (std::size_t index = 0U; index < list->elements.size(); ++index) {
                validate_expected(*list->elements[index], *expected.element,
                                  expected.element_nullable,
                                  component_path + "[" + std::to_string(index) + "]", scope);
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
                    if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key)) {
                        keys.push_back(identifier->name);
                    } else if (const auto* string = std::get_if<StringMapKey>(&entry.key)) {
                        keys.push_back(string->value);
                    }
                }
                std::vector<const SemanticType*> matches;
                for (const SemanticType* option : options) {
                    const bool declares_every_key =
                        std::ranges::all_of(keys, [option](const std::string& key) {
                            return option->allow_unknown_fields ||
                                   option->find_field(key) != nullptr;
                        });
                    if (declares_every_key)
                        matches.push_back(option);
                }
                if (matches.size() == 1U) {
                    validate_object_fields(*matches.front(), *map, expression.span, component_path,
                                           scope);
                    return;
                }
                if (matches.empty()) {
                    type_mismatch(expected, *infer(expression, scope, component_path), expression,
                                  component_path);
                    return;
                }
            }
        }
        if (expected.kind == SemanticTypeKind::path) {
            if (const std::string* commands = string_literal_value(expression);
                commands != nullptr) {
                // The outline is parsed by the same code the runtime uses, so a malformed one is
                // a compile error pointing at the literal instead of a shape missing at runtime.
                try {
                    static_cast<void>(ui::Path::parse(*commands));
                } catch (const std::exception& error) {
                    report("STRATA.DSL.SEMANTIC_INVALID_PATH",
                           std::string("Path outline is invalid: ") + error.what(), expression.span,
                           component_path, "a path outline of M/L/H/V/Q/C/Z commands");
                }
                return;
            }
        }
        SemanticTypePtr actual = infer(expression, scope, component_path);
        if (!type_matches(expected, *actual, nullable)) {
            type_mismatch(expected, *actual, expression, component_path);
        }
    }

    /**
     * The declared object type an authored object must satisfy. A union offers several shapes, so
     * the authored keys select it: the one shape that declares every key present. No match means
     * the object fits nothing the type allows; several matches stay unchecked rather than guessed.
     */
    static void collect_object_options(const SemanticType& expected,
                                       std::vector<const SemanticType*>& output) {
        if (expected.kind == SemanticTypeKind::map && !expected.fields.empty()) {
            output.push_back(&expected);
            return;
        }
        if (expected.kind != SemanticTypeKind::union_value)
            return;
        for (const SemanticTypePtr& option : expected.options) {
            if (option != nullptr)
                collect_object_options(*option, output);
        }
    }

    /**
     * Structural validation of a declared object type: every authored entry is checked against its
     * declared field type, unknown entries are rejected unless the type opts into them, and missing
     * required fields are reported at the object itself.
     */
    void validate_object_fields(const SemanticType& expected, const MapExpression& map,
                                const SourceSpan span, const std::string& component_path,
                                const Scope& scope) {
        std::set<std::string, std::less<>> seen;
        for (const MapEntry& entry : map.entries) {
            std::string name;
            if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key)) {
                name = identifier->name;
            } else if (const auto* string = std::get_if<StringMapKey>(&entry.key)) {
                name = string->value;
            }
            const std::string path = component_path + "." + name;
            const ObjectField* field = expected.find_field(name);
            if (field == nullptr) {
                if (expected.allow_unknown_fields && expected.value != nullptr) {
                    validate_expected(*entry.value, *expected.value, expected.value_nullable, path,
                                      scope);
                    continue;
                }
                if (expected.allow_unknown_fields) {
                    static_cast<void>(infer(*entry.value, scope, path));
                    continue;
                }
                std::vector<std::string> names;
                names.reserve(expected.fields.size());
                for (const ObjectField& declared : expected.fields)
                    names.push_back(declared.name);
                report("STRATA.DSL.SEMANTIC_UNKNOWN_FIELD",
                       "Field '" + name + "' is not part of " + expected.diagnostic_name() + ".",
                       entry.span, path, expected_names(names));
                static_cast<void>(infer(*entry.value, scope, path));
                continue;
            }
            seen.insert(name);
            validate_expected(*entry.value, *field->type, field->nullable, path, scope);
        }
        for (const ObjectField& field : expected.fields) {
            if (!field.required || seen.contains(field.name))
                continue;
            report("STRATA.DSL.SEMANTIC_MISSING_FIELD",
                   "Field '" + field.name + "' is required by " + expected.diagnostic_name() + ".",
                   span, component_path + "." + field.name, field.type->diagnostic_name());
        }
    }

    void type_mismatch(const SemanticType& expected, const SemanticType& actual,
                       const Expression& expression, const std::string& component_path) {
        report("STRATA.DSL.SEMANTIC_TYPE_MISMATCH",
               "Expected " + expected.diagnostic_name() + ", but found " +
                   actual.diagnostic_name() + ".",
               expression.span, component_path, expected.diagnostic_name());
    }

    [[nodiscard]] SemanticTypePtr infer(const Expression& expression, const Scope& scope,
                                        const std::string& component_path) {
        static_cast<void>(component_path);
        if (const auto* literal = std::get_if<LiteralExpression>(&expression.node)) {
            if (const auto* string = std::get_if<StringLiteral>(&literal->value)) {
                return literal_string(string->value);
            }
            if (const auto* number = std::get_if<NumberLiteral>(&literal->value)) {
                return simple(number->unit.has_value() ? SemanticTypeKind::duration
                                                       : SemanticTypeKind::number);
            }
            if (std::holds_alternative<ColorLiteral>(literal->value))
                return simple(SemanticTypeKind::color);
            if (std::holds_alternative<BooleanLiteral>(literal->value))
                return simple(SemanticTypeKind::boolean);
            return simple(SemanticTypeKind::null_value);
        }
        if (const auto* identifier = std::get_if<IdentifierExpression>(&expression.node)) {
            const auto found = scope.find(identifier->name);
            if (found != scope.end())
                return found->second;
            const auto component = components_.find(identifier->name);
            if (component != components_.end())
                return component_template_type(component->second);
            if (std::ranges::find(styles_, identifier->name) != styles_.end())
                return simple(SemanticTypeKind::style);
            if (std::ranges::find(animations_, identifier->name) != animations_.end()) {
                return simple(SemanticTypeKind::animation);
            }
            return simple(SemanticTypeKind::unknown);
        }
        if (const auto* grouping = std::get_if<GroupingExpression>(&expression.node)) {
            return infer(*grouping->expression, scope, component_path);
        }
        if (const auto* list = std::get_if<ListExpression>(&expression.node)) {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::list;
            type->maximum_items = list->elements.size();
            type->minimum_items = list->elements.size();
            type->element = list->elements.empty()
                                ? simple(SemanticTypeKind::any)
                                : infer(*list->elements.front(), scope, component_path);
            return type;
        }
        if (const auto* map = std::get_if<MapExpression>(&expression.node)) {
            auto type = std::make_shared<SemanticType>();
            type->kind = SemanticTypeKind::map;
            type->label = "object";
            for (const MapEntry& entry : map->entries) {
                std::string name;
                if (const auto* identifier = std::get_if<IdentifierMapKey>(&entry.key))
                    name = identifier->name;
                if (const auto* string = std::get_if<StringMapKey>(&entry.key))
                    name = string->value;
                if (!name.empty()) {
                    type->fields.push_back(ObjectField{
                        std::move(name),
                        infer(*entry.value, scope, component_path),
                        true,
                        false,
                    });
                }
            }
            return type;
        }
        if (const auto* unary = std::get_if<UnaryExpression>(&expression.node)) {
            static_cast<void>(infer(*unary->operand, scope, component_path));
            return simple(unary->operation == UnaryOperator::logical_not
                              ? SemanticTypeKind::boolean
                              : SemanticTypeKind::number);
        }
        if (const auto* binary = std::get_if<BinaryExpression>(&expression.node)) {
            SemanticTypePtr left = infer(*binary->left, scope, component_path);
            SemanticTypePtr right = infer(*binary->right, scope, component_path);
            static_cast<void>(right);
            switch (binary->operation) {
            case BinaryOperator::equal:
            case BinaryOperator::not_equal:
            case BinaryOperator::less:
            case BinaryOperator::less_equal:
            case BinaryOperator::greater:
            case BinaryOperator::greater_equal:
            case BinaryOperator::logical_and:
            case BinaryOperator::logical_or:
                return simple(SemanticTypeKind::boolean);
            case BinaryOperator::coalesce:
                return left;
            default:
                return simple(SemanticTypeKind::number);
            }
        }
        if (const auto* conditional = std::get_if<ConditionalExpression>(&expression.node)) {
            static_cast<void>(infer(*conditional->condition, scope, component_path));
            SemanticTypePtr then_type = infer(*conditional->then_expression, scope, component_path);
            static_cast<void>(infer(*conditional->else_expression, scope, component_path));
            return then_type;
        }
        if (const auto* property = std::get_if<PropertyAccessExpression>(&expression.node)) {
            SemanticTypePtr receiver = infer(*property->receiver, scope, component_path);
            if (receiver->kind == SemanticTypeKind::async_value &&
                property->property_name == "value") {
                report("STRATA.DSL.SEMANTIC_ASYNC_VALUE_ACCESS",
                       "An async value is available only inside its READY when branch.",
                       expression.span, component_path, "READY branch");
            }
            if (const ObjectField* field = receiver->find_field(property->property_name);
                field != nullptr) {
                return field->type;
            }
            return simple(SemanticTypeKind::unknown);
        }
        if (const auto* indexed = std::get_if<IndexExpression>(&expression.node)) {
            SemanticTypePtr receiver = infer(*indexed->receiver, scope, component_path);
            static_cast<void>(infer(*indexed->index, scope, component_path));
            if (receiver->kind == SemanticTypeKind::async_value) {
                if (const std::string* field = string_literal_value(*indexed->index);
                    field != nullptr && *field == "value") {
                    report("STRATA.DSL.SEMANTIC_ASYNC_VALUE_ACCESS",
                           "An async value is available only inside its READY when branch.",
                           expression.span, component_path, "READY branch");
                }
            }
            return receiver->element != nullptr ? receiver->element
                                                : simple(SemanticTypeKind::unknown);
        }
        if (const auto* call = std::get_if<CallExpression>(&expression.node)) {
            const std::string name = call->target.qualified_name();
            if (name == "action") {
                validate_action_call(*call, scope, component_path);
                return simple(SemanticTypeKind::action);
            }
            if (name == "material") {
                validate_expected(expression, *simple(SemanticTypeKind::material), false,
                                  component_path, scope);
                return simple(SemanticTypeKind::material);
            }
            if (name == "style" || name == "effect" || name == "animation") {
                const SemanticTypeKind kind = name == "style"    ? SemanticTypeKind::style
                                              : name == "effect" ? SemanticTypeKind::effect
                                                                 : SemanticTypeKind::animation;
                validate_expected(expression, *simple(kind), false, component_path, scope);
                return simple(kind);
            }
            if (const auto component = components_.find(name);
                component != components_.end()) {
                const std::set<std::string, std::less<>> bound =
                    validate_component_template_arguments(
                        *call,
                        name,
                        component->second,
                        nullptr,
                        scope,
                        component_path
                    );
                return component_template_type(component->second, bound);
            }
            const HelperSchema* helper = registry_.helper(name);
            if (helper == nullptr) {
                report("STRATA.DSL.SEMANTIC_UNKNOWN_HELPER",
                       "Helper function '" + name + "' is not registered as a pure DSL helper.",
                       call->target.span, component_path, "registered helper");
                for (const Argument& argument : call->arguments) {
                    static_cast<void>(infer(*argument.value, scope, component_path));
                }
                return simple(SemanticTypeKind::unknown);
            }
            validate_helper_arguments(*call, *helper, scope, component_path);
            const auto argument_for =
                [call, helper](const std::string_view requested) -> const Argument* {
                std::size_t positional_index = 0U;
                for (const Argument& argument : call->arguments) {
                    const SchemaParameter* parameter = nullptr;
                    if (argument.name.has_value()) {
                        const auto found = std::ranges::find_if(
                            helper->parameters, [&argument](const SchemaParameter& candidate) {
                                return candidate.accepts_name(*argument.name);
                            });
                        if (found != helper->parameters.end())
                            parameter = &*found;
                    } else if (positional_index < helper->parameters.size()) {
                        parameter = &helper->parameters[positional_index++];
                    }
                    if (parameter != nullptr && parameter->name == requested)
                        return &argument;
                }
                return nullptr;
            };
            const std::string& implementation = helper->implementation;
            if (implementation == "persisted") {
                if (persisted_owner_ != &expression) {
                    report("STRATA.DSL.SEMANTIC_PERSISTED_CONTEXT",
                           "persisted() is valid only as the direct initializer of a state "
                           "declaration.",
                           expression.span, component_path,
                           "state value: type = persisted(\"stable.key\", initial)");
                }
                const Argument* key_argument = argument_for("key");
                const std::string* key =
                    key_argument != nullptr ? string_literal_value(*key_argument->value) : nullptr;
                if (key == nullptr || key->empty()) {
                    report("STRATA.DSL.SEMANTIC_PERSISTENCE_KEY_STATIC",
                           "persisted() requires a non-empty string-literal key.",
                           key_argument != nullptr ? key_argument->value->span : expression.span,
                           component_path, "persisted(\"stable.key\", initial)");
                } else {
                    const auto [found, inserted] =
                        persistence_keys_.try_emplace(*key, component_path);
                    if (!inserted && found->second != component_path) {
                        report("STRATA.DSL.SEMANTIC_DUPLICATE_PERSISTENCE_KEY",
                               "Persistence key '" + *key + "' is already used by '" +
                                   found->second + "'.",
                               key_argument->value->span, component_path, "unique persistence key");
                    }
                }
                const Argument* initial = argument_for("initial");
                return initial != nullptr
                           ? infer(*initial->value, scope, component_path + ".initial")
                           : helper->return_type;
            }
            const bool preserves_source =
                implementation == "filter" || implementation == "sort_by" ||
                implementation == "distinct_by" || implementation == "take_while" ||
                implementation == "window" || implementation == "page";
            if (implementation == "map" || preserves_source) {
                const Argument* source_argument = argument_for("source");
                if (source_argument == nullptr)
                    return helper->return_type;
                SemanticTypePtr source =
                    infer(*source_argument->value, scope, component_path + ".source");
                auto projected = std::make_shared<SemanticType>();
                projected->kind = SemanticTypeKind::collection;
                projected->element =
                    source->element != nullptr ? source->element : simple(SemanticTypeKind::any);
                projected->maximum_items = source->maximum_items;
                if (implementation == "map") {
                    const Argument* transform = argument_for("transform");
                    const auto* lambda =
                        transform != nullptr
                            ? std::get_if<LambdaExpression>(&ungrouped(*transform->value).node)
                            : nullptr;
                    if (lambda != nullptr) {
                        Scope lambda_scope = scope;
                        lambda_scope.insert_or_assign(lambda->parameter_name, projected->element);
                        projected->element =
                            infer(*lambda->body, lambda_scope, component_path + ".transform");
                    }
                }
                const std::string_view bound_name =
                    implementation == "window" ? std::string_view("limit")
                    : implementation == "page" ? std::string_view("size")
                                               : std::string_view{};
                if (!bound_name.empty()) {
                    const Argument* bound = argument_for(bound_name);
                    const std::optional<StaticNumber> number =
                        bound != nullptr ? static_number(*bound->value) : std::nullopt;
                    if (number.has_value() && !number->unit.has_value() && number->value >= 0.0) {
                        const double floored = std::floor(number->value);
                        const std::size_t maximum =
                            floored >= static_cast<double>(std::numeric_limits<std::size_t>::max())
                                ? std::numeric_limits<std::size_t>::max()
                                : static_cast<std::size_t>(floored);
                        projected->maximum_items = projected->maximum_items.has_value()
                                                       ? std::optional<std::size_t>(std::min(
                                                             *projected->maximum_items, maximum))
                                                       : std::optional<std::size_t>(maximum);
                    }
                }
                return projected;
            }
            return helper->return_type;
        }
        if (std::holds_alternative<LambdaExpression>(expression.node))
            return simple(SemanticTypeKind::lambda);
        return simple(SemanticTypeKind::unknown);
    }

    const File& file_;
    const SchemaRegistry& registry_;
    std::unordered_map<std::string, ComponentSchema> components_;
    std::vector<std::string> styles_;
    std::map<std::string, const StyleDeclaration*, std::less<>> style_declarations_;
    std::vector<std::string> animations_;
    std::vector<Diagnostic> diagnostics_;
    std::vector<Diagnostic> lowering_diagnostics_;
    std::map<std::string, ValidatedAnimation, std::less<>> validated_animations_;
    std::map<std::string, std::string, std::less<>> persistence_keys_;
    std::set<const Expression*> diagnosed_layout_properties_;
    std::vector<
        const std::map<std::string, const Expression*, std::less<>>*
    > active_widget_default_styles_;
    const Expression* persisted_owner_ = nullptr;
};

} // namespace

SemanticResult validate_semantics(const File& file, const SchemaRegistry& registry) {
    return Validator(file, registry).run();
}

} // namespace strata::compiler
