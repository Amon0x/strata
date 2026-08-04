#include "ui/motion/model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

#include "core/utf8.hpp"

namespace strata::ui {
namespace {

struct PropertyName final {
    MotionProperty property;
    std::string_view name;
};

constexpr std::array property_names{
    PropertyName{MotionProperty::width, "width"},
    PropertyName{MotionProperty::height, "height"},
    PropertyName{MotionProperty::min_width, "minWidth"},
    PropertyName{MotionProperty::min_height, "minHeight"},
    PropertyName{MotionProperty::max_width, "maxWidth"},
    PropertyName{MotionProperty::max_height, "maxHeight"},
    PropertyName{MotionProperty::margin_left, "marginLeft"},
    PropertyName{MotionProperty::margin_top, "marginTop"},
    PropertyName{MotionProperty::margin_right, "marginRight"},
    PropertyName{MotionProperty::margin_bottom, "marginBottom"},
    PropertyName{MotionProperty::padding_left, "paddingLeft"},
    PropertyName{MotionProperty::padding_top, "paddingTop"},
    PropertyName{MotionProperty::padding_right, "paddingRight"},
    PropertyName{MotionProperty::padding_bottom, "paddingBottom"},
    PropertyName{MotionProperty::placement_x, "placementX"},
    PropertyName{MotionProperty::placement_y, "placementY"},
    PropertyName{MotionProperty::background, "background"},
    PropertyName{MotionProperty::foreground, "foreground"},
    PropertyName{MotionProperty::color, "color"},
    PropertyName{MotionProperty::radius, "radius"},
    PropertyName{MotionProperty::track, "track"},
    PropertyName{MotionProperty::fill, "fill"},
    PropertyName{MotionProperty::thumb, "thumb"},
    PropertyName{MotionProperty::track_radius, "trackRadius"},
    PropertyName{MotionProperty::thumb_radius, "thumbRadius"},
    PropertyName{MotionProperty::thumb_size, "thumbSize"},
    PropertyName{MotionProperty::indicator_position, "indicatorPosition"},
    PropertyName{MotionProperty::clip, "clip"},
    PropertyName{MotionProperty::opacity, "opacity"},
    PropertyName{MotionProperty::x, "x"},
    PropertyName{MotionProperty::y, "y"},
    PropertyName{MotionProperty::translate_x, "translateX"},
    PropertyName{MotionProperty::translate_y, "translateY"},
    PropertyName{MotionProperty::scale, "scale"},
    PropertyName{MotionProperty::scale_x, "scaleX"},
    PropertyName{MotionProperty::scale_y, "scaleY"},
};

[[nodiscard]] std::string normalized(const std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char byte : value) {
        const auto character = static_cast<unsigned char>(byte);
        if (character == '-' || character == '_') continue;
        result.push_back(static_cast<char>(
            character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character
        ));
    }
    return result;
}

[[nodiscard]] std::string normalized_easing(const std::string_view value) {
    const std::optional<std::string_view> trimmed = core::trim_utf8_white_space(value);
    if (!trimmed.has_value()) {
        throw std::invalid_argument("motion easing name must be valid UTF-8");
    }
    return normalized(*trimmed);
}

[[nodiscard]] std::uint8_t channel(
    const std::uint8_t from,
    const std::uint8_t to,
    const double fraction
) noexcept {
    return static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(static_cast<double>(from) +
                         (static_cast<double>(to) - static_cast<double>(from)) * fraction),
        0,
        255
    ));
}

[[nodiscard]] double cubic_bezier(
    const double fraction,
    const double x1,
    const double y1,
    const double x2,
    const double y2
) noexcept {
    const double target = std::clamp(fraction, 0.0, 1.0);
    const auto sample = [](const double t, const double first, const double second) {
        const double inverse = 1.0 - t;
        return 3.0 * inverse * inverse * t * first +
               3.0 * inverse * t * t * second + t * t * t;
    };
    double lower = 0.0;
    double upper = 1.0;
    double t = target;
    for (std::size_t iteration = 0U; iteration < 12U; ++iteration) {
        t = (lower + upper) * 0.5;
        if (sample(t, x1, x2) < target) lower = t;
        else upper = t;
    }
    return std::clamp(sample(t, y1, y2), 0.0, 1.0);
}

} // namespace

const MotionValue* MotionComputedValues::find(const MotionProperty property) const noexcept {
    const auto found = values.find(property);
    return found != values.end() ? &found->second : nullptr;
}

std::optional<double> MotionComputedValues::number(const MotionProperty property) const noexcept {
    const MotionValue* value = find(property);
    const double* number = value != nullptr ? std::get_if<double>(value) : nullptr;
    return number != nullptr ? std::optional<double>(*number) : std::nullopt;
}

const MotionLayoutValue* MotionComputedValues::layout(
    const MotionProperty property
) const noexcept {
    const MotionValue* value = find(property);
    return value != nullptr ? std::get_if<MotionLayoutValue>(value) : nullptr;
}

