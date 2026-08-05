#include "ui/widget/presentation.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <ranges>
#include <utility>

#include "ui/command.hpp"
#include "ui/input.hpp"
#include "resource/svg_image.hpp"
#include "ui/motion.hpp"
#include "ui/svg_image.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] bool participates(
    const WidgetLifecycle& lifecycle,
    const RetainedNode& node
) {
    return !lifecycle.participates || lifecycle.participates(node);
}

[[nodiscard]] double value_number(
    const runtime::Value* value,
    const double fallback
) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
               ? *value->number()
               : fallback;
}

[[nodiscard]] bool value_boolean(
    const runtime::Value* value,
    const bool fallback
) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] std::optional<RenderColor> optional_color(
    const runtime::Value* value
) noexcept {
    if (value == nullptr || value->kind() == runtime::ValueKind::null_value ||
        value->color() == nullptr) {
        return std::nullopt;
    }
    return RenderColor{
        value->color()->red,
        value->color()->green,
        value->color()->blue,
        value->color()->alpha,
    };
}

[[nodiscard]] std::optional<RenderBorder> resolved_border(
    const runtime::Value* value,
    const std::optional<RenderBorder>& fallback
) noexcept {
    if (value == nullptr) return fallback;
    if (value->kind() == runtime::ValueKind::null_value) return std::nullopt;
    if (value->object() == nullptr) return fallback;
    return RenderBorder{
        value_number(value->field("width"), fallback.has_value() ? fallback->width : 1.0),
        widget_color(
            value->field("color"),
            fallback.has_value() ? fallback->color : RenderColor{160U, 168U, 178U, 220U}
        ),
        value_boolean(value->field("inside"), true),
    };
}

