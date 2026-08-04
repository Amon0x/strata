#include "compiler/portable_ir.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>

namespace strata::compiler {
namespace {

using JsonValue = data::JsonView;
using JsonArray = data::JsonArrayView;
using JsonObject = data::JsonObjectView;

[[nodiscard]] std::string operator+(std::string left, const std::string_view right) {
    left.append(right);
    return left;
}

[[nodiscard]] std::string operator+(const char* const left, const std::string_view right) {
    std::string result(left);
    result.append(right);
    return result;
}

[[nodiscard]] JsonValue required(const JsonValue value, const std::string_view field) {
    const JsonValue child_value = value.find(field);
    if (!child_value) throw std::runtime_error("portable IR is missing field '" + std::string(field) + "'");
    return child_value;
}

class PortableIrValidator final {
public:
    explicit PortableIrValidator(const JsonValue unit) : unit_(unit) {}

    void run(const bool validate_generic_structure = true) {
        if (validate_generic_structure) walk(unit_, 0U);
        require_object(unit_, "unit");
        require_string(unit_, "sourceId", "unit");
        validate_named_declarations("screens", "screen", true);
        validate_named_declarations("overlays", "overlay", true);
        validate_named_declarations("components", "component", true);
        validate_styles();
        validate_animations();
        validate_materials();
        validate_action_references();
    }

private:
    static constexpr std::size_t maximum_depth = 256U;
    static constexpr std::size_t maximum_values = 1'000'000U;
    static constexpr std::size_t maximum_array_items = 100'000U;

    [[noreturn]] static void fail(const std::string_view context, const std::string& message) {
        throw std::runtime_error("portable IR " + std::string(context) + " " + message);
    }

    void walk(const JsonValue value, const std::size_t depth) {
        if (depth > maximum_depth) fail("document", "exceeds the maximum nesting depth");
        if (++value_count_ > maximum_values) fail("document", "exceeds the maximum value count");
        if (const std::optional<JsonArray> array_value = value.array(); array_value.has_value()) {
            if (array_value->size() > maximum_array_items) fail("array", "exceeds the maximum item count");
            for (const JsonValue child_value : *array_value) walk(child_value, depth + 1U);
        } else if (const std::optional<JsonObject> object_value = value.object(); object_value.has_value()) {
            // Portable IR objects are overwhelmingly small fixed-schema records. A linear check
            // avoids allocating a tree node for every field while retaining bounded behavior for
            // unusually wide or hostile input objects.
            constexpr std::size_t linear_duplicate_check_limit = 16U;
            if (object_value->size() <= linear_duplicate_check_limit) {
                for (std::size_t index = 0U; index < object_value->size(); ++index) {
                    const std::string_view name = (*object_value)[index].first;
                    for (std::size_t previous = 0U; previous < index; ++previous) {
                        if ((*object_value)[previous].first == name) {
                            fail("object", "contains duplicate field '" + name + "'");
                        }
                    }
                    walk((*object_value)[index].second, depth + 1U);
                }
            } else {
                std::set<std::string_view> names;
                for (const auto& [name, child_value] : *object_value) {
                    if (!names.insert(name).second) {
                        fail("object", "contains duplicate field '" + name + "'");
                    }
                    walk(child_value, depth + 1U);
                }
            }
        }
    }

    static void require_object(const JsonValue value, const std::string_view context) {
        if (!value.object().has_value()) fail(context, "must be an object");
    }

    static JsonArray require_array(
        const JsonValue value,
        const std::string_view field,
        const std::string_view context
    ) {
        const JsonValue child_value = required(value, field);
        const std::optional<JsonArray> result = child_value.array();
        if (!result.has_value()) fail(context, "field '" + std::string(field) + "' must be an array");
        return *result;
    }

    static std::string_view require_string(
        const JsonValue value,
        const std::string_view field,
        const std::string_view context,
        const bool allow_empty = false
    ) {
        const std::optional<std::string_view> child_value = required(value, field).string();
        if (!child_value.has_value() || (!allow_empty && child_value->empty())) {
            fail(context, "field '" + std::string(field) + "' must be a non-empty string");
        }
        return *child_value;
    }

