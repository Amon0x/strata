#include "authoring_registry.hpp"

#include <map>
#include <sstream>
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
    if (child == nullptr)
        throw std::runtime_error("registry is missing field '" + std::string(field) + "'");
    return *child;
}

[[nodiscard]] const std::string& string_field(const JsonValue& value,
                                              const std::string_view field) {
    const std::string* text = required(value, field).string();
    if (text == nullptr)
        throw std::runtime_error("registry field must be a string");
    return *text;
}

[[nodiscard]] const JsonValue::Array& array_field(const JsonValue& value,
                                                  const std::string_view field) {
    const JsonValue::Array* values = required(value, field).array();
    if (values == nullptr)
        throw std::runtime_error("registry field must be an array");
    return *values;
}

[[nodiscard]] bool bool_field(const JsonValue& value, const std::string_view field) {
    const bool* result = required(value, field).boolean();
    if (result == nullptr)
        throw std::runtime_error("registry field must be a boolean");
    return *result;
}

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> fields) {
    return JsonValue(JsonValue::Object(fields));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

class TypeResolver final {
  public:
    explicit TypeResolver(const JsonValue& registry) {
        for (const JsonValue& type : array_field(registry, "types")) {
            types_.emplace(string_field(type, "id"), &required(type, "definition"));
        }
    }

    [[nodiscard]] const JsonValue& resolve(const JsonValue& type) const {
        const JsonValue* reference = type.find("ref");
        if (reference == nullptr)
            return type;
        if (reference->string() == nullptr)
            throw std::runtime_error("registry type reference is invalid");
        const auto found = types_.find(*reference->string());
        if (found == types_.end())
            throw std::runtime_error("registry type reference is unresolved");
        return *found->second;
    }

    [[nodiscard]] std::string label(const JsonValue& raw) const {
        const JsonValue& type = resolve(raw);
        const std::string& kind = string_field(type, "kind");
        if (kind == "enum")
            return "one of (" + join_strings(array_field(type, "values")) + ')';
        if (kind == "list")
            return "list of " + label(required(type, "element"));
        if (kind == "map")
            return "map of " + label(required(type, "value"));
        if (kind == "collection")
            return "collection of " + label(required(type, "item"));
        if (kind == "componentTemplate") {
            const JsonValue* parameters = type.find("parameters");
            if (parameters == nullptr || parameters->array() == nullptr ||
                parameters->array()->empty()) {
                return "component template";
            }
            std::vector<std::string> labels;
            labels.reserve(parameters->array()->size());
            for (const JsonValue& parameter : *parameters->array()) {
                std::string parameter_label =
                    string_field(parameter, "name") + ": " +
                    component_parameter_label(required(parameter, "type"));
                const JsonValue* nullable = parameter.find("nullable");
                if (nullable != nullptr && nullable->boolean() != nullptr && *nullable->boolean()) {
                    parameter_label += "?";
                }
                labels.push_back(std::move(parameter_label));
            }
            return "component (" + join(labels) + ')';
        }
        if (kind == "union") {
            std::vector<std::string> labels;
            for (const JsonValue& option : array_field(type, "options"))
                labels.push_back(label(option));
            return "one of (" + join(labels) + ')';
        }
        const JsonValue* named = type.find("label");
        return named != nullptr && named->string() != nullptr ? *named->string() : kind;
    }

    [[nodiscard]] std::vector<std::string> enum_values(const JsonValue& raw) const {
        const JsonValue& type = resolve(raw);
        const JsonValue* kind = type.find("kind");
        if (kind == nullptr || kind->string() == nullptr || *kind->string() != "enum")
            return {};
        std::vector<std::string> result;
        for (const JsonValue& value : array_field(type, "values")) {
            if (value.string() == nullptr)
                throw std::runtime_error("registry enum value is invalid");
            result.push_back(*value.string());
        }
        return result;
    }

  private:
    [[nodiscard]] std::string component_parameter_label(const JsonValue& raw) const {
        const JsonValue& type = resolve(raw);
        const JsonValue* kind = type.find("kind");
        const JsonValue* named = type.find("label");
        if (kind != nullptr && kind->string() != nullptr && *kind->string() == "map" &&
            named != nullptr && named->string() != nullptr) {
            return *named->string();
        }
        return label(raw);
    }

    [[nodiscard]] static std::string join(const std::vector<std::string>& values) {
        std::string result;
        for (const std::string& value : values) {
            if (!result.empty())
                result.append(", ");
            result.append(value);
        }
        return result;
    }

    [[nodiscard]] static std::string join_strings(const JsonValue::Array& values) {
        std::vector<std::string> strings;
        for (const JsonValue& value : values) {
            if (value.string() == nullptr)
                throw std::runtime_error("registry string list is invalid");
            strings.push_back(*value.string());
        }
        return join(strings);
    }

    std::map<std::string, const JsonValue*> types_;
};

[[nodiscard]] std::string markdown(std::string value) {
    std::size_t offset = 0U;
    while ((offset = value.find('|', offset)) != std::string::npos) {
        value.insert(offset, "\\");
        offset += 2U;
    }
    return value;
}

[[nodiscard]] std::string string_list(const JsonValue& owner, const std::string_view field) {
    std::string result;
    for (const JsonValue& value : array_field(owner, field)) {
        if (value.string() == nullptr)
            throw std::runtime_error("registry string list is invalid");
        if (!result.empty())
            result.append(", ");
        result.append(*value.string());
    }
    return result.empty() ? "—" : result;
}

void append_parameters(std::ostringstream& output, const JsonValue::Array& parameters,
                       const TypeResolver& types) {
    output << "| Name | Type | Required | Nullable | Aliases |\n"
              "|---|---|---:|---:|---|\n";
    for (const JsonValue& parameter : parameters) {
        output << "| `" << string_field(parameter, "name") << "` | "
               << markdown(types.label(required(parameter, "type"))) << " | "
               << (bool_field(parameter, "required") ? "true" : "false") << " | "
               << (bool_field(parameter, "nullable") ? "true" : "false") << " | "
               << markdown(string_list(parameter, "aliases")) << " |\n";
    }
    output << '\n';
}

[[nodiscard]] std::vector<JsonValue> strings_json(const std::vector<std::string>& values) {
    std::vector<JsonValue> result;
    result.reserve(values.size());
    for (const std::string& value : values)
        result.emplace_back(value);
    return result;
}

[[nodiscard]] JsonValue completion_parameter(const JsonValue& parameter,
                                             const TypeResolver& types) {
    return object({
        {"aliases", required(parameter, "aliases")},
        {"enumValues", array(strings_json(types.enum_values(required(parameter, "type"))))},
        {"name", required(parameter, "name")},
        {"nullable", required(parameter, "nullable")},
        {"required", required(parameter, "required")},
        {"type", JsonValue(types.label(required(parameter, "type")))},
    });
}

[[nodiscard]] std::vector<JsonValue> completion_parameters(const JsonValue::Array& parameters,
                                                           const TypeResolver& types) {
    std::vector<JsonValue> result;
    result.reserve(parameters.size());
    for (const JsonValue& parameter : parameters)
        result.push_back(completion_parameter(parameter, types));
    return result;
}

[[nodiscard]] JsonValue named_types(const JsonValue& registry, const std::string_view field,
                                    const TypeResolver& types) {
    std::vector<JsonValue> result;
    for (const JsonValue& value : array_field(registry, field)) {
        result.push_back(object({
            {"name", required(value, "name")},
            {"nullable",
             value.find("nullable") == nullptr ? JsonValue(false) : required(value, "nullable")},
            {"type", JsonValue(types.label(required(value, "type")))},
        }));
    }
    return array(std::move(result));
}

} // namespace

