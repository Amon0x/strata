#include "authoring_contract.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "compiler/schema.hpp"
#include "core/utf8.hpp"

namespace strata::tools {
namespace {

using compiler::ActionSchema;
using compiler::ObjectField;
using compiler::SchemaParameter;
using compiler::SchemaRegistry;
using compiler::SemanticType;
using compiler::SemanticTypeKind;
using compiler::SemanticTypePtr;
using data::JsonValue;

[[nodiscard]] const JsonValue& required(const JsonValue& value, const std::string_view field) {
    const JsonValue* child = value.find(field);
    if (child == nullptr) {
        throw std::runtime_error("application schema is missing field '" + std::string(field) + "'");
    }
    return *child;
}

[[nodiscard]] const std::string& string_field(
    const JsonValue& value,
    const std::string_view field
) {
    const std::string* text = required(value, field).string();
    if (text == nullptr) throw std::runtime_error("application schema field must be a string");
    return *text;
}

[[nodiscard]] const JsonValue::Array& array_field(
    const JsonValue& value,
    const std::string_view field
) {
    const JsonValue::Array* values = required(value, field).array();
    if (values == nullptr) throw std::runtime_error("application schema field must be an array");
    return *values;
}

[[nodiscard]] std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open contract input '" + path.string() + "'");
    std::ostringstream output;
    output << input.rdbuf();
    std::string text = output.str();
    if (!core::valid_utf8(text)) {
        throw std::runtime_error("contract input is not valid UTF-8: " + path.string());
    }
    return text;
}

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::error_code error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), error);
    }
    if (error) throw std::runtime_error("could not create contract output directory");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("could not create contract output '" + path.string() + "'");
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!output) throw std::runtime_error("could not write contract output '" + path.string() + "'");
}

[[nodiscard]] bool identifier_character(const unsigned char value) noexcept {
    return std::isalnum(value) != 0 || value == '_';
}

[[nodiscard]] std::vector<std::string> words(const std::string_view value) {
    std::vector<std::string> result;
    std::string word;
    bool previous_lower = false;
    for (std::size_t index = 0U; index < value.size(); ++index) {
        const auto current = static_cast<unsigned char>(value[index]);
        if (!identifier_character(current) || current == '_') {
            if (!word.empty()) result.push_back(std::move(word));
            word.clear();
            previous_lower = false;
            continue;
        }
        const bool boundary = !word.empty() && std::isupper(current) != 0 && previous_lower;
        if (boundary) {
            result.push_back(std::move(word));
            word.clear();
        }
        word.push_back(static_cast<char>(std::tolower(current)));
        previous_lower = std::islower(current) != 0 || std::isdigit(current) != 0;
    }
    if (!word.empty()) result.push_back(std::move(word));
    if (result.empty()) result.push_back("value");
    return result;
}

[[nodiscard]] bool cpp_keyword(const std::string_view value) noexcept {
    static constexpr std::string_view keywords[]{
        "alignas", "alignof", "and", "and_eq", "asm", "atomic_cancel", "atomic_commit",
        "atomic_noexcept", "auto", "bitand", "bitor", "bool", "break", "case", "catch",
        "char", "char8_t", "char16_t", "char32_t", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue", "co_await",
        "co_return", "co_yield", "decltype", "default", "delete", "do", "double",
        "dynamic_cast", "else", "enum", "explicit", "export", "extern", "false", "float",
        "for", "friend", "goto", "if", "inline", "int", "long", "mutable", "namespace",
        "new", "noexcept", "not", "not_eq", "nullptr", "operator", "or", "or_eq",
        "private", "protected", "public", "reflexpr", "register", "reinterpret_cast",
        "requires", "return", "short", "signed", "sizeof", "static", "static_assert",
        "static_cast", "struct", "switch", "synchronized", "template", "this",
        "thread_local", "throw", "true", "try", "typedef", "typeid", "typename", "union",
        "unsigned", "using", "virtual", "void", "volatile", "wchar_t", "while", "xor",
        "xor_eq",
    };
    return std::ranges::find(keywords, value) != std::end(keywords);
}

[[nodiscard]] std::string snake_identifier(const std::string_view value) {
    const std::vector<std::string> parts = words(value);
    std::string result;
    for (const std::string& part : parts) {
        if (!result.empty()) result.push_back('_');
        result += part;
    }
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
        result.insert(0U, "value_");
    }
    if (cpp_keyword(result)) result.push_back('_');
    return result;
}

[[nodiscard]] std::string pascal_identifier(const std::string_view value) {
    const std::vector<std::string> parts = words(value);
    std::string result;
    for (std::string part : parts) {
        part.front() = static_cast<char>(
            std::toupper(static_cast<unsigned char>(part.front()))
        );
        result += part;
    }
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result.front())) != 0) {
        result.insert(0U, "Value");
    }
    if (result.empty()) result = "Value";
    return result;
}

[[nodiscard]] std::string cpp_quote(const std::string_view value) {
    std::ostringstream output;
    output << '"';
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (byte < 0x20U) {
                output << '\\' << std::oct << std::setw(3) << std::setfill('0')
                       << static_cast<unsigned int>(byte) << std::dec;
            } else {
                output << character;
            }
        }
    }
    output << '"';
    return output.str();
}