    static void require_boolean(
        const JsonValue value,
        const std::string_view field,
        const std::string_view context
    ) {
        if (!required(value, field).boolean().has_value()) {
            fail(context, "field '" + std::string(field) + "' must be boolean");
        }
    }

    static void validate_position(const JsonValue value, const std::string_view context) {
        require_object(value, context);
        for (const std::string_view field : {"column", "line", "offset"}) {
            const std::optional<std::int64_t> number = required(value, field).integer();
            if (!number.has_value() || *number < 0) {
                fail(context, "field '" + std::string(field) + "' must be a non-negative integer");
            }
        }
    }

    static void validate_span(const JsonValue owner, const std::string_view context) {
        const JsonValue value = required(owner, "span");
        require_object(value, context);
        require_string(value, "sourceId", context);
        const std::optional<std::int64_t> length = required(value, "length").integer();
        if (!length.has_value() || *length < 0) fail(context, "span length must be a non-negative integer");
        validate_position(required(value, "start"), context);
        validate_position(required(value, "end"), context);
    }

    static std::string_view validate_path(const JsonValue value, const std::string_view context) {
        const std::string_view path = require_string(value, "path", context);
        if (!path.starts_with('/')) fail(context, "path must begin with '/'");
        return path;
    }

    void validate_named_declarations(
        const std::string_view field,
        const std::string_view role,
        const bool has_body
    ) {
        std::set<std::string_view> names;
        for (const JsonValue declaration : require_array(unit_, field, "unit")) {
            require_object(declaration, role);
            const std::string_view name = require_string(declaration, "name", role);
            if (!names.insert(name).second) fail(role, "name '" + name + "' is duplicated");
            validate_path(declaration, role);
            validate_span(declaration, role);
            if (has_body) validate_block(
                required(declaration, "body"),
                std::string(role) + " " + std::string(name)
            );
            if (role == "component") {
                for (const JsonValue parameter : require_array(declaration, "parameters", role)) {
                    require_object(parameter, "component parameter");
                    validate_span(parameter, "component parameter");
                    const JsonValue default_value = required(parameter, "default");
                    if (!default_value.is_null()) validate_expression(default_value, "component parameter default");
                    require_object(required(parameter, "schema"), "component parameter schema");
                }
                static_cast<void>(require_array(declaration, "widgetDefaults", role));
            }
        }
    }

    void validate_block(
        const JsonValue block,
        const std::string& context,
        const bool repeater_body = false
    ) {
        require_object(block, context);
        validate_path(block, context);
        validate_span(block, context);
        for (const JsonValue statement : require_array(block, "statements", context)) {
            validate_statement(statement, context, repeater_body);
        }
    }

    void validate_statement(
        const JsonValue statement,
        const std::string& context,
        const bool repeater_body
    ) {
        require_object(statement, context);
        const std::string_view kind = require_string(statement, "kind", context);
        validate_path(statement, context);
        validate_span(statement, context);
        if (kind == "state") {
            require_string(statement, "name", context);
            const JsonValue declared = required(statement, "declaredType");
            if (!declared.is_null() &&
                (!declared.string().has_value() || declared.string()->empty())) {
                fail(context, "state declaredType must be a non-empty string or null");
            }
            const JsonValue initializer = required(statement, "initializer");
            if (!initializer.is_null()) validate_expression(initializer, context + "/state initializer");
        } else if (kind == "derived") {
            require_string(statement, "name", context);
            validate_expression(required(statement, "expression"), context + "/derived expression");
        } else if (kind == "node") {
            require_boolean(statement, "root", context);
            validate_call(required(statement, "call"), context + "/node");
        } else if (kind == "if") {
            validate_expression(required(statement, "condition"), context + "/if condition");
            validate_block(required(statement, "then"), context + "/if then");
            const JsonValue otherwise = required(statement, "else");
            if (!otherwise.is_null()) validate_block(otherwise, context + "/if else");
        } else if (kind == "when") {
            validate_expression(required(statement, "subject"), context + "/when subject");
            for (const JsonValue branch : require_array(statement, "branches", context)) {
                require_object(branch, "when branch");
                validate_span(branch, "when branch");
                const JsonValue match = required(branch, "match");
                if (!match.is_null()) validate_expression(match, "when branch match");
                validate_block(required(branch, "block"), context + "/when branch");
            }
        } else if (kind == "for") {
            require_string(statement, "itemName", context);
            const JsonValue index_name = required(statement, "indexName");
            if (!index_name.is_null() && !index_name.string().has_value()) fail(context, "for indexName must be string or null");
            validate_expression(required(statement, "collection"), context + "/for collection");
            const JsonValue filter = required(statement, "filter");
            if (!filter.is_null()) validate_expression(filter, context + "/for filter");
            if (repeater_body) {
                const JsonValue identity = required(statement, "identity");
                if (identity.is_null()) {
                    fail(context, "Repeater for identity extractor must not be null");
                }
                validate_repeater_identity(identity, context + "/for identity");
            }
            validate_block(required(statement, "block"), context + "/for body");
        } else {
            fail(context, "contains unknown statement kind '" + kind + "'");
        }
    }

