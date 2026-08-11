#include <strata/extension.hpp>

#include <algorithm>
#include <cstdint>
#include <format>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace strata::extension {
namespace {

[[nodiscard]] constexpr strata_string_view view(const std::string_view value) noexcept {
    return strata_string_view{value.data(), value.size()};
}

[[nodiscard]] std::string quoted(const std::string_view value) {
    std::string result;
    result.reserve(value.size() + 2U);
    result.push_back('"');
    for (const char character : value) {
        switch (character) {
        case '"':
            result += "\\\"";
            break;
        case '\\':
            result += "\\\\";
            break;
        case '\n':
            result += "\\n";
            break;
        case '\r':
            result += "\\r";
            break;
        case '\t':
            result += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                result += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                result.push_back(character);
            }
        }
    }
    result.push_back('"');
    return result;
}

[[nodiscard]] std::string json_number(const double value) {
    return std::format("{}", value);
}

/** Shared probe-then-copy read for every buffer-out text entry point. */
template <typename Reader>
[[nodiscard]] std::optional<std::string> read_text(const Reader& read,
                                                   const std::string_view name) {
    std::size_t length = 0U;
    if (read(view(name), nullptr, 0U, &length).status == STRATA_STATUS_NOT_FOUND)
        return std::nullopt;
    std::string value(length, '\0');
    if (read(view(name), value.data(), value.size(), &length).status != STRATA_STATUS_OK) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] strata_widget_invalidation invalidation_value(const Invalidation value) noexcept {
    switch (value) {
    case Invalidation::layout:
        return STRATA_WIDGET_INVALIDATION_LAYOUT;
    case Invalidation::style:
        return STRATA_WIDGET_INVALIDATION_STYLE;
    case Invalidation::text:
        return STRATA_WIDGET_INVALIDATION_TEXT;
    case Invalidation::semantics:
        return STRATA_WIDGET_INVALIDATION_SEMANTICS;
    case Invalidation::paint:
        return STRATA_WIDGET_INVALIDATION_PAINT;
    case Invalidation::input:
        return STRATA_WIDGET_INVALIDATION_INPUT;
    case Invalidation::properties:
        break;
    }
    return STRATA_WIDGET_INVALIDATION_PROPERTIES;
}

[[nodiscard]] std::string parameter_json(const std::string_view name, const std::string_view kind,
                                         const bool required) {
    return std::format(
        R"({{"name":{},"type":{{"kind":{}}},"required":{},"nullable":false,"aliases":[],"default":null}})",
        quoted(name), quoted(kind), required ? "true" : "false");
}

[[nodiscard]] std::string action_json(const ActionContract& contract) {
    std::string arguments;
    for (const ActionArgument& argument : contract.arguments) {
        if (!arguments.empty())
            arguments.push_back(',');
        arguments += std::format(
            R"({{"name":{},"type":{{"kind":{}}},"required":{},"nullable":{},"aliases":[],"default":null}})",
            quoted(argument.name), quoted(argument.kind), argument.required ? "true" : "false",
            argument.nullable ? "true" : "false");
    }
    return std::format(
        R"({{"id":{},"dispatchPolicy":{},"summary":{},"payloadContract":{},"arguments":[{}]}})",
        quoted(contract.id), quoted(contract.dispatch_policy),
        quoted(contract.summary.empty() ? contract.id : contract.summary),
        quoted(contract.payload_contract), arguments);
}

void draw_mesh(strata_widget_render_context* const context, const Rect bounds,
               const std::string_view id, const Mesh& geometry, const std::string_view texture,
               const Material* const material) {
    const strata_mesh_geometry converted{
        sizeof(strata_mesh_geometry), geometry.vertices.data(), geometry.vertices.size(),
        geometry.indices.data(),      geometry.indices.size(),
    };
    strata_material_state state{};
    if (material != nullptr) {
        state.struct_size = sizeof(strata_material_state);
        state.id = view(material->id);
        state.blend_mode = view(material->blend_mode);
        state.opacity = material->opacity;
        state.parameters = material->parameters.data();
        state.parameter_count = material->parameters.size();
    }
    strata_widget_render_custom_mesh(context, bounds, view(id), &converted, view(texture),
                                     material != nullptr ? &state : nullptr);
}

strata_extension_input_result activate_trampoline(void* const user_data,
                                                  strata_widget_input_context* const context) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    Input input(context);
    return hooks->activate(input) ? STRATA_EXTENSION_INPUT_CONSUMED
                                  : STRATA_EXTENSION_INPUT_IGNORED;
}

