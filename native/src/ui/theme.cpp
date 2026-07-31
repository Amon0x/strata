#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <iterator>
#include <ranges>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "core/utf8.hpp"

namespace strata::ui {
namespace {

using ValueFields = std::map<std::string, runtime::Value, std::less<>>;

[[nodiscard]] runtime::Value object(ValueFields fields) {
    std::vector<std::pair<std::string, runtime::Value>> values;
    values.reserve(fields.size());
    for (auto& [name, value] : fields) {
        values.emplace_back(std::move(name), std::move(value));
    }
    return runtime::Value(std::move(values));
}

[[nodiscard]] runtime::Value color(const runtime::ColorValue value) {
    return runtime::Value(value);
}

[[nodiscard]] runtime::Value border(const ThemeBorder& value) {
    return object({
        {"color", color(value.color)},
        {"inside", runtime::Value(value.inside)},
        {"width", runtime::Value(value.width)},
    });
}

void validate_non_negative(const std::optional<double> value, const std::string_view label) {
    if (value.has_value() && (!std::isfinite(*value) || *value < 0.0)) {
        throw std::invalid_argument(std::string(label) + " must be finite and non-negative");
    }
}

void validate_non_negative(const double value, const std::string_view label) {
    validate_non_negative(std::optional<double>(value), label);
}

[[nodiscard]] runtime::Value nullable_color(
    const std::optional<runtime::ColorValue>& value
) {
    return value.has_value() ? color(*value) : runtime::Value{};
}

[[nodiscard]] runtime::Value nullable_border(const std::optional<ThemeBorder>& value) {
    return value.has_value() ? border(*value) : runtime::Value{};
}

[[nodiscard]] runtime::Value nullable_number(const std::optional<double> value) {
    return value.has_value() ? runtime::Value(*value) : runtime::Value{};
}

void add_visual(ValueFields& fields, const ThemeWidgetVisualStyle& style) {
    fields.insert_or_assign("background", nullable_color(style.background));
    fields.insert_or_assign("foreground", color(style.foreground));
    fields.insert_or_assign("border", nullable_border(style.border));
    fields.insert_or_assign("radius", runtime::Value(style.radius));
    fields.insert_or_assign("hoverOverlay", nullable_color(style.hover_overlay));
    fields.insert_or_assign("activeOverlay", nullable_color(style.active_overlay));
    fields.insert_or_assign("focusRing", nullable_border(style.focus_ring));
    fields.insert_or_assign("disabledOpacity", runtime::Value(style.disabled_opacity));
    fields.insert_or_assign("opacity", runtime::Value(style.opacity));
    fields.insert_or_assign("translateX", runtime::Value(style.translate_x));
    fields.insert_or_assign("translateY", runtime::Value(style.translate_y));
    fields.insert_or_assign("scale", runtime::Value(style.scale));
    fields.insert_or_assign("scaleX", runtime::Value(style.scale_x));
    fields.insert_or_assign("scaleY", runtime::Value(style.scale_y));
    fields.insert_or_assign("track", nullable_color(style.track));
    fields.insert_or_assign("fill", nullable_color(style.fill));
    fields.insert_or_assign("thumb", nullable_color(style.thumb));
    fields.insert_or_assign("selection", nullable_color(style.selection));
    fields.insert_or_assign("scrim", nullable_color(style.scrim));
    fields.insert_or_assign("indicatorSize", nullable_number(style.indicator_size));
    fields.insert_or_assign("indicatorInset", nullable_number(style.indicator_inset));
    fields.insert_or_assign("trackWidth", nullable_number(style.track_width));
    fields.insert_or_assign("trackHeight", nullable_number(style.track_height));
    fields.insert_or_assign("trackRadius", nullable_number(style.track_radius));
    fields.insert_or_assign("thumbSize", nullable_number(style.thumb_size));
    fields.insert_or_assign("thumbRadius", nullable_number(style.thumb_radius));
    fields.insert_or_assign("indicatorPosition", nullable_number(style.indicator_position));
}

void add_text_visual(ValueFields& fields, const ThemeWidgetTextVisualStyle& style) {
    fields.insert_or_assign("color", color(style.color));
    fields.insert_or_assign("hintColor", color(style.hint_color));
    fields.insert_or_assign("selectionColor", color(style.selection_color));
    fields.insert_or_assign("caretColor", color(style.caret_color));
}

void add_text_layout(ValueFields& fields, const ThemeTextLayoutStyle& style) {
    fields.insert_or_assign("font", runtime::Value(style.primary_font));
    std::vector<runtime::Value> fallbacks;
    fallbacks.reserve(style.fallback_fonts.size());
    for (const std::string& fallback : style.fallback_fonts) {
        fallbacks.emplace_back(fallback);
    }
    fields.insert_or_assign("fallbackFonts", runtime::Value(std::move(fallbacks)));
    fields.insert_or_assign("pixelSize", runtime::Value(style.pixel_size));
    fields.insert_or_assign("fontStyleFlags", runtime::Value(static_cast<double>(style.style_flags)));
    fields.insert_or_assign("lineHeight", nullable_number(style.line_height));
    fields.insert_or_assign("lineHeightMultiplier", runtime::Value(style.line_height_multiplier));
    fields.insert_or_assign("letterSpacing", runtime::Value(style.letter_spacing));
}

[[nodiscard]] runtime::Value edges(const Edges& value) {
    return object({
        {"bottom", runtime::Value(value.bottom)},
        {"left", runtime::Value(value.left)},
        {"right", runtime::Value(value.right)},
        {"top", runtime::Value(value.top)},
    });
}

[[nodiscard]] runtime::Value point(const Point& value) {
    return object({{"x", runtime::Value(value.x)}, {"y", runtime::Value(value.y)}});
}

[[nodiscard]] runtime::Value size(const Size& value) {
    return object({{"height", runtime::Value(value.height)}, {"width", runtime::Value(value.width)}});
}

[[nodiscard]] runtime::Value layout_size_value(const LayoutSize& value) {
    switch (value.kind) {
    case LayoutSize::Kind::automatic: return runtime::Value("auto");
    case LayoutSize::Kind::content: return runtime::Value("content");
    case LayoutSize::Kind::fixed: return runtime::Value(value.value);
    case LayoutSize::Kind::percent:
        return object({{"fraction", runtime::Value(value.value)}});
    case LayoutSize::Kind::fill:
        return object({{"weight", runtime::Value(value.value)}});
    case LayoutSize::Kind::clamp:
        return object({
            {"max", value.maximum != nullptr ? layout_size_value(*value.maximum) : runtime::Value{}},
            {"min", value.minimum != nullptr ? layout_size_value(*value.minimum) : runtime::Value{}},
            {"preferred", value.preferred != nullptr
                              ? layout_size_value(*value.preferred)
                              : runtime::Value("auto")},
        });
    }
    return runtime::Value("auto");
}

[[nodiscard]] std::string_view serialized_layout_kind_name(const LayoutKind value) noexcept {
    switch (value) {
    case LayoutKind::stack: return "STACK";
    case LayoutKind::row: return "ROW";
    case LayoutKind::column: return "COLUMN";
    case LayoutKind::grid: return "GRID";
    case LayoutKind::panel: return "PANEL";
    case LayoutKind::overlay: return "OVERLAY";
    case LayoutKind::spacer: return "SPACER";
    case LayoutKind::scroll: return "SCROLL";
    case LayoutKind::portal: return "PORTAL";
    }
    return "PANEL";
}

[[nodiscard]] std::string_view align_name(const LayoutAlign value) noexcept {
    switch (value) {
    case LayoutAlign::start: return "START";
    case LayoutAlign::center: return "CENTER";
    case LayoutAlign::end: return "END";
    case LayoutAlign::stretch: return "STRETCH";
    }
    return "START";
}

[[nodiscard]] std::string_view justify_name(const LayoutJustify value) noexcept {
    switch (value) {
    case LayoutJustify::start: return "START";
    case LayoutJustify::center: return "CENTER";
    case LayoutJustify::end: return "END";
    case LayoutJustify::space_between: return "SPACE_BETWEEN";
    case LayoutJustify::space_around: return "SPACE_AROUND";
    case LayoutJustify::space_evenly: return "SPACE_EVENLY";
    }
    return "START";
}

[[nodiscard]] runtime::Value optional_layout_size(const std::optional<LayoutSize>& value) {
    return value.has_value() ? layout_size_value(*value) : runtime::Value{};
}

[[nodiscard]] runtime::Value layout_tracks(const std::vector<LayoutSize>& values) {
    std::vector<runtime::Value> result;
    result.reserve(values.size());
    for (const LayoutSize& value : values) result.push_back(layout_size_value(value));
    return runtime::Value(std::move(result));
}

void add_layout(ValueFields& fields, const ThemeWidgetLayoutStyle& style) {
    fields.insert_or_assign(
        "kind",
        runtime::Value(std::string(serialized_layout_kind_name(style.kind)))
    );
    fields.insert_or_assign("width", layout_size_value(style.width));
    fields.insert_or_assign("height", layout_size_value(style.height));
    fields.insert_or_assign("minWidth", optional_layout_size(style.min_width));
    fields.insert_or_assign("minHeight", optional_layout_size(style.min_height));
    fields.insert_or_assign("maxWidth", optional_layout_size(style.max_width));
    fields.insert_or_assign("maxHeight", optional_layout_size(style.max_height));
    fields.insert_or_assign("aspectRatio", nullable_number(style.aspect_ratio));
    fields.insert_or_assign(
        "intrinsicSize",
        style.intrinsic_size.has_value() ? size(*style.intrinsic_size) : runtime::Value{}
    );
    fields.insert_or_assign("padding", edges(style.padding));
    fields.insert_or_assign("margin", edges(style.margin));
    fields.insert_or_assign("gap", object({
        {"horizontal", runtime::Value(style.gap.x)},
        {"vertical", runtime::Value(style.gap.y)},
    }));
    fields.insert_or_assign("alignItems", runtime::Value(std::string(align_name(style.align_items))));
    fields.insert_or_assign("justifyContent", runtime::Value(std::string(justify_name(style.justify_content))));
    fields.insert_or_assign("alignContent", runtime::Value(std::string(justify_name(style.align_content))));
    fields.insert_or_assign(
        "alignSelf",
        style.align_self.has_value()
            ? runtime::Value(std::string(align_name(*style.align_self)))
            : runtime::Value{}
    );
    fields.insert_or_assign(
        "justifySelf",
        style.justify_self.has_value()
            ? runtime::Value(std::string(align_name(*style.justify_self)))
            : runtime::Value{}
    );
    fields.insert_or_assign("wrap", runtime::Value(style.wrap));
    fields.insert_or_assign("clip", runtime::Value(style.clip));
    fields.insert_or_assign("zIndex", runtime::Value(static_cast<double>(style.z_index)));
    fields.insert_or_assign("columns", layout_tracks(style.grid_columns));
    fields.insert_or_assign("rows", layout_tracks(style.grid_rows));
    fields.insert_or_assign(
        "gridColumn",
        style.grid_column.has_value() ? runtime::Value(static_cast<double>(*style.grid_column)) : runtime::Value{}
    );
    fields.insert_or_assign(
        "gridRow",
        style.grid_row.has_value() ? runtime::Value(static_cast<double>(*style.grid_row)) : runtime::Value{}
    );
    fields.insert_or_assign("columnSpan", runtime::Value(static_cast<double>(style.column_span)));
    fields.insert_or_assign("rowSpan", runtime::Value(static_cast<double>(style.row_span)));
    fields.insert_or_assign("scrollHorizontal", runtime::Value(style.scroll_horizontal));
    fields.insert_or_assign("scrollVertical", runtime::Value(style.scroll_vertical));
    fields.insert_or_assign("viewportInsets", edges(style.scroll_viewport_insets));
    fields.insert_or_assign(
        "viewportInsetsFromInsideBorder",
        runtime::Value(style.scroll_viewport_insets_from_inside_border)
    );
    fields.insert_or_assign("contentPadding", edges(style.scroll_content_padding));
    fields.insert_or_assign("scrollbarGutter", runtime::Value(style.scrollbar_gutter));
    fields.insert_or_assign("scrollOffset", point(style.scroll_offset));
    fields.insert_or_assign("scrollPin", object({
        {"horizontal", runtime::Value(style.pin_horizontal)},
        {"vertical", runtime::Value(style.pin_vertical)},
    }));
    fields.insert_or_assign("portalTarget", runtime::Value(style.portal_target));
    fields.insert_or_assign("detachFromParentClip", runtime::Value(style.detach_from_parent_clip));
    if (!style.virtual_list.has_value()) return;
    const VirtualListSpec& list = *style.virtual_list;
    fields.insert_or_assign("virtualAxis", runtime::Value(list.axis == LayoutAxis::horizontal ? "HORIZONTAL" : "VERTICAL"));
    fields.insert_or_assign("virtualItemExtent", runtime::Value(list.item_extent));
    fields.insert_or_assign("virtualItemCount", runtime::Value(static_cast<double>(list.item_count())));
    fields.insert_or_assign("virtualOverscan", runtime::Value(static_cast<double>(list.overscan)));
    fields.insert_or_assign("virtualMeasureItemExtents", runtime::Value(list.measure_item_extents));
    std::vector<runtime::Value> keys;
    keys.reserve(list.item_count());
    for (std::size_t index = 0U; index < list.item_count(); ++index) {
        keys.emplace_back(runtime::KeyValue{list.items->key_at(index)});
    }
    fields.insert_or_assign("virtualItemKeys", runtime::Value(std::move(keys)));
    std::vector<runtime::Value> members;
    if (list.item_members != nullptr) {
        members.reserve(list.item_members->size());
        for (const std::vector<std::string>& band : *list.item_members) {
            std::vector<runtime::Value> values;
            values.reserve(band.size());
            for (const std::string& key : band) values.emplace_back(runtime::KeyValue{key});
            members.emplace_back(std::move(values));
        }
    }
    fields.insert_or_assign("virtualItemMembers", runtime::Value(std::move(members)));
    if (list.item_extents.has_value()) {
        std::vector<runtime::Value> extents;
        extents.reserve(list.item_extents->size());
        for (const double value : list.item_extents->values()) extents.emplace_back(value);
        fields.insert_or_assign("virtualItemExtents", runtime::Value(std::move(extents)));
    }
}

[[nodiscard]] runtime::Value motion_value(const MotionValue& value) {
    if (const double* number = std::get_if<double>(&value)) return runtime::Value(*number);
    if (const runtime::ColorValue* tint = std::get_if<runtime::ColorValue>(&value)) {
        return runtime::Value(*tint);
    }
    return runtime::Value(*std::get_if<bool>(&value));
}

[[nodiscard]] runtime::Value motion_attachment(
    const ThemeMotionAttachment& value,
    std::string animation
) {
    ValueFields fields{
        {"animation", runtime::Value(std::move(animation))},
        {"cancelOnDetach", runtime::Value(value.cancel_on_detach)},
        {"direction", runtime::Value(std::string(motion_direction_name(value.direction)))},
    };
    if (value.continuity_trigger.has_value()) {
        fields.emplace(
            "continuityTrigger",
            runtime::Value(std::string(motion_trigger_name(*value.continuity_trigger)))
        );
    }
    return object(std::move(fields));
}

[[nodiscard]] std::string qualified_animation_prefix(
    const std::string_view theme_namespace,
    const ThemeWidgetKey& key
) {
    const auto segment = [](const std::string_view value) {
        return std::to_string(value.size()) + "#" + std::string(value);
    };
    return "$theme|" + segment(theme_namespace) + segment(key.component_type) +
           segment(key.variant);
}

[[nodiscard]] std::string animation_owner_namespace(
    const std::string_view theme_name,
    const std::optional<std::string>& scope_key
) {
    return scope_key.has_value()
               ? "$scope|" + std::to_string(scope_key->size()) + "#" + *scope_key +
                     std::to_string(theme_name.size()) + "#" + std::string(theme_name)
               : "$global|" + std::to_string(theme_name.size()) + "#" +
                     std::string(theme_name);
}

[[nodiscard]] std::string motion_reference(
    const ThemeAnimationSet& motion,
    const std::string_view prefix,
    const ThemeAnimationSpec& spec,
    const std::string_view inline_slot
) {
    if (spec.inline_animation() != nullptr) {
        return std::string(prefix) + "I" + std::to_string(inline_slot.size()) + "#" +
               std::string(inline_slot);
    }
    const std::string& name = *spec.named();
    const bool local = std::ranges::any_of(motion.declared_animations, [&name](const CompiledMotion& value) {
        return value.name == name;
    });
    return local ? std::string(prefix) + "N" + std::to_string(name.size()) + "#" + name
                 : name;
}

void add_motion(
    ValueFields& fields,
    const ThemeAnimationSet& motion,
    const std::string_view prefix
) {
    fields.insert_or_assign("$themeAnimationSet", runtime::Value("present"));
    for (const ThemeMotionAttachment& attachment : motion.attachments) {
        fields.insert_or_assign(
            std::string(motion_trigger_name(attachment.trigger)),
            motion_attachment(
                attachment,
                motion_reference(
                    motion, prefix, attachment.animation,
                    std::string("trigger/") +
                        std::string(motion_trigger_name(attachment.trigger))
                )
            )
        );
    }
    std::vector<runtime::Value> channels;
    channels.reserve(motion.channels.size() + motion.value_channels.size());
    for (const ThemeMotionChannel& channel : motion.channels) {
        ValueFields declaration{
            {"animation", runtime::Value(motion_reference(
                motion, prefix, channel.animation, std::string("channel/") + channel.id
            ))},
            {"id", runtime::Value(channel.id)},
        };
        if (channel.interaction.has_value()) {
            declaration.emplace(
                "interaction",
                runtime::Value(std::string(motion_interaction_name(*channel.interaction)))
            );
        }
        if (channel.state_target.has_value()) {
            declaration.emplace("target", runtime::Value(*channel.state_target));
        }
        channels.push_back(object(std::move(declaration)));
    }
    for (const ThemeMotionValueChannel& channel : motion.value_channels) {
        channels.push_back(object({
            {"id", runtime::Value(channel.id)},
            {"policy", runtime::Value(channel.timing)},
            {"property", runtime::Value(std::string(motion_property_name(channel.property)))},
            {"target", motion_value(channel.target)},
        }));
    }
    fields.insert_or_assign("motions", runtime::Value(std::move(channels)));
    if (motion.resolved_properties.has_value()) {
        std::vector<runtime::Value> properties;
        properties.reserve(motion.resolved_properties->properties.size());
        for (const MotionProperty property : motion.resolved_properties->properties) {
            properties.emplace_back(std::string(motion_property_name(property)));
        }
        fields.insert_or_assign("animateChanges", object({
            {"policy", runtime::Value(motion.resolved_properties->timing)},
            {"properties", runtime::Value(std::move(properties))},
        }));
    }
    if (motion.disclosure.has_value()) {
        fields.insert_or_assign("disclosure", object({
            {"collapsedExtent", runtime::Value(motion.disclosure->collapsed_extent)},
            {"expanded", runtime::Value(motion.disclosure->expanded)},
            {"timing", runtime::Value(motion.disclosure->timing)},
        }));
    }
    if (motion.content_size.has_value()) {
        fields.insert_or_assign("animateContentSize", object({
            {"clip", runtime::Value(motion.content_size->clip)},
            {"height", runtime::Value(motion.content_size->animate_height)},
            {"timing", runtime::Value(motion.content_size->timing)},
            {"width", runtime::Value(motion.content_size->animate_width)},
        }));
    }
}

void disable_motion(ValueFields& fields, const std::string_view presence) {
    fields.insert_or_assign("$themeAnimationSet", runtime::Value(std::string(presence)));
    for (const std::string_view name : {
             "animation", "enter", "exit", "hover", "pressed", "focus", "checked", "move",
             "animate", "motions", "animateChanges", "disclosure", "animateContentSize",
         }) {
        fields.insert_or_assign(std::string(name), runtime::Value{});
    }
}

[[nodiscard]] bool empty_motion(const ThemeAnimationSet& value) noexcept {
    return value.attachments.empty() && value.declared_animations.empty() &&
           value.channels.empty() && value.value_channels.empty() &&
           !value.resolved_properties.has_value() && !value.disclosure.has_value() &&
           !value.content_size.has_value();
}

void apply_motion(
    ValueFields& fields,
    const ThemeNullable<ThemeAnimationSet>& value,
    const std::string_view prefix
) {
    if (!value.has_value()) return;
    if (!value->has_value()) disable_motion(fields, "none");
    else if (empty_motion(**value)) disable_motion(fields, "present-empty");
    else add_motion(fields, **value, prefix);
}

[[nodiscard]] bool blank(const std::string_view value) noexcept {
    return core::utf8_blankness(value) != core::Utf8Blankness::non_blank;
}

[[nodiscard]] std::string node_variant(const DescriptionNode& node) {
    const auto property = node.properties.find("variant");
    if (property == node.properties.end() || property->second.value() == nullptr) {
        return std::string(default_widget_variant);
    }
    const runtime::Value& value = *property->second.value();
    if (value.string() != nullptr && !value.string()->empty()) return *value.string();
    if (value.key() != nullptr && !value.key()->value.empty()) return value.key()->value;
    return std::string(default_widget_variant);
}

[[nodiscard]] const runtime::Value* property(
    const DescriptionNode& node,
    const std::string_view name
) noexcept {
    const auto found = node.properties.find(name);
    return found != node.properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] const std::string* text(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    return value->key() != nullptr ? &value->key()->value : nullptr;
}

[[nodiscard]] const double* number(const runtime::Value* value) noexcept {
    return value != nullptr ? value->number() : nullptr;
}

[[nodiscard]] const runtime::Value* style_field(
    const DescriptionNode& node,
    const std::string_view name
) noexcept {
    const runtime::Value* layout = property(node, "$layout");
    if (layout != nullptr && layout->object() != nullptr) {
        if (const runtime::Value* value = layout->field(name); value != nullptr) return value;
    }
    return property(node, name);
}

[[nodiscard]] bool mapped_color_property(const std::string_view name) noexcept {
    return name == "background" || name == "foreground" || name == "color" ||
           name == "hoverOverlay" || name == "activeOverlay" || name == "track" ||
           name == "fill" || name == "thumb" || name == "selection" || name == "scrim" ||
           name == "hintColor" || name == "selectionColor" || name == "caretColor";
}

[[nodiscard]] bool mapped_number_property(const std::string_view name) noexcept {
    return name == "radius" || name == "indicatorSize" || name == "indicatorInset" ||
           name == "trackWidth" || name == "trackHeight" || name == "trackRadius" ||
           name == "thumbSize" || name == "thumbRadius" || name == "padding" ||
           name == "margin" || name == "gap";
}

[[nodiscard]] std::optional<runtime::Value> resolve_token(
    const std::string_view property_name,
    const runtime::ThemeTokenValue& token,
    const ThemeTokens& tokens
) {
    if (mapped_color_property(property_name)) {
        if (const runtime::ColorValue* value = tokens.color(token.name); value != nullptr) {
            return runtime::Value(*value);
        }
        return std::nullopt;
    }
    if (mapped_number_property(property_name)) {
        if (const std::optional<double> value = tokens.number(token.name); value.has_value()) {
            return runtime::Value(*value);
        }
    }
    return std::nullopt;
}

[[nodiscard]] runtime::Value motion_policy_value(const ThemeMotionPolicy& policy) {
    const auto easing_value = [](const MotionEasing& easing) {
        if (easing.kind != MotionEasingKind::cubic_bezier) {
            return runtime::Value(motion_easing_name(easing));
        }
        return object({
            {"kind", runtime::Value("cubic-bezier")},
            {"x1", runtime::Value(easing.x1)},
            {"x2", runtime::Value(easing.x2)},
            {"y1", runtime::Value(easing.y1)},
            {"y2", runtime::Value(easing.y2)},
        });
    };
    const auto repeat_value = [](const MotionRepeat& repeat) {
        return object({
            {"iterations", runtime::Value(static_cast<double>(repeat.iterations))},
            {"kind", runtime::Value(
                repeat.kind == MotionRepeatKind::forever ? "forever" :
                repeat.kind == MotionRepeatKind::count ? "count" : "none"
            )},
        });
    };
    const auto fill_value = [](const MotionFillMode fill) {
        switch (fill) {
        case MotionFillMode::none: return "none";
        case MotionFillMode::forwards: return "forwards";
        case MotionFillMode::backwards: return "backwards";
        case MotionFillMode::both: return "both";
        }
        return "both";
    };
    ValueFields timings;
    for (const auto& [name, timing] : policy.timings()) {
        timings.emplace(name, object({
            {"delayNanos", runtime::Value(static_cast<double>(timing.delay_nanos))},
            {"durationNanos", runtime::Value(static_cast<double>(timing.duration_nanos))},
            {"easing", easing_value(timing.easing)},
            {"fillMode", runtime::Value(fill_value(timing.fill_mode))},
            {"repeat", repeat_value(timing.repeat)},
            {"reverse", runtime::Value(timing.reverse)},
        }));
    }
    return object({
        {"reducedMotion", runtime::Value(policy.reduced_motion())},
        {"timings", object(std::move(timings))},
    });
}

[[nodiscard]] bool has_authored_style_container(const DescriptionNode& node) noexcept {
    return property(node, "style") != nullptr || property(node, "layout") != nullptr;
}

void report_timing_if_unknown(
    const Theme& theme,
    const DescriptionNode& node,
    const runtime::Value* value,
    const UnknownThemeTiming& callback
) {
    const std::string* name = text(value);
    if (callback != nullptr && name != nullptr && !name->empty() &&
        theme.motion_policy().find(*name) == nullptr) {
        callback(*name, node);
    }
}

void report_unknown_timings(
    const Theme& theme,
    const DescriptionNode& node,
    const UnknownThemeTiming& callback
) {
    if (callback == nullptr) return;
    const runtime::Value* motions = style_field(node, "motions");
    if (motions != nullptr && motions->list() != nullptr) {
        for (const runtime::Value& item : motions->list()->values) {
            report_timing_if_unknown(theme, node, item.field("policy"), callback);
        }
    }
    for (const std::string_view field_name : {
             "animateChanges", "animateContentSize", "disclosure", "$disclosureDefaults",
             "$contentSizeMotionDefaults",
         }) {
        const runtime::Value* declaration = style_field(node, field_name);
        if (declaration == nullptr || declaration->object() == nullptr) continue;
        const runtime::Value* timing = declaration->field("timing");
        if (timing == nullptr) timing = declaration->field("policy");
        report_timing_if_unknown(theme, node, timing, callback);
    }
}

[[nodiscard]] std::shared_ptr<const DescriptionNode> resolve_node(
    const std::shared_ptr<const DescriptionNode>& source,
    const ThemeCatalog& catalog,
    const std::shared_ptr<const Theme>& inherited,
    const std::optional<std::string>& inherited_scope_namespace,
    const ThemeTypePredicate& themed_type,
    const UnknownThemeTiming& unknown_timing,
    ThemeMaterializationStats& stats,
    ThemeMaterializationCache* const cache
) {
    if (source == nullptr) return nullptr;
    std::shared_ptr<const Theme> effective = inherited;
    std::optional<std::string> scope_namespace = inherited_scope_namespace;
    const std::shared_ptr<const Theme>* scoped = source->key.has_value()
                                                     ? catalog.scoped_theme(*source->key)
                                                     : nullptr;
    if (scoped != nullptr) {
        effective = *scoped;
        scope_namespace = *source->key;
    } else if (const std::string* reference = text(property(*source, "theme")); reference != nullptr) {
        if (const auto* registered = catalog.find(*reference); registered != nullptr) {
            effective = *registered;
            scope_namespace.reset();
        }
    }
    if (cache != nullptr) {
        if (std::shared_ptr<const DescriptionNode> cached = cache->find(
                source, effective, scope_namespace, catalog.generation()
            ); cached != nullptr) {
            return cached;
        }
    }
    ++stats.resolved_nodes;

    DescriptionNode::Properties properties = source->properties;
    const bool style_node = themed_type(source->type);
    const std::optional<ResolvedThemeWidgetStyle> themed = style_node
                                                               ? std::optional<ResolvedThemeWidgetStyle>(
                                                                     effective->resolved_style(
                                                                         source->type,
                                                                         node_variant(*source)
                                                                     )
                                                                 )
                                                               : std::nullopt;
    const std::string motion_prefix = themed.has_value()
                                          ? qualified_animation_prefix(
                                                animation_owner_namespace(
                                                    themed->owner_theme, scope_namespace
                                                ),
                                                themed->key
                                            )
                                          : std::string{};
    ValueFields resolved;
    if (themed.has_value()) {
        if (themed->style.visual.has_value()) add_visual(resolved, *themed->style.visual);
        if (themed->style.text_visual.has_value()) add_text_visual(resolved, *themed->style.text_visual);
        if (themed->style.text_layout.has_value()) add_text_layout(resolved, *themed->style.text_layout);
        if (themed->style.layout.has_value()) add_layout(resolved, *themed->style.layout);
        apply_motion(resolved, themed->style.motion, motion_prefix);
    }
    const runtime::Value* authored = property(*source, "$layout");
    bool authored_color_field = false;
    bool authored_foreground_field = false;
    const auto resolve_authored_fields = [
        &effective,
        &resolved,
        &stats,
        &authored_color_field,
        &authored_foreground_field
    ](
        const runtime::Value* container,
        const bool merge_all
    ) -> std::optional<runtime::Value> {
        if (container == nullptr || container->object() == nullptr) return std::nullopt;
        ValueFields concrete_fields;
        for (const auto& [name, value] : container->object()->fields) {
            authored_color_field = authored_color_field || name == "color";
            authored_foreground_field = authored_foreground_field || name == "foreground";
            if (const runtime::ThemeTokenValue* token = value.theme_token(); token != nullptr) {
                const std::optional<runtime::Value> concrete = resolve_token(
                    name, *token, effective->tokens()
                );
                if (!concrete.has_value()) continue;
                concrete_fields.insert_or_assign(name, *concrete);
                resolved.insert_or_assign(name, *concrete);
                if (name == "color") {
                    resolved.insert_or_assign("foreground", *concrete);
                } else if (name == "foreground") {
                    resolved.insert_or_assign("color", *concrete);
                }
                ++stats.symbolic_token_patches;
                continue;
            }
            concrete_fields.insert_or_assign(name, value);
            if (merge_all) resolved.insert_or_assign(name, value);
        }
        return object(std::move(concrete_fields));
    };
    const bool has_authored_layout = authored != nullptr && authored->object() != nullptr;
    if (has_authored_layout) {
        static_cast<void>(resolve_authored_fields(authored, true));
    }
    for (const std::string_view container_name : {"visualStyle", "textVisualStyle"}) {
        const std::optional<runtime::Value> concrete = resolve_authored_fields(
            property(*source, container_name), true
        );
        if (concrete.has_value()) {
            properties.insert_or_assign(
                std::string(container_name), runtime::ExpressionValue(*concrete)
            );
        }
    }
    if (const std::optional<runtime::Value> concrete = resolve_authored_fields(
            property(*source, "textStyle"), false
        ); concrete.has_value()) {
        properties.insert_or_assign("textStyle", runtime::ExpressionValue(*concrete));
    }
    if (authored_color_field && !authored_foreground_field) {
        const auto authored_color = resolved.find("color");
        if (authored_color != resolved.end()) {
            resolved.insert_or_assign("foreground", authored_color->second);
        }
    }

    if (themed.has_value()) {
        // Description expansion has already installed widget layout defaults. A themed layout or
        // text default wins over those framework defaults, while any authored style/layout remains
        // authoritative exactly as it was written.
        if (!has_authored_style_container(*source)) {
            if (themed->style.layout.has_value()) add_layout(resolved, *themed->style.layout);
            if (themed->style.text_layout.has_value()) add_text_layout(resolved, *themed->style.text_layout);
            apply_motion(resolved, themed->style.motion, motion_prefix);
        }
        for (const std::string_view motion_field : {
                 "animation", "enter", "exit", "hover", "pressed", "focus", "focusVisible",
                 "checked", "move", "animate", "motions", "animateChanges", "disclosure",
                 "animateContentSize",
             }) {
            if (property(*source, motion_field) != nullptr) {
                resolved.erase(motion_field);
                if (motion_field == "animateContentSize" || motion_field == "disclosure") {
                    resolved.erase("$themeAnimationSet");
                }
            }
        }
        if (themed->style.layout.has_value() && !has_authored_style_container(*source)) {
            properties.insert_or_assign(
                "$layoutParticipates",
                runtime::ExpressionValue(runtime::Value(themed->style.layout->participates))
            );
        }
    }
    if (themed.has_value() || has_authored_layout) {
        properties.insert_or_assign(
            "$layout",
            runtime::ExpressionValue(object(std::move(resolved)))
        );
    }
    properties.insert_or_assign(
        "$themeName",
        runtime::ExpressionValue(runtime::Value(effective->name()))
    );
    properties.insert_or_assign(
        "$motionPolicy",
        runtime::ExpressionValue(motion_policy_value(effective->motion_policy()))
    );

    const std::size_t child_count = source->children->size();
    MaterializationRange range{0U, child_count};
    const bool lazy = source->materialization.has_value();
    if (lazy) {
        range.start = std::min(source->materialization->start, child_count);
        range.end_exclusive = std::clamp(
            source->materialization->end_exclusive,
            range.start,
            child_count
        );
    }
    std::map<std::size_t, std::shared_ptr<const DescriptionNode>> resolved_children;
    for (std::size_t index = range.start; index < range.end_exclusive; ++index) {
        resolved_children.emplace(index, resolve_node(
            source->children->at(index),
            catalog,
            effective,
            scope_namespace,
            themed_type,
            unknown_timing,
            stats,
            cache
        ));
    }

    auto result = std::make_shared<DescriptionNode>(*source);
    result->generated_source = source;
    result->projected_theme = effective;
    result->projected_theme_scope = scope_namespace;
    result->projected_theme_generation = catalog.generation();
    result->properties = std::move(properties);
    if (lazy) {
        const std::shared_ptr<const DescriptionChildren> original = source->children;
        result->children = std::make_shared<const GeneratedDescriptionChildren>(
            child_count,
            [original, children = std::move(resolved_children)](const std::size_t index) {
                const auto found = children.find(index);
                return found != children.end() ? found->second : original->at(index);
            }
        );
    } else {
        std::vector<std::shared_ptr<const DescriptionNode>> children;
        children.reserve(child_count);
        for (std::size_t index = 0U; index < child_count; ++index) {
            children.push_back(std::move(resolved_children.at(index)));
        }
        result->children = std::make_shared<const EagerDescriptionChildren>(std::move(children));
    }
    report_unknown_timings(*effective, *result, unknown_timing);
    if (cache != nullptr) {
        cache->store(
            source,
            effective,
            scope_namespace,
            catalog.generation(),
            result
        );
    }
    return result;
}

void validate_finite(const double value, const std::string_view label) {
    if (!std::isfinite(value)) {
        throw std::invalid_argument(std::string(label) + " must be finite");
    }
}

void validate_layout_size(const LayoutSize& value, const std::string_view label) {
    if (value.kind > LayoutSize::Kind::clamp) {
        throw std::invalid_argument(std::string(label) + " kind is invalid");
    }
    switch (value.kind) {
    case LayoutSize::Kind::automatic:
    case LayoutSize::Kind::content: break;
    case LayoutSize::Kind::fixed:
    case LayoutSize::Kind::percent:
        validate_non_negative(value.value, label);
        break;
    case LayoutSize::Kind::fill:
        if (!std::isfinite(value.value) || value.value <= 0.0) {
            throw std::invalid_argument(std::string(label) + " fill weight must be finite and positive");
        }
        break;
    case LayoutSize::Kind::clamp:
        if (value.preferred == nullptr) {
            throw std::invalid_argument(std::string(label) + " clamp must define a preferred size");
        }
        if (value.minimum != nullptr) validate_layout_size(*value.minimum, label);
        validate_layout_size(*value.preferred, label);
        if (value.maximum != nullptr) validate_layout_size(*value.maximum, label);
        break;
    }
}

void validate_edges(const Edges& value, const std::string_view label) {
    validate_non_negative(value.left, label);
    validate_non_negative(value.top, label);
    validate_non_negative(value.right, label);
    validate_non_negative(value.bottom, label);
}

void validate_theme_layout(const ThemeWidgetLayoutStyle& style) {
    if (style.kind > LayoutKind::portal || style.align_items > LayoutAlign::stretch ||
        style.justify_content > LayoutJustify::space_evenly ||
        style.align_content > LayoutJustify::space_evenly ||
        (style.align_self.has_value() && *style.align_self > LayoutAlign::stretch) ||
        (style.justify_self.has_value() && *style.justify_self > LayoutAlign::stretch)) {
        throw std::invalid_argument("theme layout enum is invalid");
    }
    validate_layout_size(style.width, "theme width");
    validate_layout_size(style.height, "theme height");
    for (const auto& [value, label] : {
             std::pair{&style.min_width, std::string_view("theme minimum width")},
             std::pair{&style.min_height, std::string_view("theme minimum height")},
             std::pair{&style.max_width, std::string_view("theme maximum width")},
             std::pair{&style.max_height, std::string_view("theme maximum height")},
         }) {
        if (value->has_value()) validate_layout_size(**value, label);
    }
    if (style.aspect_ratio.has_value() &&
        (!std::isfinite(*style.aspect_ratio) || *style.aspect_ratio <= 0.0)) {
        throw std::invalid_argument("theme aspect ratio must be finite and positive");
    }
    if (style.intrinsic_size.has_value()) {
        validate_non_negative(style.intrinsic_size->width, "theme intrinsic width");
        validate_non_negative(style.intrinsic_size->height, "theme intrinsic height");
    }
    validate_edges(style.padding, "theme padding");
    validate_edges(style.margin, "theme margin");
    validate_non_negative(style.gap.x, "theme horizontal gap");
    validate_non_negative(style.gap.y, "theme vertical gap");
    for (const LayoutSize& track : style.grid_columns) validate_layout_size(track, "theme grid column");
    for (const LayoutSize& track : style.grid_rows) validate_layout_size(track, "theme grid row");
    if (style.column_span == 0U || style.row_span == 0U) {
        throw std::invalid_argument("theme grid spans must be positive");
    }
    validate_edges(style.scroll_viewport_insets, "theme scroll viewport insets");
    validate_edges(style.scroll_content_padding, "theme scroll content padding");
    validate_non_negative(style.scrollbar_gutter, "theme scrollbar gutter");
    validate_finite(style.scroll_offset.x, "theme horizontal scroll offset");
    validate_finite(style.scroll_offset.y, "theme vertical scroll offset");
    if (blank(style.portal_target) || !core::valid_utf8(style.portal_target)) {
        throw std::invalid_argument("theme portal target must be non-blank valid UTF-8");
    }
    if (!style.virtual_list.has_value()) return;
    const VirtualListSpec& list = *style.virtual_list;
    if (list.items == nullptr) throw std::invalid_argument("theme virtual list requires items");
    if (!std::isfinite(list.item_extent) || list.item_extent <= 0.0) {
        throw std::invalid_argument("theme virtual item extent must be finite and positive");
    }
    if (list.item_members != nullptr &&
        list.item_members->size() != list.item_count()) {
        throw std::invalid_argument("theme virtual item member bands must match the item count");
    }
    if (list.item_extents.has_value() && list.item_extents->size() != list.item_count()) {
        throw std::invalid_argument("theme virtual item extents must match the item count");
    }
    std::set<std::string, std::less<>> keys;
    for (std::size_t index = 0U; index < list.item_count(); ++index) {
        const std::string key = list.items->key_at(index);
        if (blank(key) || !core::valid_utf8(key) || !keys.insert(key).second) {
            throw std::invalid_argument("theme virtual item keys must be unique non-blank valid UTF-8");
        }
    }
    if (list.item_members != nullptr) {
        for (const std::vector<std::string>& band : *list.item_members) {
            for (const std::string& key : band) {
                if (blank(key) || !core::valid_utf8(key)) {
                    throw std::invalid_argument(
                        "theme virtual item member keys must be non-blank valid UTF-8"
                    );
                }
            }
        }
    }
}

void validate_motion_reference(const std::string& value, const std::string_view label) {
    if (blank(value) || !core::valid_utf8(value)) {
        throw std::invalid_argument(std::string(label) + " must be non-blank valid UTF-8");
    }
}

void validate_compiled_motion(const CompiledMotion& motion, const bool require_name = true) {
    if (require_name || !motion.name.empty()) {
        validate_motion_reference(motion.name, "theme animation name");
    }
    if (motion.timing.duration_nanos <= 0 || motion.timing.delay_nanos < 0 ||
        motion.timing.repeat.kind > MotionRepeatKind::forever ||
        motion.timing.fill_mode > MotionFillMode::both ||
        (motion.timing.repeat.kind == MotionRepeatKind::count &&
         motion.timing.repeat.iterations == 0U)) {
        throw std::invalid_argument("theme animation timing is invalid");
    }
    if (motion.trigger > MotionTrigger::focus_visible) {
        throw std::invalid_argument("theme animation trigger is invalid");
    }
    motion.timing.easing.validate();
    if (motion.tracks.empty()) throw std::invalid_argument("theme animation requires tracks");
    std::set<MotionProperty> properties;
    for (const MotionTrack& track : motion.tracks) {
        if (track.property > MotionProperty::scale_y) {
            throw std::invalid_argument("theme animation property is invalid");
        }
        if (!properties.insert(track.property).second || track.keyframes.empty()) {
            throw std::invalid_argument("theme animation tracks must be unique and contain keyframes");
        }
        double previous = -1.0;
        for (const MotionKeyframe& frame : track.keyframes) {
            if (!std::isfinite(frame.offset) || frame.offset < 0.0 || frame.offset > 1.0 ||
                frame.offset <= previous) {
                throw std::invalid_argument(
                    "theme animation keyframe offsets must be unique and strictly increasing"
                );
            }
            if (const double* number = std::get_if<double>(&frame.value); number != nullptr &&
                !std::isfinite(*number)) {
                throw std::invalid_argument("theme animation numeric values must be finite");
            }
            if (!motion_property_accepts(track.property, frame.value) ||
                frame.value.index() != track.keyframes.front().value.index()) {
                throw std::invalid_argument("theme animation keyframe value kind is incompatible");
            }
            if (frame.easing.has_value()) frame.easing->validate();
            previous = frame.offset;
        }
    }
}

[[nodiscard]] MotionTiming standard_timing() {
    return MotionTiming{
        180'000'000,
        0,
        "cubic-in-out",
        {},
        false,
        MotionFillMode::both,
    };
}

} // namespace

void ThemeTokens::validate() const {
    if (!std::isfinite(spacing_unit) || spacing_unit <= 0.0) {
        throw std::invalid_argument("theme spacing unit must be finite and positive");
    }
    if (!std::isfinite(radius) || radius < 0.0) {
        throw std::invalid_argument("theme radius must be finite and non-negative");
    }
    if (!std::isfinite(density) || density <= 0.0) {
        throw std::invalid_argument("theme density must be finite and positive");
    }
}

const runtime::ColorValue* ThemeTokens::color(const std::string_view name) const noexcept {
    if (name == "surface") return &surface;
    if (name == "surfaceRaised") return &surface_raised;
    if (name == "foreground") return &foreground;
    if (name == "mutedForeground") return &muted_foreground;
    if (name == "accent") return &accent;
    if (name == "danger") return &danger;
    if (name == "focus") return &focus;
    return nullptr;
}

std::optional<double> ThemeTokens::number(const std::string_view name) const noexcept {
    if (name == "spacingUnit") return spacing_unit;
    if (name == "radius") return radius;
    if (name == "density") return density;
    return std::nullopt;
}

void ThemeBorder::validate() const {
    if (!std::isfinite(width) || width < 0.0) {
        throw std::invalid_argument("theme border width must be finite and non-negative");
    }
}

void ThemeWidgetVisualStyle::validate() const {
    validate_non_negative(radius, "theme radius");
    validate_non_negative(indicator_size, "theme indicator size");
    validate_non_negative(indicator_inset, "theme indicator inset");
    validate_non_negative(track_width, "theme track width");
    validate_non_negative(track_height, "theme track height");
    validate_non_negative(track_radius, "theme track radius");
    validate_non_negative(thumb_size, "theme thumb size");
    validate_non_negative(thumb_radius, "theme thumb radius");
    validate_non_negative(scale, "theme scale");
    validate_non_negative(scale_x, "theme x scale");
    validate_non_negative(scale_y, "theme y scale");
    for (const auto& [value, label] : std::array{
             std::pair{std::optional<double>(disabled_opacity), std::string_view("theme disabled opacity")},
             std::pair{std::optional<double>(opacity), std::string_view("theme opacity")},
             std::pair{indicator_position, std::string_view("theme indicator position")},
         }) {
        if (value.has_value() && (!std::isfinite(*value) || *value < 0.0 || *value > 1.0)) {
            throw std::invalid_argument(std::string(label) + " must be between zero and one");
        }
    }
    validate_finite(translate_x, "theme x translation");
    validate_finite(translate_y, "theme y translation");
    if (border.has_value()) border->validate();
    if (focus_ring.has_value()) focus_ring->validate();
}

void ThemeTextLayoutStyle::validate() const {
    if (blank(primary_font) || !core::valid_utf8(primary_font)) {
        throw std::invalid_argument("theme primary font id must be non-blank valid UTF-8");
    }
    for (const std::string& fallback : fallback_fonts) {
        if (blank(fallback) || !core::valid_utf8(fallback)) {
            throw std::invalid_argument("theme fallback font ids must be non-blank valid UTF-8");
        }
    }
    for (const auto& [value, label] : std::array{
             std::pair{std::optional<double>(pixel_size), std::string_view("theme pixel size")},
             std::pair{line_height, std::string_view("theme line height")},
             std::pair{std::optional<double>(line_height_multiplier), std::string_view("theme line-height multiplier")},
         }) {
        if (value.has_value() && (!std::isfinite(*value) || *value <= 0.0)) {
            throw std::invalid_argument(std::string(label) + " must be finite and positive");
        }
    }
    validate_finite(letter_spacing, "theme letter spacing");
}

void ThemeAnimationSet::validate() const {
    const auto validate_spec = [this](const ThemeAnimationSpec& spec, const std::string_view label) {
        if (const std::string* name = spec.named(); name != nullptr) {
            validate_motion_reference(*name, label);
            if (std::ranges::find(declared_animations, *name, &CompiledMotion::name) ==
                declared_animations.end()) {
                throw std::invalid_argument(
                    std::string(label) + " must reference a declaration in the same animation set"
                );
            }
        } else {
            validate_compiled_motion(*spec.inline_animation(), false);
        }
    };
    const auto properties_for = [this](const ThemeAnimationSpec& spec) {
        const CompiledMotion* animation = spec.inline_animation();
        if (animation == nullptr) {
            const std::string& name = *spec.named();
            const auto found = std::ranges::find(declared_animations, name, &CompiledMotion::name);
            if (found != declared_animations.end()) animation = &*found;
        }
        std::set<MotionProperty> result;
        if (animation != nullptr) {
            for (const MotionTrack& track : animation->tracks) result.insert(track.property);
        }
        return result;
    };
    std::set<MotionTrigger> triggers;
    for (const ThemeMotionAttachment& attachment : attachments) {
        if (attachment.trigger > MotionTrigger::focus_visible ||
            attachment.direction > MotionDirection::collapse ||
            (attachment.continuity_trigger.has_value() &&
             *attachment.continuity_trigger > MotionTrigger::focus_visible)) {
            throw std::invalid_argument("theme animation attachment enum is invalid");
        }
        validate_spec(attachment.animation, "theme attached animation");
        if (!triggers.insert(attachment.trigger).second) {
            throw std::invalid_argument("theme animation attachment triggers must be unique");
        }
        if (attachment.continuity_trigger == attachment.trigger) {
            throw std::invalid_argument("theme animation continuity trigger must differ from its trigger");
        }
    }
    std::set<std::string, std::less<>> animation_names;
    for (const CompiledMotion& animation : declared_animations) {
        validate_compiled_motion(animation);
        if (!animation_names.insert(animation.name).second) {
            throw std::invalid_argument("theme declared animation names must be unique");
        }
    }
    std::set<std::string, std::less<>> channel_ids;
    for (const ThemeMotionChannel& channel : channels) {
        if (channel.interaction.has_value() &&
            *channel.interaction > MotionInteraction::focus_visible) {
            throw std::invalid_argument("theme motion channel interaction is invalid");
        }
        validate_motion_reference(channel.id, "theme motion channel id");
        validate_spec(channel.animation, "theme motion channel animation");
        if (channel.interaction.has_value() == channel.state_target.has_value()) {
            throw std::invalid_argument("theme motion channels require exactly one driver");
        }
        if (!channel_ids.insert(channel.id).second) {
            throw std::invalid_argument("theme motion channel ids must be unique");
        }
        const CompiledMotion* resolved = channel.animation.inline_animation();
        if (resolved == nullptr) {
            const auto found = std::ranges::find(
                declared_animations, *channel.animation.named(), &CompiledMotion::name
            );
            if (found != declared_animations.end()) resolved = &*found;
        }
        if (resolved != nullptr && resolved->timing.repeat.kind != MotionRepeatKind::none) {
            throw std::invalid_argument("theme timeline channels require non-repeating animations");
        }
    }
    for (const ThemeMotionValueChannel& channel : value_channels) {
        if (channel.property > MotionProperty::scale_y) {
            throw std::invalid_argument("theme motion value channel property is invalid");
        }
        validate_motion_reference(channel.id, "theme motion value channel id");
        validate_motion_reference(channel.timing, "theme motion value channel timing");
        if (const double* number = std::get_if<double>(&channel.target); number != nullptr &&
            !std::isfinite(*number)) {
            throw std::invalid_argument("theme motion value channel targets must be finite");
        }
        if (!channel_ids.insert(channel.id).second) {
            throw std::invalid_argument("theme motion channel ids must be unique");
        }
        if (!motion_property_interpolable(channel.property) ||
            !motion_property_accepts(channel.property, channel.target)) {
            throw std::invalid_argument("theme motion value channel target is not interpolable for its property");
        }
    }
    if (resolved_properties.has_value()) {
        validate_motion_reference(resolved_properties->timing, "theme resolved-property timing");
        if (resolved_properties->properties.empty()) {
            throw std::invalid_argument("theme resolved-property motion requires properties");
        }
        std::set<MotionProperty> properties;
        for (const MotionProperty property : resolved_properties->properties) {
            if (property > MotionProperty::scale_y) {
                throw std::invalid_argument("theme resolved-property motion property is invalid");
            }
            if (!properties.insert(property).second) {
                throw std::invalid_argument("theme resolved-property motion properties must be unique");
            }
        }
    }
    if (disclosure.has_value()) {
        validate_non_negative(disclosure->collapsed_extent, "theme collapsed extent");
        validate_motion_reference(disclosure->timing, "theme disclosure timing");
    }
    if (content_size.has_value()) {
        validate_motion_reference(content_size->timing, "theme content-size timing");
        if (!content_size->animate_width && !content_size->animate_height) {
            throw std::invalid_argument("theme content-size motion requires an animated axis");
        }
    }

    struct Source final {
        std::string name;
        std::set<MotionProperty> properties;
        bool attachment = false;
    };
    std::vector<Source> sources;
    for (const ThemeMotionAttachment& attachment : attachments) {
        sources.push_back(Source{
            std::string(motion_trigger_name(attachment.trigger)),
            attachment.trigger == MotionTrigger::move
                ? std::set<MotionProperty>{MotionProperty::translate_x, MotionProperty::translate_y}
                : properties_for(attachment.animation),
            true,
        });
    }
    for (const ThemeMotionChannel& channel : channels) {
        sources.push_back(Source{"channel '" + channel.id + "'", properties_for(channel.animation)});
    }
    for (const ThemeMotionValueChannel& channel : value_channels) {
        sources.push_back(Source{"value channel '" + channel.id + "'", {channel.property}});
    }
    if (resolved_properties.has_value()) {
        sources.push_back(Source{
            "resolved-property motion",
            std::set<MotionProperty>(resolved_properties->properties.begin(),
                                     resolved_properties->properties.end()),
        });
    }
    const ThemeContentSizeMotion* effective_content = content_size.has_value()
                                                      ? &*content_size : nullptr;
    ThemeContentSizeMotion disclosure_content;
    if (effective_content == nullptr && disclosure.has_value()) {
        disclosure_content.timing = disclosure->timing;
        effective_content = &disclosure_content;
    }
    if (effective_content != nullptr) {
        std::set<MotionProperty> properties;
        if (effective_content->animate_width) properties.insert(MotionProperty::width);
        if (effective_content->animate_height) properties.insert(MotionProperty::height);
        sources.push_back(Source{"content-size motion", std::move(properties)});
    }
    for (std::size_t first = 0U; first < sources.size(); ++first) {
        for (std::size_t second = first + 1U; second < sources.size(); ++second) {
            if (sources[first].attachment && sources[second].attachment) continue;
            std::vector<MotionProperty> overlap;
            std::ranges::set_intersection(
                sources[first].properties, sources[second].properties,
                std::back_inserter(overlap)
            );
            if (!overlap.empty()) {
                throw std::invalid_argument(
                    "theme motion sources '" + sources[first].name + "' and '" +
                    sources[second].name + "' compete for properties"
                );
            }
        }
    }
}

void ThemedWidgetStyle::validate() const {
    if (visual.has_value()) visual->validate();
    if (text_layout.has_value()) text_layout->validate();
    if (layout.has_value()) validate_theme_layout(*layout);
    if (motion.has_value() && motion->has_value()) (**motion).validate();
}

void ThemeWidgetKey::validate() const {
    if (blank(component_type) || blank(variant)) {
        throw std::invalid_argument("theme widget type and variant must not be empty");
    }
    if (!core::valid_utf8(component_type) || !core::valid_utf8(variant)) {
        throw std::invalid_argument("theme widget type and variant must be valid UTF-8");
    }
}

ThemeMotionPolicy::ThemeMotionPolicy()
    : ThemeMotionPolicy(false, {
          {"emphasized", MotionTiming{
               260'000'000, 0, "cubic-in-out", {}, false, MotionFillMode::both,
           }},
          {"fast", MotionTiming{
               120'000'000, 0, "cubic-out", {}, false, MotionFillMode::both,
           }},
          {std::string(default_motion_timing_name), standard_timing()},
      }) {}

ThemeMotionPolicy::ThemeMotionPolicy(
    const bool reduced_motion,
    std::map<std::string, MotionTiming, std::less<>> timings
) : reduced_motion_(reduced_motion), timings_(std::move(timings)) {
    if (!timings_.contains(default_motion_timing_name)) {
        throw std::invalid_argument("motion policy must define 'standard'");
    }
    for (const auto& [name, timing] : timings_) {
        if (blank(name) || !core::valid_utf8(name)) {
            throw std::invalid_argument("motion timing names must be non-empty valid UTF-8");
        }
        if (timing.duration_nanos <= 0 || timing.delay_nanos < 0 ||
            timing.repeat.kind > MotionRepeatKind::forever ||
            timing.fill_mode > MotionFillMode::both ||
            (timing.repeat.kind == MotionRepeatKind::count && timing.repeat.iterations == 0U)) {
            throw std::invalid_argument("theme motion timings require valid full timing");
        }
        timing.easing.validate();
    }
}

bool ThemeMotionPolicy::reduced_motion() const noexcept { return reduced_motion_; }

const std::map<std::string, MotionTiming, std::less<>>& ThemeMotionPolicy::timings() const noexcept {
    return timings_;
}

const MotionTiming* ThemeMotionPolicy::find(const std::string_view name) const noexcept {
    const auto found = timings_.find(name);
    return found != timings_.end() ? &found->second : nullptr;
}

const MotionTiming& ThemeMotionPolicy::timing_or_default(const std::string_view name) const noexcept {
    if (const MotionTiming* timing = find(name); timing != nullptr) return *timing;
    return timings_.find(default_motion_timing_name)->second;
}

Theme::Theme() : Theme(std::string(default_theme_name), ThemeTokens{}) {}

Theme::Theme(
    std::string name,
    ThemeTokens tokens,
    std::shared_ptr<const Theme> parent,
    std::optional<ThemeMotionPolicy> motion_policy,
    std::map<ThemeWidgetKey, ThemedWidgetStyle> widget_styles
) : name_(std::move(name)),
    tokens_(std::move(tokens)),
    parent_(std::move(parent)),
    motion_policy_(motion_policy.has_value()
                       ? std::move(*motion_policy)
                       : parent_ != nullptr ? parent_->motion_policy() : ThemeMotionPolicy{}),
    widget_styles_(std::move(widget_styles)) {
    if (blank(name_) || !core::valid_utf8(name_)) {
        throw std::invalid_argument("theme name must be non-empty valid UTF-8");
    }
    tokens_.validate();
    for (const auto& [key, style] : widget_styles_) {
        key.validate();
        style.validate();
    }
}

const std::string& Theme::name() const noexcept { return name_; }
const ThemeTokens& Theme::tokens() const noexcept { return tokens_; }
const std::shared_ptr<const Theme>& Theme::parent() const noexcept { return parent_; }
const ThemeMotionPolicy& Theme::motion_policy() const noexcept { return motion_policy_; }

const std::map<ThemeWidgetKey, ThemedWidgetStyle>& Theme::widget_styles() const noexcept {
    return widget_styles_;
}

std::optional<ResolvedThemeWidgetStyle> Theme::explicit_style(
    const std::string_view component_type,
    const std::string_view variant
) const {
    const ThemeWidgetKey exact{std::string(component_type), std::string(variant)};
    if (const auto found = widget_styles_.find(exact); found != widget_styles_.end()) {
        return ResolvedThemeWidgetStyle{found->second, name_, found->first, true};
    }
    if (variant != default_widget_variant) {
        const ThemeWidgetKey fallback{
            std::string(component_type),
            std::string(default_widget_variant),
        };
        if (const auto found = widget_styles_.find(fallback); found != widget_styles_.end()) {
            return ResolvedThemeWidgetStyle{found->second, name_, found->first, true};
        }
    }
    return parent_ != nullptr
               ? parent_->explicit_style(component_type, variant)
               : std::nullopt;
}

ThemedWidgetStyle Theme::semantic_style(
    const std::string_view component_type,
    const std::string_view raw_variant
) const {
    const std::string variant(raw_variant.empty() ? default_widget_variant : raw_variant);
    const bool text_only = component_type == "Text" || component_type == "RichText";
    const bool structural = component_type == "Slot";
    const bool choice_group = component_type == "RadioGroup";
    const bool raised = component_type == "Modal" || component_type == "Select" ||
                        component_type == "Tooltip";
    const runtime::ColorValue accent = variant == "danger" ? tokens_.danger : tokens_.accent;
    const runtime::ColorValue text_foreground = text_only && variant == "primary"
                                                    ? tokens_.accent
        : text_only && variant == "danger" ? tokens_.danger
        : text_only && variant == "subtle" ? tokens_.muted_foreground
                                            : tokens_.foreground;
    ThemeWidgetVisualStyle visual;
    if (text_only || structural || choice_group || variant == "subtle") {
        visual.background.reset();
        visual.border.reset();
    } else {
        visual.background = std::optional<runtime::ColorValue>(
            variant == "primary" || variant == "danger"
                ? accent
                : variant == "secondary" || raised ? tokens_.surface_raised : tokens_.surface
        );
        visual.border = std::optional<ThemeBorder>(ThemeBorder{
            1.0,
            tokens_.muted_foreground,
            true,
        });
    }
    visual.foreground = text_foreground;
    visual.radius = variant == "compact" ? tokens_.radius * 0.75 : tokens_.radius;
    visual.hover_overlay = std::optional<runtime::ColorValue>(
        runtime::ColorValue{255U, 255U, 255U, 18U}
    );
    visual.active_overlay = std::optional<runtime::ColorValue>(
        runtime::ColorValue{0U, 0U, 0U, 32U}
    );
    visual.focus_ring = std::optional<ThemeBorder>(ThemeBorder{2.0, tokens_.focus, true});
    visual.track = std::optional<runtime::ColorValue>(tokens_.surface_raised);
    visual.fill = std::optional<runtime::ColorValue>(accent);
    visual.thumb = std::optional<runtime::ColorValue>(tokens_.foreground);
    visual.selection = std::optional<runtime::ColorValue>(accent);
    visual.scrim = std::optional<runtime::ColorValue>(
        runtime::ColorValue{0U, 0U, 0U, 150U}
    );

    ThemeWidgetTextVisualStyle text_visual;
    text_visual.color = text_foreground;
    text_visual.hint_color = tokens_.muted_foreground;
    text_visual.selection_color = accent;
    text_visual.caret_color = tokens_.foreground;
    ThemeTextLayoutStyle text_layout;
    text_layout.pixel_size = 12.0 * tokens_.density * (variant == "compact" ? 0.9 : 1.0);
    return ThemedWidgetStyle{
        std::move(visual),
        std::move(text_visual),
        std::move(text_layout),
        std::nullopt,
        std::nullopt,
    };
}

ThemedWidgetStyle Theme::style(
    const std::string_view component_type,
    const std::string_view variant
) const {
    return resolved_style(component_type, variant).style;
}

ResolvedThemeWidgetStyle Theme::resolved_style(
    const std::string_view component_type,
    const std::string_view variant
) const {
    if (blank(component_type) || (!variant.empty() && blank(variant)) ||
        !core::valid_utf8(component_type) || !core::valid_utf8(variant)) {
        throw std::invalid_argument(
            "theme style component type and non-empty variant must be non-blank valid UTF-8"
        );
    }
    const std::string_view effective_variant = variant.empty() ? default_widget_variant : variant;
    if (std::optional<ResolvedThemeWidgetStyle> explicit_value = explicit_style(
            component_type,
            effective_variant
        ); explicit_value.has_value()) {
        return std::move(*explicit_value);
    }
    return ResolvedThemeWidgetStyle{
        semantic_style(component_type, variant),
        name_,
        ThemeWidgetKey{std::string(component_type), std::string(effective_variant)},
        false,
    };
}

std::shared_ptr<const Theme> Theme::derive(
    std::string name,
    ThemeTokens tokens,
    std::map<ThemeWidgetKey, ThemedWidgetStyle> widget_styles
) const {
    return std::make_shared<const Theme>(
        std::move(name),
        std::move(tokens),
        std::make_shared<const Theme>(*this),
        motion_policy_,
        std::move(widget_styles)
    );
}

ThemeCatalog::ThemeCatalog() : ThemeCatalog(Theme{}) {}

ThemeCatalog::ThemeCatalog(Theme root) {
    root_ = std::make_shared<const Theme>(std::move(root));
    themes_.emplace(root_->name(), root_);
}

const std::shared_ptr<const Theme>& ThemeCatalog::root() const noexcept { return root_; }

const std::shared_ptr<const Theme>* ThemeCatalog::find(const std::string_view name) const noexcept {
    const auto found = themes_.find(name);
    return found != themes_.end() ? &found->second : nullptr;
}

bool ThemeCatalog::register_theme(Theme theme) {
    const auto existing = themes_.find(theme.name());
    if (existing != themes_.end() && *existing->second == theme) return false;
    auto value = std::make_shared<const Theme>(std::move(theme));
    const std::string name = value->name();
    themes_.insert_or_assign(name, std::move(value));
    advance_generation();
    return true;
}

bool ThemeCatalog::set_root(Theme theme) {
    const bool root_changed = *root_ != theme;
    const std::string name = theme.name();
    const auto existing = themes_.find(name);
    std::shared_ptr<const Theme> value;
    if (existing != themes_.end() && *existing->second == theme) value = existing->second;
    else {
        value = std::make_shared<const Theme>(std::move(theme));
        themes_.insert_or_assign(name, value);
    }
    if (!root_changed && root_ == value) return false;
    root_ = std::move(value);
    advance_generation();
    return true;
}

bool ThemeCatalog::unregister_theme(const std::string_view name) {
    if (blank(name) || !core::valid_utf8(name)) {
        throw std::invalid_argument("theme name must be non-blank valid UTF-8");
    }
    if (name == root_->name()) return false;
    const auto found = themes_.find(name);
    if (found == themes_.end()) return false;
    themes_.erase(found);
    advance_generation();
    return true;
}

const std::shared_ptr<const Theme>* ThemeCatalog::scoped_theme(
    const std::string_view node_key
) const noexcept {
    const auto found = scoped_themes_.find(node_key);
    return found != scoped_themes_.end() ? &found->second : nullptr;
}

bool ThemeCatalog::set_scoped_theme(std::string node_key, Theme theme) {
    if (blank(node_key) || !core::valid_utf8(node_key)) {
        throw std::invalid_argument("theme scope node keys must be non-blank valid UTF-8");
    }
    const auto found = scoped_themes_.find(node_key);
    if (found != scoped_themes_.end() && *found->second == theme) return false;
    scoped_themes_.insert_or_assign(
        std::move(node_key),
        std::make_shared<const Theme>(std::move(theme))
    );
    advance_generation();
    return true;
}

bool ThemeCatalog::clear_scoped_theme(const std::string_view node_key) {
    if (blank(node_key) || !core::valid_utf8(node_key)) {
        throw std::invalid_argument("theme scope node keys must be non-blank valid UTF-8");
    }
    if (scoped_themes_.erase(node_key) == 0U) return false;
    advance_generation();
    return true;
}

std::map<std::string, CompiledMotion, std::less<>> ThemeCatalog::declared_animations() const {
    std::map<std::string, CompiledMotion, std::less<>> result;
    const auto collect = [&result](
                             const auto& self,
                             const Theme& theme,
                             const std::optional<std::string>& scope_namespace
                         ) -> void {
        const std::string owner_namespace = animation_owner_namespace(
            theme.name(), scope_namespace
        );
        for (const auto& [key, style] : theme.widget_styles()) {
            if (!style.motion.has_value() || !style.motion->has_value()) continue;
            const std::string prefix = qualified_animation_prefix(owner_namespace, key);
            for (const CompiledMotion& declaration : (**style.motion).declared_animations) {
                CompiledMotion qualified = declaration;
                qualified.name = prefix + "N" + std::to_string(declaration.name.size()) + "#" +
                                 declaration.name;
                const auto [found, inserted] = result.emplace(qualified.name, qualified);
                if (!inserted && found->second != qualified) {
                    throw std::logic_error("structurally duplicate scoped animation identity");
                }
            }
            const auto collect_inline = [&result, &prefix](
                                            const CompiledMotion& declaration,
                                            const std::string_view slot
                                        ) {
                CompiledMotion qualified = declaration;
                qualified.name = prefix + "I" + std::to_string(slot.size()) + "#" +
                                 std::string(slot);
                const auto [found, inserted] = result.emplace(qualified.name, qualified);
                if (!inserted && found->second != qualified) {
                    throw std::logic_error("duplicate inline scoped animation identity");
                }
            };
            for (const ThemeMotionAttachment& attachment : (**style.motion).attachments) {
                if (const CompiledMotion* inline_animation = attachment.animation.inline_animation();
                    inline_animation != nullptr) {
                    collect_inline(
                        *inline_animation,
                        std::string("trigger/") +
                            std::string(motion_trigger_name(attachment.trigger))
                    );
                }
            }
            for (const ThemeMotionChannel& channel : (**style.motion).channels) {
                if (const CompiledMotion* inline_animation = channel.animation.inline_animation();
                    inline_animation != nullptr) {
                    collect_inline(*inline_animation, std::string("channel/") + channel.id);
                }
            }
        }
        if (theme.parent() != nullptr) self(self, *theme.parent(), scope_namespace);
    };
    for (const auto& [name, theme] : themes_) {
        static_cast<void>(name);
        collect(collect, *theme, std::nullopt);
    }
    for (const auto& [key, theme] : scoped_themes_) {
        collect(collect, *theme, std::optional<std::string>(key));
    }
    return result;
}

std::uint64_t ThemeCatalog::generation() const noexcept { return generation_; }

void ThemeCatalog::advance_generation() {
    if (generation_ == std::numeric_limits<std::uint64_t>::max()) {
        throw std::overflow_error("theme catalog generation exhausted");
    }
    ++generation_;
}

std::shared_ptr<const DescriptionNode> ThemeMaterializationCache::find(
    const std::shared_ptr<const DescriptionNode>& source,
    const std::shared_ptr<const Theme>& effective_theme,
    const std::optional<std::string>& scope_namespace,
    const std::uint64_t catalog_generation
) {
    const auto found = entries_.find(source.get());
    if (found == entries_.end()) return nullptr;
    const std::shared_ptr<const DescriptionNode> cached_source = found->second.source.lock();
    const std::shared_ptr<const Theme> cached_theme = found->second.effective_theme.lock();
    const std::shared_ptr<const DescriptionNode> materialized =
        found->second.materialized.lock();
    if (cached_source != source || cached_theme != effective_theme || materialized == nullptr ||
        found->second.scope_namespace != scope_namespace ||
        found->second.catalog_generation != catalog_generation) {
        entries_.erase(found);
        return nullptr;
    }
    return materialized;
}

void ThemeMaterializationCache::store(
    const std::shared_ptr<const DescriptionNode>& source,
    const std::shared_ptr<const Theme>& effective_theme,
    std::optional<std::string> scope_namespace,
    const std::uint64_t catalog_generation,
    const std::shared_ptr<const DescriptionNode>& materialized
) {
    entries_.insert_or_assign(source.get(), Entry{
        source,
        effective_theme,
        std::move(scope_namespace),
        catalog_generation,
        materialized,
    });
}

void ThemeMaterializationCache::purge(const std::uint64_t catalog_generation) {
    std::erase_if(entries_, [catalog_generation](const auto& value) {
        return value.second.catalog_generation != catalog_generation ||
            value.second.source.expired() || value.second.effective_theme.expired() ||
            value.second.materialized.expired();
    });
}

void ThemeMaterializationCache::clear() noexcept { entries_.clear(); }

ThemeMaterializationResult materialize_theme_tree(
    const std::shared_ptr<const DescriptionNode>& root,
    const ThemeCatalog& catalog,
    const ThemeTypePredicate& themed_type,
    const UnknownThemeTiming& unknown_timing,
    ThemeMaterializationCache* const cache
) {
    if (root == nullptr) return {};
    if (themed_type == nullptr) {
        throw std::invalid_argument("theme materialization requires a widget-type predicate");
    }
    if (cache != nullptr) cache->purge(catalog.generation());
    ThemeMaterializationResult result;
    result.root = resolve_node(
        root,
        catalog,
        catalog.root(),
        std::nullopt,
        themed_type,
        unknown_timing,
        result.stats,
        cache
    );
    return result;
}

ThemeMaterializationResult materialize_theme_subtree(
    const std::shared_ptr<const DescriptionNode>& root,
    const ThemeCatalog& catalog,
    const std::shared_ptr<const Theme>& inherited_theme,
    const std::optional<std::string>& inherited_scope_namespace,
    const ThemeTypePredicate& themed_type,
    const UnknownThemeTiming& unknown_timing,
    ThemeMaterializationCache* const cache
) {
    if (root == nullptr) return {};
    if (inherited_theme == nullptr) {
        throw std::invalid_argument(
            "virtual subtree theme materialization requires an inherited theme"
        );
    }
    if (themed_type == nullptr) {
        throw std::invalid_argument("theme materialization requires a widget-type predicate");
    }
    if (cache != nullptr) cache->purge(catalog.generation());
    ThemeMaterializationResult result;
    result.root = resolve_node(
        root,
        catalog,
        inherited_theme,
        inherited_scope_namespace,
        themed_type,
        unknown_timing,
        result.stats,
        cache
    );
    return result;
}

bool theme_motion_reduced(
    const DescriptionNode& description,
    const bool environment_reduced_motion
) noexcept {
    if (environment_reduced_motion) return true;
    const runtime::Value* policy = property(description, "$motionPolicy");
    const runtime::Value* reduced = policy != nullptr ? policy->field("reducedMotion") : nullptr;
    return reduced != nullptr && reduced->boolean() != nullptr && *reduced->boolean();
}

MotionTiming theme_motion_timing(
    const DescriptionNode& description,
    const std::string_view name
) noexcept {
    const runtime::Value* policy = property(description, "$motionPolicy");
    const runtime::Value* timings = policy != nullptr ? policy->field("timings") : nullptr;
    const auto decoded = [timings](const std::string_view timing_name) -> std::optional<MotionTiming> {
        const runtime::Value* value = timings != nullptr ? timings->field(timing_name) : nullptr;
        if (value == nullptr || value->object() == nullptr) return std::nullopt;
        const runtime::Value* duration = value->field("durationNanos");
        const runtime::Value* delay = value->field("delayNanos");
        const runtime::Value* easing = value->field("easing");
        if (duration == nullptr || duration->number() == nullptr ||
            !std::isfinite(*duration->number()) || *duration->number() <= 0.0 ||
            *duration->number() > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            delay == nullptr || delay->number() == nullptr || !std::isfinite(*delay->number()) ||
            *delay->number() < 0.0 ||
            *delay->number() > static_cast<double>(std::numeric_limits<std::int64_t>::max()) ||
            easing == nullptr) {
            return std::nullopt;
        }
        MotionEasing parsed_easing;
        try {
            if (easing->string() != nullptr) {
                parsed_easing = MotionEasing(*easing->string());
            } else if (easing->object() != nullptr && text(easing->field("kind")) != nullptr &&
                       *text(easing->field("kind")) == "cubic-bezier") {
                const double* x1 = number(easing->field("x1"));
                const double* y1 = number(easing->field("y1"));
                const double* x2 = number(easing->field("x2"));
                const double* y2 = number(easing->field("y2"));
                if (x1 == nullptr || y1 == nullptr || x2 == nullptr || y2 == nullptr) {
                    return std::nullopt;
                }
                parsed_easing = MotionEasing::cubic_bezier(
                    *x1, *y1, *x2, *y2
                );
            } else {
                return std::nullopt;
            }
        } catch (const std::invalid_argument&) {
            return std::nullopt;
        }
        MotionRepeat repeat;
        if (const runtime::Value* repeated = value->field("repeat"); repeated != nullptr) {
            if (repeated->object() == nullptr) return std::nullopt;
            const std::string* kind = text(repeated->field("kind"));
            if (kind == nullptr) return std::nullopt;
            if (*kind == "forever") repeat.kind = MotionRepeatKind::forever;
            else if (*kind == "count") {
                const double* iterations = number(repeated->field("iterations"));
                if (iterations == nullptr || !std::isfinite(*iterations) || *iterations < 1.0 ||
                    *iterations > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
                    return std::nullopt;
                }
                repeat = {MotionRepeatKind::count, static_cast<std::uint32_t>(*iterations)};
            } else if (*kind != "none") return std::nullopt;
        }
        MotionFillMode fill = MotionFillMode::both;
        if (const std::string* mode = text(value->field("fillMode")); mode != nullptr) {
            if (*mode == "none") fill = MotionFillMode::none;
            else if (*mode == "forwards") fill = MotionFillMode::forwards;
            else if (*mode == "backwards") fill = MotionFillMode::backwards;
            else if (*mode != "both") return std::nullopt;
        }
        const runtime::Value* reversed = value->field("reverse");
        if (reversed != nullptr && reversed->boolean() == nullptr) return std::nullopt;
        return MotionTiming{
            static_cast<std::int64_t>(*duration->number()),
            static_cast<std::int64_t>(*delay->number()),
            parsed_easing,
            repeat,
            reversed != nullptr ? *reversed->boolean() : false,
            fill,
        };
    };
    if (const std::optional<MotionTiming> requested = decoded(name); requested.has_value()) {
        return *requested;
    }
    if (const std::optional<MotionTiming> fallback = decoded(default_motion_timing_name);
        fallback.has_value()) {
        return *fallback;
    }
    return standard_timing();
}

bool theme_motion_timing_defined(
    const DescriptionNode& description,
    const std::string_view name
) noexcept {
    const runtime::Value* policy = property(description, "$motionPolicy");
    const runtime::Value* timings = policy != nullptr ? policy->field("timings") : nullptr;
    const runtime::Value* timing = timings != nullptr ? timings->field(name) : nullptr;
    return timing != nullptr && timing->object() != nullptr;
}

} // namespace strata::ui
