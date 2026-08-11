#include "abi_extension.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "core/runtime.hpp"
#include "core/utf8.hpp"
#include "data/json.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/text.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"

namespace {

using strata::runtime::Value;

[[nodiscard]] bool valid_view(const strata_string_view value, const bool allow_empty) noexcept {
    return (value.data != nullptr || value.size == 0U) && (allow_empty || value.size != 0U);
}

[[nodiscard]] std::string copied_string(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] std::string checked_string(const strata_string_view value, const bool allow_empty,
                                         const char* const name) {
    if (!valid_view(value, allow_empty))
        throw std::invalid_argument(std::string(name) + " view is invalid");
    std::string result = copied_string(value);
    if (!strata::core::valid_utf8(result)) {
        throw std::invalid_argument(std::string(name) + " must be valid UTF-8");
    }
    return result;
}

using ValueFields = std::map<std::string, Value, std::less<>>;

[[nodiscard]] ValueFields parsed_fields(const strata_string_view json, const char* const name) {
    if (json.size == 0U)
        return {};
    const std::string source = checked_string(json, false, name);
    const strata::data::JsonValue parsed = strata::data::parse_json(source);
    const strata::data::JsonValue::Object* object = parsed.object();
    if (object == nullptr)
        throw std::invalid_argument(std::string(name) + " must contain one JSON object");
    ValueFields result;
    for (const auto& [field, value] : *object) {
        result.emplace(field, strata::runtime::value_from_json(value));
    }
    return result;
}

[[nodiscard]] strata::ui::Rect rect(const strata_rect value) noexcept {
    return strata::ui::Rect{value.x, value.y, value.width, value.height};
}

[[nodiscard]] strata_rect rect(const strata::ui::Rect value) noexcept {
    return strata_rect{value.x, value.y, value.width, value.height};
}

[[nodiscard]] strata::ui::RenderColor color(const strata_color value) noexcept {
    return strata::ui::RenderColor{value.red, value.green, value.blue, value.alpha};
}

[[nodiscard]] strata::ui::RenderBorder border(const strata_border value) noexcept {
    return strata::ui::RenderBorder{value.width, color(value.color), true};
}

[[nodiscard]] strata::ui::TextureRegion region(const strata_texture_region value) noexcept {
    return strata::ui::TextureRegion{value.u, value.v, value.width, value.height};
}

[[nodiscard]] strata::ui::Edges edges(const strata_edges value) noexcept {
    return strata::ui::Edges{value.left, value.top, value.right, value.bottom};
}

/** Copies one material state; an incomplete or unnamed material leaves the draw unmaterialized. */
[[nodiscard]] std::optional<strata::ui::MaterialState>
material_state(const strata_material_state* const value) {
    if (value == nullptr)
        return std::nullopt;
    if (value->struct_size < sizeof(strata_material_state) || !valid_view(value->id, false) ||
        !valid_view(value->blend_mode, true) || !std::isfinite(value->opacity) ||
        (value->parameters == nullptr && value->parameter_count != 0U)) {
        return std::nullopt;
    }
    strata::ui::MaterialState state;
    state.id = checked_string(value->id, false, "material id");
    if (value->blend_mode.size != 0U) {
        state.blend_mode = checked_string(value->blend_mode, false, "material blend mode");
    }
    state.opacity = value->opacity;
    state.parameters.reserve(value->parameter_count);
    for (std::size_t index = 0U; index < value->parameter_count; ++index) {
        const strata_material_parameter parameter = value->parameters[index];
        if (parameter.struct_size < sizeof(strata_material_parameter))
            return std::nullopt;
        std::string name = checked_string(parameter.name, false, "material parameter name");
        Value parameter_value;
        switch (parameter.kind) {
        case STRATA_MATERIAL_PARAMETER_NUMBER:
            if (!std::isfinite(parameter.number))
                return std::nullopt;
            parameter_value = Value(parameter.number);
            break;
        case STRATA_MATERIAL_PARAMETER_BOOLEAN:
            parameter_value = Value(parameter.boolean_value != 0U);
            break;
        case STRATA_MATERIAL_PARAMETER_TEXT:
            parameter_value =
                Value(checked_string(parameter.text, true, "material parameter text"));
            break;
        case STRATA_MATERIAL_PARAMETER_COLOR:
            parameter_value = Value(strata::runtime::ColorValue{
                parameter.color.red,
                parameter.color.green,
                parameter.color.blue,
                parameter.color.alpha,
            });
            break;
        default:
            return std::nullopt;
        }
        state.parameters.push_back(strata::ui::MaterialParameter{
            std::move(name),
            std::move(parameter_value),
        });
    }
    return state;
}

[[nodiscard]] std::uint32_t modifiers(const strata::ui::KeyModifiers& value) noexcept {
    std::uint32_t result = 0U;
    if (value.shift)
        result |= STRATA_KEY_MODIFIER_SHIFT;
    if (value.control)
        result |= STRATA_KEY_MODIFIER_CONTROL;
    if (value.alt)
        result |= STRATA_KEY_MODIFIER_ALT;
    if (value.super_key)
        result |= STRATA_KEY_MODIFIER_SUPER;
    return result;
}

[[nodiscard]] std::uint32_t event_kind(const strata::ui::PointerEventType value) noexcept {
    switch (value) {
    case strata::ui::PointerEventType::move:
        return STRATA_INPUT_POINTER_MOVE;
    case strata::ui::PointerEventType::press:
        return STRATA_INPUT_POINTER_PRESS;
    case strata::ui::PointerEventType::release:
        return STRATA_INPUT_POINTER_RELEASE;
    case strata::ui::PointerEventType::cancel:
        return STRATA_INPUT_POINTER_CANCEL;
    }
    return STRATA_INPUT_POINTER_CANCEL;
}

[[nodiscard]] std::uint32_t event_phase(const strata::ui::BehaviorInputEventPhase value) noexcept {
    switch (value) {
    case strata::ui::BehaviorInputEventPhase::capture:
        return STRATA_BEHAVIOR_EVENT_CAPTURE;
    case strata::ui::BehaviorInputEventPhase::target:
        return STRATA_BEHAVIOR_EVENT_TARGET;
    case strata::ui::BehaviorInputEventPhase::bubble:
        return STRATA_BEHAVIOR_EVENT_BUBBLE;
    case strata::ui::BehaviorInputEventPhase::advance:
    case strata::ui::BehaviorInputEventPhase::after_layout:
        return STRATA_BEHAVIOR_EVENT_TARGET;
    }
    return STRATA_BEHAVIOR_EVENT_TARGET;
}

[[nodiscard]] Value json_value(const strata_string_view json, const bool empty_is_null) {
    if (json.size == 0U && empty_is_null)
        return Value{};
    return strata::runtime::value_from_json(
        strata::data::parse_json(checked_string(json, false, "extension JSON")));
}

[[nodiscard]] strata_widget_invalidation
retained_invalidation(const strata_widget_invalidation value, const std::string_view field) {
    switch (value) {
    case STRATA_WIDGET_INVALIDATION_PROPERTIES:
    case STRATA_WIDGET_INVALIDATION_LAYOUT:
    case STRATA_WIDGET_INVALIDATION_STYLE:
    case STRATA_WIDGET_INVALIDATION_TEXT:
    case STRATA_WIDGET_INVALIDATION_SEMANTICS:
    case STRATA_WIDGET_INVALIDATION_PAINT:
    case STRATA_WIDGET_INVALIDATION_INPUT:
        return value;
    default:
        break;
    }
    throw std::invalid_argument("retained field '" + std::string(field) +
                                "' declares an unknown invalidation class");
}

[[nodiscard]] strata::ui::DirtyReason dirty_invalidation(const strata_widget_invalidation value) {
    switch (value) {
    case STRATA_WIDGET_INVALIDATION_PROPERTIES:
        return strata::ui::DirtyReason::properties;
    case STRATA_WIDGET_INVALIDATION_LAYOUT:
        return strata::ui::DirtyReason::layout;
    case STRATA_WIDGET_INVALIDATION_STYLE:
        return strata::ui::DirtyReason::style;
    case STRATA_WIDGET_INVALIDATION_TEXT:
        return strata::ui::DirtyReason::text;
    case STRATA_WIDGET_INVALIDATION_SEMANTICS:
        return strata::ui::DirtyReason::semantics;
    case STRATA_WIDGET_INVALIDATION_PAINT:
        return strata::ui::DirtyReason::paint;
    case STRATA_WIDGET_INVALIDATION_INPUT:
        return strata::ui::DirtyReason::input;
    default:
        break;
    }
    throw std::invalid_argument("paint and input invalidation do not use the dirty scheduler");
}

/** Shared buffer-out convention for every extension text read. */
[[nodiscard]] strata_result copied_text(const std::string* const value, char* const buffer,
                                        const std::size_t capacity, std::size_t* const out_length) {
    if (out_length == nullptr || (buffer == nullptr && capacity != 0U)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    if (value == nullptr) {
        *out_length = 0U;
        return strata::core::result(STRATA_STATUS_NOT_FOUND);
    }
    *out_length = value->size();
    if (value->size() > capacity)
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    if (!value->empty())
        std::copy(value->begin(), value->end(), buffer);
    return strata::core::result(STRATA_STATUS_OK);
}

template <typename Context>
[[nodiscard]] const Value* declared_retained(const Context* const context,
                                             const strata_string_view name) {
    if (context == nullptr || context->scope == nullptr || context->fields == nullptr)
        return nullptr;
    const std::string field = checked_string(name, false, "retained field");
    return context->fields->find(field) != nullptr ? context->scope->retained(field) : nullptr;
}

template <typename Context>
[[nodiscard]] std::span<const std::byte> declared_retained_bytes(const Context* const context,
                                                                 const strata_string_view name) {
    if (context == nullptr || context->scope == nullptr || context->fields == nullptr)
        return {};
    const std::string field = checked_string(name, false, "retained field");
    return context->fields->find(field) != nullptr ? context->scope->node().retained_bytes(field)
                                                   : std::span<const std::byte>{};
}

template <typename Context>
[[nodiscard]] strata_result copy_retained_bytes(const Context* const context,
                                                const strata_string_view name, void* const buffer,
                                                const std::size_t size) {
    if (buffer == nullptr || size == 0U) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const std::span<const std::byte> value = declared_retained_bytes(context, name);
        if (value.empty())
            return strata::core::result(STRATA_STATUS_NOT_FOUND);
        if (value.size() != size)
            return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
        std::memcpy(buffer, value.data(), value.size());
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

[[nodiscard]] strata_result set_declared_retained(strata_widget_input_context* const context,
                                                  const strata_string_view name, Value value) {
    if (context == nullptr || context->scope == nullptr || context->fields == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        std::string field = checked_string(name, false, "retained field");
        const strata_widget_invalidation* const field_invalidation = context->fields->find(field);
        if (field_invalidation == nullptr)
            return strata::core::result(STRATA_STATUS_NOT_FOUND);
        if (*field_invalidation == STRATA_WIDGET_INVALIDATION_PAINT) {
            context->scope->set_paint(std::move(field), std::move(value));
        } else if (*field_invalidation == STRATA_WIDGET_INVALIDATION_INPUT) {
            context->scope->set_input(std::move(field), std::move(value));
        } else {
            context->scope->set_retained(std::move(field), std::move(value),
                                         dirty_invalidation(*field_invalidation));
        }
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

[[nodiscard]] strata_result set_declared_retained_bytes(strata_widget_input_context* const context,
                                                        const strata_string_view name,
                                                        const void* const value,
                                                        const std::size_t size) {
    if (context == nullptr || context->scope == nullptr || context->fields == nullptr ||
        value == nullptr || size == 0U) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        std::string field = checked_string(name, false, "retained field");
        const strata_widget_invalidation* const field_invalidation = context->fields->find(field);
        if (field_invalidation == nullptr)
            return strata::core::result(STRATA_STATUS_NOT_FOUND);
        const std::span bytes(static_cast<const std::byte*>(value), size);
        if (*field_invalidation == STRATA_WIDGET_INVALIDATION_INPUT) {
            context->scope->set_input_bytes(std::move(field), bytes);
        } else {
            context->scope->set_retained_bytes(std::move(field), bytes,
                                               dirty_invalidation(*field_invalidation));
        }
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

template <typename Scope>
[[nodiscard]] strata_result emit_action_json(Scope& scope, const strata_string_view action_id,
                                             const strata_string_view payload_json,
                                             const strata_string_view event_kind_value,
                                             const strata_string_view event_value_json) {
    const std::string id = checked_string(action_id, false, "extension action id");
    const std::string kind = checked_string(event_kind_value, false, "extension event kind");
    const bool emitted = scope.emit_action(id, json_value(payload_json, true), kind,
                                           json_value(event_value_json, true));
    return strata::core::result(emitted ? STRATA_STATUS_OK : STRATA_STATUS_NOT_FOUND);
}

[[nodiscard]] const Value* runtime_value(const strata_widget_value* const value) noexcept {
    return reinterpret_cast<const Value*>(value);
}

[[nodiscard]] const strata_widget_value* public_value(const Value* const value) noexcept {
    return reinterpret_cast<const strata_widget_value*>(value);
}

} // namespace

namespace strata::abi_detail {

void ExtensionRetainedFields::declare(std::string name,
                                      const strata_widget_invalidation field_invalidation) {
    fields_.emplace_back(std::move(name), field_invalidation);
}

const strata_widget_invalidation*
ExtensionRetainedFields::find(const std::string_view name) const noexcept {
    for (const auto& [field, field_invalidation] : fields_) {
        if (field == name)
            return &field_invalidation;
    }
    return nullptr;
}

ExtensionRegistries extension_registries(const strata_surface_extension_bundle* const bundle) {
    ExtensionRegistries result;
    if (bundle == nullptr)
        return result;
    const bool has_widget_inputs =
        bundle->struct_size >= STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_2_SIZE;
    const bool has_widget_scrolls = bundle->struct_size >= sizeof(strata_surface_extension_bundle);
    if (bundle->struct_size < STRATA_SURFACE_EXTENSION_BUNDLE_VERSION_1_SIZE ||
        (bundle->widgets == nullptr && bundle->widget_count != 0U) ||
        (bundle->behaviors == nullptr && bundle->behavior_count != 0U) ||
        (has_widget_inputs && bundle->widget_inputs == nullptr &&
         bundle->widget_input_count != 0U) ||
        (has_widget_scrolls && bundle->widget_scrolls == nullptr &&
         bundle->widget_scroll_count != 0U)) {
        throw std::invalid_argument("surface extension bundle is incomplete");
    }
    constexpr std::size_t maximum_extensions = 256U;
    if (bundle->widget_count > maximum_extensions || bundle->behavior_count > maximum_extensions ||
        (has_widget_inputs && bundle->widget_input_count > maximum_extensions) ||
        (has_widget_scrolls && bundle->widget_scroll_count > maximum_extensions)) {
        throw std::invalid_argument("surface extension bundle exceeds its bounded lifecycle count");
    }
    std::map<std::string, strata_widget_input_extension, std::less<>> widget_inputs;
    if (has_widget_inputs) {
        for (std::size_t index = 0U; index < bundle->widget_input_count; ++index) {
            const strata_widget_input_extension descriptor = bundle->widget_inputs[index];
            if (descriptor.struct_size < sizeof(strata_widget_input_extension)) {
                throw std::invalid_argument("widget input extension descriptor is incomplete");
            }
            std::string type =
                checked_string(descriptor.type, false, "widget input extension type");
            if (!widget_inputs.emplace(std::move(type), descriptor).second) {
                throw std::invalid_argument(
                    "widget input extension declares one widget type twice");
            }
        }
    }
    std::map<std::string, strata_widget_scroll_extension, std::less<>> widget_scrolls;
    if (has_widget_scrolls) {
        for (std::size_t index = 0U; index < bundle->widget_scroll_count; ++index) {
            const strata_widget_scroll_extension descriptor = bundle->widget_scrolls[index];
            if (descriptor.struct_size < sizeof(strata_widget_scroll_extension)) {
                throw std::invalid_argument("widget scroll extension descriptor is incomplete");
            }
            std::string type =
                checked_string(descriptor.type, false, "widget scroll extension type");
            if (!widget_scrolls.emplace(std::move(type), descriptor).second) {
                throw std::invalid_argument(
                    "widget scroll extension declares one widget type twice");
            }
        }
    }
    for (std::size_t index = 0U; index < bundle->widget_count; ++index) {
        const strata_widget_extension descriptor = bundle->widgets[index];
        if (descriptor.struct_size < STRATA_WIDGET_EXTENSION_VERSION_1_SIZE) {
            throw std::invalid_argument("widget extension descriptor is incomplete");
        }
        const bool has_subtargets =
            descriptor.struct_size >= STRATA_WIDGET_EXTENSION_VERSION_2_SIZE &&
            descriptor.subtargets != nullptr;
        const bool has_frame = descriptor.struct_size >= sizeof(strata_widget_extension) &&
                               descriptor.frame != nullptr;
        const std::string type = checked_string(descriptor.type, false, "widget extension type");
        if (result.widgets.find(type) != nullptr) {
            throw std::invalid_argument("widget extension conflicts with lifecycle '" + type + "'");
        }
        constexpr std::uint64_t known_flags =
            STRATA_WIDGET_EXTENSION_FOCUSABLE | STRATA_WIDGET_EXTENSION_DETACHED_OVERLAY |
            STRATA_WIDGET_EXTENSION_DEPENDS_ON_MOTION | STRATA_WIDGET_EXTENSION_DEPENDS_ON_STATUS;
        if ((descriptor.flags & ~known_flags) != 0U) {
            throw std::invalid_argument("widget extension '" + type + "' declares unknown flags");
        }
        if ((descriptor.flags & STRATA_WIDGET_EXTENSION_DETACHED_OVERLAY) != 0U &&
            descriptor.overlay == nullptr) {
            throw std::invalid_argument("widget extension '" + type +
                                        "' declares a detached overlay without an overlay hook");
        }
        if (descriptor.retained_fields == nullptr && descriptor.retained_field_count != 0U) {
            throw std::invalid_argument("widget extension '" + type +
                                        "' declares retained fields without a table");
        }
        auto fields = std::make_shared<ExtensionRetainedFields>();
        for (std::size_t field = 0U; field < descriptor.retained_field_count; ++field) {
            const strata_widget_retained_field entry = descriptor.retained_fields[field];
            if (entry.struct_size < sizeof(strata_widget_retained_field)) {
                throw std::invalid_argument("widget extension '" + type +
                                            "' has an incomplete retained field descriptor");
            }
            std::string name = checked_string(entry.name, false, "widget retained field name");
            if (fields->find(name) != nullptr) {
                throw std::invalid_argument("widget extension '" + type +
                                            "' declares retained field '" + name + "' twice");
            }
            const strata_widget_invalidation field_invalidation =
                retained_invalidation(entry.invalidation, name);
            fields->declare(std::move(name), field_invalidation);
        }
        const ValueFields description =
            parsed_fields(descriptor.description_properties_json, "widget description defaults");
        const ValueFields layout =
            parsed_fields(descriptor.layout_defaults_json, "widget layout defaults");
        const std::string popup_retained =
            checked_string(descriptor.popup_retained, true, "widget popup retained name");
        if (!popup_retained.empty() && fields->find(popup_retained) == nullptr) {
            throw std::invalid_argument("widget extension '" + type +
                                        "' names undeclared popup retained field '" +
                                        popup_retained + "'");
        }
        strata::ui::WidgetLifecycle lifecycle;
        lifecycle.type = type;
        if (!layout.empty()) {
            lifecycle.describe.layout_defaults =
                [layout](strata::ui::WidgetLayoutDefaultsScope& scope) {
                    for (const auto& [name, value] : layout)
                        scope.set(name, value);
                };
        }
        if (!description.empty()) {
            lifecycle.describe.expand = [description](strata::ui::WidgetDescriptionScope& scope) {
                for (const auto& [name, value] : description) {
                    scope.description().properties.try_emplace(
                        name, strata::runtime::ExpressionValue(value));
                }
            };
        }
        lifecycle.input.focusable = (descriptor.flags & STRATA_WIDGET_EXTENSION_FOCUSABLE) != 0U;
        lifecycle.input.popup_retained = popup_retained;
        if (descriptor.activate != nullptr) {
            lifecycle.input.click = [descriptor, fields](strata::ui::WidgetInputScope& scope) {
                strata_widget_input_context context{&scope, fields.get()};
                return descriptor.activate(descriptor.user_data, &context) ==
                       STRATA_EXTENSION_INPUT_CONSUMED;
            };
        }
        if (descriptor.key != nullptr) {
            lifecycle.input.key = [descriptor, fields](strata::ui::WidgetInputScope& scope) {
                const strata_widget_key_event event{
                    sizeof(strata_widget_key_event),
                    strata_string_view{scope.key().data(), scope.key().size()},
                    modifiers(scope.modifiers()),
                    0U,
                };
                strata_widget_input_context context{&scope, fields.get()};
                return descriptor.key(descriptor.user_data, &context, &event) ==
                       STRATA_EXTENSION_INPUT_CONSUMED;
            };
        }
        if (const auto input = widget_inputs.find(type); input != widget_inputs.end()) {
            const strata_widget_input_extension input_descriptor = input->second;
            if (input_descriptor.pointer != nullptr) {
                lifecycle.input.pointer = [input_descriptor,
                                           fields](strata::ui::WidgetInputScope& scope) {
                    const strata::ui::PointerInputEvent* pointer = scope.pointer();
                    if (pointer == nullptr)
                        return false;
                    const strata::ui::LayoutRecord* layout = scope.layout();
                    const strata::ui::Rect bounds =
                        layout != nullptr ? layout->bounds : strata::ui::Rect{};
                    const strata::ui::WidgetSubtarget* const subtarget = scope.subtarget();
                    const strata_widget_pointer_event event{
                        sizeof(strata_widget_pointer_event),
                        event_kind(pointer->type),
                        event_phase(scope.phase()),
                        modifiers(scope.modifiers()),
                        pointer->pointer_id,
                        pointer->button,
                        pointer->position.x,
                        pointer->position.y,
                        pointer->position.x - bounds.x,
                        pointer->position.y - bounds.y,
                        pointer->delta.x,
                        pointer->delta.y,
                        pointer->timestamp_nanos > 0 ? pointer->timestamp_nanos
                                                     : scope.frame_time_nanos(),
                        scope.pointer_target() == &scope.node() ? 1U : 0U,
                        0U,
                        subtarget != nullptr
                            ? strata_string_view{subtarget->id.data(), subtarget->id.size()}
                            : strata_string_view{},
                        subtarget != nullptr ? subtarget->index : SIZE_MAX,
                    };
                    strata_widget_input_context context{&scope, fields.get()};
                    return input_descriptor.pointer(input_descriptor.user_data, &context, &event) ==
                           STRATA_EXTENSION_INPUT_CONSUMED;
                };
            }
            widget_inputs.erase(input);
        }
        if (const auto scroll = widget_scrolls.find(type); scroll != widget_scrolls.end()) {
            const strata_widget_scroll_extension scroll_descriptor = scroll->second;
            if (scroll_descriptor.scroll != nullptr) {
                lifecycle.input.event = [scroll_descriptor,
                                         fields](strata::ui::WidgetInputScope& scope) {
                    const strata::ui::ScrollInputEvent* input = scope.scroll();
                    if (input == nullptr)
                        return false;
                    const strata::ui::LayoutRecord* layout = scope.layout();
                    const strata::ui::Rect bounds =
                        layout != nullptr ? layout->bounds : strata::ui::Rect{};
                    const strata_widget_scroll_event event{
                        sizeof(strata_widget_scroll_event),
                        event_phase(scope.phase()),
                        modifiers(input->modifiers),
                        input->position.x,
                        input->position.y,
                        input->position.x - bounds.x,
                        input->position.y - bounds.y,
                        input->delta_x,
                        input->delta_y,
                        scope.dispatch() != nullptr && scope.dispatch()->target() == &scope.node()
                            ? 1U
                            : 0U,
                        0U,
                    };
                    strata_widget_input_context context{&scope, fields.get()};
                    return scroll_descriptor.scroll(scroll_descriptor.user_data, &context,
                                                    &event) == STRATA_EXTENSION_INPUT_CONSUMED;
                };
            }
            widget_scrolls.erase(scroll);
        }
        if (has_frame) {
            lifecycle.frame.advance = [descriptor,
                                       fields](strata::ui::WidgetInputScope& scope,
                                               const strata::ui::WidgetFrameInfo& frame) {
                strata_widget_input_context context{&scope, fields.get()};
                const strata_widget_frame_info info{
                    sizeof(strata_widget_frame_info),
                    frame.time_nanos,
                    frame.delta_nanos,
                    frame.reduced_motion ? 1U : 0U,
                    0U,
                };
                descriptor.frame(descriptor.user_data, &context, &info);
            };
        }
        lifecycle.semantics.role =
            checked_string(descriptor.semantics_role, true, "widget semantics role");
        if (lifecycle.semantics.role.empty())
            lifecycle.semantics.role = "group";
        if (descriptor.semantics != nullptr) {
            lifecycle.semantics.derive = [descriptor,
                                          fields](strata::ui::WidgetSemanticsScope& scope) {
                std::vector<std::string> actions;
                strata_widget_semantics_context context{&scope, fields.get(), &actions};
                descriptor.semantics(descriptor.user_data, &context);
                if (!actions.empty())
                    scope.actions(std::move(actions));
            };
        }
        if (descriptor.present != nullptr) {
            lifecycle.present.content = [descriptor, fields](strata::ui::WidgetRenderScope& scope) {
                strata_widget_render_context context{&scope, fields.get()};
                descriptor.present(descriptor.user_data, &context);
            };
        }
        if (descriptor.overlay != nullptr) {
            lifecycle.present.overlay = [descriptor, fields](strata::ui::WidgetRenderScope& scope) {
                strata_widget_render_context context{&scope, fields.get()};
                descriptor.overlay(descriptor.user_data, &context);
            };
        }
        lifecycle.present.detached_overlay =
            (descriptor.flags & STRATA_WIDGET_EXTENSION_DETACHED_OVERLAY) != 0U;
        lifecycle.present.depends_on_motion_progress =
            (descriptor.flags & STRATA_WIDGET_EXTENSION_DEPENDS_ON_MOTION) != 0U;
        lifecycle.present.depends_on_status_feedback =
            (descriptor.flags & STRATA_WIDGET_EXTENSION_DEPENDS_ON_STATUS) != 0U;
        if (descriptor.hit_bounds != nullptr) {
            lifecycle.inspection.derive = [descriptor](strata::ui::WidgetInspectionScope& scope) {
                strata_widget_inspection_context context{&scope};
                scope.hit_bounds(rect(descriptor.hit_bounds(descriptor.user_data, &context)));
            };
        }
        if (has_subtargets) {
            lifecycle.inspection.subtargets = [descriptor,
                                               fields](const strata::ui::RetainedNode& node,
                                                       const strata::ui::LayoutRecord& layout) {
                std::vector<strata::ui::WidgetSubtarget> targets;
                strata_widget_subtargets_context context{
                    &node,
                    &layout,
                    fields.get(),
                    &targets,
                };
                descriptor.subtargets(descriptor.user_data, &context);
                return targets;
            };
        }
        result.widgets.register_lifecycle(std::move(lifecycle));
    }
    if (!widget_inputs.empty()) {
        throw std::invalid_argument("widget input extension names unknown widget type '" +
                                    widget_inputs.begin()->first + "'");
    }
    for (std::size_t index = 0U; index < bundle->behavior_count; ++index) {
        const strata_behavior_extension descriptor = bundle->behaviors[index];
        if (descriptor.struct_size < sizeof(strata_behavior_extension)) {
            throw std::invalid_argument("behavior extension descriptor is incomplete");
        }
        const std::string id = checked_string(descriptor.id, false, "behavior extension id");
        if (result.behaviors.find(id) != nullptr) {
            throw std::invalid_argument("behavior extension conflicts with lifecycle '" + id + "'");
        }
        constexpr std::uint64_t known_flags =
            STRATA_BEHAVIOR_EXTENSION_FOCUSABLE | STRATA_BEHAVIOR_EXTENSION_ACCEPTS_POINTER;
        if ((descriptor.flags & ~known_flags) != 0U) {
            throw std::invalid_argument("behavior extension flags are invalid");
        }
        strata::ui::BehaviorLifecycle lifecycle;
        lifecycle.id = id;
        lifecycle.input.focusable = (descriptor.flags & STRATA_BEHAVIOR_EXTENSION_FOCUSABLE) != 0U;
        lifecycle.input.accepts_pointer =
            (descriptor.flags & STRATA_BEHAVIOR_EXTENSION_ACCEPTS_POINTER) != 0U;
        if (descriptor.pointer != nullptr) {
            lifecycle.input.pointer = [descriptor](strata::ui::BehaviorInputScope& scope) {
                const strata::ui::PointerInputEvent& input = scope.event();
                const strata_behavior_pointer_event event{
                    sizeof(strata_behavior_pointer_event),
                    event_kind(input.type),
                    event_phase(scope.phase()),
                    modifiers(input.modifiers),
                    input.pointer_id,
                    input.button,
                    input.position.x,
                    input.position.y,
                    scope.target() ? 1U : 0U,
                    0U,
                };
                strata_behavior_input_context context{&scope};
                return descriptor.pointer(descriptor.user_data, &context, &event) ==
                       STRATA_EXTENSION_INPUT_CONSUMED;
            };
        }
        result.behaviors.register_lifecycle(std::move(lifecycle));
    }
    return result;
}

} // namespace strata::abi_detail

extern "C" {
strata_widget_value_kind strata_widget_value_get_kind(const strata_widget_value* const value) {
    const Value* const runtime = runtime_value(value);
    if (runtime == nullptr)
        return STRATA_WIDGET_VALUE_NULL;
    switch (runtime->kind()) {
    case strata::runtime::ValueKind::null_value:
        return STRATA_WIDGET_VALUE_NULL;
    case strata::runtime::ValueKind::boolean:
        return STRATA_WIDGET_VALUE_BOOLEAN;
    case strata::runtime::ValueKind::number:
        return STRATA_WIDGET_VALUE_NUMBER;
    case strata::runtime::ValueKind::duration:
        return STRATA_WIDGET_VALUE_DURATION;
    case strata::runtime::ValueKind::string:
        return STRATA_WIDGET_VALUE_TEXT;
    case strata::runtime::ValueKind::color:
        return STRATA_WIDGET_VALUE_COLOR;
    case strata::runtime::ValueKind::image:
        return STRATA_WIDGET_VALUE_IMAGE;
    case strata::runtime::ValueKind::key:
        return STRATA_WIDGET_VALUE_KEY;
    case strata::runtime::ValueKind::theme_token:
        return STRATA_WIDGET_VALUE_THEME_TOKEN;
    case strata::runtime::ValueKind::list:
        return STRATA_WIDGET_VALUE_LIST;
    case strata::runtime::ValueKind::object:
        return STRATA_WIDGET_VALUE_OBJECT;
    }
    return STRATA_WIDGET_VALUE_NULL;
}

uint32_t strata_widget_value_get_boolean(const strata_widget_value* const value,
                                         const uint32_t fallback) {
    const Value* const runtime = runtime_value(value);
    return runtime != nullptr && runtime->boolean() != nullptr ? *runtime->boolean() ? 1U : 0U
           : fallback != 0U                                    ? 1U
                                                               : 0U;
}

double strata_widget_value_get_number(const strata_widget_value* const value,
                                      const double fallback) {
    const Value* const runtime = runtime_value(value);
    return runtime != nullptr && runtime->number() != nullptr && std::isfinite(*runtime->number())
               ? *runtime->number()
               : fallback;
}

uint32_t strata_widget_value_get_color(const strata_widget_value* const value,
                                       strata_color* const out_color) {
    const Value* const runtime = runtime_value(value);
    if (runtime == nullptr || runtime->color() == nullptr || out_color == nullptr)
        return 0U;
    const strata::runtime::ColorValue& source = *runtime->color();
    *out_color = strata_color{source.red, source.green, source.blue, source.alpha};
    return 1U;
}

strata_string_view strata_widget_value_get_text(const strata_widget_value* const value) {
    const Value* const runtime = runtime_value(value);
    if (runtime == nullptr)
        return {};
    const std::string* text = runtime->string();
    if (text == nullptr && runtime->image() != nullptr)
        text = &runtime->image()->id;
    if (text == nullptr && runtime->key() != nullptr)
        text = &runtime->key()->value;
    if (text == nullptr && runtime->theme_token() != nullptr)
        text = &runtime->theme_token()->name;
    return text != nullptr ? strata_string_view{text->data(), text->size()} : strata_string_view{};
}

size_t strata_widget_value_list_size(const strata_widget_value* const value) {
    const Value* const runtime = runtime_value(value);
    return runtime != nullptr && runtime->list() != nullptr ? runtime->list()->values.size() : 0U;
}

const strata_widget_value* strata_widget_value_list_at(const strata_widget_value* const value,
                                                       const size_t index) {
    const Value* const runtime = runtime_value(value);
    if (runtime == nullptr || runtime->list() == nullptr ||
        index >= runtime->list()->values.size()) {
        return nullptr;
    }
    return public_value(&runtime->list()->values[index]);
}

const strata_widget_value* strata_widget_value_object_field(const strata_widget_value* const value,
                                                            const strata_string_view name) {
    const Value* const runtime = runtime_value(value);
    return runtime != nullptr && valid_view(name, false)
               ? public_value(runtime->field(std::string_view(name.data, name.size)))
               : nullptr;
}

const strata_widget_value*
strata_widget_input_property_value(const strata_widget_input_context* const context,
                                   const strata_string_view name) {
    return context != nullptr && context->scope != nullptr && valid_view(name, false)
               ? public_value(context->scope->property(std::string_view(name.data, name.size)))
               : nullptr;
}

const strata_widget_value*
strata_widget_render_property_value(const strata_widget_render_context* const context,
                                    const strata_string_view name) {
    return context != nullptr && context->scope != nullptr && valid_view(name, false)
               ? public_value(context->scope->property(std::string_view(name.data, name.size)))
               : nullptr;
}

const strata_widget_value*
strata_widget_render_style_value(const strata_widget_render_context* const context,
                                 const strata_string_view name) {
    return context != nullptr && context->scope != nullptr && valid_view(name, false)
               ? public_value(context->scope->style(std::string_view(name.data, name.size)))
               : nullptr;
}

const strata_widget_value*
strata_widget_semantics_property_value(const strata_widget_semantics_context* const context,
                                       const strata_string_view name) {
    return context != nullptr && context->scope != nullptr && valid_view(name, false)
               ? public_value(context->scope->property(std::string_view(name.data, name.size)))
               : nullptr;
}

strata_rect strata_widget_subtargets_bounds(const strata_widget_subtargets_context* const context) {
    return context != nullptr && context->layout != nullptr ? rect(context->layout->bounds)
                                                            : strata_rect{};
}

const strata_widget_value*
strata_widget_subtargets_property_value(const strata_widget_subtargets_context* const context,
                                        const strata_string_view name) {
    if (context == nullptr || context->node == nullptr || !valid_view(name, false))
        return nullptr;
    const auto found =
        context->node->description().properties.find(std::string_view(name.data, name.size));
    return found != context->node->description().properties.end()
               ? public_value(found->second.data_value())
               : nullptr;
}

strata_result
strata_widget_subtargets_retained_bytes(const strata_widget_subtargets_context* const context,
                                        const strata_string_view name, void* const buffer,
                                        const size_t size) {
    if (context == nullptr || context->node == nullptr || context->fields == nullptr ||
        buffer == nullptr || size == 0U || !valid_view(name, false)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    const std::string_view field(name.data, name.size);
    if (context->fields->find(field) == nullptr) {
        return strata::core::result(STRATA_STATUS_NOT_FOUND);
    }
    const std::span<const std::byte> value = context->node->retained_bytes(field);
    if (value.empty())
        return strata::core::result(STRATA_STATUS_NOT_FOUND);
    if (value.size() != size)
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    std::memcpy(buffer, value.data(), value.size());
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_widget_subtargets_reserve(strata_widget_subtargets_context* const context,
                                               const size_t capacity) {
    if (context == nullptr || context->targets == nullptr)
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    try {
        context->targets->reserve(capacity);
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_subtargets_add(strata_widget_subtargets_context* const context,
                                           const strata_widget_subtarget* const subtarget) {
    if (context == nullptr || context->node == nullptr || context->layout == nullptr ||
        context->targets == nullptr || subtarget == nullptr ||
        subtarget->struct_size < sizeof(strata_widget_subtarget) ||
        !valid_view(subtarget->id, false) || !std::isfinite(subtarget->bounds.x) ||
        !std::isfinite(subtarget->bounds.y) || !std::isfinite(subtarget->bounds.width) ||
        !std::isfinite(subtarget->bounds.height) || subtarget->bounds.width < 0.0 ||
        subtarget->bounds.height < 0.0 || subtarget->kind > STRATA_WIDGET_SUBTARGET_ITEM) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        strata::ui::WidgetSubtarget target;
        target.owner_identity = context->node->identity();
        target.id = checked_string(subtarget->id, false, "widget subtarget id");
        target.index = subtarget->index;
        target.bounds = strata::ui::Rect{
            context->layout->bounds.x + subtarget->bounds.x,
            context->layout->bounds.y + subtarget->bounds.y,
            subtarget->bounds.width,
            subtarget->bounds.height,
        };
        target.enabled = subtarget->enabled != 0U;
        target.kind = subtarget->kind == STRATA_WIDGET_SUBTARGET_ITEM
                          ? strata::ui::WidgetSubtargetKind::choice
                          : strata::ui::WidgetSubtargetKind::control;
        target.z_index = subtarget->z_index;
        context->targets->push_back(std::move(target));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_rect strata_widget_input_bounds(const strata_widget_input_context* const context) {
    if (context == nullptr || context->scope == nullptr)
        return strata_rect{};
    const strata::ui::LayoutRecord* const layout = context->scope->layout();
    return layout != nullptr ? rect(layout->bounds) : strata_rect{};
}
double strata_widget_input_scale(const strata_widget_input_context* const context) {
    return context != nullptr && context->scope != nullptr ? context->scope->scale() : 1.0;
}

uint32_t strata_widget_input_claim_gesture(strata_widget_input_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->claim_gesture() ? 1U
                                                                                              : 0U;
}

uint32_t strata_widget_input_cancel_gesture(strata_widget_input_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->cancel_gesture() ? 1U
                                                                                               : 0U;
}
strata_result strata_widget_input_invalidate(strata_widget_input_context* const context,
                                             const strata_widget_invalidation value) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->invalidate(dirty_invalidation(value));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_input_request_frame(strata_widget_input_context* const context,
                                                const strata_widget_frame_cost cost) {
    if (context == nullptr || context->scope == nullptr || cost > STRATA_WIDGET_FRAME_LAYOUT) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    const bool requested = context->scope->request_frame(cost == STRATA_WIDGET_FRAME_LAYOUT
                                                             ? strata::ui::WidgetFrameCost::layout
                                                             : strata::ui::WidgetFrameCost::paint);
    return strata::core::result(requested ? STRATA_STATUS_OK : STRATA_STATUS_NOT_FOUND);
}

uint32_t strata_widget_input_cancel_frame(strata_widget_input_context* const context) {
    if (context == nullptr || context->scope == nullptr)
        return 0U;
    context->scope->cancel_frame();
    return 1U;
}

double strata_widget_input_retained_number(const strata_widget_input_context* const context,
                                           const strata_string_view name, const double fallback) {
    if (!std::isfinite(fallback))
        return fallback;
    try {
        const Value* value = declared_retained(context, name);
        return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
                   ? *value->number()
                   : fallback;
    } catch (...) {
        return fallback;
    }
}

uint32_t strata_widget_input_retained_boolean(const strata_widget_input_context* const context,
                                              const strata_string_view name,
                                              const uint32_t fallback) {
    try {
        const Value* value = declared_retained(context, name);
        return value != nullptr && value->boolean() != nullptr ? *value->boolean() ? 1U : 0U
               : fallback != 0U                                ? 1U
                                                               : 0U;
    } catch (...) {
        return fallback != 0U ? 1U : 0U;
    }
}

strata_result strata_widget_input_retained_text(const strata_widget_input_context* const context,
                                                const strata_string_view name, char* const buffer,
                                                const size_t capacity, size_t* const out_length) {
    try {
        const Value* value = declared_retained(context, name);
        return copied_text(value != nullptr ? value->string() : nullptr, buffer, capacity,
                           out_length);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_input_retained_bytes(const strata_widget_input_context* const context,
                                                 const strata_string_view name, void* const buffer,
                                                 const size_t size) {
    return copy_retained_bytes(context, name, buffer, size);
}

strata_result strata_widget_input_set_retained_bytes(strata_widget_input_context* const context,
                                                     const strata_string_view name,
                                                     const void* const value, const size_t size) {
    return set_declared_retained_bytes(context, name, value, size);
}

strata_result strata_widget_input_set_retained_number(strata_widget_input_context* const context,
                                                      const strata_string_view name,
                                                      const double value) {
    if (!std::isfinite(value))
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    return set_declared_retained(context, name, Value(value));
}

strata_result strata_widget_input_set_retained_boolean(strata_widget_input_context* const context,
                                                       const strata_string_view name,
                                                       const uint32_t value) {
    if (value > 1U)
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    return set_declared_retained(context, name, Value(value != 0U));
}

strata_result strata_widget_input_set_retained_text(strata_widget_input_context* const context,
                                                    const strata_string_view name,
                                                    const strata_string_view value) {
    try {
        return set_declared_retained(context, name,
                                     Value(checked_string(value, true, "retained text")));
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}
uint32_t strata_widget_input_has_property(const strata_widget_input_context* const context,
                                          const strata_string_view name) {
    if (context == nullptr || context->scope == nullptr)
        return 0U;
    try {
        return context->scope->property(checked_string(name, false, "property")) != nullptr ? 1U
                                                                                            : 0U;
    } catch (...) {
        return 0U;
    }
}

double strata_widget_input_property_number(const strata_widget_input_context* const context,
                                           const strata_string_view name, const double fallback) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(fallback))
        return fallback;
    try {
        return context->scope->number(checked_string(name, false, "property"), fallback);
    } catch (...) {
        return fallback;
    }
}

uint32_t strata_widget_input_property_boolean(const strata_widget_input_context* const context,
                                              const strata_string_view name,
                                              const uint32_t fallback) {
    if (context == nullptr || context->scope == nullptr)
        return fallback != 0U ? 1U : 0U;
    try {
        const Value* const value =
            context->scope->property(checked_string(name, false, "property"));
        return value != nullptr && value->boolean() != nullptr ? *value->boolean() ? 1U : 0U
               : fallback != 0U                                ? 1U
                                                               : 0U;
    } catch (...) {
        return fallback != 0U ? 1U : 0U;
    }
}

strata_result strata_widget_input_property_text(const strata_widget_input_context* const context,
                                                const strata_string_view name, char* const buffer,
                                                const size_t capacity, size_t* const out_length) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const Value* const value =
            context->scope->property(checked_string(name, false, "property"));
        return copied_text(value != nullptr ? value->string() : nullptr, buffer, capacity,
                           out_length);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_input_emit_action_json(strata_widget_input_context* const context,
                                                   const strata_string_view action_id,
                                                   const strata_string_view payload_json,
                                                   const strata_string_view event_kind_value,
                                                   const strata_string_view event_value_json) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        return emit_action_json(*context->scope, action_id, payload_json, event_kind_value,
                                event_value_json);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_behavior_input_emit_action_json(strata_behavior_input_context* const context,
                                                     const strata_string_view action_id,
                                                     const strata_string_view payload_json,
                                                     const strata_string_view event_kind_value,
                                                     const strata_string_view event_value_json) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        return emit_action_json(*context->scope, action_id, payload_json, event_kind_value,
                                event_value_json);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_input_emit_event_json(strata_widget_input_context* const context,
                                                  const strata_string_view event_kind_value,
                                                  const strata_string_view event_value_json) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->emit_event(checked_string(event_kind_value, false, "extension event kind"),
                                   json_value(event_value_json, true));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}
strata_result strata_widget_input_emit_number_event(strata_widget_input_context* const context,
                                                    const strata_string_view event_kind_value,
                                                    const double value) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(value)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->emit_event(checked_string(event_kind_value, false, "extension event kind"),
                                   Value(value));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_input_emit_boolean_event(strata_widget_input_context* const context,
                                                     const strata_string_view event_kind_value,
                                                     const uint32_t value) {
    if (context == nullptr || context->scope == nullptr || value > 1U) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->emit_event(checked_string(event_kind_value, false, "extension event kind"),
                                   Value(value != 0U));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_input_emit_text_event(strata_widget_input_context* const context,
                                                  const strata_string_view event_kind_value,
                                                  const strata_string_view value) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->emit_event(checked_string(event_kind_value, false, "extension event kind"),
                                   Value(checked_string(value, true, "extension event text")));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_rect strata_widget_render_bounds(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr ? rect(context->scope->layout().bounds)
                                                           : strata_rect{};
}

strata_rect strata_widget_render_root_bounds(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr ? rect(context->scope->root_bounds())
                                                           : strata_rect{};
}
double strata_widget_render_scale(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr ? context->scope->layout_result().scale
                                                           : 1.0;
}

uint32_t strata_widget_render_focused(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->focused() ? 1U : 0U;
}

uint32_t strata_widget_render_focus_visible(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->focus_visible() ? 1U
                                                                                              : 0U;
}

uint32_t strata_widget_render_hovered(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->hovered() ? 1U : 0U;
}

double strata_widget_render_retained_number(const strata_widget_render_context* const context,
                                            const strata_string_view name, const double fallback) {
    if (!std::isfinite(fallback))
        return fallback;
    try {
        const Value* value = declared_retained(context, name);
        return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
                   ? *value->number()
                   : fallback;
    } catch (...) {
        return fallback;
    }
}

uint32_t strata_widget_render_retained_boolean(const strata_widget_render_context* const context,
                                               const strata_string_view name,
                                               const uint32_t fallback) {
    try {
        const Value* value = declared_retained(context, name);
        return value != nullptr && value->boolean() != nullptr ? *value->boolean() ? 1U : 0U
               : fallback != 0U                                ? 1U
                                                               : 0U;
    } catch (...) {
        return fallback != 0U ? 1U : 0U;
    }
}

strata_result strata_widget_render_retained_text(const strata_widget_render_context* const context,
                                                 const strata_string_view name, char* const buffer,
                                                 const size_t capacity, size_t* const out_length) {
    try {
        const Value* value = declared_retained(context, name);
        return copied_text(value != nullptr ? value->string() : nullptr, buffer, capacity,
                           out_length);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_render_retained_bytes(const strata_widget_render_context* const context,
                                                  const strata_string_view name, void* const buffer,
                                                  const size_t size) {
    return copy_retained_bytes(context, name, buffer, size);
}

uint32_t strata_widget_render_enabled(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->enabled() ? 1U : 0U;
}

uint32_t strata_widget_render_active(const strata_widget_render_context* const context) {
    return context != nullptr && context->scope != nullptr && context->scope->active() ? 1U : 0U;
}
uint32_t strata_widget_render_has_property(const strata_widget_render_context* const context,
                                           const strata_string_view name) {
    if (context == nullptr || context->scope == nullptr)
        return 0U;
    try {
        return context->scope->property(checked_string(name, false, "property")) != nullptr ? 1U
                                                                                            : 0U;
    } catch (...) {
        return 0U;
    }
}

double strata_widget_render_property_number(const strata_widget_render_context* const context,
                                            const strata_string_view name, const double fallback) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(fallback))
        return fallback;
    try {
        return context->scope->number(checked_string(name, false, "property"), fallback);
    } catch (...) {
        return fallback;
    }
}

uint32_t strata_widget_render_property_boolean(const strata_widget_render_context* const context,
                                               const strata_string_view name,
                                               const uint32_t fallback) {
    if (context == nullptr || context->scope == nullptr)
        return fallback != 0U ? 1U : 0U;
    try {
        return context->scope->boolean(checked_string(name, false, "property"), fallback != 0U)
                   ? 1U
                   : 0U;
    } catch (...) {
        return fallback != 0U ? 1U : 0U;
    }
}

strata_result strata_widget_render_property_text(const strata_widget_render_context* const context,
                                                 const strata_string_view name, char* const buffer,
                                                 const size_t capacity, size_t* const out_length) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const Value* const value =
            context->scope->property(checked_string(name, false, "property"));
        return copied_text(value != nullptr ? value->string() : nullptr, buffer, capacity,
                           out_length);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

double strata_widget_render_motion_progress(const strata_widget_render_context* const context,
                                            const strata_string_view id, const double fallback) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(fallback))
        return fallback;
    try {
        return context->scope->motion_progress(checked_string(id, false, "motion channel"),
                                               fallback);
    } catch (...) {
        return fallback;
    }
}

void strata_widget_render_rounded_rect(strata_widget_render_context* const context,
                                       const strata_rect bounds, const double radius,
                                       const strata_color fill,
                                       const strata_border* const border_value) {
    if (context == nullptr || context->scope == nullptr)
        return;
    const std::optional<strata::ui::RenderBorder> resolved =
        border_value != nullptr ? std::optional<strata::ui::RenderBorder>(border(*border_value))
                                : std::nullopt;
    context->scope->rounded_rect(rect(bounds), color(fill), resolved, radius);
}

void strata_widget_render_solid_rect(strata_widget_render_context* const context,
                                     const strata_rect bounds, const strata_color value) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->solid_rect(rect(bounds), color(value));
}

void strata_widget_render_border(strata_widget_render_context* const context,
                                 const strata_rect bounds, const double radius,
                                 const strata_border value) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->border(rect(bounds), border(value), radius);
}

void strata_widget_render_text(strata_widget_render_context* const context,
                               const strata_string_view text_value, const double x, const double y,
                               const strata_color value) {
    if (context == nullptr || context->scope == nullptr || !valid_view(text_value, true))
        return;
    context->scope->text(std::string_view(text_value.data, text_value.size),
                         strata::ui::Point{x, y}, color(value));
}

void strata_widget_render_aligned_text(strata_widget_render_context* const context,
                                       const strata_string_view text_value,
                                       const strata_rect bounds,
                                       const strata_widget_text_alignment horizontal,
                                       const strata_widget_text_alignment vertical,
                                       const strata_color value) {
    if (context == nullptr || context->scope == nullptr || !valid_view(text_value, true) ||
        horizontal > STRATA_WIDGET_TEXT_ALIGN_END || vertical > STRATA_WIDGET_TEXT_ALIGN_END ||
        !std::isfinite(bounds.x) || !std::isfinite(bounds.y) || !std::isfinite(bounds.width) ||
        !std::isfinite(bounds.height) || bounds.width < 0.0 || bounds.height < 0.0) {
        return;
    }
    context->scope->text(std::string_view(text_value.data, text_value.size),
                         strata::ui::Point{bounds.x, bounds.y}, color(value), bounds.width,
                         static_cast<strata::ui::WidgetTextAlignment>(horizontal), bounds.height,
                         static_cast<strata::ui::WidgetTextAlignment>(vertical));
}

void strata_widget_render_image(strata_widget_render_context* const context,
                                const strata_rect bounds, const strata_string_view image,
                                const strata_color tint, const strata_texture_region source) {
    if (context == nullptr || context->scope == nullptr || !valid_view(image, false))
        return;
    context->scope->image(rect(bounds), std::string(image.data, image.size), color(tint),
                          region(source));
}

void strata_widget_render_nine_patch(strata_widget_render_context* const context,
                                     const strata_rect bounds, const strata_string_view texture,
                                     const strata_edges source_insets,
                                     const strata_edges destination_insets,
                                     const strata_texture_region source, const strata_color tint) {
    if (context == nullptr || context->scope == nullptr || !valid_view(texture, false))
        return;
    context->scope->nine_patch(rect(bounds), std::string(texture.data, texture.size),
                               edges(source_insets), edges(destination_insets), region(source),
                               color(tint));
}

void strata_widget_render_custom_mesh(strata_widget_render_context* const context,
                                      const strata_rect bounds, const strata_string_view mesh,
                                      const strata_mesh_geometry* const geometry,
                                      const strata_string_view texture,
                                      const strata_material_state* const material_value) {
    if (context == nullptr || context->scope == nullptr || !valid_view(mesh, false) ||
        geometry == nullptr || geometry->struct_size < sizeof(strata_mesh_geometry) ||
        (geometry->vertices == nullptr && geometry->vertex_count != 0U) ||
        (geometry->indices == nullptr && geometry->index_count != 0U) ||
        !valid_view(texture, true)) {
        return;
    }
    /*
     * Geometry is validated here so a malformed extension mesh drops its own draw; the submission
     * planner treats the same defect as a fatal frame error for engine-authored meshes.
     */
    if (geometry->vertex_count == 0U || geometry->index_count == 0U ||
        geometry->index_count % 3U != 0U) {
        return;
    }
    for (std::size_t index = 0U; index < geometry->index_count; ++index) {
        if (geometry->indices[index] >= geometry->vertex_count)
            return;
    }
    for (std::size_t index = 0U; index < geometry->vertex_count; ++index) {
        const strata_mesh_vertex vertex = geometry->vertices[index];
        if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y) || !std::isfinite(vertex.z) ||
            !std::isfinite(vertex.u) || !std::isfinite(vertex.v) || vertex.x < 0.0 ||
            vertex.x > 1.0 || vertex.y < 0.0 || vertex.y > 1.0) {
            return;
        }
    }
    try {
        strata::ui::MeshGeometry converted;
        converted.vertices.reserve(geometry->vertex_count);
        for (std::size_t index = 0U; index < geometry->vertex_count; ++index) {
            const strata_mesh_vertex vertex = geometry->vertices[index];
            converted.vertices.push_back(strata::ui::MeshVertex{
                vertex.x,
                vertex.y,
                vertex.z,
                vertex.u,
                vertex.v,
                color(vertex.color),
            });
        }
        converted.indices.assign(geometry->indices, geometry->indices + geometry->index_count);
        context->scope->custom_mesh(
            rect(bounds), std::string(mesh.data, mesh.size), std::move(converted),
            texture.size != 0U ? std::optional<std::string>(std::string(texture.data, texture.size))
                               : std::nullopt,
            material_state(material_value));
    } catch (...) {
        // A malformed mesh is dropped; the frame keeps every other command this pass emitted.
    }
}

void strata_widget_render_blur(strata_widget_render_context* const context,
                               const strata_rect bounds, const double radius,
                               const uint32_t downsample) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(radius) ||
        downsample == 0U) {
        return;
    }
    context->scope->blur_region(rect(bounds), radius, downsample);
}

void strata_widget_render_shadow(strata_widget_render_context* const context,
                                 const strata_rect bounds, const strata_corner_radii radii,
                                 const strata_color value, const double radius,
                                 const double spread) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(radius) ||
        !std::isfinite(spread)) {
        return;
    }
    context->scope->shadow(rect(bounds),
                           strata::ui::CornerRadii{
                               radii.top_left,
                               radii.top_right,
                               radii.bottom_right,
                               radii.bottom_left,
                           },
                           color(value), radius, spread);
}

void strata_widget_render_push_clip(strata_widget_render_context* const context,
                                    const strata_rect bounds) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->push_clip(rect(bounds));
}

void strata_widget_render_pop_clip(strata_widget_render_context* const context) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->pop_clip();
}