[[nodiscard]] WidgetVisualStyle resolve_visual(
    const WidgetRenderScope& scope,
    const WidgetVisualProfile profile
) {
    const std::string variant = scope.string("variant", "default");
    const RenderColor accent = variant == "danger"
                                   ? RenderColor{224U, 74U, 74U, 255U}
                                   : RenderColor{91U, 141U, 239U, 255U};

    WidgetVisualStyle result;
    result.fill = accent;
    result.selection = accent;
    if (profile.text_variant_foreground && variant == "primary") result.foreground = accent;
    else if (profile.text_variant_foreground && variant == "danger") {
        result.foreground = RenderColor{224U, 74U, 74U, 255U};
    } else if (profile.text_variant_foreground && variant == "subtle") {
        result.foreground = RenderColor{160U, 168U, 178U, 220U};
    }
    if (profile.transparent_chrome || variant == "subtle") {
        result.background.reset();
        result.border.reset();
    } else {
        result.background = variant == "primary" || variant == "danger"
                                ? accent
                                : variant == "secondary" || profile.raised_chrome
                                      ? RenderColor{24U, 24U, 42U, 240U}
                                      : RenderColor{34U, 38U, 46U, 220U};
        result.border = RenderBorder{1.0, RenderColor{160U, 168U, 178U, 220U}, true};
    }
    const runtime::Value* authored_style = scope.property("$layout");
    const bool has_authored_visuals = authored_style != nullptr &&
        std::ranges::any_of(
            std::array{
                "background", "foreground", "color", "border", "radius", "hoverOverlay",
                "activeOverlay", "focusRing", "disabledOpacity", "opacity", "translateX",
                "translateY", "scale", "scaleX", "scaleY", "track", "fill", "thumb",
                "selection", "scrim", "indicatorSize", "indicatorInset", "trackWidth",
                "trackHeight", "trackRadius", "thumbSize", "thumbRadius",
                "hintColor", "selectionColor", "caretColor",
            },
            [authored_style](const std::string_view name) {
                return authored_style->field(name) != nullptr;
            }
        );
    if (has_authored_visuals) {
        result.border = RenderBorder{1.0, RenderColor{92U, 102U, 118U, 180U}, true};
        result.track = RenderColor{18U, 22U, 28U, 220U};
        result.thumb = RenderColor{242U, 245U, 249U, 255U};
        result.selection = RenderColor{91U, 141U, 239U, 150U};
    }
    if (variant == "compact") result.radius = 3.0;

    if (const runtime::Value* value = scope.style("background"); value != nullptr) {
        result.background = paint_from_value(value);
    }
    result.foreground = widget_color(
        scope.style("foreground"),
        widget_color(scope.style("color"), result.foreground)
    );
    result.border = resolved_border(scope.style("border"), result.border);
    result.radius = std::max(0.0, value_number(scope.style("radius"), result.radius));
    if (const runtime::Value* value = scope.style("hoverOverlay"); value != nullptr) {
        result.hover_overlay = optional_color(value);
    }
    if (const runtime::Value* value = scope.style("activeOverlay"); value != nullptr) {
        result.active_overlay = optional_color(value);
    }
    result.focus_ring = resolved_border(scope.style("focusRing"), result.focus_ring);
    result.disabled_opacity = std::clamp(
        value_number(scope.style("disabledOpacity"), result.disabled_opacity), 0.0, 1.0
    );
    result.opacity = std::clamp(value_number(scope.style("opacity"), 1.0), 0.0, 1.0);
    result.track = paint_from_value(scope.style("track")).value_or(result.track);
    result.fill = paint_from_value(scope.style("fill")).value_or(result.fill);
    result.thumb = widget_color(scope.style("thumb"), result.thumb);
    result.selection = widget_color(scope.style("selection"), result.selection);
    result.text_hint = widget_color(scope.style("hintColor"), result.text_hint);
    result.text_selection = widget_color(scope.style("selectionColor"), result.text_selection);
    result.caret = widget_color(scope.style("caretColor"), result.foreground);
    result.scrim = paint_from_value(scope.style("scrim")).value_or(result.scrim);
    const auto optional_dimension = [&scope](const std::string_view name) -> std::optional<double> {
        const runtime::Value* value = scope.style(name);
        return value != nullptr && value->number() != nullptr
                   ? std::optional<double>(std::max(0.0, *value->number()))
                   : std::nullopt;
    };
    result.indicator_size = optional_dimension("indicatorSize");
    result.indicator_inset = optional_dimension("indicatorInset");
    result.track_width = optional_dimension("trackWidth");
    result.track_height = optional_dimension("trackHeight");
    result.track_radius = optional_dimension("trackRadius");
    result.track_gap = optional_dimension("trackGap");
    result.active_track_gap = optional_dimension("activeTrackGap");
    result.thumb_size = optional_dimension("thumbSize");
    result.thumb_width = optional_dimension("thumbWidth");
    result.thumb_height = optional_dimension("thumbHeight");
    result.active_thumb_width = optional_dimension("activeThumbWidth");
    result.active_thumb_height = optional_dimension("activeThumbHeight");
    result.thumb_radius = optional_dimension("thumbRadius");
    if (const MotionComputedValues* motion = scope.motion_values(); motion != nullptr) {
        const auto animated_color = [motion](const MotionProperty property)
            -> std::optional<RenderColor> {
            const runtime::ColorValue* value = motion->color(property);
            return value != nullptr
                       ? std::optional<RenderColor>(RenderColor{
                             value->red, value->green, value->blue, value->alpha,
                         })
                       : std::nullopt;
        };
        if (const auto value = animated_color(MotionProperty::background); value.has_value()) {
            result.background = Paint(*value);
        }
        if (const auto foreground_color = animated_color(MotionProperty::foreground);
            foreground_color.has_value()) {
            result.foreground = *foreground_color;
        } else if (const auto text_color = animated_color(MotionProperty::color);
                   text_color.has_value()) {
            result.foreground = *text_color;
        }
        if (const auto value = motion->number(MotionProperty::radius); value.has_value()) {
            result.radius = *value;
        }
        if (const auto value = animated_color(MotionProperty::track); value.has_value()) result.track = Paint(*value);
        if (const auto value = animated_color(MotionProperty::fill); value.has_value()) result.fill = Paint(*value);
        if (const auto value = animated_color(MotionProperty::thumb); value.has_value()) result.thumb = *value;
        if (const auto value = motion->number(MotionProperty::track_radius); value.has_value()) {
            result.track_radius = *value;
        }
        if (const auto value = motion->number(MotionProperty::thumb_radius); value.has_value()) {
            result.thumb_radius = *value;
        }
        if (const auto value = motion->number(MotionProperty::thumb_size); value.has_value()) {
            result.thumb_size = *value;
        }
        result.indicator_position = motion->number(MotionProperty::indicator_position);
        if (const auto value = motion->number(MotionProperty::opacity); value.has_value()) {
            result.opacity = std::clamp(*value, 0.0, 1.0);
        }
    }
    if (scope.string("appearance", "DEFAULT") == "BARE") {
        result.background.reset();
        result.border.reset();
        result.focus_ring.reset();
        result.hover_overlay.reset();
        result.active_overlay.reset();
    }
    return result;
}

} // namespace