strata_extension_input_result key_trampoline(void* const user_data,
                                             strata_widget_input_context* const context,
                                             const strata_widget_key_event* const event) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    if (event == nullptr)
        return STRATA_EXTENSION_INPUT_IGNORED;
    const Key key{
        std::string_view(event->key.data, event->key.size),
        (event->modifiers & STRATA_KEY_MODIFIER_SHIFT) != 0U,
        (event->modifiers & STRATA_KEY_MODIFIER_CONTROL) != 0U,
        (event->modifiers & STRATA_KEY_MODIFIER_ALT) != 0U,
        (event->modifiers & STRATA_KEY_MODIFIER_SUPER) != 0U,
    };
    Input input(context);
    return hooks->key(input, key) ? STRATA_EXTENSION_INPUT_CONSUMED
                                  : STRATA_EXTENSION_INPUT_IGNORED;
}
strata_extension_input_result
widget_pointer_trampoline(void* const user_data, strata_widget_input_context* const context,
                          const strata_widget_pointer_event* const event) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    if (event == nullptr)
        return STRATA_EXTENSION_INPUT_IGNORED;
    Pointer pointer;
    switch (event->kind) {
    case STRATA_INPUT_POINTER_MOVE:
        pointer.kind = Pointer::Kind::move;
        break;
    case STRATA_INPUT_POINTER_PRESS:
        pointer.kind = Pointer::Kind::press;
        break;
    case STRATA_INPUT_POINTER_RELEASE:
        pointer.kind = Pointer::Kind::release;
        break;
    default:
        pointer.kind = Pointer::Kind::cancel;
        break;
    }
    switch (event->phase) {
    case STRATA_EXTENSION_EVENT_CAPTURE:
        pointer.phase = Pointer::Phase::capture;
        break;
    case STRATA_EXTENSION_EVENT_BUBBLE:
        pointer.phase = Pointer::Phase::bubble;
        break;
    default:
        pointer.phase = Pointer::Phase::target;
        break;
    }
    pointer.button = event->button;
    pointer.pointer_id = event->pointer_id;
    pointer.x = event->x;
    pointer.y = event->y;
    pointer.local_x = event->local_x;
    pointer.local_y = event->local_y;
    pointer.delta_x = event->delta_x;
    pointer.delta_y = event->delta_y;
    pointer.timestamp_nanoseconds = event->timestamp_nanoseconds;
    pointer.on_target = event->target != 0U;
    pointer.has_local_position = true;
    pointer.shift = (event->modifiers & STRATA_KEY_MODIFIER_SHIFT) != 0U;
    pointer.control = (event->modifiers & STRATA_KEY_MODIFIER_CONTROL) != 0U;
    pointer.alt = (event->modifiers & STRATA_KEY_MODIFIER_ALT) != 0U;
    pointer.super = (event->modifiers & STRATA_KEY_MODIFIER_SUPER) != 0U;
    if (event->struct_size >= sizeof(strata_widget_pointer_event)) {
        pointer.subtarget_id =
            std::string_view(event->subtarget_id.data != nullptr ? event->subtarget_id.data : "",
                             event->subtarget_id.size);
        pointer.subtarget_index = event->subtarget_index;
    }
    Input input(context);
    return hooks->pointer(input, pointer) ? STRATA_EXTENSION_INPUT_CONSUMED
                                          : STRATA_EXTENSION_INPUT_IGNORED;
}
strata_extension_input_result
widget_scroll_trampoline(void* const user_data, strata_widget_input_context* const context,
                         const strata_widget_scroll_event* const event) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    if (event == nullptr)
        return STRATA_EXTENSION_INPUT_IGNORED;
    Scroll scroll;
    switch (event->phase) {
    case STRATA_EXTENSION_EVENT_CAPTURE:
        scroll.phase = Pointer::Phase::capture;
        break;
    case STRATA_EXTENSION_EVENT_BUBBLE:
        scroll.phase = Pointer::Phase::bubble;
        break;
    default:
        scroll.phase = Pointer::Phase::target;
        break;
    }
    scroll.x = event->x;
    scroll.y = event->y;
    scroll.local_x = event->local_x;
    scroll.local_y = event->local_y;
    scroll.delta_x = event->delta_x;
    scroll.delta_y = event->delta_y;
    scroll.on_target = event->target != 0U;
    scroll.shift = (event->modifiers & STRATA_KEY_MODIFIER_SHIFT) != 0U;
    scroll.control = (event->modifiers & STRATA_KEY_MODIFIER_CONTROL) != 0U;
    scroll.alt = (event->modifiers & STRATA_KEY_MODIFIER_ALT) != 0U;
    scroll.super = (event->modifiers & STRATA_KEY_MODIFIER_SUPER) != 0U;
    Input input(context);
    return hooks->scroll(input, scroll) ? STRATA_EXTENSION_INPUT_CONSUMED
                                        : STRATA_EXTENSION_INPUT_IGNORED;
}

void present_trampoline(void* const user_data, strata_widget_render_context* const context) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    Present present(context);
    hooks->present(present);
}

void overlay_trampoline(void* const user_data, strata_widget_render_context* const context) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    Present present(context);
    hooks->overlay(present);
}

void semantics_trampoline(void* const user_data, strata_widget_semantics_context* const context) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    Semantics semantics(context);
    hooks->semantics(semantics);
}

void subtargets_trampoline(void* const user_data, strata_widget_subtargets_context* const context) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    Subtargets subtargets(context);
    hooks->subtargets(subtargets);
}

strata_rect hit_bounds_trampoline(void* const user_data,
                                  strata_widget_inspection_context* const context) {
    const auto* const hooks = static_cast<const detail::WidgetHooks*>(user_data);
    Inspect inspect(context);
    return hooks->hit_bounds(inspect);
}

