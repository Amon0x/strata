#include "scenario.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.h>

namespace strata::headless {
namespace {

using data::JsonValue;

[[nodiscard]] const JsonValue& required(const JsonValue& object, const std::string_view field) {
    const JsonValue* value = object.find(field);
    if (value == nullptr) {
        throw std::invalid_argument("headless scenario is missing '" + std::string(field) + "'");
    }
    return *value;
}

[[nodiscard]] const JsonValue* optional(const JsonValue& object,
                                        const std::string_view field) noexcept {
    return object.find(field);
}

[[nodiscard]] const JsonValue::Object& as_object(const JsonValue& value,
                                                 const std::string_view label) {
    const JsonValue::Object* result = value.object();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON object");
    }
    return *result;
}

[[nodiscard]] const JsonValue::Array& as_array(const JsonValue& value,
                                               const std::string_view label) {
    const JsonValue::Array* result = value.array();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON array");
    }
    return *result;
}

[[nodiscard]] std::string text(const JsonValue& value, const std::string_view label) {
    const std::string* result = value.string();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON string");
    }
    return *result;
}

[[nodiscard]] double number(const JsonValue& value, const std::string_view label) {
    if (const double* result = value.number(); result != nullptr)
        return *result;
    if (const std::int64_t* result = value.integer(); result != nullptr) {
        return static_cast<double>(*result);
    }
    throw std::invalid_argument(std::string(label) + " must be a JSON number");
}

[[nodiscard]] std::int64_t integer(const JsonValue& value, const std::string_view label) {
    const std::int64_t* result = value.integer();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON integer");
    }
    return *result;
}

[[nodiscard]] bool boolean(const JsonValue& value, const std::string_view label) {
    const bool* result = value.boolean();
    if (result == nullptr) {
        throw std::invalid_argument(std::string(label) + " must be a JSON boolean");
    }
    return *result;
}

[[nodiscard]] std::vector<std::string> strings(const JsonValue* value,
                                               const std::string_view label) {
    if (value == nullptr)
        return {};
    const JsonValue::Array& array = as_array(*value, label);
    std::vector<std::string> result;
    result.reserve(array.size());
    for (const JsonValue& entry : array)
        result.push_back(text(entry, label));
    return result;
}

[[nodiscard]] std::uint8_t hexadecimal(const char value) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
        return static_cast<std::uint8_t>(value - 'A' + 10);
    throw std::invalid_argument("headless clearColor contains a non-hexadecimal digit");
}

[[nodiscard]] std::array<std::uint8_t, 4U> color(const JsonValue& value) {
    const std::string encoded = text(value, "surface.clearColor");
    if ((encoded.size() != 7U && encoded.size() != 9U) || encoded.front() != '#') {
        throw std::invalid_argument("surface.clearColor must be #rrggbb or #rrggbbaa");
    }
    std::array<std::uint8_t, 4U> result{0U, 0U, 0U, 255U};
    for (std::size_t index = 0U; index < encoded.size() / 2U; ++index) {
        result[index] = static_cast<std::uint8_t>(hexadecimal(encoded[index * 2U + 1U]) * 16U +
                                                  hexadecimal(encoded[index * 2U + 2U]));
    }
    return result;
}

void require_relative_resource(const std::filesystem::path& path, const std::string_view label) {
    if (path.empty() || path.is_absolute()) {
        throw std::invalid_argument(std::string(label) + " must be a relative resource path");
    }
    for (const std::filesystem::path& part : path) {
        if (part == "..") {
            throw std::invalid_argument(std::string(label) + " must not escape the resource root");
        }
    }
}