const std::string* widget_string_value(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->string() != nullptr) return value->string();
    if (value->key() != nullptr) return &value->key()->value;
    return nullptr;
}

const std::string* widget_image_value(const runtime::Value* value) noexcept {
    if (value == nullptr) return nullptr;
    if (value->image() != nullptr) return &value->image()->id;
    return widget_string_value(value);
}

TextureRegion widget_texture_region(
    const runtime::Value* value,
    const TextureRegion fallback
) noexcept {
    if (value == nullptr || value->object() == nullptr) return fallback;
    const auto coordinate = [value](const std::string_view name, const double otherwise) {
        const runtime::Value* field = value->field(name);
        return field != nullptr && field->number() != nullptr ? *field->number() : otherwise;
    };
    return TextureRegion{
        coordinate("u", fallback.u),
        coordinate("v", fallback.v),
        coordinate("width", fallback.width),
        coordinate("height", fallback.height),
    };
}

RenderColor widget_color(
    const runtime::Value* value,
    const RenderColor fallback
) noexcept {
    return optional_color(value).value_or(fallback);
}

RenderColor widget_opacity(RenderColor color, const double multiplier) noexcept {
    color.alpha = static_cast<std::uint8_t>(std::clamp(
        static_cast<int>(static_cast<double>(color.alpha) * std::clamp(multiplier, 0.0, 1.0)),
        0,
        255
    ));
    return color;
}

std::string widget_number_text(const double value) {
    std::array<char, 64U> buffer{};
    const double rounded = std::round(value);
    if (std::abs(value - rounded) <= 1.0e-9 &&
        rounded >= static_cast<double>(std::numeric_limits<std::int64_t>::min()) &&
        rounded <= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        const auto result = std::to_chars(
            buffer.data(), buffer.data() + buffer.size(), static_cast<std::int64_t>(rounded)
        );
        return std::string(buffer.data(), result.ptr);
    }
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general
    );
    return std::string(buffer.data(), result.ptr);
}

void command_tooltip_overlay(WidgetRenderScope& scope) {
    if (!scope.input().command_tooltip_ready(scope.node().identity()) ||
        scope.text_engine() == nullptr) {
        return;
    }
    const std::optional<CommandActivationBinding> binding =
        scope.command_index().activation_binding(scope.node());
    if (!binding.has_value() || binding->command == nullptr || binding->command->label.empty()) {
        return;
    }
    const std::string shortcut = format_command_shortcut(*binding->command);
    const std::string text = shortcut.empty()
                                 ? binding->command->label
                                 : binding->command->label + "  " + shortcut;
    const font::ShapedText shaped = scope.text_engine()->shape(scope.node(), text);
    const double width = shaped.metrics.width + 16.0;
    const double height = shaped.metrics.height + 10.0;
    const Rect anchor = scope.layout().bounds;
    const Rect viewport = scope.root_bounds();
    const double x = std::clamp(
        anchor.x + (anchor.width - width) * 0.5,
        viewport.x,
        std::max(viewport.x, viewport.right() - width)
    );
    double y = anchor.y - 4.0 - height;
    if (y < viewport.y) y = anchor.bottom() + 4.0;
    y = std::clamp(y, viewport.y, std::max(viewport.y, viewport.bottom() - height));
    const Rect popup{x, y, width, height};
    scope.rounded_rect(
        popup,
        RenderColor{20U, 24U, 32U, 248U},
        RenderBorder{1.0, RenderColor{90U, 102U, 120U, 220U}, true},
        4.0
    );
    scope.text(
        text,
        Point{popup.x + 8.0, popup.y + 5.0},
        RenderColor{236U, 240U, 244U, 255U}
    );
}