strata_result strata_widget_render_text_metrics(const strata_widget_render_context* const context,
                                                const strata_string_view text_value,
                                                double* const out_width, double* const out_height) {
    if (context == nullptr || context->scope == nullptr || out_width == nullptr ||
        out_height == nullptr || !valid_view(text_value, true)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    const strata::ui::TextEngine* const engine = context->scope->text_engine();
    if (engine == nullptr)
        return strata::core::result(STRATA_STATUS_NOT_FOUND);
    try {
        const strata::ui::TextLayout layout = engine->layout(
            context->scope->node(), std::string_view(text_value.data, text_value.size));
        *out_width = layout.shaped.metrics.width;
        *out_height = layout.shaped.metrics.height;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_rect
strata_widget_inspection_layout_bounds(const strata_widget_inspection_context* const context) {
    return context != nullptr && context->scope != nullptr ? rect(context->scope->layout().bounds)
                                                           : strata_rect{};
}

strata_result strata_widget_semantics_set_name(strata_widget_semantics_context* const context,
                                               const strata_string_view value) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->name(checked_string(value, true, "semantics name"));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_semantics_set_value_text(strata_widget_semantics_context* const context,
                                                     const strata_string_view value) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->scope->value_text(checked_string(value, true, "semantics value"));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}
void strata_widget_semantics_set_value_range(strata_widget_semantics_context* const context,
                                             const double current, const double minimum,
                                             const double maximum) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(current) ||
        !std::isfinite(minimum) || !std::isfinite(maximum) || minimum > maximum) {
        return;
    }
    context->scope->value_range(strata::data::JsonValue(strata::data::JsonValue::Object{
        {"current", strata::data::JsonValue(current)},
        {"maximum", strata::data::JsonValue(maximum)},
        {"minimum", strata::data::JsonValue(minimum)},
    }));
}