void validate_namespace(const std::string_view value) {
    if (value.empty()) throw std::runtime_error("C++ contract namespace must not be empty");
    std::size_t offset = 0U;
    while (offset < value.size()) {
        const std::size_t separator = value.find("::", offset);
        const std::string_view part = separator == std::string_view::npos
            ? value.substr(offset)
            : value.substr(offset, separator - offset);
        if (part.empty() || std::isdigit(static_cast<unsigned char>(part.front())) != 0 ||
            !std::ranges::all_of(part, [](const char byte) {
                return identifier_character(static_cast<unsigned char>(byte));
            }) || cpp_keyword(part)) {
            throw std::runtime_error("invalid C++ contract namespace '" + std::string(value) + "'");
        }
        if (separator == std::string_view::npos) break;
        offset = separator + 2U;
    }
}

struct TypeName final {
    std::string cpp;
    bool optional = false;
};

struct FieldDefinition final {
    std::string wire_name;
    std::string member_name;
    TypeName type;
    SemanticTypePtr semantic;
    bool required = false;
};

struct Definition final {
    enum class Kind { enumeration, object };
    Kind kind = Kind::object;
    std::string name;
    SemanticTypePtr semantic;
    std::vector<std::string> enum_values;
    std::vector<FieldDefinition> fields;
};

struct RootDefinition final {
    std::string path;
    std::string function_name;
    TypeName type;
    SemanticTypePtr semantic;
    bool wrapper = false;
    std::string wrapper_name;
    std::string model_name;
};

struct ActionParameter final {
    std::string wire_name;
    std::string member_name;
    TypeName type;
    SemanticTypePtr semantic;
    const JsonValue* default_value = nullptr;
    bool required = false;
};

struct ActionDefinition final {
    std::string id;
    std::string name;
    std::vector<ActionParameter> parameters;
};

class ContractGenerator final {
public:
    ContractGenerator(
        const JsonValue& application_schema,
        std::string cpp_namespace,
        std::string source_name
    ) : application_schema_(application_schema),
        registry_(SchemaRegistry::builtins()),
        cpp_namespace_(std::move(cpp_namespace)),
        source_name_(std::move(source_name)) {
        validate_namespace(cpp_namespace_);
        registry_.apply_scenario_declarations(application_schema_);
        discover_roots();
        discover_actions();
    }

    [[nodiscard]] std::string render() const {
        std::ostringstream output;
        output << "#pragma once\n\n"
                  "// Generated by strata_authoring from " << source_name_ << ". Do not edit.\n\n"
                  "#include <array>\n"
                  "#include <cstdint>\n"
                  "#include <map>\n"
                  "#include <optional>\n"
                  "#include <stdexcept>\n"
                  "#include <string>\n"
                  "#include <string_view>\n"
                  "#include <utility>\n"
                  "#include <variant>\n"
                  "#include <vector>\n\n"
                  "#include <strata/host.hpp>\n\n"
               << "namespace " << cpp_namespace_ << " {\n\n";
        render_support(output);
        for (const Definition& definition : definitions_) render_definition(output, definition);
        for (const RootDefinition& root : roots_) {
            if (!root.wrapper) continue;
            output << "struct " << root.wrapper_name << " final {\n    "
                   << root.type.cpp << " value{};\n};\n\n"
                   << "[[nodiscard]] inline strata::host::Value to_value(const "
                   << root.wrapper_name << "& value) {\n"
                   << "    return to_value(value.value);\n}\n\n"
                   << "[[nodiscard]] inline " << root.wrapper_name << " decode_"
                   << snake_identifier(root.wrapper_name)
                   << "(const strata::host::Value& value) {\n"
                   << "    return " << root.wrapper_name << "{"
                   << decode_expression(root.semantic, "value", root.path) << "};\n}\n\n";
        }
        render_roots(output);
        render_actions(output);
        output << "} // namespace " << cpp_namespace_ << "\n";
        return output.str();
    }

private:
    [[nodiscard]] std::string unique_name(const std::string_view hint) {
        const std::string base = pascal_identifier(hint);
        std::string candidate = base;
        std::size_t suffix = 2U;
        while (!used_names_.insert(candidate).second) {
            candidate = base + std::to_string(suffix++);
        }
        return candidate;
    }

    [[nodiscard]] static bool nullable_union(const SemanticType& type) noexcept {
        if (type.kind != SemanticTypeKind::union_value || type.options.size() != 2U) return false;
        return std::ranges::count_if(type.options, [](const SemanticTypePtr& option) {
            return option->kind == SemanticTypeKind::null_value;
        }) == 1;
    }

    [[nodiscard]] static SemanticTypePtr non_null_union_option(const SemanticType& type) {
        const auto found = std::ranges::find_if(type.options, [](const SemanticTypePtr& option) {
            return option->kind != SemanticTypeKind::null_value;
        });
        if (found == type.options.end()) throw std::runtime_error("nullable union has no value type");
        return *found;
    }