WidgetRenderScope::WidgetRenderScope(
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputRouter& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    const double inherited_opacity,
    const WidgetVisualProfile visual_profile,
    std::vector<RenderCommand>& output,
    const bool apply_presentation_opacity
) : node_(node),
    layout_(layout),
    layout_result_(layout_result),
    input_(input),
    commands_(commands),
    text_(text),
    svg_images_(svg_images),
    motion_(motion),
    inherited_opacity_(inherited_opacity),
    apply_presentation_opacity_(apply_presentation_opacity),
    output_(output),
    enabled_([&node, &commands]() {
        const auto found = node.description().properties.find("enabled");
        const runtime::Value* value = found != node.description().properties.end()
                                          ? found->second.value()
                                          : nullptr;
        if (value != nullptr && value->boolean() != nullptr && !*value->boolean()) return false;
        const bool behavior_enabled = std::ranges::none_of(
            node.description().behaviors,
            [](const DescriptionBehavior& behavior) {
                return behavior.id == "strata.disabled" && behavior.enabled;
            }
        );
        if (!behavior_enabled) return false;
        const std::optional<CommandActivationBinding> binding =
            commands.activation_binding(node);
        return !binding.has_value() || binding->command == nullptr || binding->command->enabled;
    }()) {
    visual_ = resolve_visual(*this, visual_profile);
}

const RetainedNode& WidgetRenderScope::node() const noexcept { return node_; }
const LayoutRecord& WidgetRenderScope::layout() const noexcept { return layout_; }
const LayoutResult& WidgetRenderScope::layout_result() const noexcept { return layout_result_; }
const InputRouter& WidgetRenderScope::input() const noexcept { return input_; }
const CommandIndex& WidgetRenderScope::command_index() const noexcept { return commands_; }
const TextEngine* WidgetRenderScope::text_engine() const noexcept { return text_; }
const WidgetVisualStyle& WidgetRenderScope::visual() const noexcept { return visual_; }
bool WidgetRenderScope::enabled() const noexcept { return enabled_; }
bool WidgetRenderScope::focused() const noexcept { return input_.focused(node_.identity()); }
bool WidgetRenderScope::focus_visible() const noexcept {
    return input_.focus_visible(node_.identity());
}
bool WidgetRenderScope::hovered() const noexcept { return input_.hovered(node_.identity()); }
bool WidgetRenderScope::active() const noexcept { return input_.active(node_.identity()); }
Rect WidgetRenderScope::root_bounds() const noexcept {
    const LayoutRecord* root = layout_result_.find(layout_result_.root_identity);
    return root != nullptr ? root->bounds : layout_.bounds;
}
double WidgetRenderScope::motion_progress(
    const std::string_view id,
    const double fallback
) const noexcept {
    if (motion_ == nullptr) return fallback;
    const std::vector<MotionInspectionChannel>* channels =
        motion_->inspection_channels(node_.identity());
    if (channels == nullptr) return fallback;
    const auto found = std::ranges::find(*channels, id, &MotionInspectionChannel::id);
    return found != channels->end() ? found->progress : fallback;
}
double WidgetRenderScope::alpha() const noexcept {
    if (!apply_presentation_opacity_) return 1.0;
    const MotionComputedValues* computed = motion_values();
    const double presentation_opacity = computed != nullptr
                                            ? computed->number(MotionProperty::opacity)
                                                  .value_or(visual_.opacity)
                                            : visual_.opacity;
    return inherited_opacity_ * presentation_opacity *
           (enabled_ ? 1.0 : visual_.disabled_opacity);
}