strata_extension_input_result
behavior_pointer_trampoline(void* const user_data, strata_behavior_input_context* const context,
                            const strata_behavior_pointer_event* const event) {
    const auto* const hooks = static_cast<const detail::BehaviorHooks*>(user_data);
    if (event == nullptr)
        return STRATA_EXTENSION_INPUT_IGNORED;
    Pointer pointer;
    switch (event->kind) {
    case STRATA_INPUT_POINTER_MOVE:
        pointer.kind = Pointer::Kind::move;
        break;
    case STRATA_INPUT_POINTER_PRESS:
        pointer.kind = Pointer::Kind::press;
        break;
    case STRATA_INPUT_POINTER_RELEASE:
        pointer.kind = Pointer::Kind::release;
        break;
    default:
        pointer.kind = Pointer::Kind::cancel;
        break;
    }
    switch (event->phase) {
    case STRATA_BEHAVIOR_EVENT_CAPTURE:
        pointer.phase = Pointer::Phase::capture;
        break;
    case STRATA_BEHAVIOR_EVENT_BUBBLE:
        pointer.phase = Pointer::Phase::bubble;
        break;
    default:
        pointer.phase = Pointer::Phase::target;
        break;
    }
    pointer.button = event->button;
    pointer.pointer_id = event->pointer_id;
    pointer.x = event->x;
    pointer.y = event->y;
    pointer.on_target = event->target != 0U;
    pointer.shift = (event->modifiers & STRATA_KEY_MODIFIER_SHIFT) != 0U;
    pointer.control = (event->modifiers & STRATA_KEY_MODIFIER_CONTROL) != 0U;
    pointer.alt = (event->modifiers & STRATA_KEY_MODIFIER_ALT) != 0U;
    pointer.super = (event->modifiers & STRATA_KEY_MODIFIER_SUPER) != 0U;
    BehaviorInput input(context);
    return hooks->pointer(input, pointer) ? STRATA_EXTENSION_INPUT_CONSUMED
                                          : STRATA_EXTENSION_INPUT_IGNORED;
}

} // namespace

double Input::get(const Retained<number>& field) const noexcept {
    return strata_widget_input_retained_number(context_, view(field.name), field.fallback);
}

bool Input::get(const Retained<boolean>& field) const noexcept {
    return strata_widget_input_retained_boolean(context_, view(field.name),
                                                field.fallback ? 1U : 0U) != 0U;
}

std::string Input::get(const Retained<extension::text>& field) const {
    return read_text(
               [this](const auto... arguments) {
                   return strata_widget_input_retained_text(context_, arguments...);
               },
               field.name)
        .value_or(std::string(field.fallback));
}

double Input::get(const Parameter<number>& field) const noexcept {
    return strata_widget_input_property_number(context_, view(field.name),
                                               field.value.value_or(0.0));
}

bool Input::get(const Parameter<boolean>& field) const noexcept {
    return strata_widget_input_property_boolean(context_, view(field.name),
                                                field.value.value_or(false) ? 1U : 0U) != 0U;
}

std::string Input::get(const Parameter<extension::text>& field) const {
    return read_text(
               [this](const auto... arguments) {
                   return strata_widget_input_property_text(context_, arguments...);
               },
               field.name)
        .value_or(std::string(field.value.value_or(std::string_view{})));
}
Rect Input::bounds() const noexcept {
    return strata_widget_input_bounds(context_);
}
double Input::scale() const noexcept {
    return strata_widget_input_scale(context_);
}

bool Input::set(const Retained<number>& field, const double value) noexcept {
    return strata_widget_input_set_retained_number(context_, view(field.name), value).status ==
           STRATA_STATUS_OK;
}

bool Input::set(const Retained<boolean>& field, const bool value) noexcept {
    return strata_widget_input_set_retained_boolean(context_, view(field.name), value ? 1U : 0U)
               .status == STRATA_STATUS_OK;
}

bool Input::set(const Retained<extension::text>& field, const std::string_view value) noexcept {
    return strata_widget_input_set_retained_text(context_, view(field.name), view(value)).status ==
           STRATA_STATUS_OK;
}
bool Input::claim_gesture() noexcept {
    return strata_widget_input_claim_gesture(context_) != 0U;
}

bool Input::cancel_gesture() noexcept {
    return strata_widget_input_cancel_gesture(context_) != 0U;
}
bool Input::invalidate(const Invalidation value) noexcept {
    return strata_widget_input_invalidate(context_, invalidation_value(value)).status ==
           STRATA_STATUS_OK;
}

bool Input::emit(const std::string_view action_id, const std::string_view payload_json,
                 const std::string_view event_kind,
                 const std::string_view event_value_json) noexcept {
    return strata_widget_input_emit_action_json(context_, view(action_id), view(payload_json),
                                                view(event_kind), view(event_value_json))
               .status == STRATA_STATUS_OK;
}

bool Input::emit_event(const std::string_view event_kind,
                       const std::string_view event_value_json) noexcept {
    return strata_widget_input_emit_event_json(context_, view(event_kind), view(event_value_json))
               .status == STRATA_STATUS_OK;
}
bool Input::emit_event(const std::string_view event_kind, const double value) noexcept {
    return strata_widget_input_emit_number_event(context_, view(event_kind), value).status ==
           STRATA_STATUS_OK;
}

