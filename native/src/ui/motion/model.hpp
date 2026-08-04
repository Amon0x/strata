#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "runtime/value.hpp"

namespace strata::ui {

enum class MotionProperty {
    width,
    height,
    min_width,
    min_height,
    max_width,
    max_height,
    margin_left,
    margin_top,
    margin_right,
    margin_bottom,
    padding_left,
    padding_top,
    padding_right,
    padding_bottom,
    placement_x,
    placement_y,
    background,
    foreground,
    color,
    radius,
    track,
    fill,
    thumb,
    track_radius,
    thumb_radius,
    thumb_size,
    indicator_position,
    clip,
    opacity,
    x,
    y,
    translate_x,
    translate_y,
    scale,
    scale_x,
    scale_y,
};

enum class MotionTrigger {
    enter,
    exit,
    hover,
    pressed,
    focus,
    checked,
    move,
    animate,
    focus_visible,
};
enum class MotionDirection { forward, reverse, to_target, expand, collapse };
enum class MotionInteraction { hover, pressed, focus, focus_visible };
enum class MotionFillMode { none, forwards, backwards, both };
enum class MotionRepeatKind { none, count, forever };
enum class MotionEasingKind { linear, cubic_in, cubic_out, cubic_in_out, cubic_bezier };

struct MotionEasing final {
    MotionEasingKind kind = MotionEasingKind::linear;
    double x1 = 0.0;
    double y1 = 0.0;
    double x2 = 1.0;
    double y2 = 1.0;

    MotionEasing() = default;
    MotionEasing(MotionEasingKind kind) noexcept : kind(kind) {}
    MotionEasing(std::string_view name);
    MotionEasing(const char* name) : MotionEasing(std::string_view(name != nullptr ? name : "")) {}
    [[nodiscard]] static MotionEasing cubic_bezier(double x1, double y1, double x2, double y2);
    void validate() const;
    [[nodiscard]] friend bool operator==(const MotionEasing&, const MotionEasing&) = default;
    friend bool operator==(const MotionEasing& value, std::string_view name) noexcept;
};

struct MotionRepeat final {
    MotionRepeatKind kind = MotionRepeatKind::none;
    std::uint32_t iterations = 1U;
    [[nodiscard]] friend bool operator==(const MotionRepeat&, const MotionRepeat&) = default;
};

enum class MotionLayoutUnit { fixed, percent, fill };

struct MotionLayoutValue final {
    MotionLayoutUnit unit = MotionLayoutUnit::fixed;
    double value = 0.0;
    [[nodiscard]] friend bool operator==(const MotionLayoutValue&, const MotionLayoutValue&) =
        default;
};

using MotionValue = std::variant<double, runtime::ColorValue, bool, MotionLayoutValue>;

struct MotionKeyframe final {
    double offset = 0.0;
    MotionValue value = 0.0;
    std::optional<MotionEasing> easing;
    [[nodiscard]] friend bool operator==(const MotionKeyframe&, const MotionKeyframe&) = default;
};

struct MotionTrack final {
    MotionProperty property = MotionProperty::opacity;
    std::vector<MotionKeyframe> keyframes;
    [[nodiscard]] friend bool operator==(const MotionTrack&, const MotionTrack&) = default;
};

struct MotionTiming final {
    std::int64_t duration_nanos = 300'000'000;
    std::int64_t delay_nanos = 0;
    MotionEasing easing;
    MotionRepeat repeat;
    bool reverse = false;
    MotionFillMode fill_mode = MotionFillMode::both;
    [[nodiscard]] friend bool operator==(const MotionTiming&, const MotionTiming&) = default;
};

/** A property-neutral retained clock used by renderers that consume normalized progress. */
struct MotionTimelineSpec final {
    std::int64_t duration_nanos = 300'000'000;
    bool running = true;
    bool loop = false;
    bool affects_layout = false;
    [[nodiscard]] friend bool operator==(const MotionTimelineSpec&, const MotionTimelineSpec&) = default;
};

struct CompiledMotion final {
    std::string name;
    MotionTrigger trigger = MotionTrigger::animate;
    MotionTiming timing;
    std::vector<MotionTrack> tracks;
    [[nodiscard]] friend bool operator==(const CompiledMotion&, const CompiledMotion&) = default;
};

struct MotionComputedValues final {
    double progress = 0.0;
    std::map<MotionProperty, MotionValue> values;

    [[nodiscard]] const MotionValue* find(MotionProperty property) const noexcept;
    [[nodiscard]] std::optional<double> number(MotionProperty property) const noexcept;
    [[nodiscard]] const MotionLayoutValue* layout(MotionProperty property) const noexcept;
    [[nodiscard]] const runtime::ColorValue* color(MotionProperty property) const noexcept;
    [[nodiscard]] std::optional<bool> boolean(MotionProperty property) const noexcept;
    [[nodiscard]] bool affects_layout() const noexcept;
    [[nodiscard]] friend bool operator==(const MotionComputedValues&, const MotionComputedValues&) = default;
};

struct MotionTransform final {
    double translate_x = 0.0;
    double translate_y = 0.0;
    double scale_x = 1.0;
    double scale_y = 1.0;
    [[nodiscard]] bool identity() const noexcept;
    [[nodiscard]] friend bool operator==(const MotionTransform&, const MotionTransform&) = default;
};

[[nodiscard]] std::optional<MotionProperty> motion_property(std::string_view name) noexcept;
[[nodiscard]] std::string_view motion_property_name(MotionProperty property) noexcept;
[[nodiscard]] bool motion_property_affects_layout(MotionProperty property) noexcept;
[[nodiscard]] bool motion_property_interpolable(MotionProperty property) noexcept;
[[nodiscard]] bool motion_property_accepts(MotionProperty property, const MotionValue& value) noexcept;
[[nodiscard]] std::optional<MotionTrigger> motion_trigger(std::string_view name) noexcept;
[[nodiscard]] std::string_view motion_trigger_name(MotionTrigger trigger) noexcept;
[[nodiscard]] std::string_view motion_direction_name(MotionDirection direction) noexcept;
[[nodiscard]] std::string_view motion_interaction_name(MotionInteraction interaction) noexcept;
[[nodiscard]] MotionValue interpolate_motion_value(
    const MotionValue& from,
    const MotionValue& to,
    double fraction
);
[[nodiscard]] double motion_easing(const MotionEasing& easing, double fraction) noexcept;
[[nodiscard]] std::string motion_easing_name(const MotionEasing& easing);
[[nodiscard]] double motion_terminal_progress(
    const CompiledMotion& animation,
    MotionDirection direction
) noexcept;
[[nodiscard]] MotionTransform motion_transform(const MotionComputedValues& values) noexcept;

} // namespace strata::ui