const runtime::ColorValue* MotionComputedValues::color(const MotionProperty property) const noexcept {
    const MotionValue* value = find(property);
    return value != nullptr ? std::get_if<runtime::ColorValue>(value) : nullptr;
}

std::optional<bool> MotionComputedValues::boolean(const MotionProperty property) const noexcept {
    const MotionValue* value = find(property);
    const bool* boolean = value != nullptr ? std::get_if<bool>(value) : nullptr;
    return boolean != nullptr ? std::optional<bool>(*boolean) : std::nullopt;
}

bool MotionComputedValues::affects_layout() const noexcept {
    return std::ranges::any_of(values, [](const auto& value) {
        return motion_property_affects_layout(value.first);
    });
}

bool MotionTransform::identity() const noexcept {
    return translate_x == 0.0 && translate_y == 0.0 && scale_x == 1.0 && scale_y == 1.0;
}

std::optional<MotionProperty> motion_property(const std::string_view name) noexcept {
    const std::string candidate = normalized(name);
    for (const PropertyName& entry : property_names) {
        if (candidate == normalized(entry.name)) return entry.property;
    }
    return std::nullopt;
}

std::string_view motion_property_name(const MotionProperty property) noexcept {
    for (const PropertyName& entry : property_names) {
        if (entry.property == property) return entry.name;
    }
    return "opacity";
}

bool motion_property_affects_layout(const MotionProperty property) noexcept {
    return property >= MotionProperty::width && property <= MotionProperty::placement_y;
}

bool motion_property_interpolable(const MotionProperty property) noexcept {
    return property != MotionProperty::clip;
}

bool motion_property_accepts(
    const MotionProperty property,
    const MotionValue& value
) noexcept {
    const bool color_property = property == MotionProperty::background ||
                                property == MotionProperty::foreground ||
                                property == MotionProperty::color ||
                                property == MotionProperty::track ||
                                property == MotionProperty::fill ||
                                property == MotionProperty::thumb;
    if (color_property) return std::holds_alternative<runtime::ColorValue>(value);
    if (property == MotionProperty::clip) return std::holds_alternative<bool>(value);
    if (std::holds_alternative<MotionLayoutValue>(value)) {
        return motion_property_affects_layout(property);
    }
    return std::holds_alternative<double>(value);
}

std::optional<MotionTrigger> motion_trigger(const std::string_view name) noexcept {
    const std::string value = normalized(name);
    if (value == "enter") return MotionTrigger::enter;
    if (value == "exit") return MotionTrigger::exit;
    if (value == "hover") return MotionTrigger::hover;
    if (value == "pressed") return MotionTrigger::pressed;
    if (value == "focus") return MotionTrigger::focus;
    if (value == "checked") return MotionTrigger::checked;
    if (value == "move") return MotionTrigger::move;
    if (value == "animate") return MotionTrigger::animate;
    if (value == "focusvisible") return MotionTrigger::focus_visible;
    return std::nullopt;
}

std::string_view motion_trigger_name(const MotionTrigger trigger) noexcept {
    switch (trigger) {
    case MotionTrigger::enter: return "enter";
    case MotionTrigger::exit: return "exit";
    case MotionTrigger::hover: return "hover";
    case MotionTrigger::pressed: return "pressed";
    case MotionTrigger::focus: return "focus";
    case MotionTrigger::checked: return "checked";
    case MotionTrigger::move: return "move";
    case MotionTrigger::animate: return "animate";
    case MotionTrigger::focus_visible: return "focusVisible";
    }
    return "animate";
}

std::string_view motion_direction_name(const MotionDirection direction) noexcept {
    switch (direction) {
    case MotionDirection::forward: return "FORWARD";
    case MotionDirection::reverse: return "REVERSE";
    case MotionDirection::to_target: return "TO_TARGET";
    case MotionDirection::expand: return "EXPAND";
    case MotionDirection::collapse: return "COLLAPSE";
    }
    return "FORWARD";
}

std::string_view motion_interaction_name(const MotionInteraction interaction) noexcept {
    switch (interaction) {
    case MotionInteraction::hover: return "hover";
    case MotionInteraction::pressed: return "pressed";
    case MotionInteraction::focus: return "focus";
    case MotionInteraction::focus_visible: return "focus-visible";
    }
    return "hover";
}

MotionValue interpolate_motion_value(
    const MotionValue& from,
    const MotionValue& to,
    const double fraction
) {
    const double t = std::clamp(fraction, 0.0, 1.0);
    if (from.index() != to.index()) return t < 1.0 ? from : to;
    if (const double* start = std::get_if<double>(&from)) {
        return *start + (*std::get_if<double>(&to) - *start) * t;
    }
    if (const auto* start = std::get_if<runtime::ColorValue>(&from)) {
        const runtime::ColorValue& end = *std::get_if<runtime::ColorValue>(&to);
        return runtime::ColorValue{
            channel(start->red, end.red, t),
            channel(start->green, end.green, t),
            channel(start->blue, end.blue, t),
            channel(start->alpha, end.alpha, t),
        };
    }
    if (const auto* start = std::get_if<MotionLayoutValue>(&from)) {
        const MotionLayoutValue& end = *std::get_if<MotionLayoutValue>(&to);
        if (start->unit != end.unit) return t < 1.0 ? from : to;
        return MotionLayoutValue{
            start->unit,
            start->value + (end.value - start->value) * t,
        };
    }
    return t < 1.0 ? from : to;
}

