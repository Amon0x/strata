#include "authoring_grammar.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace strata::tools {
namespace {

using data::JsonValue;

[[nodiscard]] const JsonValue& required(const JsonValue& value, const std::string_view field) {
    const JsonValue* child = value.find(field);
    if (child == nullptr) throw std::runtime_error("lexical specification is missing a required field");
    return *child;
}

[[nodiscard]] const JsonValue::Array& array_field(const JsonValue& value, const std::string_view field) {
    const JsonValue::Array* values = required(value, field).array();
    if (values == nullptr) throw std::runtime_error("lexical specification field must be an array");
    return *values;
}

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> fields) {
    return JsonValue(JsonValue::Object(fields));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] std::vector<std::string> strings(
    const JsonValue& value,
    const std::string_view field
) {
    std::vector<std::string> result;
    for (const JsonValue& item : array_field(value, field)) {
        if (item.string() == nullptr) throw std::runtime_error("lexical specification entry must be a string");
        result.push_back(*item.string());
    }
    if (result.empty()) throw std::runtime_error("lexical specification categories must not be empty");
    return result;
}

[[nodiscard]] std::string regex_escape(const std::string_view value) {
    std::string result;
    for (const char character : value) {
        if (std::string_view("\\.^$|?*+()[]{}-").contains(character)) result.push_back('\\');
        result.push_back(character);
    }
    return result;
}

[[nodiscard]] std::string alternation(std::vector<std::string> values) {
    std::ranges::sort(values, [](const std::string& left, const std::string& right) {
        if (left.size() != right.size()) return left.size() > right.size();
        return left < right;
    });
    std::string result = "(?:";
    bool first = true;
    for (const std::string& value : values) {
        if (!first) result.push_back('|');
        result.append(regex_escape(value));
        first = false;
    }
    result.push_back(')');
    return result;
}

} // namespace

std::string render_grammar(const JsonValue& lexical) {
    const JsonValue* format = lexical.find("format");
    const JsonValue* version = lexical.find("version");
    if (format == nullptr || format->string() == nullptr ||
        *format->string() != "strata.lexical" || version == nullptr ||
        version->integer() == nullptr || *version->integer() != 1) {
        throw std::runtime_error("unsupported lexical specification");
    }
    std::vector<std::string> keywords = strings(lexical, "keywords");
    std::ranges::sort(keywords);
    std::string keyword_pattern = "\\b(?:";
    for (std::size_t index = 0U; index < keywords.size(); ++index) {
        if (index != 0U) keyword_pattern.push_back('|');
        keyword_pattern.append(regex_escape(keywords[index]));
    }
    keyword_pattern.append(")\\b");

    std::vector<std::string> color_counts;
    for (const JsonValue& count : array_field(lexical, "colorDigitCounts")) {
        if (count.integer() == nullptr || *count.integer() <= 0) {
            throw std::runtime_error("color digit count must be a positive integer");
        }
        color_counts.push_back("[0-9A-Fa-f]{" + std::to_string(*count.integer()) + '}');
    }
    const std::string* number_body = required(lexical, "numberBodyPattern").string();
    if (number_body == nullptr) throw std::runtime_error("number body pattern must be a string");

    const JsonValue grammar = object({
        {"fileTypes", array({JsonValue("strata")})},
        {"name", JsonValue("Strata UI")},
        {"patterns", array({
            object({{"include", JsonValue("#comments")}}),
            object({
                {"begin", JsonValue("\\\"")},
                {"end", JsonValue("\\\"")},
                {"name", JsonValue("string.quoted.double.strata")},
                {"patterns", array({object({
                    {"match", JsonValue("\\\\.")},
                    {"name", JsonValue("constant.character.escape.strata")},
                })})},
            }),
            object({
                {"match", JsonValue("#" + alternation(std::move(color_counts)) + "\\b")},
                {"name", JsonValue("constant.other.color.strata")},
            }),
            object({
                {"match", JsonValue("\\b" + *number_body + alternation(strings(lexical, "numericUnits")) + "?\\b")},
                {"name", JsonValue("constant.numeric.strata")},
            }),
            object({{"match", JsonValue(keyword_pattern)}, {"name", JsonValue("keyword.control.strata")}}),
            object({{"match", JsonValue("\\b[A-Z][A-Za-z0-9_]*\\b")}, {"name", JsonValue("entity.name.type.widget.strata")}}),
            object({{"match", JsonValue(alternation(strings(lexical, "operators")))}, {"name", JsonValue("keyword.operator.strata")}}),
            object({{"match", JsonValue(alternation(strings(lexical, "punctuation")))}, {"name", JsonValue("punctuation.strata")}}),
            object({{"match", JsonValue("\\b[_\\p{L}][_\\p{L}\\p{N}]*\\b")}, {"name", JsonValue("variable.other.strata")}}),
        })},
        {"repository", object({
            {"comments", object({
                {"patterns", array({
                    object({{"begin", JsonValue("//")}, {"end", JsonValue("$")}, {"name", JsonValue("comment.line.double-slash.strata")}}),
                    object({{"begin", JsonValue("/\\*")}, {"end", JsonValue("\\*/")}, {"name", JsonValue("comment.block.strata")}}),
                })},
            })},
        })},
        {"scopeName", JsonValue("source.strata")},
    });
    return data::encode_canonical_json(grammar);
}

} // namespace strata::tools