bool Input::emit_event(const std::string_view event_kind, const bool value) noexcept {
    return strata_widget_input_emit_boolean_event(context_, view(event_kind), value ? 1U : 0U)
               .status == STRATA_STATUS_OK;
}

bool Input::emit_text_event(const std::string_view event_kind,
                            const std::string_view value) noexcept {
    return strata_widget_input_emit_text_event(context_, view(event_kind), view(value)).status ==
           STRATA_STATUS_OK;
}

bool Input::live(const double value) noexcept {
    return emit_event("number-changing", value);
}
bool Input::live(const bool value) noexcept {
    return emit_event("boolean-changing", value);
}
bool Input::live_text(const std::string_view value) noexcept {
    return emit_text_event("text-changing", value);
}
bool Input::commit(const double value) noexcept {
    return invalidate(Invalidation::semantics) && emit_event("number-changed", value);
}
bool Input::commit(const bool value) noexcept {
    return invalidate(Invalidation::semantics) && emit_event("boolean-changed", value);
}
bool Input::commit_text(const std::string_view value) noexcept {
    return invalidate(Invalidation::semantics) && emit_text_event("text-changed", value);
}

Rect Present::bounds() const noexcept {
    return strata_widget_render_bounds(context_);
}
Rect Present::root_bounds() const noexcept {
    return strata_widget_render_root_bounds(context_);
}
bool Present::focused() const noexcept {
    return strata_widget_render_focused(context_) != 0U;
}
bool Present::focus_visible() const noexcept {
    return strata_widget_render_focus_visible(context_) != 0U;
}
bool Present::hovered() const noexcept {
    return strata_widget_render_hovered(context_) != 0U;
}
bool Present::enabled() const noexcept {
    return strata_widget_render_enabled(context_) != 0U;
}
bool Present::active() const noexcept {
    return strata_widget_render_active(context_) != 0U;
}
double Present::scale() const noexcept {
    return strata_widget_render_scale(context_);
}

double Present::motion(const std::string_view channel, const double fallback) const noexcept {
    return strata_widget_render_motion_progress(context_, view(channel), fallback);
}

double Present::get(const Retained<number>& field) const noexcept {
    return strata_widget_render_retained_number(context_, view(field.name), field.fallback);
}

bool Present::get(const Retained<boolean>& field) const noexcept {
    return strata_widget_render_retained_boolean(context_, view(field.name),
                                                 field.fallback ? 1U : 0U) != 0U;
}

/* Qualified: the `text` render call shadows the `text` kind inside Present's member definitions. */
std::string Present::get(const Retained<extension::text>& field) const {
    return read_text(
               [this](const auto... arguments) {
                   return strata_widget_render_retained_text(context_, arguments...);
               },
               field.name)
        .value_or(std::string(field.fallback));
}

double Present::get(const Parameter<number>& field) const noexcept {
    return strata_widget_render_property_number(context_, view(field.name),
                                                field.value.value_or(0.0));
}

bool Present::get(const Parameter<boolean>& field) const noexcept {
    return strata_widget_render_property_boolean(context_, view(field.name),
                                                 field.value.value_or(false) ? 1U : 0U) != 0U;
}

std::string Present::get(const Parameter<extension::text>& field) const {
    return read_text(
               [this](const auto... arguments) {
                   return strata_widget_render_property_text(context_, arguments...);
               },
               field.name)
        .value_or(std::string(field.value.value_or(std::string_view{})));
}

std::optional<Size> Present::measure(const std::string_view value) const noexcept {
    Size size;
    if (strata_widget_render_text_metrics(context_, view(value), &size.width, &size.height)
            .status != STRATA_STATUS_OK) {
        return std::nullopt;
    }
    return size;
}

void Present::rect(const Rect bounds, const Color fill) {
    strata_widget_render_solid_rect(context_, bounds, fill);
}

void Present::rounded_rect(const Rect bounds, const double radius, const Color fill) {
    strata_widget_render_rounded_rect(context_, bounds, radius, fill, nullptr);
}

void Present::rounded_rect(const Rect bounds, const double radius, const Color fill,
                           const Border border) {
    strata_widget_render_rounded_rect(context_, bounds, radius, fill, &border);
}

void Present::border(const Rect bounds, const double radius, const Border border) {
    strata_widget_render_border(context_, bounds, radius, border);
}

void Present::text(const std::string_view value, const double x, const double y,
                   const Color color) {
    strata_widget_render_text(context_, view(value), x, y, color);
}

void Present::text(const std::string_view value, const Rect bounds, const Color color,
                   const TextAlignment horizontal, const TextAlignment vertical) {
    strata_widget_render_aligned_text(context_, view(value), bounds,
                                      static_cast<strata_widget_text_alignment>(horizontal),
                                      static_cast<strata_widget_text_alignment>(vertical), color);
}

void Present::image(const Rect bounds, const std::string_view image, const Color tint,
                    const TextureRegion source) {
    strata_widget_render_image(context_, bounds, view(image), tint, source);
}

void Present::nine_patch(const Rect bounds, const std::string_view texture,
                         const Edges source_insets, const Edges destination_insets,
                         const Color tint, const TextureRegion source) {
    strata_widget_render_nine_patch(context_, bounds, view(texture), source_insets,
                                    destination_insets, source, tint);
}