MotionEasing::MotionEasing(const std::string_view name) {
    const std::string normalized_kind = normalized_easing(name);
    if (normalized_kind == "linear") this->kind = MotionEasingKind::linear;
    else if (normalized_kind == "cubicin") this->kind = MotionEasingKind::cubic_in;
    else if (normalized_kind == "cubicout") this->kind = MotionEasingKind::cubic_out;
    else if (
        normalized_kind == "cubicinout" || normalized_kind == "ease" ||
        normalized_kind == "easeinout"
    ) {
        this->kind = MotionEasingKind::cubic_in_out;
    } else if (normalized_kind == "easein") {
        *this = cubic_bezier(0.42, 0.0, 1.0, 1.0);
    } else if (normalized_kind == "easeout") {
        *this = cubic_bezier(0.0, 0.0, 0.58, 1.0);
    } else {
        throw std::invalid_argument("unknown motion easing '" + std::string(name) + "'");
    }
}

MotionEasing MotionEasing::cubic_bezier(
    const double x1,
    const double y1,
    const double x2,
    const double y2
) {
    MotionEasing result;
    result.kind = MotionEasingKind::cubic_bezier;
    result.x1 = x1;
    result.y1 = y1;
    result.x2 = x2;
    result.y2 = y2;
    result.validate();
    return result;
}

void MotionEasing::validate() const {
    if (kind > MotionEasingKind::cubic_bezier || !std::isfinite(x1) || !std::isfinite(y1) ||
        !std::isfinite(x2) || !std::isfinite(y2) ||
        (kind == MotionEasingKind::cubic_bezier &&
         (x1 < 0.0 || x1 > 1.0 || x2 < 0.0 || x2 > 1.0))) {
        throw std::invalid_argument("motion easing is invalid");
    }
}

bool operator==(const MotionEasing& value, const std::string_view name) noexcept {
    try {
        return value == MotionEasing(name);
    } catch (...) {
        return false;
    }
}

std::string motion_easing_name(const MotionEasing& easing) {
    switch (easing.kind) {
    case MotionEasingKind::linear: return "linear";
    case MotionEasingKind::cubic_in: return "cubic-in";
    case MotionEasingKind::cubic_out: return "cubic-out";
    case MotionEasingKind::cubic_in_out: return "cubic-in-out";
    case MotionEasingKind::cubic_bezier:
        return "cubic-bezier(" + std::to_string(easing.x1) + "," +
               std::to_string(easing.y1) + "," + std::to_string(easing.x2) + "," +
               std::to_string(easing.y2) + ")";
    }
    return "linear";
}

double motion_easing(const MotionEasing& easing, const double fraction) noexcept {
    const double t = std::clamp(fraction, 0.0, 1.0);
    if (easing.kind == MotionEasingKind::cubic_in) return t * t * t;
    if (easing.kind == MotionEasingKind::cubic_out) {
        const double shifted = t - 1.0;
        return shifted * shifted * shifted + 1.0;
    }
    if (easing.kind == MotionEasingKind::cubic_in_out) {
        if (t < 0.5) return 4.0 * t * t * t;
        const double shifted = 2.0 * t - 2.0;
        return 0.5 * shifted * shifted * shifted + 1.0;
    }
    if (easing.kind == MotionEasingKind::cubic_bezier) {
        return cubic_bezier(t, easing.x1, easing.y1, easing.x2, easing.y2);
    }
    return t;
}

double motion_terminal_progress(
    const CompiledMotion& animation,
    const MotionDirection direction
) noexcept {
    const std::uint32_t iterations = animation.timing.repeat.kind == MotionRepeatKind::count
                                         ? std::max(1U, animation.timing.repeat.iterations)
                                         : 1U;
    const bool alternate = animation.timing.reverse && (iterations % 2U == 0U);
    const MotionDirection terminal_direction = alternate
                                                   ? (direction == MotionDirection::reverse
                                                          ? MotionDirection::forward
                                                          : MotionDirection::reverse)
                                                   : direction;
    return terminal_direction == MotionDirection::reverse ? 0.0 : 1.0;
}

MotionTransform motion_transform(const MotionComputedValues& values) noexcept {
    const double uniform = values.number(MotionProperty::scale).value_or(1.0);
    return MotionTransform{
        values.number(MotionProperty::x)
            .value_or(values.number(MotionProperty::translate_x).value_or(0.0)),
        values.number(MotionProperty::y)
            .value_or(values.number(MotionProperty::translate_y).value_or(0.0)),
        values.number(MotionProperty::scale_x).value_or(uniform),
        values.number(MotionProperty::scale_y).value_or(uniform),
    };
}

} // namespace strata::ui
