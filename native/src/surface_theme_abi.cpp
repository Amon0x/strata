#include <strata/strata.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "abi_internal.hpp"
#include "abi_support.hpp"
#include "core/utf8.hpp"
#include "ui/theme.hpp"

namespace {

[[nodiscard]] strata_result surface_failure(
    const strata_surface& surface,
    const strata_status status,
    const char* const code,
    const char* const message
) noexcept {
    return strata::abi_detail::runtime_failure(*surface.owner, status, code, message);
}

[[nodiscard]] strata::runtime::ColorValue color(const strata_color value) noexcept {
    return strata::runtime::ColorValue{value.red, value.green, value.blue, value.alpha};
}

[[nodiscard]] std::string string(
    const strata_string_view value,
    const std::string_view label,
    const bool allow_empty
) {
    if (!strata::abi_detail::valid_view(value, allow_empty)) {
        throw std::invalid_argument(std::string(label) + " view is invalid");
    }
    std::string result = strata::abi_detail::copied_string(value);
    if (!strata::core::valid_utf8(result)) {
        throw std::invalid_argument(std::string(label) + " must be valid UTF-8");
    }
    return result;
}

void validate_mode(
    const strata_theme_value_mode mode,
    const std::uint32_t reserved,
    const bool nullable,
    const std::string_view label
) {
    if (reserved != 0U || mode > STRATA_THEME_VALUE_NONE ||
        (!nullable && mode == STRATA_THEME_VALUE_NONE)) {
        throw std::invalid_argument(std::string(label) + " mode is invalid");
    }
}

void validate_boolean(const std::uint32_t value, const std::string_view label) {
    if (value > 1U) throw std::invalid_argument(std::string(label) + " boolean is invalid");
}

void validate_table(
    const void* const values,
    const std::size_t count,
    const std::size_t maximum,
    const std::string_view label
) {
    if (count > maximum || ((count == 0U) != (values == nullptr))) {
        throw std::invalid_argument(std::string(label) + " table is invalid");
    }
}

[[nodiscard]] std::optional<strata::runtime::ColorValue> nullable_color(
    const strata_theme_optional_color value,
    const std::string_view label
) {
    validate_mode(value.mode, value.reserved, true, label);
    return value.mode == STRATA_THEME_VALUE_SET
               ? std::optional<strata::runtime::ColorValue>(color(value.value))
               : std::nullopt;
}

[[nodiscard]] std::optional<strata::ui::ThemeBorder> nullable_border(
    const strata_theme_optional_border value,
    const std::string_view label
) {
    validate_mode(value.mode, value.reserved, true, label);
    if (value.value.reserved != 0U) throw std::invalid_argument(std::string(label) + " is invalid");
    validate_boolean(value.value.inside, label);
    return value.mode == STRATA_THEME_VALUE_SET
               ? std::optional<strata::ui::ThemeBorder>(strata::ui::ThemeBorder{
                     value.value.width,
                     color(value.value.color),
                     value.value.inside != 0U,
                 })
               : std::nullopt;
}

[[nodiscard]] std::optional<double> optional_number(
    const strata_theme_optional_number value,
    const std::string_view label
) {
    validate_mode(value.mode, value.reserved, true, label);
    if (value.mode == STRATA_THEME_VALUE_SET && !std::isfinite(value.value)) {
        throw std::invalid_argument(std::string(label) + " optional number is invalid");
    }
    return value.mode == STRATA_THEME_VALUE_SET ? std::optional<double>(value.value) : std::nullopt;
}

[[nodiscard]] strata::ui::ThemeTokens tokens(const strata_theme_tokens& value) {
    if (value.struct_size < sizeof(strata_theme_tokens) || value.reserved != 0U) {
        throw std::invalid_argument("theme tokens header is invalid");
    }
    strata::ui::ThemeTokens result{
        color(value.surface),
        color(value.surface_raised),
        color(value.foreground),
        color(value.muted_foreground),
        color(value.accent),
        color(value.danger),
        color(value.focus),
        value.spacing_unit,
        value.radius,
        value.density,
    };
    result.validate();
    return result;
}

[[nodiscard]] strata::ui::MotionEasing motion_easing(
    const strata_theme_motion_easing& source
);

[[nodiscard]] std::optional<strata::ui::ThemeMotionPolicy> motion_policy(
    const strata_theme_motion_policy* const policy
) {
    if (policy == nullptr) return std::nullopt;
    constexpr std::size_t maximum_theme_timings = 64U;
    if (policy->struct_size < sizeof(strata_theme_motion_policy) ||
        policy->reduced_motion > 1U || policy->reserved != 0U ||
        policy->timing_count > maximum_theme_timings ||
        ((policy->timing_count == 0U) != (policy->timings == nullptr))) {
        throw std::invalid_argument("theme motion policy header or timing table is invalid");
    }
    std::map<std::string, strata::ui::MotionTiming, std::less<>> timings;
    for (std::size_t index = 0U; index < policy->timing_count; ++index) {
        const strata_theme_motion_timing& source = policy->timings[index];
        if (source.struct_size < sizeof(strata_theme_motion_timing) || source.reserved != 0U ||
            source.repeat_kind > STRATA_THEME_MOTION_REPEAT_FOREVER ||
            source.fill_mode > STRATA_THEME_MOTION_FILL_BOTH ||
            (source.repeat_kind == STRATA_THEME_MOTION_REPEAT_COUNT
                 ? source.repeat_count == 0U
                 : source.repeat_count != 0U)) {
            throw std::invalid_argument("theme motion timing header is invalid");
        }
        validate_boolean(source.reverse, "theme motion timing reverse");
        std::string name = string(source.name, "theme motion timing name", false);
        const strata::ui::MotionRepeat repeat =
            source.repeat_kind == STRATA_THEME_MOTION_REPEAT_FOREVER
                ? strata::ui::MotionRepeat{strata::ui::MotionRepeatKind::forever, 1U}
                : source.repeat_kind == STRATA_THEME_MOTION_REPEAT_COUNT
                      ? strata::ui::MotionRepeat{
                            strata::ui::MotionRepeatKind::count, source.repeat_count
                        }
                      : strata::ui::MotionRepeat{};
        strata::ui::MotionTiming timing{
            source.duration_nanoseconds,
            source.delay_nanoseconds,
            motion_easing(source.easing),
            repeat,
            source.reverse != 0U,
            static_cast<strata::ui::MotionFillMode>(source.fill_mode),
        };
        if (!timings.emplace(std::move(name), std::move(timing)).second) {
            throw std::invalid_argument("theme motion timing names must be unique");
        }
    }
    return strata::ui::ThemeMotionPolicy(policy->reduced_motion != 0U, std::move(timings));
}

class ThemeKeySequence final : public strata::runtime::KeyedSequence {
public:
    explicit ThemeKeySequence(std::vector<std::string> keys) : keys_(std::move(keys)) {}
    [[nodiscard]] std::uint64_t generation() const noexcept override { return 0U; }
    [[nodiscard]] std::size_t count() const noexcept override { return keys_.size(); }
    [[nodiscard]] std::string key_at(const std::size_t index) const override { return keys_.at(index); }
    [[nodiscard]] std::optional<std::size_t> index_of_key(const std::string_view key) const override {
        const auto found = std::ranges::find(keys_, key);
        return found != keys_.end()
                   ? std::optional<std::size_t>(static_cast<std::size_t>(found - keys_.begin()))
                   : std::nullopt;
    }
    [[nodiscard]] bool same_generation(const strata::runtime::KeyedSequence& other) const noexcept override {
        const auto* sequence = dynamic_cast<const ThemeKeySequence*>(&other);
        return sequence != nullptr && sequence->keys_ == keys_;
    }

private:
    std::vector<std::string> keys_;
};

[[nodiscard]] strata::ui::ThemeWidgetVisualStyle visual_style(
    const strata_theme_visual_style& source
) {
    if (source.struct_size < sizeof(strata_theme_visual_style) || source.reserved != 0U) {
        throw std::invalid_argument("theme visual-style header is invalid");
    }
    strata::ui::ThemeWidgetVisualStyle result{
        nullable_color(source.background, "theme background"),
        color(source.foreground),
        nullable_border(source.border, "theme border"),
        source.radius,
        nullable_color(source.hover_overlay, "theme hover overlay"),
        nullable_color(source.active_overlay, "theme active overlay"),
        nullable_border(source.focus_ring, "theme focus ring"),
        source.disabled_opacity,
        source.opacity,
        source.translate_x,
        source.translate_y,
        source.scale,
        source.scale_x,
        source.scale_y,
        nullable_color(source.track, "theme track"),
        nullable_color(source.fill, "theme fill"),
        nullable_color(source.thumb, "theme thumb"),
        nullable_color(source.selection, "theme selection"),
        nullable_color(source.scrim, "theme scrim"),
        optional_number(source.indicator_size, "theme indicator size"),
        optional_number(source.indicator_inset, "theme indicator inset"),
        optional_number(source.track_width, "theme track width"),
        optional_number(source.track_height, "theme track height"),
        optional_number(source.track_radius, "theme track radius"),
        optional_number(source.thumb_size, "theme thumb size"),
        optional_number(source.thumb_radius, "theme thumb radius"),
        optional_number(source.indicator_position, "theme indicator position"),
    };
    result.validate();
    return result;
}

[[nodiscard]] strata::ui::ThemeWidgetTextVisualStyle text_visual_style(
    const strata_theme_text_visual_style& source
) {
    if (source.struct_size < sizeof(strata_theme_text_visual_style) || source.reserved != 0U) {
        throw std::invalid_argument("theme text-visual-style header is invalid");
    }
    return {color(source.color), color(source.hint_color), color(source.selection_color), color(source.caret_color)};
}

[[nodiscard]] strata::ui::ThemeTextLayoutStyle text_layout_style(
    const strata_theme_text_layout_style& source
) {
    constexpr std::size_t maximum_fallback_fonts = 64U;
    if (source.struct_size < sizeof(strata_theme_text_layout_style) || source.reserved != 0U) {
        throw std::invalid_argument("theme text-layout-style header is invalid");
    }
    validate_table(source.fallback_fonts, source.fallback_font_count, maximum_fallback_fonts, "theme fallback font");
    strata::ui::ThemeTextLayoutStyle result;
    result.primary_font = string(source.primary_font, "theme primary font", false);
    result.fallback_fonts.reserve(source.fallback_font_count);
    for (std::size_t index = 0U; index < source.fallback_font_count; ++index) {
        result.fallback_fonts.push_back(string(source.fallback_fonts[index], "theme fallback font", false));
    }
    result.pixel_size = source.pixel_size;
    result.style_flags = source.style_flags;
    result.line_height = optional_number(source.line_height, "theme line height");
    result.line_height_multiplier = source.line_height_multiplier;
    result.letter_spacing = source.letter_spacing;
    result.validate();
    return result;
}

[[nodiscard]] strata::ui::LayoutSize layout_size(
    const strata_theme_layout_size& source,
    const std::string_view label,
    std::set<const strata_theme_layout_size*>& path,
    const std::size_t depth = 0U
) {
    constexpr std::size_t maximum_depth = 64U;
    if (depth > maximum_depth || source.struct_size < sizeof(strata_theme_layout_size) ||
        source.reserved != 0U || source.kind > STRATA_THEME_SIZE_CLAMP) {
        throw std::invalid_argument(std::string(label) + " is invalid");
    }
    if (!path.insert(&source).second) {
        throw std::invalid_argument(std::string(label) + " contains a recursive pointer cycle");
    }
    strata::ui::LayoutSize result;
    result.kind = static_cast<strata::ui::LayoutSize::Kind>(source.kind);
    result.value = source.value;
    if (source.kind == STRATA_THEME_SIZE_CLAMP) {
        if (source.preferred == nullptr) {
            throw std::invalid_argument(std::string(label) + " clamp has no preferred size");
        }
        if (source.minimum != nullptr) {
            result.minimum = std::make_shared<const strata::ui::LayoutSize>(
                layout_size(*source.minimum, std::string(label) + " minimum", path, depth + 1U)
            );
        }
        result.preferred = std::make_shared<const strata::ui::LayoutSize>(
            layout_size(*source.preferred, std::string(label) + " preferred", path, depth + 1U)
        );
        if (source.maximum != nullptr) {
            result.maximum = std::make_shared<const strata::ui::LayoutSize>(
                layout_size(*source.maximum, std::string(label) + " maximum", path, depth + 1U)
            );
        }
    } else if (source.minimum != nullptr || source.preferred != nullptr || source.maximum != nullptr) {
        throw std::invalid_argument(std::string(label) + " non-clamp contains clamp children");
    }
    path.erase(&source);
    return result;
}

[[nodiscard]] strata::ui::LayoutSize layout_size(
    const strata_theme_layout_size& source,
    const std::string_view label
) {
    std::set<const strata_theme_layout_size*> path;
    return layout_size(source, label, path);
}

[[nodiscard]] strata::ui::Edges edges(const strata_theme_edges value) noexcept {
    return {value.left, value.top, value.right, value.bottom};
}

[[nodiscard]] strata::ui::LayoutStyle layout_style(const strata_theme_layout_style& source) {
    constexpr std::size_t maximum_tracks = 1'024U;
    if (source.struct_size < sizeof(strata_theme_layout_style) || source.reserved != 0U ||
        source.kind > STRATA_THEME_LAYOUT_PORTAL || source.align_items > STRATA_THEME_ALIGN_STRETCH ||
        source.justify_content > STRATA_THEME_JUSTIFY_SPACE_EVENLY ||
        source.align_content > STRATA_THEME_JUSTIFY_SPACE_EVENLY ||
        (source.align_self != STRATA_THEME_ALIGN_UNSPECIFIED && source.align_self > STRATA_THEME_ALIGN_STRETCH) ||
        (source.justify_self != STRATA_THEME_ALIGN_UNSPECIFIED && source.justify_self > STRATA_THEME_ALIGN_STRETCH)) {
        throw std::invalid_argument("theme layout-style header is invalid");
    }
    for (const auto& [value, label] : {
             std::pair{source.participates, std::string_view("theme participates")},
             std::pair{source.wrap, std::string_view("theme wrap")},
             std::pair{source.clip, std::string_view("theme clip")},
             std::pair{source.scroll_horizontal, std::string_view("theme horizontal scroll")},
             std::pair{source.scroll_vertical, std::string_view("theme vertical scroll")},
             std::pair{source.scroll_viewport_insets_from_inside_border,
                       std::string_view("theme inside-border viewport insets")},
             std::pair{source.pin_horizontal, std::string_view("theme horizontal scroll pin")},
             std::pair{source.pin_vertical, std::string_view("theme vertical scroll pin")},
             std::pair{source.detach_from_parent_clip, std::string_view("theme portal clip detachment")},
         }) validate_boolean(value, label);
    validate_table(source.grid_columns, source.grid_column_count, maximum_tracks, "theme grid columns");
    validate_table(source.grid_rows, source.grid_row_count, maximum_tracks, "theme grid rows");
    strata::ui::LayoutStyle result;
    result.participates = source.participates != 0U;
    result.kind = static_cast<strata::ui::LayoutKind>(source.kind);
    result.width = layout_size(source.width, "theme width");
    result.height = layout_size(source.height, "theme height");
    if (source.min_width != nullptr) result.min_width = layout_size(*source.min_width, "theme minimum width");
    if (source.min_height != nullptr) result.min_height = layout_size(*source.min_height, "theme minimum height");
    if (source.max_width != nullptr) result.max_width = layout_size(*source.max_width, "theme maximum width");
    if (source.max_height != nullptr) result.max_height = layout_size(*source.max_height, "theme maximum height");
    result.aspect_ratio = optional_number(source.aspect_ratio, "theme aspect ratio");
    if (source.intrinsic_size != nullptr) result.intrinsic_size = strata::ui::Size{source.intrinsic_size->width, source.intrinsic_size->height};
    result.padding = edges(source.padding);
    result.margin = edges(source.margin);
    result.gap = {source.gap.x, source.gap.y};
    result.align_items = static_cast<strata::ui::LayoutAlign>(source.align_items);
    result.justify_content = static_cast<strata::ui::LayoutJustify>(source.justify_content);
    result.align_content = static_cast<strata::ui::LayoutJustify>(source.align_content);
    if (source.align_self != STRATA_THEME_ALIGN_UNSPECIFIED) result.align_self = static_cast<strata::ui::LayoutAlign>(source.align_self);
    if (source.justify_self != STRATA_THEME_ALIGN_UNSPECIFIED) result.justify_self = static_cast<strata::ui::LayoutAlign>(source.justify_self);
    result.wrap = source.wrap != 0U;
    result.clip = source.clip != 0U;
    result.z_index = source.z_index;
    result.grid_columns.reserve(source.grid_column_count);
    for (std::size_t index = 0U; index < source.grid_column_count; ++index) result.grid_columns.push_back(layout_size(source.grid_columns[index], "theme grid column"));
    result.grid_rows.reserve(source.grid_row_count);
    for (std::size_t index = 0U; index < source.grid_row_count; ++index) result.grid_rows.push_back(layout_size(source.grid_rows[index], "theme grid row"));
    if (source.grid_column != nullptr) result.grid_column = *source.grid_column;
    if (source.grid_row != nullptr) result.grid_row = *source.grid_row;
    result.column_span = source.column_span;
    result.row_span = source.row_span;
    result.scroll_horizontal = source.scroll_horizontal != 0U;
    result.scroll_vertical = source.scroll_vertical != 0U;
    result.scroll_viewport_insets = edges(source.scroll_viewport_insets);
    result.scroll_viewport_insets_from_inside_border =
        source.scroll_viewport_insets_from_inside_border != 0U;
    result.scroll_content_padding = edges(source.scroll_content_padding);
    result.scrollbar_gutter = source.scrollbar_gutter;
    result.scroll_offset = {source.scroll_offset.x, source.scroll_offset.y};
    result.pin_horizontal = source.pin_horizontal != 0U;
    result.pin_vertical = source.pin_vertical != 0U;
    result.portal_target = string(source.portal_target, "theme portal target", false);
    result.detach_from_parent_clip = source.detach_from_parent_clip != 0U;
    if (source.virtual_list != nullptr) {
        constexpr std::size_t maximum_virtual_items = 1'000'000U;
        const strata_theme_virtual_list& list = *source.virtual_list;
        if (list.struct_size < sizeof(strata_theme_virtual_list) || list.reserved != 0U ||
            list.axis > STRATA_THEME_AXIS_VERTICAL) {
            throw std::invalid_argument("theme virtual-list header is invalid");
        }
        validate_boolean(list.measure_item_extents, "theme virtual measurement");
        validate_table(list.item_keys, list.item_count, maximum_virtual_items, "theme virtual item keys");
        validate_table(list.item_members, list.item_member_count, maximum_virtual_items, "theme virtual item members");
        validate_table(list.item_extents, list.item_extent_count, maximum_virtual_items, "theme virtual item extents");
        if ((list.item_member_count != 0U && list.item_member_count != list.item_count) ||
            (list.item_extent_count != 0U && list.item_extent_count != list.item_count)) {
            throw std::invalid_argument("theme virtual item metadata must match the item count");
        }
        strata::ui::VirtualListSpec spec;
        spec.axis = static_cast<strata::ui::LayoutAxis>(list.axis);
        spec.item_extent = list.item_extent;
        spec.overscan = list.overscan;
        spec.measure_item_extents = list.measure_item_extents != 0U;
        std::vector<std::string> keys;
        keys.reserve(list.item_count);
        for (std::size_t index = 0U; index < list.item_count; ++index) keys.push_back(string(list.item_keys[index], "theme virtual item key", false));
        spec.items = std::make_shared<const ThemeKeySequence>(std::move(keys));
        strata::ui::VirtualItemMembers item_members;
        item_members.reserve(list.item_member_count);
        for (std::size_t index = 0U; index < list.item_member_count; ++index) {
            const strata_theme_virtual_item_members& band = list.item_members[index];
            validate_table(band.keys, band.key_count, maximum_virtual_items, "theme virtual member keys");
            std::vector<std::string> members;
            members.reserve(band.key_count);
            for (std::size_t member = 0U; member < band.key_count; ++member) members.push_back(string(band.keys[member], "theme virtual member key", false));
            item_members.push_back(std::move(members));
        }
        if (!item_members.empty()) {
            spec.item_members = std::make_shared<const strata::ui::VirtualItemMembers>(
                std::move(item_members)
            );
        }
        if (list.item_extent_count != 0U) spec.item_extents.emplace(std::vector<double>(list.item_extents, list.item_extents + list.item_extent_count));
        result.virtual_list = std::move(spec);
    }
    return result;
}

[[nodiscard]] strata::ui::MotionValue motion_value(const strata_theme_motion_value& source) {
    if (source.reserved != 0U || source.kind > STRATA_THEME_MOTION_VALUE_BOOLEAN) {
        throw std::invalid_argument("theme motion value is invalid");
    }
    if (source.kind == STRATA_THEME_MOTION_VALUE_NUMBER) {
        if (!std::isfinite(source.number)) throw std::invalid_argument("theme motion number must be finite");
        return source.number;
    }
    if (source.kind == STRATA_THEME_MOTION_VALUE_COLOR) return color(source.color);
    validate_boolean(source.boolean, "theme motion value");
    return source.boolean != 0U;
}

[[nodiscard]] strata::ui::MotionProperty motion_property(const strata_theme_motion_property value) {
    if (value > STRATA_THEME_MOTION_PROPERTY_SCALE_Y) throw std::invalid_argument("theme motion property is invalid");
    return static_cast<strata::ui::MotionProperty>(value);
}

[[nodiscard]] strata::ui::MotionTrigger motion_trigger(const strata_theme_motion_trigger value) {
    if (value > STRATA_THEME_MOTION_FOCUS_VISIBLE) {
        throw std::invalid_argument("theme motion trigger is invalid");
    }
    return static_cast<strata::ui::MotionTrigger>(value);
}

[[nodiscard]] strata::ui::MotionEasing motion_easing(
    const strata_theme_motion_easing& source
) {
    if (source.struct_size < sizeof(strata_theme_motion_easing) || source.reserved != 0U ||
        source.kind > STRATA_THEME_MOTION_EASING_CUBIC_BEZIER) {
        throw std::invalid_argument("theme motion easing is invalid");
    }
    if (source.kind == STRATA_THEME_MOTION_EASING_CUBIC_BEZIER) {
        return strata::ui::MotionEasing::cubic_bezier(
            source.x1, source.y1, source.x2, source.y2
        );
    }
    return strata::ui::MotionEasing(
        static_cast<strata::ui::MotionEasingKind>(source.kind)
    );
}

[[nodiscard]] strata::ui::CompiledMotion declared_animation(
    const strata_theme_declared_animation& source,
    const bool require_name = true
) {
    constexpr std::size_t maximum_tracks = 128U;
    constexpr std::size_t maximum_keyframes = 4'096U;
    if (source.struct_size < sizeof(strata_theme_declared_animation) || source.reserved != 0U ||
        source.fill_mode > static_cast<std::uint32_t>(strata::ui::MotionFillMode::both)) {
        throw std::invalid_argument("theme declared-animation header is invalid");
    }
    if (source.repeat_kind > STRATA_THEME_MOTION_REPEAT_FOREVER ||
        (source.repeat_kind == STRATA_THEME_MOTION_REPEAT_COUNT
             ? source.repeat_count == 0U
             : source.repeat_count != 0U)) {
        throw std::invalid_argument("theme animation repeat is invalid");
    }
    validate_boolean(source.reverse, "theme animation reverse");
    validate_table(source.tracks, source.track_count, maximum_tracks, "theme animation tracks");
    strata::ui::CompiledMotion result;
    result.name = string(source.name, "theme animation name", !require_name);
    result.trigger = motion_trigger(source.trigger);
    result.timing.duration_nanos = source.duration_nanoseconds;
    result.timing.delay_nanos = source.delay_nanoseconds;
    result.timing.easing = motion_easing(source.easing);
    result.timing.repeat = source.repeat_kind == STRATA_THEME_MOTION_REPEAT_FOREVER
                               ? strata::ui::MotionRepeat{strata::ui::MotionRepeatKind::forever, 1U}
                               : source.repeat_kind == STRATA_THEME_MOTION_REPEAT_COUNT
                                     ? strata::ui::MotionRepeat{strata::ui::MotionRepeatKind::count, source.repeat_count}
                                     : strata::ui::MotionRepeat{};
    result.timing.reverse = source.reverse != 0U;
    result.timing.fill_mode = static_cast<strata::ui::MotionFillMode>(source.fill_mode);
    result.tracks.reserve(source.track_count);
    for (std::size_t index = 0U; index < source.track_count; ++index) {
        const strata_theme_motion_track& track = source.tracks[index];
        if (track.reserved != 0U) throw std::invalid_argument("theme motion-track header is invalid");
        validate_table(track.keyframes, track.keyframe_count, maximum_keyframes, "theme motion keyframes");
        strata::ui::MotionTrack decoded;
        decoded.property = motion_property(track.property);
        decoded.keyframes.reserve(track.keyframe_count);
        for (std::size_t frame_index = 0U; frame_index < track.keyframe_count; ++frame_index) {
            const strata_theme_motion_keyframe& frame = track.keyframes[frame_index];
            strata::ui::MotionKeyframe decoded_frame{frame.offset, motion_value(frame.value), std::nullopt};
            if (frame.easing != nullptr) decoded_frame.easing = motion_easing(*frame.easing);
            decoded.keyframes.push_back(std::move(decoded_frame));
        }
        result.tracks.push_back(std::move(decoded));
    }
    return result;
}

[[nodiscard]] strata::ui::ThemeAnimationSpec animation_spec(
    const strata_theme_animation_spec& source
) {
    if (source.reserved != 0U || source.kind > STRATA_THEME_ANIMATION_INLINE) {
        throw std::invalid_argument("theme animation spec is invalid");
    }
    if (source.kind == STRATA_THEME_ANIMATION_NAMED) {
        if (source.inline_animation != nullptr) {
            throw std::invalid_argument("named theme animation spec contains inline payload");
        }
        return strata::ui::ThemeAnimationSpec(
            string(source.name, "theme animation reference", false)
        );
    }
    if (source.inline_animation == nullptr ||
        !string(source.name, "inline theme animation name", true).empty()) {
        throw std::invalid_argument("inline theme animation spec has invalid payload");
    }
    return strata::ui::ThemeAnimationSpec(declared_animation(*source.inline_animation, false));
}

[[nodiscard]] strata::ui::ThemeAnimationSet animation_set(const strata_theme_animation_set& source) {
    constexpr std::size_t maximum_entries = 1'024U;
    if (source.struct_size < sizeof(strata_theme_animation_set) || source.reserved != 0U) {
        throw std::invalid_argument("theme animation-set header is invalid");
    }
    validate_table(source.attachments, source.attachment_count, maximum_entries, "theme motion attachments");
    validate_table(source.declared_animations, source.declared_animation_count, maximum_entries, "theme declared animations");
    validate_table(source.channels, source.channel_count, maximum_entries, "theme motion channels");
    validate_table(source.value_channels, source.value_channel_count, maximum_entries, "theme motion value channels");
    strata::ui::ThemeAnimationSet result;
    result.attachments.reserve(source.attachment_count);
    for (std::size_t index = 0U; index < source.attachment_count; ++index) {
        const strata_theme_motion_attachment& attachment = source.attachments[index];
        if (attachment.reserved != 0U || attachment.direction > STRATA_THEME_MOTION_COLLAPSE ||
            (attachment.continuity_trigger != STRATA_THEME_MOTION_TRIGGER_UNSPECIFIED &&
             attachment.continuity_trigger > STRATA_THEME_MOTION_FOCUS_VISIBLE)) {
            throw std::invalid_argument("theme motion attachment is invalid");
        }
        validate_boolean(attachment.cancel_on_detach, "theme motion cancellation");
        result.attachments.push_back(strata::ui::ThemeMotionAttachment{
            motion_trigger(attachment.trigger),
            animation_spec(attachment.animation),
            attachment.cancel_on_detach != 0U,
            static_cast<strata::ui::MotionDirection>(attachment.direction),
            attachment.continuity_trigger != STRATA_THEME_MOTION_TRIGGER_UNSPECIFIED
                ? std::optional<strata::ui::MotionTrigger>(motion_trigger(attachment.continuity_trigger))
                : std::nullopt,
        });
    }
    result.declared_animations.reserve(source.declared_animation_count);
    for (std::size_t index = 0U; index < source.declared_animation_count; ++index) result.declared_animations.push_back(declared_animation(source.declared_animations[index]));
    result.channels.reserve(source.channel_count);
    for (std::size_t index = 0U; index < source.channel_count; ++index) {
        const strata_theme_motion_channel& channel = source.channels[index];
        if (channel.reserved != 0U ||
            (channel.interaction != STRATA_THEME_MOTION_INTERACTION_UNSPECIFIED &&
             channel.interaction > STRATA_THEME_MOTION_INTERACTION_FOCUS_VISIBLE) ||
            channel.state_target_mode > STRATA_THEME_VALUE_SET) {
            throw std::invalid_argument("theme motion channel is invalid");
        }
        if (channel.state_target_mode == STRATA_THEME_VALUE_SET) validate_boolean(channel.state_target, "theme motion channel target");
        result.channels.push_back(strata::ui::ThemeMotionChannel{
            string(channel.id, "theme motion channel id", false),
            animation_spec(channel.animation),
            channel.interaction != STRATA_THEME_MOTION_INTERACTION_UNSPECIFIED
                ? std::optional<strata::ui::MotionInteraction>(static_cast<strata::ui::MotionInteraction>(channel.interaction))
                : std::nullopt,
            channel.state_target_mode == STRATA_THEME_VALUE_SET
                ? std::optional<bool>(channel.state_target != 0U)
                : std::nullopt,
        });
    }
    result.value_channels.reserve(source.value_channel_count);
    for (std::size_t index = 0U; index < source.value_channel_count; ++index) {
        const strata_theme_motion_value_channel& channel = source.value_channels[index];
        if (channel.reserved != 0U) throw std::invalid_argument("theme motion value-channel header is invalid");
        result.value_channels.push_back(strata::ui::ThemeMotionValueChannel{
            string(channel.id, "theme motion value channel id", false),
            motion_property(channel.property),
            motion_value(channel.target),
            string(channel.timing, "theme motion value channel timing", false),
        });
    }
    if (source.resolved_properties != nullptr) {
        validate_table(source.resolved_properties->properties, source.resolved_properties->property_count, maximum_entries, "theme resolved properties");
        strata::ui::ThemeResolvedPropertyMotion decoded;
        decoded.properties.reserve(source.resolved_properties->property_count);
        for (std::size_t index = 0U; index < source.resolved_properties->property_count; ++index) decoded.properties.push_back(motion_property(source.resolved_properties->properties[index]));
        decoded.timing = string(source.resolved_properties->timing, "theme resolved-property timing", false);
        result.resolved_properties = std::move(decoded);
    }
    if (source.disclosure != nullptr) {
        if (source.disclosure->reserved != 0U) throw std::invalid_argument("theme disclosure motion is invalid");
        validate_boolean(source.disclosure->expanded, "theme disclosure expanded");
        result.disclosure = strata::ui::ThemeDisclosureMotion{
            source.disclosure->expanded != 0U,
            source.disclosure->collapsed_extent,
            string(source.disclosure->timing, "theme disclosure timing", false),
        };
    }
    if (source.content_size != nullptr) {
        if (source.content_size->reserved != 0U) throw std::invalid_argument("theme content-size motion is invalid");
        validate_boolean(source.content_size->animate_width, "theme content-size width");
        validate_boolean(source.content_size->animate_height, "theme content-size height");
        validate_boolean(source.content_size->clip, "theme content-size clip");
        result.content_size = strata::ui::ThemeContentSizeMotion{
            source.content_size->animate_width != 0U,
            source.content_size->animate_height != 0U,
            source.content_size->clip != 0U,
            string(source.content_size->timing, "theme content-size timing", false),
        };
    }
    result.validate();
    return result;
}

[[nodiscard]] strata::ui::ThemedWidgetStyle widget_style(
    const strata_theme_widget_style& source,
    strata::ui::ThemeWidgetKey& key
) {
    if (source.struct_size < sizeof(strata_theme_widget_style) || source.reserved != 0U ||
        source.motion_mode > STRATA_THEME_VALUE_NONE ||
        ((source.motion_mode == STRATA_THEME_VALUE_SET) != (source.motion != nullptr)) ||
        (source.motion_mode == STRATA_THEME_VALUE_NONE && source.motion != nullptr)) {
        throw std::invalid_argument("theme widget-style header is invalid");
    }
    key.component_type = string(source.component_type, "theme widget component type", false);
    key.variant = string(source.variant, "theme widget variant", true);
    if (key.variant.empty()) key.variant = std::string(strata::ui::default_widget_variant);
    key.validate();
    strata::ui::ThemedWidgetStyle result;
    if (source.visual != nullptr) result.visual = visual_style(*source.visual);
    if (source.text_visual != nullptr) result.text_visual = text_visual_style(*source.text_visual);
    if (source.text_layout != nullptr) result.text_layout = text_layout_style(*source.text_layout);
    if (source.layout != nullptr) result.layout = layout_style(*source.layout);
    if (source.motion_mode == STRATA_THEME_VALUE_NONE) result.motion.emplace(std::nullopt);
    else if (source.motion_mode == STRATA_THEME_VALUE_SET) result.motion.emplace(animation_set(*source.motion));
    result.validate();
    return result;
}

[[nodiscard]] strata::ui::Theme theme(
    strata_surface& surface,
    const strata_theme& source
) {
    constexpr std::size_t maximum_theme_widget_styles = 1'024U;
    if (source.struct_size < sizeof(strata_theme) || source.reserved != 0U ||
        source.model_version != STRATA_THEME_MODEL_VERSION_CURRENT ||
        source.widget_style_count > maximum_theme_widget_styles ||
        ((source.widget_style_count == 0U) != (source.widget_styles == nullptr))) {
        throw std::invalid_argument("theme header or widget-style table is invalid");
    }
    std::string name = string(source.name, "theme name", false);
    const std::string parent_name = string(source.parent, "theme parent", true);
    std::shared_ptr<const strata::ui::Theme> parent;
    if (!parent_name.empty()) {
        const auto* registered = surface.core.registered_theme(parent_name);
        if (registered == nullptr) {
            throw std::invalid_argument("theme parent is not registered on this Surface");
        }
        parent = *registered;
    }
    std::map<strata::ui::ThemeWidgetKey, strata::ui::ThemedWidgetStyle> styles;
    for (std::size_t index = 0U; index < source.widget_style_count; ++index) {
        strata::ui::ThemeWidgetKey key;
        strata::ui::ThemedWidgetStyle style = widget_style(source.widget_styles[index], key);
        if (!styles.emplace(std::move(key), std::move(style)).second) {
            throw std::invalid_argument("theme widget type/variant entries must be unique");
        }
    }
    return strata::ui::Theme(
        std::move(name),
        tokens(source.tokens),
        std::move(parent),
        motion_policy(source.motion_policy),
        std::move(styles)
    );
}

template <typename Mutation>
[[nodiscard]] strata_result mutate_theme(
    strata_surface* const surface,
    const strata_theme* const source,
    std::uint32_t* const out_changed,
    Mutation&& mutation
) {
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    if (out_changed != nullptr) *out_changed = 0U;
    if (source == nullptr || out_changed == nullptr) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_THEME",
            "Surface theme mutation requires a typed theme and output pointer."
        );
    }
    try {
        *out_changed = mutation(surface->core, theme(*surface, *source)) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Surface theme mutation exhausted memory."
        );
    } catch (const std::invalid_argument& error) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.THEME.INVALID",
            error.what()
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Surface theme mutation failed inside the C ABI boundary."
        );
    }
}

} // namespace