    [[nodiscard]] TypeName type_name(const SemanticTypePtr& type, const std::string_view hint) {
        if (type == nullptr) throw std::runtime_error("host contract contains a null semantic type");
        switch (type->kind) {
        case SemanticTypeKind::null_value: return {"std::monostate", false};
        case SemanticTypeKind::boolean: return {"bool", false};
        case SemanticTypeKind::number: return {"double", false};
        case SemanticTypeKind::duration: return {"std::int64_t", false};
        case SemanticTypeKind::string:
        case SemanticTypeKind::string_literal:
        case SemanticTypeKind::color:
        case SemanticTypeKind::path:
        case SemanticTypeKind::image:
        case SemanticTypeKind::key: return {"std::string", false};
        case SemanticTypeKind::unknown:
        case SemanticTypeKind::any: return {"strata::host::Value", false};
        case SemanticTypeKind::state_binding:
            throw std::runtime_error("state bindings cannot cross a host contract");
        case SemanticTypeKind::enumeration:
        case SemanticTypeKind::host_object: return discover_named(type, hint);
        case SemanticTypeKind::map:
            if (!type->fields.empty()) return discover_named(type, hint);
            if (type->value == nullptr) throw std::runtime_error("host map has no value type");
            {
                TypeName value = type_name(type->value, std::string(hint) + "Value");
                if (type->value_nullable && !value.optional) {
                    value.cpp = "std::optional<" + value.cpp + ">";
                    value.optional = true;
                }
                return {"std::map<std::string, " + value.cpp + ">", false};
            }
        case SemanticTypeKind::list:
        case SemanticTypeKind::collection: {
            if (type->element == nullptr) throw std::runtime_error("host list has no element type");
            TypeName element = type_name(type->element, std::string(hint) + "Item");
            if (type->element_nullable && !element.optional) {
                element.cpp = "std::optional<" + element.cpp + ">";
                element.optional = true;
            }
            return {"std::vector<" + element.cpp + ">", false};
        }
        case SemanticTypeKind::union_value: {
            if (nullable_union(*type)) {
                TypeName value = type_name(non_null_union_option(*type), hint);
                if (!value.optional) value.cpp = "std::optional<" + value.cpp + ">";
                value.optional = true;
                return value;
            }
            if (type->options.empty()) throw std::runtime_error("host union has no options");
            std::string result = "std::variant<";
            for (std::size_t index = 0U; index < type->options.size(); ++index) {
                if (index != 0U) result += ", ";
                result += type_name(
                    type->options[index], std::string(hint) + "Option" + std::to_string(index + 1U)
                ).cpp;
            }
            result += '>';
            return {std::move(result), false};
        }
        case SemanticTypeKind::async_value:
            if (type->value == nullptr) throw std::runtime_error("async host type has no value schema");
            return type_name(type->value, std::string(hint) + "Value");
        default:
            throw std::runtime_error(
                "semantic type '" + type->diagnostic_name() +
                "' cannot cross a generated host contract"
            );
        }
    }

    [[nodiscard]] TypeName discover_named(
        const SemanticTypePtr& type,
        const std::string_view hint
    ) {
        if (const auto found = named_types_.find(type.get()); found != named_types_.end()) {
            return {found->second, false};
        }
        const std::string name = unique_name(
            type->schema_name.empty() ? hint : std::string_view(type->schema_name)
        );
        named_types_.emplace(type.get(), name);
        if (type->kind == SemanticTypeKind::enumeration) {
            definitions_.push_back(Definition{
                Definition::Kind::enumeration,
                name,
                type,
                type->values,
                {},
            });
            return {name, false};
        }
        if (type->fields.empty()) {
            throw std::runtime_error(
                "map host contract '" + type->diagnostic_name() + "' must declare object fields"
            );
        }
        std::vector<FieldDefinition> fields;
        fields.reserve(type->fields.size());
        for (const ObjectField& field : type->fields) {
            TypeName field_type = type_name(field.type, name + pascal_identifier(field.name));
            if ((!field.required || field.nullable) && !field_type.optional) {
                field_type.cpp = "std::optional<" + field_type.cpp + ">";
                field_type.optional = true;
            }
            fields.push_back(FieldDefinition{
                field.name,
                snake_identifier(field.name),
                std::move(field_type),
                field.type,
                field.required,
            });
        }
        definitions_.push_back(Definition{
            Definition::Kind::object,
            name,
            type,
            {},
            std::move(fields),
        });
        return {name, false};
    }

    void discover_roots() {
        std::vector<std::pair<std::string, SemanticTypePtr>> host_types;
        host_types.reserve(registry_.host_types().size());
        for (const auto& [path, type] : registry_.host_types()) host_types.emplace_back(path, type);
        std::ranges::sort(host_types, {}, &std::pair<std::string, SemanticTypePtr>::first);
        for (const auto& [path, semantic] : host_types) {
            const std::string root_name = pascal_identifier(path);
            TypeName type = type_name(semantic, root_name);
            const bool named_object =
                (semantic->kind == SemanticTypeKind::map && !semantic->fields.empty()) ||
                semantic->kind == SemanticTypeKind::host_object;
            RootDefinition root{
                path,
                snake_identifier(path),
                type,
                semantic,
                !named_object,
                {},
                named_object ? unique_name(root_name + "Model") : std::string{},
            };
            if (root.wrapper) root.wrapper_name = unique_name(root_name);
            roots_.push_back(std::move(root));
        }
    }

    [[nodiscard]] static const JsonValue* action_parameter_default(
        const JsonValue& action,
        const std::string_view name
    ) {
        for (const JsonValue& parameter : array_field(action, "arguments")) {
            if (string_field(parameter, "name") != name) continue;
            const JsonValue* value = parameter.find("default");
            return value != nullptr && !value->is_null() ? value : nullptr;
        }
        return nullptr;
    }