const MotionComputedValues* WidgetRenderScope::motion_values() const noexcept {
    return motion_ != nullptr ? motion_->computed_values(node_.identity()) : nullptr;
}

const runtime::Value* WidgetRenderScope::property(const std::string_view name) const noexcept {
    const auto found = node_.description().properties.find(name);
    return found != node_.description().properties.end() ? found->second.data_value() : nullptr;
}

const runtime::Value* WidgetRenderScope::style(const std::string_view name) const noexcept {
    // Generated presentation wrappers use explicit null to suppress default theme chrome.
    const auto direct = node_.description().properties.find(name);
    if (direct != node_.description().properties.end()) {
        return direct->second.data_value();
    }
    const runtime::Value* layout = property("$layout");
    if (layout != nullptr && layout->object() != nullptr) {
        if (const runtime::Value* field = layout->field(name); field != nullptr) return field;
    }
    return nullptr;
}

const runtime::Value* WidgetRenderScope::retained(const std::string_view name) const noexcept {
    return node_.retained_value(name);
}

double WidgetRenderScope::number(const std::string_view name, const double fallback) const noexcept {
    return value_number(property(name), fallback);
}

bool WidgetRenderScope::boolean(const std::string_view name, const bool fallback) const noexcept {
    return value_boolean(property(name), fallback);
}

std::string WidgetRenderScope::string(
    const std::string_view name,
    std::string fallback
) const {
    const std::string* value = widget_string_value(property(name));
    return value != nullptr ? *value : std::move(fallback);
}

const runtime::ValueList* WidgetRenderScope::list(const std::string_view name) const noexcept {
    const runtime::Value* value = property(name);
    return value != nullptr ? value->list() : nullptr;
}

double WidgetRenderScope::effective_number(
    const std::string_view controlled,
    const std::string_view retained_name,
    const std::string_view initial,
    const double fallback
) const noexcept {
    const runtime::Value* value = property(controlled);
    if (value == nullptr || value->number() == nullptr) value = retained(retained_name);
    if (value == nullptr || value->number() == nullptr) value = property(initial);
    return value_number(value, fallback);
}

bool WidgetRenderScope::effective_boolean(
    const std::string_view controlled,
    const std::string_view retained_name,
    const std::string_view initial,
    const bool fallback
) const noexcept {
    const runtime::Value* value = property(controlled);
    if (value == nullptr || value->boolean() == nullptr) value = retained(retained_name);
    if (value == nullptr || value->boolean() == nullptr) value = property(initial);
    return value_boolean(value, fallback);
}

std::optional<std::string_view> WidgetRenderScope::node_text() const noexcept {
    const runtime::Value* value = property("text");
    if (value == nullptr) value = retained("$text");
    if (value == nullptr) value = property("label");
    const std::string* text = widget_string_value(value);
    return text != nullptr ? std::optional<std::string_view>(*text) : std::nullopt;
}

void WidgetRenderScope::rounded_rect(
    const Rect bounds,
    Paint fill,
    std::optional<RenderBorder> border_value,
    const std::optional<double> radius
) {
    fill.multiply_alpha(alpha());
    if (border_value.has_value()) border_value->color = widget_opacity(border_value->color, alpha());
    output_.emplace_back(RoundedRectRenderCommand{
        bounds,
        CornerRadii::all(radius.value_or(visual_.radius)),
        fill,
        border_value,
        1.0,
    });
}

void WidgetRenderScope::border(
    const Rect bounds,
    RenderBorder border_value,
    const std::optional<double> radius
) {
    border_value.color = widget_opacity(border_value.color, alpha());
    output_.emplace_back(BorderRenderCommand{
        bounds,
        border_value,
        CornerRadii::all(radius.value_or(visual_.radius)),
    });
}

void WidgetRenderScope::solid_rect(const Rect bounds, Paint fill) {
    fill.multiply_alpha(alpha());
    output_.emplace_back(SolidRectRenderCommand{bounds, std::move(fill)});
}