[[nodiscard]] Selector selector(const JsonValue& value, const std::string_view label) {
    if (value.string() != nullptr)
        return Selector{.key = *value.string()};
    static_cast<void>(as_object(value, label));
    Selector result;
    if (const JsonValue* field = optional(value, "path"); field != nullptr) {
        result.path = text(*field, std::string(label) + ".path");
    }
    if (const JsonValue* field = optional(value, "key"); field != nullptr) {
        result.key = text(*field, std::string(label) + ".key");
    }
    if (const JsonValue* field = optional(value, "name"); field != nullptr) {
        result.name = text(*field, std::string(label) + ".name");
    }
    if (const JsonValue* field = optional(value, "role"); field != nullptr) {
        result.role = text(*field, std::string(label) + ".role");
    }
    if (const JsonValue* field = optional(value, "x"); field != nullptr) {
        result.x = number(*field, std::string(label) + ".x");
    }
    if (const JsonValue* field = optional(value, "y"); field != nullptr) {
        result.y = number(*field, std::string(label) + ".y");
    }
    const bool coordinates = result.x.has_value() || result.y.has_value();
    if (coordinates && (!result.x.has_value() || !result.y.has_value())) {
        throw std::invalid_argument(std::string(label) + " coordinates require both x and y");
    }
    if (!coordinates && !result.path.has_value() && !result.key.has_value() &&
        !result.name.has_value() && !result.role.has_value()) {
        throw std::invalid_argument(std::string(label) +
                                    " requires coordinates or a key/name/role selector");
    }
    return result;
}

[[nodiscard]] std::uint32_t modifiers(const JsonValue* value) {
    std::uint32_t result = 0U;
    for (const std::string& entry : strings(value, "key.modifiers")) {
        if (entry == "shift")
            result |= STRATA_KEY_MODIFIER_SHIFT;
        else if (entry == "control")
            result |= STRATA_KEY_MODIFIER_CONTROL;
        else if (entry == "alt")
            result |= STRATA_KEY_MODIFIER_ALT;
        else if (entry == "super")
            result |= STRATA_KEY_MODIFIER_SUPER;
        else
            throw std::invalid_argument("key.modifiers contains unknown modifier '" + entry + "'");
    }
    return result;
}

[[nodiscard]] std::int32_t pointer_button(const JsonValue* const value,
                                          const std::string_view label) {
    if (value == nullptr)
        return 0;
    const std::string name = text(*value, label);
    if (name == "primary" || name == "left")
        return 0;
    if (name == "secondary" || name == "right")
        return 1;
    if (name == "middle")
        return 2;
    throw std::invalid_argument(std::string(label) +
                                " must be 'primary'/'left', 'secondary'/'right', or 'middle'");
}

[[nodiscard]] SnapshotConfig snapshot(const JsonValue& value, const std::string_view label) {
    static_cast<void>(as_object(value, label));
    SnapshotConfig result;
    result.id = text(required(value, "id"), std::string(label) + ".id");
    result.values = required(value, "values");
    static_cast<void>(as_object(result.values, std::string(label) + ".values"));
    return result;
}

