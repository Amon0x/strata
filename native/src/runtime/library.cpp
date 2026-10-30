#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "compiler/builtin_catalog.hpp"
#include "data/json.hpp"
#include "runtime/expression.hpp"

// StrataScript's library: helpers computed from their evaluated arguments, by name. Each is
// declared in the built-in catalog, which names and orders its parameters.
namespace strata::runtime {
namespace {

using Fields = std::vector<std::pair<std::string, Value>>;

[[nodiscard]] const std::vector<compiler::DeclaredParameter>* library_parameters(
    const std::string_view name) {
    static const std::map<std::string, const compiler::DeclaredHelper*, std::less<>> helpers = [] {
        std::map<std::string, const compiler::DeclaredHelper*, std::less<>> result;
        for (const compiler::DeclaredHelper& helper : compiler::builtin_catalog().helpers) {
            if (helper.evaluator == compiler::HelperEvaluator::library)
                result.emplace(helper.name, &helper);
        }
        return result;
    }();
    const auto found = helpers.find(name);
    return found != helpers.end() ? &found->second->parameters : nullptr;
}

[[nodiscard]] const Value& data(const ExpressionValue& value) {
    static const Value null;
    const Value* result = value.data_value();
    return result != nullptr ? *result : null;
}

[[nodiscard]] double number(const Value& value, const double fallback = 0.0) {
    return value.number() != nullptr ? *value.number() : fallback;
}

[[nodiscard]] Value finite(const double value) {
    return Value(std::isfinite(value) ? value : 0.0);
}

/** The order `sort` puts values in without a comparison: by kind, then by value. */
[[nodiscard]] bool naturally_before(const Value& left, const Value& right) {
    if (left.number() != nullptr && right.number() != nullptr)
        return *left.number() < *right.number();
    if (left.string() != nullptr && right.string() != nullptr)
        return *left.string() < *right.string();
    if (left.kind() != right.kind())
        return left.kind() < right.kind();
    return display_string(left) < display_string(right);
}

[[nodiscard]] std::string type_name(const ExpressionValue& value) {
    if (value.lambda() != nullptr)
        return "function";
    if (value.action() != nullptr)
        return "action";
    const Value& plain = data(value);
    switch (plain.kind()) {
    case ValueKind::null_value: return "null";
    case ValueKind::boolean: return "boolean";
    case ValueKind::number: return "number";
    case ValueKind::duration: return "duration";
    case ValueKind::string: return "string";
    case ValueKind::color: return "color";
    case ValueKind::image: return "image";
    case ValueKind::key: return "key";
    case ValueKind::list: return "list";
    case ValueKind::object: return "record";
    default: return "value";
    }
}

} // namespace

ExpressionValue ExpressionRuntime::evaluate_library(const Program& program,
                                                    const ProgramExpression& node,
                                                    const ExpressionScope& scope,
                                                    DependencyRecorder& reads) {
    const std::string& name = node.text;
    const std::vector<compiler::DeclaredParameter>* parameters = library_parameters(name);
    if (parameters == nullptr) {
        report(node, scope, "STRATA.DSL.RUNTIME_UNKNOWN_HELPER",
               "Helper '" + name + "' is not part of the library.");
        return ExpressionValue{};
    }
    std::vector<ExpressionValue> arguments;
    arguments.reserve(parameters->size());
    for (std::size_t index = 0U; index < parameters->size(); ++index) {
        const ProgramArgument* found =
            argument(program, node, Symbol::intern((*parameters)[index].name), index);
        arguments.push_back(found != nullptr ? evaluate_node(program, found->value, scope, reads)
                                             : ExpressionValue{});
    }
    const auto value = [&arguments](const std::size_t index) -> const Value& {
        return data(arguments[index]);
    };
    const auto fail = [&](const std::string& message) -> ExpressionValue {
        report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH", name + ": " + message);
        return ExpressionValue{};
    };
    const auto items = [&](const std::size_t index) -> const ValueList* {
        return value(index).list();
    };
    const auto call = [&](const std::size_t index,
                          std::vector<ExpressionValue> given) -> ExpressionValue {
        const auto* function = arguments[index].lambda();
        if (function == nullptr || *function == nullptr) {
            report(node, scope, "STRATA.DSL.RUNTIME_TYPE_MISMATCH",
                   name + ": argument '" + (*parameters)[index].name + "' must be a function.");
            return ExpressionValue{};
        }
        return invoke(**function, std::move(given), {}, reads);
    };
    const auto position = [](const Value& index, const std::size_t size,
                             const bool end_allowed) -> std::optional<std::size_t> {
        const std::optional<std::size_t> bounded = bounded_index(index);
        if (!bounded.has_value() || *bounded > size || (!end_allowed && *bounded == size))
            return std::nullopt;
        return bounded;
    };

    // Collections.
    if (name == "reduce") {
        const ValueList* list = items(0U);
        if (list == nullptr)
            return fail("needs a list.");
        ExpressionValue accumulated = arguments[2];
        for (std::size_t index = 0U; index < list->values.size(); ++index) {
            accumulated = call(1U, {accumulated, ExpressionValue(list->values[index]),
                                    ExpressionValue(Value(static_cast<double>(index)))});
        }
        return accumulated;
    }
    if (name == "findIndex") {
        const ValueList* list = items(0U);
        if (list == nullptr)
            return fail("needs a list.");
        for (std::size_t index = 0U; index < list->values.size(); ++index) {
            if (truthy(data(call(1U, {ExpressionValue(list->values[index]),
                                      ExpressionValue(Value(static_cast<double>(index)))})))) {
                return ExpressionValue(Value(static_cast<double>(index)));
            }
        }
        return ExpressionValue(Value(-1.0));
    }
    if (name == "keys" || name == "values" || name == "entries") {
        const ValueObject* record = value(0U).object();
        if (record == nullptr)
            return fail("needs a record.");
        std::vector<Value> result;
        result.reserve(record->fields.size());
        for (const auto& [key, field] : record->fields) {
            if (name == "keys")
                result.emplace_back(key);
            else if (name == "values")
                result.push_back(field);
            else
                result.emplace_back(Fields{{"key", Value(key)}, {"value", field}});
        }
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "fromEntries") {
        const ValueList* list = items(0U);
        if (list == nullptr)
            return fail("needs a list of { key, value } records or [key, value] pairs.");
        Fields result;
        for (const Value& entry : list->values) {
            const Value* key = entry.field("key");
            const Value* field = entry.field("value");
            if (key == nullptr && entry.list() != nullptr && entry.list()->values.size() == 2U) {
                key = &entry.list()->values[0];
                field = &entry.list()->values[1];
            }
            if (key == nullptr)
                return fail("an entry has no key.");
            const std::string text = key->string() != nullptr ? *key->string()
                                                              : display_string(*key);
            const auto existing = std::ranges::find(result, text, &Fields::value_type::first);
            if (existing != result.end())
                existing->second = field != nullptr ? *field : Value{};
            else
                result.emplace_back(text, field != nullptr ? *field : Value{});
        }
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "has" || name == "without") {
        const ValueObject* record = value(0U).object();
        const std::string key = value(1U).string() != nullptr ? *value(1U).string()
                                                              : display_string(value(1U));
        if (record == nullptr) {
            if (name == "has")
                return ExpressionValue(Value(false));
            return fail("needs a record.");
        }
        const auto found = std::ranges::find(record->fields, key, &Fields::value_type::first);
        if (name == "has")
            return ExpressionValue(Value(found != record->fields.end()));
        Fields result = record->fields;
        std::erase_if(result, [&key](const auto& field) { return field.first == key; });
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "insertAt" || name == "removeAt" || name == "replaceAt") {
        const ValueList* list = items(0U);
        if (list == nullptr)
            return fail("needs a list.");
        const std::optional<std::size_t> at =
            position(value(1U), list->values.size(), name == "insertAt");
        if (!at.has_value()) {
            return fail("index " + display_string(value(1U)) + " is outside a list of " +
                        std::to_string(list->values.size()) + ".");
        }
        std::vector<Value> result = list->values;
        const auto where = result.begin() + static_cast<std::ptrdiff_t>(*at);
        if (name == "insertAt")
            result.insert(where, value(2U));
        else if (name == "removeAt")
            result.erase(where);
        else
            *where = value(2U);
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "unique") {
        const ValueList* list = items(0U);
        if (list == nullptr)
            return fail("needs a list.");
        std::vector<Value> result;
        for (const Value& item : list->values) {
            if (std::ranges::find(result, item) == result.end())
                result.push_back(item);
        }
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "sort") {
        const ValueList* list = items(0U);
        if (list == nullptr)
            return fail("needs a list.");
        std::vector<Value> result = list->values;
        if (arguments[1].lambda() == nullptr) {
            std::ranges::stable_sort(result, naturally_before);
        } else {
            std::ranges::stable_sort(result, [&](const Value& left, const Value& right) {
                return number(data(call(1U, {ExpressionValue(left), ExpressionValue(right)}))) <
                       0.0;
            });
        }
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "zip") {
        const ValueList* first = items(0U);
        const ValueList* second = items(1U);
        if (first == nullptr || second == nullptr)
            return fail("needs two lists.");
        std::vector<Value> result;
        const std::size_t size = std::min(first->values.size(), second->values.size());
        result.reserve(size);
        for (std::size_t index = 0U; index < size; ++index)
            result.emplace_back(std::vector<Value>{first->values[index], second->values[index]});
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "chunk") {
        const ValueList* list = items(0U);
        const double size = std::floor(number(value(1U)));
        if (list == nullptr || size < 1.0)
            return fail("needs a list and a size of at least one.");
        const auto step = static_cast<std::size_t>(size);
        std::vector<Value> result;
        for (std::size_t begin = 0U; begin < list->values.size(); begin += step) {
            const std::size_t end = std::min(list->values.size(), begin + step);
            result.emplace_back(std::vector<Value>(
                list->values.begin() + static_cast<std::ptrdiff_t>(begin),
                list->values.begin() + static_cast<std::ptrdiff_t>(end)));
        }
        return ExpressionValue(Value(std::move(result)));
    }

    // A handler's writes as one undo step.
    if (name == "undoStep") {
        std::optional<std::string> coalesce;
        if (value(1U).kind() != ValueKind::null_value)
            coalesce = display_string(value(1U));
        if (!name_undo_step(display_string(value(0U)), std::move(coalesce)))
            return fail("names a handler's undo step, so only a handler can call it.");
        return ExpressionValue{};
    }

    // Numbers.
    if (name == "sin") return ExpressionValue(finite(std::sin(number(value(0U)))));
    if (name == "cos") return ExpressionValue(finite(std::cos(number(value(0U)))));
    if (name == "tan") return ExpressionValue(finite(std::tan(number(value(0U)))));
    if (name == "atan2")
        return ExpressionValue(finite(std::atan2(number(value(0U)), number(value(1U)))));
    if (name == "hypot")
        return ExpressionValue(finite(std::hypot(number(value(0U)), number(value(1U)))));
    if (name == "sign") {
        const double input = number(value(0U));
        return ExpressionValue(Value(input > 0.0 ? 1.0 : input < 0.0 ? -1.0 : 0.0));
    }
    if (name == "trunc") return ExpressionValue(finite(std::trunc(number(value(0U)))));
    if (name == "log") {
        const double input = number(value(0U));
        return ExpressionValue(finite(input > 0.0 ? std::log(input) : 0.0));
    }
    if (name == "exp") return ExpressionValue(finite(std::exp(number(value(0U)))));
    if (name == "lerp") {
        const double from = number(value(0U));
        return ExpressionValue(finite(from + (number(value(1U)) - from) * number(value(2U))));
    }

    // Text and values.
    if (name == "repeat") {
        const std::string text = display_string(value(0U));
        const double count = std::floor(number(value(1U)));
        if (count < 0.0 || count * static_cast<double>(text.size()) > 16'000'000.0)
            return fail("would build too long a text.");
        std::string result;
        result.reserve(text.size() * static_cast<std::size_t>(count));
        for (std::size_t index = 0U; index < static_cast<std::size_t>(count); ++index)
            result += text;
        return ExpressionValue(Value(std::move(result)));
    }
    if (name == "parseNumber") {
        if (value(0U).number() != nullptr)
            return ExpressionValue(value(0U));
        const std::string* text = value(0U).string();
        if (text == nullptr)
            return ExpressionValue{};
        try {
            std::size_t used = 0U;
            const double parsed = std::stod(*text, &used);
            const bool whole = std::all_of(text->begin() + static_cast<std::ptrdiff_t>(used),
                                           text->end(), [](const char c) { return c == ' '; });
            return whole && std::isfinite(parsed) ? ExpressionValue(Value(parsed))
                                                  : ExpressionValue{};
        } catch (const std::exception&) {
            return ExpressionValue{};
        }
    }
    if (name == "toText") return ExpressionValue(Value(display_string(value(0U))));
    if (name == "typeOf") return ExpressionValue(Value(type_name(arguments[0])));
    if (name == "toJson") {
        std::string text = data::encode_json_line(value_to_json(value(0U)));
        if (!text.empty() && text.back() == '\n')
            text.pop_back();
        return ExpressionValue(Value(std::move(text)));
    }
    if (name == "parseJson") {
        const std::string* text = value(0U).string();
        if (text == nullptr)
            return fail("needs text.");
        try {
            return ExpressionValue(value_from_json(data::parse_json(*text)));
        } catch (const std::exception& error) {
            return fail(std::string("the text is not JSON: ") + error.what());
        }
    }
    return fail("is declared but has no implementation.");
}

} // namespace strata::runtime