void WidgetRenderScope::shape(const Rect bounds, PathShape value) {
    if (value.fill.has_value()) value.fill->multiply_alpha(alpha());
    if (value.stroke.has_value()) value.stroke->multiply_alpha(alpha());
    output_.emplace_back(PathRenderCommand{bounds, std::move(value)});
}

void WidgetRenderScope::image(
    const Rect bounds,
    std::string image,
    RenderColor tint,
    const TextureRegion source
) {
    if (svg_images_ != nullptr) {
        if (const svg::Document* document = svg_images_->find(image); document != nullptr) {
            append_svg_image(output_, *document, bounds, source, tint, alpha());
            return;
        }
    }
    output_.emplace_back(ImageRenderCommand{
        bounds,
        std::move(image),
        source,
        widget_opacity(tint, alpha()),
    });
}

void WidgetRenderScope::nine_patch(
    const Rect bounds,
    std::string texture,
    const Edges source_insets,
    const Edges destination_insets,
    const TextureRegion source,
    RenderColor tint
) {
    output_.emplace_back(NinePatchRenderCommand{
        bounds,
        std::move(texture),
        source,
        source_insets,
        destination_insets,
        widget_opacity(tint, alpha()),
    });
}

void WidgetRenderScope::custom_mesh(
    const Rect bounds,
    std::string mesh,
    MeshGeometry geometry,
    std::optional<std::string> texture,
    std::optional<MaterialState> material
) {
    output_.emplace_back(CustomMeshRenderCommand{
        bounds,
        std::move(mesh),
        std::move(geometry),
        std::move(texture),
        std::move(material),
        alpha(),
    });
}

void WidgetRenderScope::blur_region(
    const Rect bounds,
    const double radius,
    const std::size_t downsample
) {
    output_.emplace_back(BlurRegionRenderCommand{bounds, radius, downsample});
}

void WidgetRenderScope::shadow(
    const Rect bounds,
    const CornerRadii radii,
    RenderColor color,
    const double radius,
    const double spread
) {
    output_.emplace_back(ShadowRenderCommand{
        bounds,
        radii,
        widget_opacity(color, alpha()),
        radius,
        spread,
    });
}

void WidgetRenderScope::push_clip(const Rect bounds, const CornerRadii radii) {
    output_.emplace_back(ClipPushRenderCommand{bounds, radii});
}

void WidgetRenderScope::pop_clip() { output_.emplace_back(ClipPopRenderCommand{}); }

void WidgetRenderScope::push_transform(const double scale, const Point translation) {
    output_.emplace_back(TransformPushRenderCommand{
        scale, 0.0, translation.x, 0.0, scale, translation.y,
    });
}

void WidgetRenderScope::pop_transform() {
    output_.emplace_back(TransformPopRenderCommand{});
}

void WidgetRenderScope::append(RenderCommand command, const double opacity) {
    output_.push_back(render_command_with_opacity(std::move(command), alpha() * opacity));
}