void Present::mesh(const Rect bounds, const std::string_view id, const Mesh& geometry) {
    draw_mesh(context_, bounds, id, geometry, {}, nullptr);
}

void Present::mesh(const Rect bounds, const std::string_view id, const Mesh& geometry,
                   const std::string_view texture) {
    draw_mesh(context_, bounds, id, geometry, texture, nullptr);
}

void Present::mesh(const Rect bounds, const std::string_view id, const Mesh& geometry,
                   const std::string_view texture, const Material& material) {
    draw_mesh(context_, bounds, id, geometry, texture, &material);
}

void Present::blur(const Rect bounds, const double radius, const unsigned int downsample) {
    strata_widget_render_blur(context_, bounds, radius, downsample);
}

void Present::shadow(const Rect bounds, const CornerRadii radii, const Color color,
                     const double radius, const double spread) {
    strata_widget_render_shadow(context_, bounds, radii, color, radius, spread);
}

ClipScope Present::clip(const Rect bounds) {
    strata_widget_render_push_clip(context_, bounds);
    return ClipScope(context_);
}

double Semantics::get(const Retained<number>& field) const noexcept {
    return strata_widget_semantics_retained_number(context_, view(field.name), field.fallback);
}
std::string Semantics::get(const Retained<extension::text>& field) const {
    return read_text(
               [this](const auto... arguments) {
                   return strata_widget_semantics_retained_text(context_, arguments...);
               },
               field.name)
        .value_or(std::string(field.fallback));
}

bool Semantics::get(const Retained<boolean>& field) const noexcept {
    return strata_widget_semantics_retained_boolean(context_, view(field.name),
                                                    field.fallback ? 1U : 0U) != 0U;
}
double Semantics::get(const Parameter<number>& field) const noexcept {
    return strata_widget_semantics_property_number(context_, view(field.name),
                                                   field.value.value_or(0.0));
}

std::string Semantics::get(const Parameter<extension::text>& field) const {
    return read_text(
               [this](const auto... arguments) {
                   return strata_widget_semantics_property_text(context_, arguments...);
               },
               field.name)
        .value_or(std::string(field.value.value_or(std::string_view{})));
}

bool Semantics::get(const Parameter<boolean>& field) const noexcept {
    return strata_widget_semantics_property_boolean(context_, view(field.name),
                                                    field.value.value_or(false) ? 1U : 0U) != 0U;
}

void Semantics::name(const std::string_view value) noexcept {
    static_cast<void>(strata_widget_semantics_set_name(context_, view(value)));
}

void Semantics::value_text(const std::string_view value) noexcept {
    static_cast<void>(strata_widget_semantics_set_value_text(context_, view(value)));
}

void Semantics::add_action(const std::string_view value) noexcept {
    static_cast<void>(strata_widget_semantics_add_action(context_, view(value)));
}

void Semantics::checked(const bool value) noexcept {
    strata_widget_semantics_set_checked(context_, value ? 1U : 0U);
}

void Semantics::expanded(const bool value) noexcept {
    strata_widget_semantics_set_expanded(context_, value ? 1U : 0U);
}
void Semantics::value_range(const double current, const double minimum,
                            const double maximum) noexcept {
    strata_widget_semantics_set_value_range(context_, current, minimum, maximum);
}

void Semantics::selected(const bool value) noexcept {
    strata_widget_semantics_set_selected(context_, value ? 1U : 0U);
}

Rect Inspect::layout_bounds() const noexcept {
    return strata_widget_inspection_layout_bounds(context_);
}

bool BehaviorInput::emit(const std::string_view action_id, const std::string_view payload_json,
                         const std::string_view event_kind,
                         const std::string_view event_value_json) noexcept {
    return strata_behavior_input_emit_action_json(context_, view(action_id), view(payload_json),
                                                  view(event_kind), view(event_value_json))
               .status == STRATA_STATUS_OK;
}

Widget::Widget(std::string type) : type_(std::move(type)) {
    if (type_.empty())
        throw std::invalid_argument("extension widget type must not be empty");
}

Widget& Widget::declare_parameter(std::string name, const std::string_view kind,
                                  const bool required, std::optional<DefaultValue> default_value) {
    if (name.empty()) {
        throw std::invalid_argument("extension widget '" + type_ +
                                    "' declares an unnamed parameter");
    }
    const bool duplicate = std::ranges::any_of(
        parameters_, [&name](const Parameter& parameter) { return parameter.name == name; });
    if (duplicate) {
        throw std::invalid_argument("extension widget '" + type_ + "' declares parameter '" + name +
                                    "' twice");
    }
    if (required && default_value.has_value()) {
        throw std::invalid_argument("extension widget '" + type_ + "' gives required parameter '" +
                                    name + "' a default");
    }
    parameters_.push_back(
        Parameter{std::move(name), std::string(kind), required, std::move(default_value)});
    return *this;
}

Widget& Widget::children() {
    allows_children_ = true;
    return *this;
}

Widget& Widget::no_children() {
    allows_children_ = false;
    return *this;
}

Widget& Widget::focusable() {
    focusable_ = true;
    return *this;
}