std::string render_reference(const JsonValue& registry) {
    const TypeResolver types(registry);
    std::ostringstream output;
    output << "# `.strata` generated reference\n\n"
              "> Generated by the native authoring tool from Strata's built-in catalog. Do not "
              "edit by hand.\n\n"
              "## Widgets\n\n";
    for (const JsonValue& widget : array_field(registry, "widgets")) {
        output << "### `" << string_field(widget, "name") << "`\n\n"
               << "Children: `" << (bool_field(widget, "allowsChildren") ? "true" : "false")
               << "` · Capabilities: " << markdown(string_list(widget, "capabilities")) << "\n\n";
        append_parameters(output, array_field(widget, "parameters"), types);
        const JsonValue::Array& events = array_field(widget, "events");
        if (!events.empty()) {
            output << "| Event | Callback | Phase | Contract |\n"
                      "|---|---|---|---|\n";
            for (const JsonValue& event : events) {
                const JsonValue* phase = event.find("phase");
                output << "| `" << string_field(event, "name") << "` | `"
                       << string_field(event, "callbackParameter") << "` | "
                       << (phase != nullptr && phase->string() != nullptr
                               ? markdown(*phase->string())
                               : "unspecified")
                       << " | " << markdown(string_field(event, "description")) << " |\n";
            }
            output << '\n';
        }
        output << "Retained state entries: " << array_field(widget, "retainedState").size()
               << " · Binding shorthands: " << array_field(widget, "bindings").size() << "\n\n";
    }

    output << "## Behaviors\n\n";
    for (const JsonValue& behavior : array_field(registry, "behaviors")) {
        output << "- `" << string_field(behavior, "id")
               << "`: " << markdown(types.label(required(behavior, "options"))) << '\n';
    }
    output << "\n## Declarative actions\n\n";
    for (const JsonValue& action : array_field(registry, "actions")) {
        output << "### `" << string_field(action, "id") << "`\n\n"
               << string_field(action, "summary") << " Payload: `"
               << string_field(action, "payloadContract") << "`. Dispatch: `"
               << string_field(action, "dispatchPolicy") << "`.\n\n";
        append_parameters(output, array_field(action, "parameters"), types);
    }

    constexpr std::pair<std::string_view, std::string_view> sections[]{
        {"Style properties", "styleProperties"},
        {"Layout properties", "layoutProperties"},
        {"Animation timing properties", "animationTimingProperties"},
        {"Animatable properties", "animationProperties"},
    };
    for (const auto& [title, field] : sections) {
        output << "## " << title << "\n\n| Name | Type |\n|---|---|\n";
        for (const JsonValue& value : array_field(registry, field)) {
            output << "| `" << string_field(value, "name") << "` | "
                   << markdown(types.label(required(value, "type"))) << " |\n";
        }
        output << '\n';
    }

    output << "## Expression helpers\n\n| Name | Returns | Capabilities |\n|---|---|---|\n";
    for (const JsonValue& helper : array_field(registry, "helpers")) {
        output << "| `" << string_field(helper, "name") << "` | "
               << markdown(types.label(required(helper, "returnType"))) << " | "
               << markdown(string_list(helper, "capabilities")) << " |\n";
    }
    output << "\n## Materials\n\n";
    for (const JsonValue& material : array_field(registry, "materials")) {
        output << "- `" << string_field(material, "id") << "`\n";
    }
    output << "\n## Effects\n\n";
    for (const JsonValue& effect : array_field(registry, "effects")) {
        output << "- `" << string_field(effect, "name") << "`\n";
    }
    return output.str();
}