    void discover_actions() {
        const JsonValue& actions = required(application_schema_, "actions");
        for (const JsonValue& declared : array_field(actions, "definitions")) {
            const std::string& id = string_field(declared, "id");
            const ActionSchema* schema = registry_.action(id);
            if (schema == nullptr) throw std::runtime_error("declared action did not enter registry");
            std::string name_hint = pascal_identifier(id);
            if (!name_hint.ends_with("Action")) name_hint += "Action";
            const std::string name = unique_name(name_hint);
            std::vector<ActionParameter> parameters;
            parameters.reserve(schema->parameters.size());
            for (const SchemaParameter& parameter : schema->parameters) {
                const JsonValue* default_value = action_parameter_default(declared, parameter.name);
                TypeName type = type_name(parameter.type, name + pascal_identifier(parameter.name));
                if ((!parameter.required || parameter.nullable) && default_value == nullptr &&
                    !type.optional) {
                    type.cpp = "std::optional<" + type.cpp + ">";
                    type.optional = true;
                }
                parameters.push_back(ActionParameter{
                    parameter.name,
                    snake_identifier(parameter.name),
                    std::move(type),
                    parameter.type,
                    default_value,
                    parameter.required,
                });
            }
            actions_.push_back(ActionDefinition{id, name, std::move(parameters)});
        }
        std::ranges::sort(actions_, {}, &ActionDefinition::id);
    }

    [[nodiscard]] std::string custom_name(const SemanticTypePtr& type) const {
        const auto found = named_types_.find(type.get());
        if (found == named_types_.end()) throw std::logic_error("contract type was not discovered");
        return found->second;
    }

    [[nodiscard]] TypeName resolved_type_name(const SemanticTypePtr& type) const {
        switch (type->kind) {
        case SemanticTypeKind::null_value: return {"std::monostate", false};
        case SemanticTypeKind::boolean: return {"bool", false};
        case SemanticTypeKind::number: return {"double", false};
        case SemanticTypeKind::duration: return {"std::int64_t", false};
        case SemanticTypeKind::string:
        case SemanticTypeKind::string_literal:
        case SemanticTypeKind::color:
        case SemanticTypeKind::path:
        case SemanticTypeKind::image:
        case SemanticTypeKind::key: return {"std::string", false};
        case SemanticTypeKind::unknown:
        case SemanticTypeKind::any: return {"strata::host::Value", false};
        case SemanticTypeKind::enumeration:
        case SemanticTypeKind::host_object: return {custom_name(type), false};
        case SemanticTypeKind::map:
            if (!type->fields.empty()) return {custom_name(type), false};
            {
                TypeName value = resolved_type_name(type->value);
                if (type->value_nullable && !value.optional) {
                    value.cpp = "std::optional<" + value.cpp + ">";
                    value.optional = true;
                }
                return {"std::map<std::string, " + value.cpp + ">", false};
            }
        case SemanticTypeKind::list:
        case SemanticTypeKind::collection: {
            TypeName element = resolved_type_name(type->element);
            if (type->element_nullable && !element.optional) {
                element.cpp = "std::optional<" + element.cpp + ">";
                element.optional = true;
            }
            return {"std::vector<" + element.cpp + ">", false};
        }
        case SemanticTypeKind::union_value: {
            if (nullable_union(*type)) {
                TypeName value = resolved_type_name(non_null_union_option(*type));
                if (!value.optional) value.cpp = "std::optional<" + value.cpp + ">";
                value.optional = true;
                return value;
            }
            std::string result = "std::variant<";
            for (std::size_t index = 0U; index < type->options.size(); ++index) {
                if (index != 0U) result += ", ";
                result += resolved_type_name(type->options[index]).cpp;
            }
            result += '>';
            return {std::move(result), false};
        }
        case SemanticTypeKind::async_value: return resolved_type_name(type->value);
        default: throw std::logic_error("unsupported resolved generated type");
        }
    }