Widget& Widget::intrinsic_size(const double width, const double height) {
    intrinsic_size_ = Size{width, height};
    return *this;
}

Widget& Widget::padding(const Padding value) {
    padding_ = value;
    return *this;
}

Widget& Widget::clip() {
    clip_ = true;
    return *this;
}

Widget& Widget::disclosure(Disclosure value) {
    if (value.expanded.name.empty()) {
        throw std::invalid_argument("extension widget '" + type_ +
                                    "' declares disclosure motion without a retained field");
    }
    disclosure_ = std::move(value);
    return *this;
}

Widget& Widget::declare_retained(std::string name, const Invalidation invalidation) {
    if (name.empty()) {
        throw std::invalid_argument("extension widget '" + type_ +
                                    "' declares an unnamed retained field");
    }
    const bool duplicate =
        std::ranges::any_of(retained_, [&name](const auto& field) { return field.first == name; });
    if (duplicate) {
        throw std::invalid_argument("extension widget '" + type_ + "' declares retained field '" +
                                    name + "' twice");
    }
    retained_.emplace_back(std::move(name), invalidation);
    return *this;
}

Widget& Widget::emits(ActionContract contract) {
    if (contract.id.empty()) {
        throw std::invalid_argument("extension widget '" + type_ + "' emits an unnamed action");
    }
    actions_.push_back(std::move(contract));
    return *this;
}

Widget& Widget::semantics_role(std::string role) {
    semantics_role_ = std::move(role);
    return *this;
}

Widget& Widget::semantics_actions(std::vector<std::string> actions) {
    semantics_actions_ = std::move(actions);
    return *this;
}

Widget& Widget::on_activate(const ActivateHook hook) {
    hooks_.activate = hook;
    return *this;
}

Widget& Widget::on_key(const KeyHook hook) {
    hooks_.key = hook;
    return *this;
}
Widget& Widget::on_pointer(const WidgetPointerHook hook) {
    hooks_.pointer = hook;
    return *this;
}
Widget& Widget::on_scroll(const ScrollHook hook) {
    hooks_.scroll = hook;
    return *this;
}

Widget& Widget::on_semantics(const SemanticsHook hook) {
    hooks_.semantics = hook;
    return *this;
}

Widget& Widget::subtargets(const SubtargetsHook hook) {
    hooks_.subtargets = hook;
    return *this;
}

Widget& Widget::present(const PresentHook hook) {
    hooks_.present = hook;
    return *this;
}

Widget& Widget::overlay(const PresentHook hook) {
    hooks_.overlay = hook;
    return *this;
}

Widget& Widget::detached_overlay(const PresentHook hook, const Retained<boolean>& open) {
    if (open.name.empty()) {
        throw std::invalid_argument("extension widget '" + type_ +
                                    "' declares a detached overlay without a retained field");
    }
    hooks_.overlay = hook;
    detached_overlay_ = true;
    popup_retained_ = std::string(open.name);
    return *this;
}

Widget& Widget::hit_bounds(const HitBoundsHook hook) {
    hooks_.hit_bounds = hook;
    return *this;
}

Widget& Widget::depends_on_motion() {
    depends_on_motion_ = true;
    return *this;
}

Widget& Widget::depends_on_status() {
    depends_on_status_ = true;
    return *this;
}