[[nodiscard]] ScenarioStep step(const JsonValue& value, const std::size_t index) {
    const JsonValue::Object& fields = as_object(value, "scenario step");
    if (fields.size() != 1U) {
        throw std::invalid_argument("headless scenario step " + std::to_string(index) +
                                    " must contain one operation");
    }
    const std::string& operation = fields.front().first;
    const JsonValue& argument = fields.front().second;
    if (operation == "advance") {
        static_cast<void>(as_object(argument, "advance"));
        const double milliseconds =
            number(required(argument, "milliseconds"), "advance.milliseconds");
        if (!std::isfinite(milliseconds) || milliseconds < 0.0 || milliseconds > 86'400'000.0) {
            throw std::invalid_argument("advance.milliseconds is outside the supported range");
        }
        std::uint32_t frames = 1U;
        if (const JsonValue* frame_value = optional(argument, "frames"); frame_value != nullptr) {
            const std::int64_t parsed = integer(*frame_value, "advance.frames");
            if (parsed <= 0 || parsed > 100'000) {
                throw std::invalid_argument("advance.frames is outside the supported range");
            }
            frames = static_cast<std::uint32_t>(parsed);
        }
        return AdvanceStep{
            static_cast<std::int64_t>(std::llround(milliseconds * 1'000'000.0)),
            frames,
        };
    }
    if (operation == "capture") {
        const std::string name = text(argument, "capture");
        if (name.empty() || !std::ranges::all_of(name, [](const char character) {
                return (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '-' ||
                       character == '_';
            })) {
            throw std::invalid_argument(
                "capture name may contain only letters, digits, '-' and '_'");
        }
        return CaptureStep{name};
    }
    if (operation == "click") {
        const Selector target = selector(argument, "click");
        return ClickStep{
            target,
            argument.object() != nullptr
                ? pointer_button(optional(argument, "button"), "click.button")
                : 0,
        };
    }
    if (operation == "move")
        return MoveStep{selector(argument, "move")};
    if (operation == "pointerDrag") {
        static_cast<void>(as_object(argument, "pointerDrag"));
        PointerDragStep result{
            selector(required(argument, "from"), "pointerDrag.from"),
            selector(required(argument, "to"), "pointerDrag.to"),
        };
        if (const JsonValue* field = optional(argument, "milliseconds"); field != nullptr) {
            const double milliseconds = number(*field, "pointerDrag.milliseconds");
            if (!std::isfinite(milliseconds) || milliseconds <= 0.0 || milliseconds > 10'000.0) {
                throw std::invalid_argument(
                    "pointerDrag.milliseconds is outside the supported range");
            }
            result.duration_nanoseconds =
                static_cast<std::int64_t>(std::llround(milliseconds * 1'000'000.0));
        }
        if (const JsonValue* field = optional(argument, "steps"); field != nullptr) {
            const std::int64_t steps = integer(*field, "pointerDrag.steps");
            if (steps <= 0 || steps > 1'000) {
                throw std::invalid_argument("pointerDrag.steps is outside the supported range");
            }
            result.steps = static_cast<std::uint32_t>(steps);
        }
        result.button = pointer_button(optional(argument, "button"), "pointerDrag.button");
        return result;
    }
    if (operation == "scroll") {
        static_cast<void>(as_object(argument, "scroll"));
        ScrollStep result;
        result.target = selector(argument, "scroll");
        if (const JsonValue* field = optional(argument, "deltaX"); field != nullptr) {
            result.delta_x = number(*field, "scroll.deltaX");
        }
        if (const JsonValue* field = optional(argument, "deltaY"); field != nullptr) {
            result.delta_y = number(*field, "scroll.deltaY");
        }
        if (result.delta_x == 0.0 && result.delta_y == 0.0) {
            throw std::invalid_argument("scroll requires a nonzero deltaX or deltaY");
        }
        return result;
    }
    if (operation == "key") {
        if (argument.string() != nullptr)
            return KeyStep{*argument.string(), 0U};
        static_cast<void>(as_object(argument, "key"));
        return KeyStep{
            text(required(argument, "name"), "key.name"),
            modifiers(optional(argument, "modifiers")),
        };
    }
    if (operation == "text")
        return TextStep{text(argument, "text")};
    if (operation == "resize") {
        static_cast<void>(as_object(argument, "resize"));
        ResizeStep result{
            number(required(argument, "width"), "resize.width"),
            number(required(argument, "height"), "resize.height"),
            1.0,
        };
        if (const JsonValue* field = optional(argument, "scale"); field != nullptr) {
            result.scale = number(*field, "resize.scale");
        }
        return result;
    }
    if (operation == "publish")
        return PublishStep{snapshot(argument, "publish")};
    throw std::invalid_argument("headless scenario step " + std::to_string(index) +
                                " has unknown operation '" + operation + "'");
}

} // namespace

ScenarioStep parse_scenario_step(const data::JsonValue& value, const std::size_t index) {
    return step(value, index);
}