    [[nodiscard]] std::string decode_expression(
        const SemanticTypePtr& type,
        const std::string_view input,
        const std::string_view context
    ) const {
        const std::string context_literal = cpp_quote(context);
        switch (type->kind) {
        case SemanticTypeKind::null_value:
            return "detail::decode_null(" + std::string(input) + ", " + context_literal + ")";
        case SemanticTypeKind::boolean:
            return "detail::decode_boolean(" + std::string(input) + ", " + context_literal + ")";
        case SemanticTypeKind::number:
            return "detail::decode_number(" + std::string(input) + ", " + context_literal + ")";
        case SemanticTypeKind::duration:
            return "detail::decode_integer(" + std::string(input) + ", " + context_literal + ")";
        case SemanticTypeKind::string:
        case SemanticTypeKind::color:
        case SemanticTypeKind::path:
        case SemanticTypeKind::image:
        case SemanticTypeKind::key:
            return "detail::decode_string(" + std::string(input) + ", " + context_literal + ")";
        case SemanticTypeKind::string_literal:
            return "detail::decode_literal(" + std::string(input) + ", " +
                cpp_quote(type->literal) + ", " + context_literal + ")";
        case SemanticTypeKind::unknown:
        case SemanticTypeKind::any: return std::string(input);
        case SemanticTypeKind::enumeration:
        case SemanticTypeKind::host_object:
            return "decode_" + snake_identifier(custom_name(type)) + "(" +
                std::string(input) + ")";
        case SemanticTypeKind::map:
            if (!type->fields.empty()) {
                return "decode_" + snake_identifier(custom_name(type)) + "(" +
                    std::string(input) + ")";
            } else {
                TypeName value = resolved_type_name(type->value);
                std::string decoded = decode_expression(
                    type->value, "item", std::string(context) + ".value"
                );
                if (type->value_nullable && !value.optional) {
                    decoded = "item.is_null() ? std::optional<" + value.cpp +
                        ">{} : std::optional<" + value.cpp + ">{" + decoded + "}";
                    value.cpp = "std::optional<" + value.cpp + ">";
                }
                return "detail::decode_map<" + value.cpp + ">(" + std::string(input) +
                    ", " + context_literal +
                    ", [](const strata::host::Value& item) { return " + decoded + "; })";
            }
        case SemanticTypeKind::list:
        case SemanticTypeKind::collection: {
            TypeName element = resolved_type_name(type->element);
            std::string item = decode_expression(
                type->element, "item", std::string(context) + "[]"
            );
            if (type->element_nullable && !element.optional) {
                item = "item.is_null() ? std::optional<" + element.cpp +
                    ">{} : std::optional<" + element.cpp + ">{" + item + "}";
                element.cpp = "std::optional<" + element.cpp + ">";
                element.optional = true;
            }
            return "detail::decode_list<" + element.cpp + ">(" +
                std::string(input) + ", " + context_literal +
                ", [](const strata::host::Value& item) { return " + item + "; })";
        }
        case SemanticTypeKind::union_value: {
            if (nullable_union(*type)) {
                const SemanticTypePtr value = non_null_union_option(*type);
                const std::string decoded = decode_expression(value, input, context);
                TypeName value_type = resolved_type_name(value);
                return std::string(input) + ".is_null() ? std::optional<" + value_type.cpp +
                    ">{} : std::optional<" + value_type.cpp + ">{" + decoded + "}";
            }
            const TypeName union_type = resolved_type_name(type);
            std::string result = "detail::decode_union<" + union_type.cpp + ">(" +
                std::string(input) + ", " + context_literal;
            for (std::size_t index = 0U; index < type->options.size(); ++index) {
                result += ", [](const strata::host::Value& option) { return " + union_type.cpp +
                    "(std::in_place_index<" + std::to_string(index) + "U>, " +
                    decode_expression(type->options[index], "option", context) + "); }";
            }
            result += ')';
            return result;
        }
        case SemanticTypeKind::async_value:
            return decode_expression(type->value, input, context);
        default:
            throw std::logic_error("unsupported generated decode type");
        }
    }

    [[nodiscard]] std::string default_expression(
        const JsonValue& value,
        const SemanticTypePtr& type
    ) const {
        const JsonValue& payload = required(value, "value");
        if (payload.is_null()) return "{}";
        switch (type->kind) {
        case SemanticTypeKind::boolean:
            if (payload.boolean() == nullptr) break;
            return *payload.boolean() ? "true" : "false";
        case SemanticTypeKind::number:
            if (payload.integer() != nullptr) return std::to_string(*payload.integer()) + ".0";
            if (payload.number() != nullptr) {
                std::ostringstream output;
                output << std::setprecision(17) << *payload.number();
                return output.str();
            }
            break;
        case SemanticTypeKind::duration:
            if (payload.integer() != nullptr) return std::to_string(*payload.integer());
            break;
        case SemanticTypeKind::enumeration:
            if (payload.string() != nullptr &&
                std::ranges::find(type->values, *payload.string()) != type->values.end()) {
                return custom_name(type) + "::" + snake_identifier(*payload.string());
            }
            break;
        case SemanticTypeKind::string:
        case SemanticTypeKind::color:
        case SemanticTypeKind::path:
        case SemanticTypeKind::image:
        case SemanticTypeKind::key:
            if (payload.string() != nullptr) return cpp_quote(*payload.string());
            break;
        default: break;
        }
        throw std::runtime_error("action default cannot be represented by generated C++ contract");
    }