strata_widget_extension Widget::descriptor() {
    std::string description;
    for (const Parameter& parameter : parameters_) {
        if (!parameter.default_value.has_value())
            continue;
        if (!description.empty())
            description.push_back(',');
        description += quoted(parameter.name);
        description.push_back(':');
        std::visit(
            [&description](const auto& value) {
                using Type = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Type, double>) {
                    description += json_number(value);
                } else if constexpr (std::is_same_v<Type, bool>) {
                    description += value ? "true" : "false";
                } else {
                    description += quoted(value);
                }
            },
            *parameter.default_value);
    }
    if (disclosure_.has_value()) {
        if (!description.empty())
            description.push_back(',');
        description += std::format(
            R"("$contentSizeMotionDefaults":{{"clip":true,"durationNanos":{},"height":true,"width":false}},)"
            R"("$disclosureDefaults":{{"collapsedExtent":{},"durationNanos":{},"retained":{}}})",
            disclosure_->duration_nanoseconds, json_number(disclosure_->collapsed_extent),
            disclosure_->duration_nanoseconds, quoted(disclosure_->expanded.name));
    }
    description_json_ = description.empty() ? std::string{} : "{" + description + "}";

    std::string layout;
    if (clip_)
        layout += R"("clip":true)";
    if (intrinsic_size_.has_value()) {
        if (!layout.empty())
            layout.push_back(',');
        layout +=
            std::format(R"("intrinsicSize":{{"height":{},"width":{}}})",
                        json_number(intrinsic_size_->height), json_number(intrinsic_size_->width));
    }
    if (padding_.has_value()) {
        if (!layout.empty())
            layout.push_back(',');
        layout += std::format(R"("padding":{{"bottom":{},"left":{},"right":{},"top":{}}})",
                              json_number(padding_->bottom), json_number(padding_->left),
                              json_number(padding_->right), json_number(padding_->top));
    }
    layout_json_ = layout.empty() ? std::string{} : "{" + layout + "}";

    retained_fields_.clear();
    retained_fields_.reserve(retained_.size());
    for (const auto& [name, invalidation] : retained_) {
        retained_fields_.push_back(strata_widget_retained_field{
            sizeof(strata_widget_retained_field),
            view(name),
            invalidation_value(invalidation),
            0U,
        });
    }

    std::uint64_t flags = 0U;
    if (focusable_)
        flags |= STRATA_WIDGET_EXTENSION_FOCUSABLE;
    if (detached_overlay_)
        flags |= STRATA_WIDGET_EXTENSION_DETACHED_OVERLAY;
    if (depends_on_motion_)
        flags |= STRATA_WIDGET_EXTENSION_DEPENDS_ON_MOTION;
    if (depends_on_status_)
        flags |= STRATA_WIDGET_EXTENSION_DEPENDS_ON_STATUS;

    strata_widget_extension descriptor{};
    descriptor.struct_size = sizeof(strata_widget_extension);
    descriptor.type = view(type_);
    descriptor.flags = flags;
    descriptor.user_data = &hooks_;
    descriptor.description_properties_json = view(description_json_);
    descriptor.layout_defaults_json = view(layout_json_);
    descriptor.popup_retained = view(popup_retained_);
    descriptor.activate = hooks_.activate != nullptr ? &activate_trampoline : nullptr;
    descriptor.present = hooks_.present != nullptr ? &present_trampoline : nullptr;
    descriptor.overlay = hooks_.overlay != nullptr ? &overlay_trampoline : nullptr;
    descriptor.hit_bounds = hooks_.hit_bounds != nullptr ? &hit_bounds_trampoline : nullptr;
    descriptor.key = hooks_.key != nullptr ? &key_trampoline : nullptr;
    descriptor.semantics = hooks_.semantics != nullptr ? &semantics_trampoline : nullptr;
    descriptor.semantics_role = view(semantics_role_);
    descriptor.retained_fields = retained_fields_.empty() ? nullptr : retained_fields_.data();
    descriptor.retained_field_count = retained_fields_.size();
    descriptor.subtargets = hooks_.subtargets != nullptr ? &subtargets_trampoline : nullptr;
    return descriptor;
}
std::optional<strata_widget_input_extension> Widget::input_descriptor() {
    if (hooks_.pointer == nullptr)
        return std::nullopt;
    return strata_widget_input_extension{
        sizeof(strata_widget_input_extension),
        view(type_),
        &hooks_,
        &widget_pointer_trampoline,
    };
}
std::optional<strata_widget_scroll_extension> Widget::scroll_descriptor() {
    if (hooks_.scroll == nullptr)
        return std::nullopt;
    return strata_widget_scroll_extension{
        sizeof(strata_widget_scroll_extension),
        view(type_),
        &hooks_,
        &widget_scroll_trampoline,
    };
}

std::string Widget::schema_json() const {
    std::string parameters;
    for (const Parameter& parameter : parameters_) {
        if (!parameters.empty())
            parameters.push_back(',');
        parameters += parameter_json(parameter.name, parameter.kind, parameter.required);
    }
    return std::format(R"({{"name":{},"allowsChildren":{},"parameters":[{}]}})", quoted(type_),
                       allows_children_ ? "true" : "false", parameters);
}

Behavior::Behavior(std::string id) : id_(std::move(id)) {
    if (id_.empty())
        throw std::invalid_argument("extension behavior id must not be empty");
}

Behavior& Behavior::focusable() {
    focusable_ = true;
    return *this;
}

Behavior& Behavior::on_pointer(const PointerHook hook) {
    hooks_.pointer = hook;
    return *this;
}

Behavior& Behavior::emits(ActionContract contract) {
    if (contract.id.empty()) {
        throw std::invalid_argument("extension behavior '" + id_ + "' emits an unnamed action");
    }
    actions_.push_back(std::move(contract));
    return *this;
}

strata_behavior_extension Behavior::descriptor() {
    std::uint64_t flags = 0U;
    if (focusable_)
        flags |= STRATA_BEHAVIOR_EXTENSION_FOCUSABLE;
    if (hooks_.pointer != nullptr)
        flags |= STRATA_BEHAVIOR_EXTENSION_ACCEPTS_POINTER;
    return strata_behavior_extension{
        sizeof(strata_behavior_extension),
        view(id_),
        flags,
        &hooks_,
        hooks_.pointer != nullptr ? &behavior_pointer_trampoline : nullptr,
    };
}

Package::Package(std::string id) : id_(std::move(id)) {
    if (id_.empty())
        throw std::invalid_argument("extension package id must not be empty");
}

Package& Package::widget(Widget definition) {
    if (finalized_) {
        throw std::logic_error("extension package '" + id_ + "' is already in use by a surface");
    }
    const bool duplicate = std::ranges::any_of(widgets_, [&definition](const Widget& existing) {
        return existing.type() == definition.type();
    });
    if (duplicate) {
        throw std::invalid_argument("extension package '" + id_ + "' declares widget '" +
                                    definition.type() + "' twice");
    }
    widgets_.push_back(std::move(definition));
    return *this;
}