std::string render_completions(const JsonValue& registry) {
    const TypeResolver types(registry);
    std::vector<JsonValue> widgets;
    for (const JsonValue& widget : array_field(registry, "widgets")) {
        widgets.push_back(object({
            {"capabilities", required(widget, "capabilities")},
            {"allowsChildren", required(widget, "allowsChildren")},
            {"bindings", required(widget, "bindings")},
            {"events", required(widget, "events")},
            {"name", required(widget, "name")},
            {"parameters", array(completion_parameters(array_field(widget, "parameters"), types))},
            {"retainedState", required(widget, "retainedState")},
        }));
    }
    std::vector<JsonValue> behaviors;
    for (const JsonValue& behavior : array_field(registry, "behaviors")) {
        behaviors.push_back(object({
            {"id", required(behavior, "id")},
            {"optionsType", JsonValue(types.label(required(behavior, "options")))},
        }));
    }
    std::vector<JsonValue> actions;
    for (const JsonValue& action : array_field(registry, "actions")) {
        actions.push_back(object({
            {"dispatchPolicy", required(action, "dispatchPolicy")},
            {"id", required(action, "id")},
            {"parameters", array(completion_parameters(array_field(action, "parameters"), types))},
            {"payloadContract", required(action, "payloadContract")},
            {"summary", required(action, "summary")},
        }));
    }
    std::vector<JsonValue> helpers;
    for (const JsonValue& helper : array_field(registry, "helpers")) {
        helpers.push_back(object({
            {"capabilities", required(helper, "capabilities")},
            {"name", required(helper, "name")},
            {"parameters", array(completion_parameters(array_field(helper, "parameters"), types))},
            {"returnType", JsonValue(types.label(required(helper, "returnType")))},
        }));
    }
    return data::encode_canonical_json(object({
        {"actions", array(std::move(actions))},
        {"animationProperties", named_types(registry, "animationProperties", types)},
        {"animationTimingProperties", named_types(registry, "animationTimingProperties", types)},
        {"behaviors", array(std::move(behaviors))},
        {"format", JsonValue("strata.completions")},
        {"helpers", array(std::move(helpers))},
        {"layoutProperties", named_types(registry, "layoutProperties", types)},
        {"registry", required(registry, "id")},
        {"styleProperties", named_types(registry, "styleProperties", types)},
        {"version", JsonValue(std::int64_t{2})},
        {"widgets", array(std::move(widgets))},
    }));
}

} // namespace strata::tools