    void render_support(std::ostringstream& output) const {
        output << R"CPP(namespace detail {

[[noreturn]] inline void contract_error(const std::string_view context, const std::string_view expected) {
    throw std::invalid_argument(std::string(context) + " must be " + std::string(expected));
}

[[nodiscard]] inline const strata::host::Value::Object& decode_object(
    const strata::host::Value& value,
    const std::string_view context
) {
    const auto* object = value.object();
    if (object == nullptr) contract_error(context, "an object");
    return *object;
}

[[nodiscard]] inline const strata::host::Value& required_field(
    const strata::host::Value& value,
    const std::string_view name,
    const std::string_view context
) {
    static_cast<void>(decode_object(value, context));
    const strata::host::Value* field = value.find(name);
    if (field == nullptr) {
        throw std::invalid_argument(std::string(context) + " is missing required field '" +
                                    std::string(name) + "'");
    }
    return *field;
}

[[nodiscard]] inline bool decode_boolean(
    const strata::host::Value& value,
    const std::string_view context
) {
    if (const bool* boolean = value.boolean(); boolean != nullptr) return *boolean;
    contract_error(context, "a boolean");
}

[[nodiscard]] inline double decode_number(
    const strata::host::Value& value,
    const std::string_view context
) {
    if (const double* number = value.number(); number != nullptr) return *number;
    if (const std::int64_t* integer = value.integer(); integer != nullptr) {
        return static_cast<double>(*integer);
    }
    contract_error(context, "a number");
}

[[nodiscard]] inline std::int64_t decode_integer(
    const strata::host::Value& value,
    const std::string_view context
) {
    if (const std::int64_t* integer = value.integer(); integer != nullptr) return *integer;
    contract_error(context, "an integer");
}

[[nodiscard]] inline std::string decode_string(
    const strata::host::Value& value,
    const std::string_view context
) {
    if (const std::string* text = value.string(); text != nullptr) return *text;
    contract_error(context, "a string");
}

[[nodiscard]] inline std::string decode_literal(
    const strata::host::Value& value,
    const std::string_view literal,
    const std::string_view context
) {
    std::string result = decode_string(value, context);
    if (result != literal) contract_error(context, literal);
    return result;
}

[[nodiscard]] inline std::monostate decode_null(
    const strata::host::Value& value,
    const std::string_view context
) {
    if (!value.is_null()) contract_error(context, "null");
    return {};
}

template <typename T, typename Decode>
[[nodiscard]] std::vector<T> decode_list(
    const strata::host::Value& value,
    const std::string_view context,
    Decode&& decode
) {
    const auto* values = value.array();
    if (values == nullptr) contract_error(context, "a list");
    std::vector<T> result;
    result.reserve(values->size());
    for (const strata::host::Value& item : *values) result.push_back(decode(item));
    return result;
}

template <typename T, typename Decode>
[[nodiscard]] std::map<std::string, T> decode_map(
    const strata::host::Value& value,
    const std::string_view context,
    Decode&& decode
) {
    const auto& fields = decode_object(value, context);
    std::map<std::string, T> result;
    for (const auto& [name, item] : fields) result.emplace(name, decode(item));
    return result;
}

template <typename Variant, typename... Decode>
[[nodiscard]] Variant decode_union(
    const strata::host::Value& value,
    const std::string_view context,
    Decode&&... decode
) {
    std::optional<Variant> result;
    const auto attempt = [&value, &result](auto&& candidate) {
        if (result.has_value()) return;
        try {
            result = candidate(value);
        } catch (const std::invalid_argument&) {
        }
    };
    (attempt(std::forward<Decode>(decode)), ...);
    if (!result.has_value()) contract_error(context, "one of the declared union options");
    return std::move(*result);
}

} // namespace detail

[[nodiscard]] inline strata::host::Value to_value(const strata::host::Value& value) { return value; }
[[nodiscard]] inline strata::host::Value to_value(const bool value) { return strata::host::Value(value); }
[[nodiscard]] inline strata::host::Value to_value(const double value) { return strata::host::Value(value); }
[[nodiscard]] inline strata::host::Value to_value(const std::int64_t value) { return strata::host::Value(value); }
[[nodiscard]] inline strata::host::Value to_value(const std::string& value) { return strata::host::Value(value); }
[[nodiscard]] inline strata::host::Value to_value(const std::monostate) { return strata::host::Value(); }

template <typename T>
[[nodiscard]] strata::host::Value to_value(const std::optional<T>& value) {
    return value.has_value() ? to_value(*value) : strata::host::Value();
}

template <typename T>
[[nodiscard]] strata::host::Value to_value(const std::vector<T>& values) {
    return strata::host::Value::array(values, [](const T& value) { return to_value(value); });
}

template <typename T>
[[nodiscard]] strata::host::Value to_value(const std::map<std::string, T>& values) {
    strata::host::Value::Object fields;
    for (const auto& [name, value] : values) fields.emplace(name, to_value(value));
    return strata::host::Value(std::move(fields));
}

template <typename... T>
[[nodiscard]] strata::host::Value to_value(const std::variant<T...>& value) {
    return std::visit([](const auto& item) { return to_value(item); }, value);
}

)CPP";
    }

    void render_definition(std::ostringstream& output, const Definition& definition) const {
        if (definition.kind == Definition::Kind::enumeration) {
            output << "enum class " << definition.name << " {\n";
            for (const std::string& value : definition.enum_values) {
                output << "    " << snake_identifier(value) << ",\n";
            }
            output << "};\n\n[[nodiscard]] inline std::string_view wire_name(const "
                   << definition.name << " value) noexcept {\n    switch (value) {\n";
            for (const std::string& value : definition.enum_values) {
                output << "    case " << definition.name << "::" << snake_identifier(value)
                       << ": return " << cpp_quote(value) << ";\n";
            }
            output << "    }\n    return {};\n}\n\n"
                   << "[[nodiscard]] inline strata::host::Value to_value(const "
                   << definition.name << " value) {\n"
                   << "    return strata::host::Value(wire_name(value));\n}\n\n"
                   << "[[nodiscard]] inline " << definition.name << " decode_"
                   << snake_identifier(definition.name)
                   << "(const strata::host::Value& value) {\n"
                   << "    const std::string decoded = detail::decode_string(value, "
                   << cpp_quote(definition.name) << ");\n";
            for (const std::string& value : definition.enum_values) {
                output << "    if (decoded == " << cpp_quote(value) << ") return "
                       << definition.name << "::" << snake_identifier(value) << ";\n";
            }
            output << "    detail::contract_error(" << cpp_quote(definition.name)
                   << ", \"a declared enum value\");\n}\n\n";
            return;
        }
        output << "struct " << definition.name << " final {\n";
        for (const FieldDefinition& field : definition.fields) {
            output << "    " << field.type.cpp << ' ' << field.member_name << "{};\n";
        }
        output << "    [[nodiscard]] friend bool operator==(const "
               << definition.name << "&, const " << definition.name
               << "&) = default;\n";
        output << "};\n\n[[nodiscard]] inline strata::host::Value to_value(const "
               << definition.name << "& value) {\n"
               << "    strata::host::Value::Object fields;\n";
        for (const FieldDefinition& field : definition.fields) {
            if (!field.required && field.type.optional) {
                output << "    if (value." << field.member_name << ".has_value()) {\n"
                       << "        fields.emplace(" << cpp_quote(field.wire_name) << ", to_value(*value."
                       << field.member_name << "));\n    }\n";
            } else {
                output << "    fields.emplace(" << cpp_quote(field.wire_name) << ", to_value(value."
                       << field.member_name << "));\n";
            }
        }
        output << "    return strata::host::Value(std::move(fields));\n}\n\n"
               << "[[nodiscard]] inline " << definition.name << " decode_"
               << snake_identifier(definition.name)
               << "(const strata::host::Value& value) {\n"
               << "    static_cast<void>(detail::decode_object(value, "
               << cpp_quote(definition.name) << "));\n"
               << "    " << definition.name << " result;\n";
        for (const FieldDefinition& field : definition.fields) {
            const std::string context = definition.name + "." + field.wire_name;
            if (!field.required) {
                output << "    if (const strata::host::Value* field = value.find("
                       << cpp_quote(field.wire_name) << "); field != nullptr && !field->is_null()) {\n"
                       << "        result." << field.member_name << " = "
                       << decode_expression(field.semantic, "*field", context) << ";\n"
                       << "    }\n";
            } else if (field.type.optional) {
                output << "    {\n"
                       << "        const strata::host::Value& field = detail::required_field(value, "
                       << cpp_quote(field.wire_name) << ", " << cpp_quote(definition.name) << ");\n"
                       << "        if (!field.is_null()) result." << field.member_name << " = "
                       << decode_expression(field.semantic, "field", context) << ";\n"
                       << "    }\n";
            } else {
                output << "    result." << field.member_name << " = "
                       << decode_expression(
                              field.semantic,
                              "detail::required_field(value, " + cpp_quote(field.wire_name) + ", " +
                                  cpp_quote(definition.name) + ")",
                              context
                          )
                       << ";\n";
            }
        }
        output << "    return result;\n}\n\n";
    }

    void render_roots(std::ostringstream& output) const {
        for (const RootDefinition& root : roots_) {
            const std::string type = root.wrapper ? root.wrapper_name : root.type.cpp;
            output << "[[nodiscard]] inline strata::host::Value encode_" << root.function_name
                   << "(const " << type << "& value) {\n"
                   << "    return strata::host::Value::object({{"
                   << cpp_quote(root.path) << ", to_value(value)}});\n}\n\n";
            if ((root.semantic->kind != SemanticTypeKind::map ||
                 root.semantic->fields.empty()) &&
                root.semantic->kind != SemanticTypeKind::host_object) {
                continue;
            }
            const Definition* definition = nullptr;
            for (const Definition& candidate : definitions_) {
                if (candidate.semantic.get() == root.semantic.get()) {
                    definition = &candidate;
                    break;
                }
            }
            if (definition == nullptr) throw std::logic_error("root object definition is missing");
            for (const FieldDefinition& field : definition->fields) {
                output << "[[nodiscard]] inline strata::host::Value encode_" << root.function_name
                       << '_' << field.member_name << "(const " << field.type.cpp << "& value) {\n"
                       << "    return strata::host::Value::object({{"
                       << cpp_quote(root.path) << ", strata::host::Value::object({{"
                       << cpp_quote(field.wire_name) << ", to_value(value)}})}});\n}\n\n";
            }
            output << "class " << root.model_name << " final {\n"
                   << "public:\n"
                   << "    " << root.model_name << "() = default;\n"
                   << "    " << root.model_name << "(const " << root.model_name
                   << "&) = delete;\n"
                   << "    " << root.model_name << "& operator=(const "
                   << root.model_name << "&) = delete;\n"
                   << "    " << root.model_name << "(" << root.model_name
                   << "&&) = delete;\n"
                   << "    " << root.model_name << "& operator=("
                   << root.model_name << "&&) = delete;\n\n"
                   << "    [[nodiscard]] bool set(" << type << " value) {\n"
                   << "        bool changed = false;\n";
            for (const FieldDefinition& field : definition->fields) {
                output << "        changed = set_" << field.member_name
                       << "(std::move(value." << field.member_name
                       << ")) || changed;\n";
            }
            output << "        return changed;\n"
                   << "    }\n\n";
            for (const FieldDefinition& field : definition->fields) {
                output << "    [[nodiscard]] bool set_" << field.member_name << "("
                       << field.type.cpp << " value) {\n"
                       << "        return " << field.member_name
                       << "_.set(std::move(value));\n"
                       << "    }\n"
                       << "\n";
                output << "    [[nodiscard]] const " << field.type.cpp << "& "
                       << field.member_name << "() const noexcept {\n"
                       << "        return " << field.member_name << "_.get();\n"
                       << "    }\n";
            }
            output << "\n    void bind(strata::host::Bindings& bindings, "
                      "const std::string_view id) const {\n"
                   << "        if (id.empty()) {\n"
                   << "            throw std::invalid_argument("
                   << cpp_quote(root.model_name + " binding id must not be empty")
                   << ");\n"
                   << "        }\n";
            for (const FieldDefinition& field : definition->fields) {
                output << "        bindings.snapshot(\n"
                       << "            std::string(id) + "
                       << cpp_quote("." + field.member_name) << ",\n"
                       << "            " << field.member_name << "_,\n"
                       << "            [](const auto& model) {\n"
                       << "                return encode_" << root.function_name << '_'
                       << field.member_name << "(model.get());\n"
                       << "            }\n"
                       << "        );\n";
            }
            output << "    }\n\nprivate:\n";
            for (const FieldDefinition& field : definition->fields) {
                output << "    strata::host::Observable<" << field.type.cpp << "> "
                       << field.member_name << "_{};\n";
            }
            output << "};\n\n";
        }
    }

    void render_actions(std::ostringstream& output) const {
        for (const ActionDefinition& action : actions_) {
            output << "struct " << action.name << " final {\n"
                   << "    static inline constexpr std::string_view id = "
                   << cpp_quote(action.id) << ";\n";
            for (const ActionParameter& parameter : action.parameters) {
                output << "    " << parameter.type.cpp << ' ' << parameter.member_name;
                if (parameter.default_value != nullptr) {
                    output << " = " << default_expression(*parameter.default_value, parameter.semantic);
                } else {
                    output << "{}";
                }
                output << ";\n";
            }
            output << "\n    [[nodiscard]] static " << action.name
                   << " decode(const strata::host::ActionEvent& event) {\n"
                   << "        if (event.id != id) {\n"
                   << "            throw std::invalid_argument(\"action event does not match "
                   << action.id << "\");\n        }\n";
            if (!action.parameters.empty()) {
                output << "        static_cast<void>(detail::decode_object(event.payload, id));\n";
            }
            output << "        " << action.name << " result;\n";
            for (const ActionParameter& parameter : action.parameters) {
                const std::string context = action.id + "." + parameter.wire_name;
                if (parameter.default_value != nullptr) {
                    output << "        if (const strata::host::Value* field = event.payload.find("
                           << cpp_quote(parameter.wire_name)
                           << "); field != nullptr && !field->is_null()) {\n"
                           << "            result." << parameter.member_name << " = "
                           << decode_expression(parameter.semantic, "*field", context)
                           << ";\n        }\n";
                } else if (!parameter.required) {
                    output << "        if (const strata::host::Value* field = event.payload.find("
                           << cpp_quote(parameter.wire_name)
                           << "); field != nullptr && !field->is_null()) {\n"
                           << "            result." << parameter.member_name << " = "
                           << decode_expression(parameter.semantic, "*field", context)
                           << ";\n        }\n";
                } else if (parameter.type.optional) {
                    output << "        {\n"
                           << "            const strata::host::Value& field = "
                              "detail::required_field(event.payload, "
                           << cpp_quote(parameter.wire_name) << ", id);\n"
                           << "            if (!field.is_null()) result." << parameter.member_name
                           << " = " << decode_expression(parameter.semantic, "field", context)
                           << ";\n        }\n";
                } else {
                    output << "        result." << parameter.member_name << " = "
                           << decode_expression(
                                  parameter.semantic,
                                  "detail::required_field(event.payload, " +
                                      cpp_quote(parameter.wire_name) + ", id)",
                                  context
                              )
                           << ";\n";
                }
            }
            output << "        return result;\n    }\n};\n\n";
        }
        output << "inline constexpr std::array<std::string_view, " << actions_.size()
               << "U> action_ids{\n";
        for (const ActionDefinition& action : actions_) {
            output << "    " << action.name << "::id,\n";
        }
        output << "};\n\n";
        if (actions_.empty()) return;
        output << "using Action = std::variant<\n";
        for (std::size_t index = 0U; index < actions_.size(); ++index) {
            output << "    " << actions_[index].name
                   << (index + 1U == actions_.size() ? "\n" : ",\n");
        }
        output << ">;\n\n[[nodiscard]] inline std::optional<Action> decode_action(\n"
                  "    const strata::host::ActionEvent& event\n) {\n";
        for (const ActionDefinition& action : actions_) {
            output << "    if (event.id == " << action.name << "::id) return Action("
                   << action.name << "::decode(event));\n";
        }
        output << "    return std::nullopt;\n}\n\n";
    }

    const JsonValue& application_schema_;
    SchemaRegistry registry_;
    std::string cpp_namespace_;
    std::string source_name_;
    std::set<std::string, std::less<>> used_names_;
    std::unordered_map<const SemanticType*, std::string> named_types_;
    std::vector<Definition> definitions_;
    std::vector<RootDefinition> roots_;
    std::vector<ActionDefinition> actions_;
};

} // namespace

std::string render_cpp_contract(
    const data::JsonValue& application_schema,
    const std::string_view cpp_namespace,
    const std::string_view source_name
) {
    return ContractGenerator(
        application_schema,
        std::string(cpp_namespace),
        std::string(source_name)
    ).render();
}

void write_cpp_contract(
    const std::filesystem::path& application_schema_path,
    const std::string_view cpp_namespace,
    const std::filesystem::path& output_path
) {
    const data::JsonValue schema = data::parse_json(read_text(application_schema_path));
    write_text(
        output_path,
        render_cpp_contract(schema, cpp_namespace, application_schema_path.filename().string())
    );
}

} // namespace strata::tools