    void validate_repeater_identity(const JsonValue identity, const std::string& context) {
        require_object(identity, context);
        const std::string_view kind = require_string(identity, "kind", context);
        if (kind == "block") {
            const JsonArray statements = require_array(identity, "statements", context);
            if (statements.size() != 1U) {
                fail(context, "must select exactly one keyed root");
            }
            validate_repeater_identity(statements.front(), context + "/statement");
        } else if (kind == "key") {
            validate_expression(required(identity, "expression"), context + "/key");
        } else if (kind == "if") {
            validate_expression(required(identity, "condition"), context + "/condition");
            validate_repeater_identity(required(identity, "then"), context + "/then");
            validate_repeater_identity(required(identity, "else"), context + "/else");
        } else if (kind == "when") {
            validate_expression(required(identity, "subject"), context + "/subject");
            const JsonArray branches = require_array(identity, "branches", context);
            if (branches.empty()) fail(context, "when identity must contain branches");
            for (std::size_t index = 0U; index < branches.size(); ++index) {
                const JsonValue branch = branches[index];
                require_object(branch, context + "/branch");
                const JsonValue match = required(branch, "match");
                if (match.is_null() != (index + 1U == branches.size())) {
                    fail(context, "when identity must end with exactly one fallback branch");
                }
                if (!match.is_null()) validate_expression(match, context + "/branch match");
                validate_repeater_identity(
                    required(branch, "identity"),
                    context + "/branch identity"
                );
            }
        } else {
            fail(context, "contains unknown Repeater identity kind '" + kind + "'");
        }
    }

    void validate_call(const JsonValue call, const std::string& context) {
        require_object(call, context);
        const std::string_view kind = require_string(call, "kind", context);
        if (kind != "widget" && kind != "component") fail(context, "call kind must be widget or component");
        require_string(call, "name", context);
        validate_path(call, context);
        validate_span(call, context);
        const JsonValue arguments = required(call, "arguments");
        require_object(arguments, context + "/arguments");
        const JsonObject object = *arguments.object();
        for (const auto& [name, expression] : object) {
            if (name.empty()) fail(context, "call argument names must not be empty");
            validate_expression(expression, context + "/argument " + name);
        }
        const JsonValue children = required(call, "children");
        const bool repeater = require_string(call, "name", context) == "Repeater";
        if (children.is_null()) {
            if (repeater) fail(context, "Repeater children must contain one keyed for statement");
            return;
        }
        validate_block(children, context + "/children", repeater);
        if (repeater) {
            const JsonArray statements = require_array(
                children,
                "statements",
                context + "/children"
            );
            if (statements.size() != 1U ||
                require_string(statements.front(), "kind", context + "/children") != "for") {
                fail(context, "Repeater children must contain exactly one keyed for statement");
            }
        }
    }