void WidgetRenderScope::text(
    const std::string_view value,
    const Point origin,
    RenderColor color,
    const double alignment_width,
    const WidgetTextAlignment alignment
) {
    if (text_ == nullptr || value.empty()) return;
    const TextLayout layout_value = text_->layout(node_, value);
    const font::ShapedText& shaped = layout_value.shaped;
    double x_offset = 0.0;
    if (alignment == WidgetTextAlignment::center) {
        x_offset = std::max(0.0, (alignment_width - shaped.metrics.width) * 0.5);
    } else if (alignment == WidgetTextAlignment::end) {
        x_offset = std::max(0.0, alignment_width - shaped.metrics.width);
    }
    const bool clipped = layout_value.clipped && layout_value.wrap_width.has_value();
    const Rect text_cull_bounds{
        origin.x,
        origin.y,
        std::max(shaped.metrics.width, alignment_width),
        shaped.metrics.height,
    };
    if (clipped) {
        push_clip(Rect{
            origin.x,
            origin.y,
            *layout_value.wrap_width,
            shaped.metrics.height,
        });
    }
    color = widget_opacity(color, alpha());
    for (const TextResolvedRun& resolved_run : layout_value.resolved_runs) {
        std::vector<LogicalGlyph> glyphs;
        glyphs.reserve(resolved_run.glyph_end_index - resolved_run.glyph_start_index);
        for (std::size_t glyph_index = resolved_run.glyph_start_index;
             glyph_index < resolved_run.glyph_end_index;
             ++glyph_index) {
            const font::ShapedGlyph& glyph = shaped.glyphs[glyph_index];
            if (glyph.glyph_id == 0U) continue;
            glyphs.push_back(LogicalGlyph{
                resolved_run.font_id,
                glyph.glyph_id,
                glyph.code_point,
                glyph.text_start_offset,
                glyph.text_end_offset,
                glyph.x + x_offset,
                glyph.baseline,
                glyph.advance,
                glyph.x_placement,
                glyph.y_placement,
                glyph.y_advance,
                resolved_run.font_style_flags,
            });
        }
        if (!glyphs.empty()) {
            output_.emplace_back(TextRunRenderCommand{
                origin,
                color,
                resolved_run.pixel_size,
                std::move(glyphs),
                resolved_run.font_rasterization,
                text_cull_bounds,
            });
        }
    }
    if (clipped) pop_clip();
}

void WidgetRenderScope::node_text(const Point origin, const RenderColor color) {
    const std::optional<std::string_view> value = node_text();
    if (value.has_value()) text(*value, origin, color);
}

void WidgetRenderScope::rich_text(const Point origin, const RenderColor fallback) {
    const std::optional<std::string_view> combined = node_text();
    if (text_ == nullptr || !combined.has_value() || combined->empty()) {
        node_text(origin, fallback);
        return;
    }
    const TextLayout layout_value = text_->layout(node_, *combined);
    const font::ShapedText& shaped = layout_value.shaped;
    const Rect text_cull_bounds{
        origin.x, origin.y, shaped.metrics.width, shaped.metrics.height,
    };
    for (const TextResolvedRun& resolved_run : layout_value.resolved_runs) {
        RenderColor color = widget_opacity(
            resolved_run.color.has_value()
                ? RenderColor{
                      resolved_run.color->red,
                      resolved_run.color->green,
                      resolved_run.color->blue,
                      resolved_run.color->alpha,
                  }
                : resolved_run.interactive
                    ? RenderColor{100U, 165U, 255U, 255U}
                    : fallback,
            alpha()
        );
        std::vector<LogicalGlyph> glyphs;
        glyphs.reserve(resolved_run.glyph_end_index - resolved_run.glyph_start_index);
        for (std::size_t glyph_index = resolved_run.glyph_start_index;
             glyph_index < resolved_run.glyph_end_index;
             ++glyph_index) {
            const font::ShapedGlyph& glyph = shaped.glyphs[glyph_index];
            if (glyph.glyph_id == 0U) continue;
            glyphs.push_back(LogicalGlyph{
                resolved_run.font_id,
                glyph.glyph_id,
                glyph.code_point,
                glyph.text_start_offset,
                glyph.text_end_offset,
                glyph.x,
                glyph.baseline,
                glyph.advance,
                glyph.x_placement,
                glyph.y_placement,
                glyph.y_advance,
                resolved_run.font_style_flags,
            });
        }
        if (!glyphs.empty()) {
            output_.emplace_back(TextRunRenderCommand{
                origin,
                color,
                resolved_run.pixel_size,
                std::move(glyphs),
                resolved_run.font_rasterization,
                text_cull_bounds,
            });
        }
    }
}

void WidgetRenderScope::focus(const Rect bounds) {
    if (!focus_visible() || !visual_.focus_ring.has_value() ||
        visual_.focus_ring->width <= 0.0) {
        return;
    }

    // An outline needs breathing room rather than reading as a duplicate control border. Focus
    // geometry owns its placement, so normalize authored border placement and keep the same gap
    // across native and packet renderers.
    constexpr double gap = 2.0;
    RenderBorder ring = *visual_.focus_ring;
    ring.inside = true;
    const double outset = gap + ring.width;
    const Rect expanded{
        bounds.x - outset,
        bounds.y - outset,
        bounds.width + outset * 2.0,
        bounds.height + outset * 2.0,
    };
    border(expanded, ring, visual_.radius + outset);
}