extern "C" {

strata_result strata_theme_tokens_defaults(strata_theme_tokens* const out_tokens) {
    if (out_tokens == nullptr || out_tokens->struct_size < sizeof(strata_theme_tokens)) {
        return strata::abi_detail::invalid_argument();
    }
    const strata::ui::ThemeTokens source;
    *out_tokens = strata_theme_tokens{
        sizeof(strata_theme_tokens),
        strata_color{source.surface.red, source.surface.green, source.surface.blue, source.surface.alpha},
        strata_color{
            source.surface_raised.red,
            source.surface_raised.green,
            source.surface_raised.blue,
            source.surface_raised.alpha,
        },
        strata_color{
            source.foreground.red,
            source.foreground.green,
            source.foreground.blue,
            source.foreground.alpha,
        },
        strata_color{
            source.muted_foreground.red,
            source.muted_foreground.green,
            source.muted_foreground.blue,
            source.muted_foreground.alpha,
        },
        strata_color{source.accent.red, source.accent.green, source.accent.blue, source.accent.alpha},
        strata_color{source.danger.red, source.danger.green, source.danger.blue, source.danger.alpha},
        strata_color{source.focus.red, source.focus.green, source.focus.blue, source.focus.alpha},
        source.spacing_unit,
        source.radius,
        source.density,
        0U,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_theme_visual_style_defaults(strata_theme_visual_style* const out_style) {
    if (out_style == nullptr || out_style->struct_size < sizeof(strata_theme_visual_style)) {
        return strata::abi_detail::invalid_argument();
    }
    strata_theme_visual_style result{};
    result.struct_size = sizeof(result);
    result.foreground = strata_color{255U, 255U, 255U, 255U};
    result.disabled_opacity = 0.45;
    result.opacity = 1.0;
    result.scale = 1.0;
    result.scale_x = 1.0;
    result.scale_y = 1.0;
    *out_style = result;
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_theme_text_visual_style_defaults(
    strata_theme_text_visual_style* const out_style
) {
    if (out_style == nullptr || out_style->struct_size < sizeof(strata_theme_text_visual_style)) {
        return strata::abi_detail::invalid_argument();
    }
    *out_style = strata_theme_text_visual_style{
        sizeof(strata_theme_text_visual_style),
        strata_color{255U, 255U, 255U, 255U},
        strata_color{160U, 168U, 178U, 180U},
        strata_color{72U, 119U, 218U, 96U},
        strata_color{255U, 255U, 255U, 255U},
        0U,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_theme_text_layout_style_defaults(
    strata_theme_text_layout_style* const out_style
) {
    if (out_style == nullptr || out_style->struct_size < sizeof(strata_theme_text_layout_style)) {
        return strata::abi_detail::invalid_argument();
    }
    static constexpr char default_font[] = "strata:fonts/default";
    *out_style = strata_theme_text_layout_style{
        sizeof(strata_theme_text_layout_style),
        strata_string_view{default_font, sizeof(default_font) - 1U},
        nullptr,
        0U,
        12.0,
        0U,
        0U,
        strata_theme_optional_number{},
        1.0,
        0.0,
    };
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_theme_layout_style_defaults(strata_theme_layout_style* const out_style) {
    if (out_style == nullptr || out_style->struct_size < sizeof(strata_theme_layout_style)) {
        return strata::abi_detail::invalid_argument();
    }
    static constexpr char root[] = "root";
    strata_theme_layout_style result{};
    result.struct_size = sizeof(result);
    result.participates = 1U;
    result.kind = STRATA_THEME_LAYOUT_PANEL;
    result.width.struct_size = sizeof(strata_theme_layout_size);
    result.width.kind = STRATA_THEME_SIZE_AUTO;
    result.height.struct_size = sizeof(strata_theme_layout_size);
    result.height.kind = STRATA_THEME_SIZE_AUTO;
    result.align_items = STRATA_THEME_ALIGN_START;
    result.justify_content = STRATA_THEME_JUSTIFY_START;
    result.align_content = STRATA_THEME_JUSTIFY_START;
    result.align_self = STRATA_THEME_ALIGN_UNSPECIFIED;
    result.justify_self = STRATA_THEME_ALIGN_UNSPECIFIED;
    result.column_span = 1U;
    result.row_span = 1U;
    result.scroll_vertical = 1U;
    result.portal_target = strata_string_view{root, sizeof(root) - 1U};
    result.detach_from_parent_clip = 1U;
    *out_style = result;
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_theme_animation_set_defaults(strata_theme_animation_set* const out_set) {
    if (out_set == nullptr || out_set->struct_size < sizeof(strata_theme_animation_set)) {
        return strata::abi_detail::invalid_argument();
    }
    *out_set = strata_theme_animation_set{};
    out_set->struct_size = sizeof(strata_theme_animation_set);
    return strata::core::result(STRATA_STATUS_OK);
}

strata_result strata_surface_register_theme(
    strata_surface* const surface,
    const strata_theme* const source,
    std::uint32_t* const out_changed
) {
    return mutate_theme(
        surface,
        source,
        out_changed,
        [](strata::ui::Surface& target, strata::ui::Theme value) {
            return target.register_theme(std::move(value));
        }
    );
}

strata_result strata_surface_set_theme(
    strata_surface* const surface,
    const strata_theme* const source,
    std::uint32_t* const out_changed
) {
    return mutate_theme(
        surface,
        source,
        out_changed,
        [](strata::ui::Surface& target, strata::ui::Theme value) {
            return target.set_theme(std::move(value));
        }
    );
}

strata_result strata_surface_unregister_theme(
    strata_surface* const surface,
    const strata_string_view name,
    std::uint32_t* const out_removed
) {
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) {
        return strata::abi_detail::terminal_surface_failure(*surface);
    }
    if (out_removed != nullptr) *out_removed = 0U;
    if (out_removed == nullptr || !strata::abi_detail::valid_view(name, false)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_THEME_NAME",
            "Surface theme removal requires a non-empty name and output pointer."
        );
    }
    try {
        const std::string value = string(name, "theme name", false);
        *out_removed = surface->core.unregister_theme(value) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(
            *surface,
            STRATA_STATUS_OUT_OF_MEMORY,
            "STRATA.CORE.OUT_OF_MEMORY",
            "Surface theme removal exhausted memory."
        );
    } catch (const std::invalid_argument& error) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.THEME.INVALID",
            error.what()
        );
    } catch (...) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INTERNAL_ERROR,
            "STRATA.ABI.UNCAUGHT_EXCEPTION",
            "Surface theme removal failed inside the C ABI boundary."
        );
    }
}

strata_result strata_surface_set_scoped_theme(
    strata_surface* const surface,
    const strata_string_view node_key,
    const strata_theme* const source,
    std::uint32_t* const out_changed
) {
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) return strata::abi_detail::terminal_surface_failure(*surface);
    if (out_changed != nullptr) *out_changed = 0U;
    if (source == nullptr || out_changed == nullptr ||
        !strata::abi_detail::valid_view(node_key, false)) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_THEME_SCOPE",
            "A scoped theme requires a node key, typed theme, and output pointer."
        );
    }
    try {
        std::string key = string(node_key, "theme scope node key", false);
        *out_changed = surface->core.set_scoped_theme(
            std::move(key),
            theme(*surface, *source)
        ) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Scoped theme mutation exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.THEME.INVALID", error.what());
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Scoped theme mutation failed inside the C ABI boundary.");
    }
}