    void validate_expression(const JsonValue expression, const std::string& context) {
        require_object(expression, context);
        const std::string_view kind = require_string(expression, "kind", context);
        validate_path(expression, context);
        validate_span(expression, context);
        if (kind == "literal") {
            validate_literal(required(expression, "value"), context + "/literal");
        } else if (kind == "variable") {
            require_string(expression, "binding", context);
            require_string(expression, "name", context);
            require_string(expression, "type", context);
        } else if (kind == "list") {
            for (const JsonValue item : require_array(expression, "elements", context)) {
                validate_expression(item, context + "/list item");
            }
        } else if (kind == "map") {
            const JsonValue entries = required(expression, "entries");
            require_object(entries, context + "/map entries");
            const JsonObject object = *entries.object();
            for (const auto& [name, value] : object) {
                if (name.empty()) fail(context, "map entry names must not be empty");
                validate_expression(value, context + "/map entry " + name);
            }
        } else if (kind == "unary") {
            const std::string_view operation = require_string(expression, "operator", context);
            if (operation != "negate" && operation != "not") fail(context, "has unknown unary operator '" + operation + "'");
            validate_expression(required(expression, "operand"), context + "/operand");
        } else if (kind == "binary") {
            static const std::set<std::string_view> operations = {
                "add", "subtract", "multiply", "divide", "modulo", "equal", "not_equal",
                "less", "less_equal", "greater", "greater_equal", "and", "or", "coalesce",
            };
            const std::string_view operation = require_string(expression, "operator", context);
            if (!operations.contains(operation)) fail(context, "has unknown binary operator '" + operation + "'");
            validate_expression(required(expression, "left"), context + "/left");
            validate_expression(required(expression, "right"), context + "/right");
        } else if (kind == "conditional") {
            validate_expression(required(expression, "condition"), context + "/condition");
            validate_expression(required(expression, "then"), context + "/then");
            validate_expression(required(expression, "else"), context + "/else");
        } else if (kind == "property") {
            require_string(expression, "name", context);
            validate_expression(required(expression, "receiver"), context + "/receiver");
        } else if (kind == "index") {
            validate_expression(required(expression, "receiver"), context + "/receiver");
            validate_expression(required(expression, "index"), context + "/index");
        } else if (kind == "helper") {
            require_string(expression, "name", context);
            validate_expression_arguments(expression, context);
        } else if (kind == "action") {
            require_string(expression, "id", context);
            validate_expression_arguments(expression, context);
        } else if (kind == "componentTemplate") {
            require_string(expression, "component", context);
            validate_expression_arguments(expression, context);
        } else if (kind == "lambda") {
            require_string(expression, "parameter", context);
            validate_expression(required(expression, "body"), context + "/lambda body");
        } else if (kind == "materialCall") {
            require_string(expression, "id", context);
            std::set<std::string_view> names;
            for (const JsonValue parameter : require_array(expression, "parameters", context)) {
                require_object(parameter, context + "/material parameter");
                const std::string_view name = require_string(
                    parameter,
                    "name",
                    context + "/material parameter"
                );
                if (name.empty() || !names.insert(name).second) {
                    fail(context, "contains an empty or duplicate material parameter");
                }
                validate_span(parameter, context + "/material parameter");
                validate_expression(
                    required(parameter, "value"),
                    context + "/material parameter value"
                );
            }
        } else if (kind == "materialReference") {
            require_string(expression, "id", context);
        } else {
            fail(context, "contains unknown expression kind '" + kind + "'");
        }
    }

    void validate_expression_arguments(const JsonValue expression, const std::string& context) {
        for (const JsonValue argument : require_array(expression, "arguments", context)) {
            require_object(argument, context + "/argument");
            const JsonValue name = required(argument, "name");
            if (!name.is_null() && !name.string().has_value()) fail(context, "argument name must be string or null");
            validate_span(argument, context + "/argument");
            validate_expression(required(argument, "value"), context + "/argument value");
        }
    }