Package& Package::behavior(Behavior definition) {
    if (finalized_) {
        throw std::logic_error("extension package '" + id_ + "' is already in use by a surface");
    }
    const bool duplicate = std::ranges::any_of(behaviors_, [&definition](const Behavior& existing) {
        return existing.id() == definition.id();
    });
    if (duplicate) {
        throw std::invalid_argument("extension package '" + id_ + "' declares behavior '" +
                                    definition.id() + "' twice");
    }
    behaviors_.push_back(std::move(definition));
    return *this;
}

void Package::finalize() {
    if (finalized_)
        return;
    widget_descriptors_.reserve(widgets_.size());
    widget_input_descriptors_.reserve(widgets_.size());
    widget_scroll_descriptors_.reserve(widgets_.size());
    for (Widget& definition : widgets_) {
        widget_descriptors_.push_back(definition.descriptor());
        if (std::optional<strata_widget_input_extension> input = definition.input_descriptor();
            input.has_value()) {
            widget_input_descriptors_.push_back(*input);
        }
        if (std::optional<strata_widget_scroll_extension> scroll = definition.scroll_descriptor();
            scroll.has_value()) {
            widget_scroll_descriptors_.push_back(*scroll);
        }
    }
    behavior_descriptors_.reserve(behaviors_.size());
    for (Behavior& definition : behaviors_)
        behavior_descriptors_.push_back(definition.descriptor());
    bundle_ = strata_surface_extension_bundle{
        sizeof(strata_surface_extension_bundle),
        widget_descriptors_.empty() ? nullptr : widget_descriptors_.data(),
        widget_descriptors_.size(),
        behavior_descriptors_.empty() ? nullptr : behavior_descriptors_.data(),
        behavior_descriptors_.size(),
        widget_input_descriptors_.empty() ? nullptr : widget_input_descriptors_.data(),
        widget_input_descriptors_.size(),
        widget_scroll_descriptors_.empty() ? nullptr : widget_scroll_descriptors_.data(),
        widget_scroll_descriptors_.size(),
    };
    finalized_ = true;
}

const strata_surface_extension_bundle& Package::bundle() {
    finalize();
    return bundle_;
}

std::string Package::schema_json() const {
    std::string widgets;
    std::string actions;
    const auto append_actions = [&actions](const std::vector<ActionContract>& contracts) {
        for (const ActionContract& contract : contracts) {
            if (!actions.empty())
                actions.push_back(',');
            actions += action_json(contract);
        }
    };
    for (const Widget& definition : widgets_) {
        if (!widgets.empty())
            widgets.push_back(',');
        widgets += definition.schema_json();
        append_actions(definition.actions());
    }
    for (const Behavior& definition : behaviors_)
        append_actions(definition.actions());
    std::string behaviors;
    for (const Behavior& definition : behaviors_) {
        if (!behaviors.empty())
            behaviors.push_back(',');
        behaviors += quoted(definition.id());
    }
    return std::format(R"({{"package":{},"widgets":{{"definitions":[{}]}},)"
                       R"("behaviors":{{"ids":[{}]}},"actions":{{"definitions":[{}]}},"host":[]}})",
                       quoted(id_), widgets, behaviors, actions);
}

std::vector<std::string> Package::widget_types() const {
    std::vector<std::string> types;
    types.reserve(widgets_.size());
    for (const Widget& definition : widgets_)
        types.push_back(definition.type());
    return types;
}

std::vector<std::string> Package::behavior_ids() const {
    std::vector<std::string> ids;
    ids.reserve(behaviors_.size());
    for (const Behavior& definition : behaviors_)
        ids.push_back(definition.id());
    return ids;
}

namespace detail {

strata_status query_plugin(const PackageFactory factory, const std::uint32_t requested_plugin_abi,
                           strata_extension_plugin* const output) noexcept {
    if (output == nullptr || output->struct_size < sizeof(strata_extension_plugin) ||
        factory == nullptr) {
        return STRATA_STATUS_INVALID_ARGUMENT;
    }
    if (requested_plugin_abi != STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT) {
        return STRATA_STATUS_UNSUPPORTED_ABI;
    }
    try {
        static std::mutex mutex;
        static PackageFactory active_factory = nullptr;
        static std::unique_ptr<Package> package;
        static std::string schema;
        const std::scoped_lock lock(mutex);
        if (active_factory != nullptr && active_factory != factory) {
            return STRATA_STATUS_INVARIANT_FAILURE;
        }
        if (package == nullptr) {
            package = factory();
            if (package == nullptr)
                return STRATA_STATUS_INTERNAL_ERROR;
            schema = package->schema_json();
            static_cast<void>(package->bundle());
            active_factory = factory;
        }
        *output = strata_extension_plugin{
            sizeof(strata_extension_plugin),
            STRATA_EXTENSION_PLUGIN_ABI_VERSION_CURRENT,
            STRATA_ABI_VERSION_CURRENT,
            view(package->id()),
            view(schema),
            &package->bundle(),
        };
        return STRATA_STATUS_OK;
    } catch (const std::bad_alloc&) {
        return STRATA_STATUS_OUT_OF_MEMORY;
    } catch (...) {
        return STRATA_STATUS_INTERNAL_ERROR;
    }
}

} // namespace detail

} // namespace strata::extension