void WidgetRenderScope::interaction(
    const Rect bounds,
    const std::string_view subtarget
) {
    const bool hovered_value = subtarget.empty()
        ? input_.hovered(node_.identity())
        : input_.subtarget_hovered(node_.identity(), subtarget);
    const bool active_value = subtarget.empty()
        ? input_.active(node_.identity())
        : input_.subtarget_active(node_.identity(), subtarget);
    if (hovered_value && visual_.hover_overlay.has_value()) {
        rounded_rect(bounds, *visual_.hover_overlay);
    }
    if (active_value && visual_.active_overlay.has_value()) {
        rounded_rect(bounds, *visual_.active_overlay);
    }
}

std::vector<RenderCommand> build_widget_fragment(
    const WidgetRegistry& registry,
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputRouter& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    const double inherited_opacity,
    const bool apply_presentation_opacity
) {
    std::vector<RenderCommand> output;
    const WidgetLifecycle* lifecycle = registry.find(node.description().type);
    if (lifecycle == nullptr || !participates(*lifecycle, node) ||
        lifecycle->present.content == nullptr) {
        return output;
    }
    WidgetRenderScope scope(
        node, layout, layout_result, input, commands, text, svg_images, motion,
        inherited_opacity, lifecycle->present.visual, output, apply_presentation_opacity
    );
    lifecycle->present.content(scope);
    return output;
}

std::vector<RenderCommand> build_widget_overlay(
    const WidgetRegistry& registry,
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputRouter& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    const double inherited_opacity
) {
    std::vector<RenderCommand> output;
    const WidgetLifecycle* lifecycle = registry.find(node.description().type);
    if (lifecycle == nullptr || !participates(*lifecycle, node) ||
        lifecycle->present.overlay == nullptr) {
        return output;
    }
    WidgetRenderScope scope(
        node, layout, layout_result, input, commands, text, svg_images, motion,
        inherited_opacity, lifecycle->present.visual, output
    );
    lifecycle->present.overlay(scope);
    return output;
}

void append_widget_foreground(
    const WidgetRegistry& registry,
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputRouter& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    const double inherited_opacity,
    std::vector<RenderCommand>& output
) {
    const WidgetLifecycle* lifecycle = registry.find(node.description().type);
    if (lifecycle == nullptr || !participates(*lifecycle, node) ||
        lifecycle->present.foreground == nullptr) {
        return;
    }
    WidgetRenderScope scope(
        node, layout, layout_result, input, commands, text, svg_images, motion,
        inherited_opacity, lifecycle->present.visual, output
    );
    lifecycle->present.foreground(scope);
}

std::optional<Rect> widget_descendant_clip(
    const WidgetRegistry& registry,
    const RetainedNode& node,
    const LayoutRecord& layout,
    const LayoutResult& layout_result,
    const InputRouter& input,
    const CommandIndex& commands,
    const TextEngine* text,
    const resource::SvgImageRegistry* svg_images,
    const MotionRuntime* motion,
    const double inherited_opacity
) {
    const WidgetLifecycle* lifecycle = registry.find(node.description().type);
    if (lifecycle == nullptr || !participates(*lifecycle, node)) return std::nullopt;
    if (const std::optional<ContentSizeMotionSpec> content_motion =
            content_size_motion(node.description());
        content_motion.has_value() && content_motion->clip) {
        return layout.bounds;
    }
    if (lifecycle->present.descendant_clip == nullptr) {
        return std::nullopt;
    }
    std::vector<RenderCommand> unused;
    WidgetRenderScope scope(
        node, layout, layout_result, input, commands, text, svg_images, motion,
        inherited_opacity, lifecycle->present.visual, unused
    );
    return lifecycle->present.descendant_clip(scope);
}

} // namespace strata::ui