    static void validate_literal(const JsonValue literal, const std::string& context) {
        require_object(literal, context);
        const std::string_view kind = require_string(literal, "kind", context);
        if (kind == "null") return;
        if (kind == "boolean") {
            require_boolean(literal, "value", context);
        } else if (kind == "number") {
            if (!required(literal, "value").integer().has_value() && !required(literal, "value").number().has_value()) {
                fail(context, "number literal value must be numeric");
            }
        } else if (kind == "duration") {
            if (!required(literal, "nanos").integer().has_value()) fail(context, "duration nanos must be an integer");
        } else if (kind == "string" || kind == "image" || kind == "key") {
            require_string(literal, "value", context, kind == "string");
        } else if (kind == "color") {
            const std::string_view rgba = require_string(literal, "rgba", context);
            if (rgba.size() != 8U || !std::ranges::all_of(rgba, [](const unsigned char character) {
                    return std::isxdigit(character) != 0;
                })) {
                fail(context, "color rgba must contain eight hexadecimal digits");
            }
        } else if (kind == "themeToken") {
            require_string(literal, "name", context);
        } else if (kind == "styleReference" || kind == "animation") {
            require_string(literal, "name", context);
        } else {
            fail(context, "contains unknown literal kind '" + kind + "'");
        }
    }

    void validate_styles() {
        std::set<std::string_view> names;
        for (const JsonValue style : require_array(unit_, "styles", "unit")) {
            require_object(style, "style");
            const std::string_view name = require_string(style, "name", "style");
            if (!names.insert(name).second) fail("style", "name '" + name + "' is duplicated");
            validate_path(style, "style");
            validate_span(style, "style");
            for (const JsonValue base : require_array(style, "bases", "style")) {
                if (!base.string().has_value() || base.string()->empty()) fail("style", "base names must be non-empty strings");
            }
            const JsonValue properties = required(style, "properties");
            require_object(properties, "style properties");
            const JsonObject object = *properties.object();
            for (const auto& [property, expression] : object) {
                validate_expression(expression, "style " + name + "." + property);
            }
        }
    }

    void validate_animations() {
        std::set<std::string_view> names;
        for (const JsonValue declaration : require_array(unit_, "animations", "unit")) {
            require_object(declaration, "animation");
            const std::string_view name = require_string(declaration, "name", "animation");
            if (!names.insert(name).second) fail("animation", "name '" + name + "' is duplicated");
            require_object(required(declaration, "animation"), "animation payload");
        }
    }

    void validate_materials() {
        std::set<std::string_view> ids;
        for (const JsonValue material : require_array(unit_, "materials", "unit")) {
            require_object(material, "material");
            const std::string_view id = require_string(material, "id", "material");
            if (!ids.insert(id).second) fail("material", "id '" + id + "' is duplicated");
            require_string(material, "name", "material");
            static_cast<void>(require_array(material, "parameters", "material"));
        }
    }

    void validate_action_references() {
        for (const JsonValue reference : require_array(unit_, "actionReferences", "unit")) {
            require_object(reference, "action reference");
            require_string(reference, "id", "action reference");
            require_string(reference, "componentPath", "action reference");
            require_string(reference, "payloadContract", "action reference");
            const std::string_view policy = require_string(reference, "dispatchPolicy", "action reference");
            if (policy != "required" && policy != "optional" && policy != "broadcast" &&
                policy != "forwarded" && policy != "framework") {
                fail("action reference", "has unknown dispatch policy '" + policy + "'");
            }
            const JsonValue range_value = required(reference, "range");
            require_object(range_value, "action reference range");
            require_string(range_value, "sourceId", "action reference range");
            validate_position(required(range_value, "start"), "action reference range");
            validate_position(required(range_value, "end"), "action reference range");
        }
    }

    const JsonValue unit_;
    std::size_t value_count_ = 0U;
};

} // namespace

void validate_portable_ir(const data::JsonValue& unit) {
    PortableIrValidator(data::JsonView(unit)).run();
}

void validate_portable_ir(const data::JsonView unit) {
    PortableIrValidator(unit).run();
}

void validate_portable_ir(const data::FrozenJsonDocument& unit) {
    PortableIrValidator(unit.root()).run(false);
}

} // namespace strata::compiler