strata_result strata_widget_semantics_add_action(strata_widget_semantics_context* const context,
                                                 const strata_string_view action) {
    if (context == nullptr || context->actions == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        context->actions->push_back(checked_string(action, false, "semantics action"));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result strata_widget_semantics_add_child(strata_widget_semantics_context* const context,
                                                const strata_widget_semantic_child* const child) {
    if (context == nullptr || context->scope == nullptr || child == nullptr ||
        child->struct_size < sizeof(strata_widget_semantic_child) ||
        !valid_view(child->role, false) || !valid_view(child->name, false) ||
        !valid_view(child->value_text, true)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    const bool has_range = (child->flags & STRATA_WIDGET_SEMANTIC_CHILD_VALUE_RANGE) != 0U;
    if (has_range && (!std::isfinite(child->current) || !std::isfinite(child->minimum) ||
                      !std::isfinite(child->maximum) || child->minimum > child->maximum)) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const bool disabled = (child->flags & STRATA_WIDGET_SEMANTIC_CHILD_DISABLED) != 0U;
        strata::data::JsonValue range;
        if (has_range) {
            range = strata::data::JsonValue(strata::data::JsonValue::Object{
                {"current", strata::data::JsonValue(child->current)},
                {"maximum", strata::data::JsonValue(child->maximum)},
                {"minimum", strata::data::JsonValue(child->minimum)},
            });
        }
        std::optional<std::string> value_text;
        if (child->value_text.size != 0U) {
            value_text = checked_string(child->value_text, false, "semantic child value");
        }
        context->scope->virtual_after(context->scope->virtual_item(
            child->index, 4'000'000U, checked_string(child->role, false, "semantic child role"),
            checked_string(child->name, false, "semantic child name"),
            disabled ? std::vector<std::string>{} : std::vector<std::string>{"activate", "focus"},
            std::nullopt, (child->flags & STRATA_WIDGET_SEMANTIC_CHILD_SELECTED) != 0U, disabled,
            child->index, std::nullopt, {}, std::nullopt, std::nullopt, "off", std::nullopt,
            std::move(range), std::move(value_text)));
        return strata::core::result(STRATA_STATUS_OK);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

void strata_widget_semantics_set_checked(strata_widget_semantics_context* const context,
                                         const uint32_t value) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->checked(value != 0U);
}

void strata_widget_semantics_set_expanded(strata_widget_semantics_context* const context,
                                          const uint32_t value) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->expanded(value != 0U);
}

void strata_widget_semantics_set_selected(strata_widget_semantics_context* const context,
                                          const uint32_t value) {
    if (context == nullptr || context->scope == nullptr)
        return;
    context->scope->selected(value != 0U);
}

double strata_widget_semantics_retained_number(const strata_widget_semantics_context* const context,
                                               const strata_string_view name,
                                               const double fallback) {
    if (!std::isfinite(fallback))
        return fallback;
    try {
        const Value* value = declared_retained(context, name);
        return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
                   ? *value->number()
                   : fallback;
    } catch (...) {
        return fallback;
    }
}

uint32_t
strata_widget_semantics_retained_boolean(const strata_widget_semantics_context* const context,
                                         const strata_string_view name, const uint32_t fallback) {
    try {
        const Value* value = declared_retained(context, name);
        return value != nullptr && value->boolean() != nullptr ? *value->boolean() ? 1U : 0U
               : fallback != 0U                                ? 1U
                                                               : 0U;
    } catch (...) {
        return fallback != 0U ? 1U : 0U;
    }
}
strata_result
strata_widget_semantics_retained_text(const strata_widget_semantics_context* const context,
                                      const strata_string_view name, char* const buffer,
                                      const size_t capacity, size_t* const out_length) {
    try {
        const Value* value = declared_retained(context, name);
        return copied_text(value != nullptr ? value->string() : nullptr, buffer, capacity,
                           out_length);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

strata_result
strata_widget_semantics_retained_bytes(const strata_widget_semantics_context* const context,
                                       const strata_string_view name, void* const buffer,
                                       const size_t size) {
    return copy_retained_bytes(context, name, buffer, size);
}

uint32_t strata_widget_semantics_has_property(const strata_widget_semantics_context* const context,
                                              const strata_string_view name) {
    if (context == nullptr || context->scope == nullptr)
        return 0U;
    try {
        return context->scope->property(checked_string(name, false, "property")) != nullptr ? 1U
                                                                                            : 0U;
    } catch (...) {
        return 0U;
    }
}

double strata_widget_semantics_property_number(const strata_widget_semantics_context* const context,
                                               const strata_string_view name,
                                               const double fallback) {
    if (context == nullptr || context->scope == nullptr || !std::isfinite(fallback))
        return fallback;
    try {
        const Value* const value =
            context->scope->property(checked_string(name, false, "property"));
        return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
                   ? *value->number()
                   : fallback;
    } catch (...) {
        return fallback;
    }
}

uint32_t
strata_widget_semantics_property_boolean(const strata_widget_semantics_context* const context,
                                         const strata_string_view name, const uint32_t fallback) {
    if (context == nullptr || context->scope == nullptr)
        return fallback != 0U ? 1U : 0U;
    try {
        const Value* const value =
            context->scope->property(checked_string(name, false, "property"));
        return value != nullptr && value->boolean() != nullptr ? *value->boolean() ? 1U : 0U
               : fallback != 0U                                ? 1U
                                                               : 0U;
    } catch (...) {
        return fallback != 0U ? 1U : 0U;
    }
}

strata_result
strata_widget_semantics_property_text(const strata_widget_semantics_context* const context,
                                      const strata_string_view name, char* const buffer,
                                      const size_t capacity, size_t* const out_length) {
    if (context == nullptr || context->scope == nullptr) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
    try {
        const Value* const value =
            context->scope->property(checked_string(name, false, "property"));
        return copied_text(value != nullptr ? value->string() : nullptr, buffer, capacity,
                           out_length);
    } catch (...) {
        return strata::core::result(STRATA_STATUS_INVALID_ARGUMENT);
    }
}

} // extern "C"