Scenario load_scenario(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("could not open headless scenario: " + path.string());
    const std::string source{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    const JsonValue document = data::parse_json(source);
    static_cast<void>(as_object(document, "headless scenario"));
    Scenario result;
    const std::int64_t version = integer(required(document, "version"), "version");
    if (version != 1)
        throw std::invalid_argument("headless scenario version must be 1");

    const JsonValue& application = required(document, "application");
    static_cast<void>(as_object(application, "application"));
    result.application_id = text(required(application, "id"), "application.id");
    result.module = text(required(application, "module"), "application.module");
    if (const JsonValue* value = optional(application, "schemas"); value != nullptr) {
        result.schemas = text(*value, "application.schemas");
    }
    result.root = text(required(application, "root"), "application.root");
    result.packages = strings(optional(application, "packages"), "application.packages");
    for (const std::string& directory :
         strings(optional(application, "extensionPaths"), "application.extensionPaths")) {
        const std::filesystem::path configured(directory);
        result.extension_search_paths.push_back(
            configured.is_absolute() ? configured : path.parent_path() / configured);
    }
    result.actions = strings(optional(application, "actions"), "application.actions");
    require_relative_resource(result.module, "application.module");
    if (!result.schemas.empty())
        require_relative_resource(result.schemas, "application.schemas");

    const JsonValue& surface = required(document, "surface");
    static_cast<void>(as_object(surface, "surface"));
    result.surface_id = optional(surface, "id") != nullptr
                            ? text(*optional(surface, "id"), "surface.id")
                            : result.application_id;
    if (const JsonValue* value = optional(surface, "role"); value != nullptr) {
        result.root_role = text(*value, "surface.role");
    }
    if (const JsonValue* value = optional(surface, "backend"); value != nullptr) {
        result.render_backend = text(*value, "surface.backend");
    }
    result.width = number(required(surface, "width"), "surface.width");
    result.height = number(required(surface, "height"), "surface.height");
    if (const JsonValue* value = optional(surface, "scale"); value != nullptr) {
        result.scale = number(*value, "surface.scale");
    }
    if (const JsonValue* value = optional(surface, "reducedMotion"); value != nullptr) {
        result.reduced_motion = boolean(*value, "surface.reducedMotion");
    }
    if (const JsonValue* value = optional(surface, "clearColor"); value != nullptr) {
        result.clear_color = color(*value);
    }
    if (result.root_role != "screen" && result.root_role != "overlay") {
        throw std::invalid_argument("surface.role must be 'screen' or 'overlay'");
    }
    if (result.render_backend != "d3d11" && result.render_backend != "reference") {
        throw std::invalid_argument("surface.backend must be 'd3d11' or 'reference'");
    }
    const auto validate_dimensions = [](const double width, const double height,
                                        const double scale) {
        return std::isfinite(width) && std::isfinite(height) && std::isfinite(scale) &&
               width > 0.0 && height > 0.0 && scale > 0.0 &&
               width * scale <= static_cast<double>(std::numeric_limits<std::int32_t>::max()) &&
               height * scale <= static_cast<double>(std::numeric_limits<std::int32_t>::max());
    };
    if (!validate_dimensions(result.width, result.height, result.scale)) {
        throw std::invalid_argument("surface dimensions and scale must be positive and finite");
    }

    if (const JsonValue* values = optional(surface, "fonts"); values != nullptr) {
        for (const JsonValue& value : as_array(*values, "surface.fonts")) {
            static_cast<void>(as_object(value, "surface font"));
            FontConfig font{
                text(required(value, "id"), "font.id"),
                text(required(value, "resource"), "font.resource"),
            };
            require_relative_resource(font.resource, "font.resource");
            result.fonts.push_back(std::move(font));
        }
    }
    if (const JsonValue* values = optional(surface, "images"); values != nullptr) {
        for (const JsonValue& value : as_array(*values, "surface.images")) {
            static_cast<void>(as_object(value, "surface image"));
            ImageConfig image{
                text(required(value, "id"), "image.id"),
                text(required(value, "resource"), "image.resource"),
                1U,
            };
            if (const JsonValue* sampling = optional(value, "sampling"); sampling != nullptr) {
                const std::string name = text(*sampling, "image.sampling");
                if (name == "nearest")
                    image.sampling = 0U;
                else if (name != "linear") {
                    throw std::invalid_argument("image.sampling must be 'nearest' or 'linear'");
                }
            }
            require_relative_resource(image.resource, "image.resource");
            result.images.push_back(std::move(image));
        }
    }

    if (const JsonValue* values = optional(document, "snapshots"); values != nullptr) {
        const JsonValue::Array& entries = as_array(*values, "snapshots");
        result.snapshots.reserve(entries.size());
        for (std::size_t index = 0U; index < entries.size(); ++index) {
            result.snapshots.push_back(snapshot(entries[index], "snapshot"));
        }
    }
    if (const JsonValue* step_values = optional(document, "steps"); step_values != nullptr) {
        const JsonValue::Array& steps = as_array(*step_values, "steps");
        result.steps.reserve(steps.size());
        for (std::size_t index = 0U; index < steps.size(); ++index) {
            result.steps.push_back(step(steps[index], index));
        }
    }
    return result;
}

} // namespace strata::headless