strata_result strata_surface_clear_scoped_theme(
    strata_surface* const surface,
    const strata_string_view node_key,
    std::uint32_t* const out_removed
) {
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) return strata::abi_detail::terminal_surface_failure(*surface);
    if (out_removed != nullptr) *out_removed = 0U;
    if (out_removed == nullptr || !strata::abi_detail::valid_view(node_key, false)) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.ABI.INVALID_THEME_SCOPE", "Clearing a scoped theme requires a node key and output pointer.");
    }
    try {
        const std::string key = string(node_key, "theme scope node key", false);
        *out_removed = surface->core.clear_scoped_theme(key) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Scoped theme removal exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.THEME.INVALID", error.what());
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Scoped theme removal failed inside the C ABI boundary.");
    }
}

strata_result strata_surface_animate_scroll_to(
    strata_surface* const surface,
    const strata_scroll_animation_request* const source,
    std::uint32_t* const out_started
) {
    if (surface == nullptr) return strata::abi_detail::invalid_argument();
    if (surface->release_packet_prepared) return strata::abi_detail::terminal_surface_failure(*surface);
    if (out_started != nullptr) *out_started = 0U;
    if (source == nullptr || out_started == nullptr ||
        source->struct_size < sizeof(strata_scroll_animation_request) ||
        source->reserved != 0U || source->has_x > 1U || source->has_y > 1U ||
        source->has_duration > 1U) {
        return surface_failure(
            *surface,
            STRATA_STATUS_INVALID_ARGUMENT,
            "STRATA.ABI.INVALID_SCROLL_ANIMATION",
            "Scroll animation request header is invalid."
        );
    }
    try {
        strata::ui::ScrollAnimationRequest request;
        request.key = string(source->key, "scroll animation key", false);
        request.timing = string(source->timing, "scroll animation timing", false);
        if (source->has_x != 0U) request.x = source->x;
        if (source->has_y != 0U) request.y = source->y;
        if (source->has_duration != 0U) request.duration_nanos = source->duration_nanoseconds;
        *out_started = surface->core.animate_scroll_to(std::move(request)) ? 1U : 0U;
        return strata::core::result(STRATA_STATUS_OK);
    } catch (const std::bad_alloc&) {
        return surface_failure(*surface, STRATA_STATUS_OUT_OF_MEMORY, "STRATA.CORE.OUT_OF_MEMORY", "Scroll animation request exhausted memory.");
    } catch (const std::invalid_argument& error) {
        return surface_failure(*surface, STRATA_STATUS_INVALID_ARGUMENT, "STRATA.SCROLL.ANIMATION_INVALID", error.what());
    } catch (...) {
        return surface_failure(*surface, STRATA_STATUS_INTERNAL_ERROR, "STRATA.ABI.UNCAUGHT_EXCEPTION", "Scroll animation failed inside the C ABI boundary.");
    }
}

} // extern "C"
