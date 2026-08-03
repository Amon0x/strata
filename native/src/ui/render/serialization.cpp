#include "ui/render.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue rectangle(const Rect& value) {
    return object({
        {"height", JsonValue(value.height)},
        {"width", JsonValue(value.width)},
        {"x", JsonValue(value.x)},
        {"y", JsonValue(value.y)},
    });
}

[[nodiscard]] JsonValue radii(const CornerRadii& value) {
    return object({
        {"bottomLeft", JsonValue(value.bottom_left)},
        {"bottomRight", JsonValue(value.bottom_right)},
        {"topLeft", JsonValue(value.top_left)},
        {"topRight", JsonValue(value.top_right)},
    });
}

[[nodiscard]] JsonValue edges(const Edges& value) {
    return object({
        {"bottom", JsonValue(value.bottom)},
        {"left", JsonValue(value.left)},
        {"right", JsonValue(value.right)},
        {"top", JsonValue(value.top)},
    });
}

[[nodiscard]] JsonValue texture_region(const TextureRegion& value) {
    return object({
        {"height", JsonValue(value.height)},
        {"u", JsonValue(value.u)},
        {"v", JsonValue(value.v)},
        {"width", JsonValue(value.width)},
    });
}

[[nodiscard]] const char* font_rasterization_name(const FontRasterization value) noexcept {
    return value == FontRasterization::msdf ? "MSDF" : "GRAYSCALE";
}

[[nodiscard]] std::string color_string(const RenderColor& value) {
    constexpr std::array<char, 16U> hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
    };
    std::string result(8U, '0');
    const std::array<std::uint8_t, 4U> bytes{value.red, value.green, value.blue, value.alpha};
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        result[index * 2U] = hex[bytes[index] >> 4U];
        result[index * 2U + 1U] = hex[bytes[index] & 0x0FU];
    }
    return result;
}

[[nodiscard]] JsonValue point(const Point& value) {
    return object({{"x", JsonValue(value.x)}, {"y", JsonValue(value.y)}});
}

[[nodiscard]] JsonValue paint(const Paint& value) {
    if (const RenderColor* solid = value.color(); solid != nullptr) {
        return JsonValue(color_string(*solid));
    }
    const Gradient& gradient = *value.gradient();
    std::vector<JsonValue> stops;
    stops.reserve(gradient.stops.size());
    for (const GradientStop& stop : gradient.stops) {
        stops.push_back(object({
            {"color", JsonValue(color_string(stop.color))},
            {"offset", JsonValue(stop.offset)},
        }));
    }
    JsonValue::Object fields{
        {"extend", JsonValue(std::string(gradient_extend_name(gradient.extend)))},
        {"kind",
         JsonValue(std::string(gradient.kind == GradientKind::linear ? "linear" : "radial"))},
        {"stops", array(std::move(stops))},
    };
    if (gradient.kind == GradientKind::linear) {
        if (gradient.angle_degrees.has_value()) {
            fields.emplace_back("angle", JsonValue(*gradient.angle_degrees));
        } else {
            fields.emplace_back("from", point(gradient.start));
            fields.emplace_back("to", point(gradient.end));
        }
    } else {
        fields.emplace_back("center", point(gradient.center));
        fields.emplace_back("radiusX", JsonValue(gradient.radius_x));
        fields.emplace_back("radiusY", JsonValue(gradient.radius_y));
    }
    std::ranges::sort(fields, {}, &JsonValue::ObjectEntry::first);
    return JsonValue(std::move(fields));
}

[[nodiscard]] JsonValue border(const RenderBorder& value) {
    return object({
        {"color", JsonValue(color_string(value.color))},
        {"inside", JsonValue(value.inside)},
        {"width", JsonValue(value.width)},
    });
}

[[nodiscard]] JsonValue material_parameter(const runtime::Value& value) {
    if (value.image() != nullptr) {
        return object({{"texture", JsonValue(value.image()->id)}});
    }
    if (value.color() != nullptr) {
        return JsonValue(color_string(RenderColor{
            value.color()->red,
            value.color()->green,
            value.color()->blue,
            value.color()->alpha,
        }));
    }
    if (value.number() != nullptr)
        return JsonValue(*value.number());
    if (value.list() != nullptr) {
        std::vector<JsonValue> values;
        values.reserve(value.list()->values.size());
        for (const runtime::Value& element : value.list()->values) {
            if (element.number() == nullptr) {
                throw std::logic_error("material vector parameter contains a non-number");
            }
            values.emplace_back(*element.number());
        }
        return array(std::move(values));
    }
    throw std::logic_error("unsupported portable material parameter");
}

[[nodiscard]] JsonValue material(const MaterialState& value) {
    std::vector<JsonValue> parameters;
    parameters.reserve(value.parameters.size());
    for (const MaterialParameter& parameter : value.parameters) {
        parameters.push_back(object({
            {"name", JsonValue(parameter.name)},
            {"value", material_parameter(parameter.value)},
        }));
    }
    return object({
        {"blendMode", JsonValue(value.blend_mode)},
        {"id", JsonValue(value.id)},
        {"opacity", JsonValue(value.opacity)},
        {"parameters", array(std::move(parameters))},
    });
}

[[nodiscard]] JsonValue effect(const EffectState& value) {
    std::vector<JsonValue> parameters;
    parameters.reserve(value.parameters.size());
    for (const MaterialParameter& parameter : value.parameters) {
        parameters.push_back(object({
            {"name", JsonValue(parameter.name)},
            {"value", material_parameter(parameter.value)},
        }));
    }
    const std::string_view input = value.input == EffectInput::content ? "CONTENT"
                                   : value.input == EffectInput::shape ? "SHAPE"
                                                                       : "BACKDROP";
    return object({
        {"id", JsonValue(value.id)},
        {"input", JsonValue(std::string(input))},
        {"opacity", JsonValue(value.opacity)},
        {"refreshRate", JsonValue(value.refresh_rate)},
        {"parameters", array(std::move(parameters))},
    });
}

[[nodiscard]] JsonValue draw(const std::string& kind,
                             std::initializer_list<JsonValue::ObjectEntry> fields) {
    JsonValue::Object result;
    result.reserve(fields.size() + 2U);
    result.emplace_back("kind", JsonValue(kind));
    result.insert(result.end(), fields.begin(), fields.end());
    result.emplace_back("hitTest", JsonValue{});
    return JsonValue(std::move(result));
}

} // namespace

CornerRadii CornerRadii::all(const double value) noexcept {
    return CornerRadii{value, value, value, value};
}

void RenderCommandBuffer::append(RenderCommand command) {
    if (commands_ == nullptr) {
        commands_ = std::make_shared<std::vector<RenderCommand>>();
    } else if (commands_.use_count() != 1L) {
        commands_ = std::make_shared<std::vector<RenderCommand>>(*commands_);
    }
    commands_->push_back(std::move(command));
}

void RenderCommandBuffer::append(const std::vector<RenderCommand>& commands) {
    if (commands.empty())
        return;
    if (commands_ == nullptr) {
        commands_ = std::make_shared<std::vector<RenderCommand>>();
    } else if (commands_.use_count() != 1L) {
        commands_ = std::make_shared<std::vector<RenderCommand>>(*commands_);
    }
    commands_->insert(commands_->end(), commands.begin(), commands.end());
}

void RenderCommandBuffer::clear() noexcept {
    if (commands_ != nullptr && commands_.use_count() == 1L)
        commands_->clear();
    else
        commands_.reset();
}

const std::vector<RenderCommand>& RenderCommandBuffer::commands() const noexcept {
    static const std::vector<RenderCommand> empty;
    return commands_ != nullptr ? *commands_ : empty;
}

std::size_t RenderCommandBuffer::size() const noexcept {
    return commands_ != nullptr ? commands_->size() : 0U;
}

JsonValue render_command_json(const RenderCommand& command) {
    return std::visit(
        [](const auto& value) -> JsonValue {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<Type, SolidRectRenderCommand>) {
                return draw("solid_rect", {
                                              {"bounds", rectangle(value.bounds)},
                                              {"fill", paint(value.fill)},
                                          });
            } else if constexpr (std::is_same_v<Type, RoundedRectRenderCommand>) {
                return draw(
                    "rounded_rect",
                    {
                        {"border", value.border.has_value() ? border(*value.border) : JsonValue{}},
                        {"bounds", rectangle(value.bounds)},
                        {"fill", paint(value.fill)},
                        {"radii", radii(value.radii)},
                        {"softness", JsonValue(value.softness)},
                    });
            } else if constexpr (std::is_same_v<Type, BorderRenderCommand>) {
                return draw("border", {
                                          {"bounds", rectangle(value.bounds)},
                                          {"color", JsonValue(color_string(value.border.color))},
                                          {"inside", JsonValue(value.border.inside)},
                                          {"radii", radii(value.radii)},
                                          {"width", JsonValue(value.border.width)},
                                      });
            } else if constexpr (std::is_same_v<Type, ImageRenderCommand>) {
                return draw("image", {
                                         {"bounds", rectangle(value.bounds)},
                                         {"source", texture_region(value.source)},
                                         {"texture", JsonValue(value.texture)},
                                         {"tint", JsonValue(color_string(value.tint))},
                                     });
            } else if constexpr (std::is_same_v<Type, NinePatchRenderCommand>) {
                return draw("nine_patch",
                            {
                                {"bounds", rectangle(value.bounds)},
                                {"destinationInsets", edges(value.destination_insets)},
                                {"source", texture_region(value.source)},
                                {"sourceInsets", edges(value.source_insets)},
                                {"texture", JsonValue(value.texture)},
                                {"tint", JsonValue(color_string(value.tint))},
                            });
            } else if constexpr (std::is_same_v<Type, TextRunRenderCommand>) {
                std::vector<JsonValue> glyphs;
                glyphs.reserve(value.glyphs.size());
                for (const LogicalGlyph& glyph : value.glyphs) {
                    JsonValue::Object fields{
                        {"advance", JsonValue(glyph.advance)},
                        {"baseline", JsonValue(glyph.baseline)},
                        {"codePoint", JsonValue(static_cast<std::int64_t>(glyph.code_point))},
                        {"fontId", JsonValue(glyph.font_id)},
                        {"glyphId", JsonValue(static_cast<std::int64_t>(glyph.glyph_id))},
                        {"textEndOffset",
                         JsonValue(static_cast<std::int64_t>(glyph.text_end_offset))},
                        {"textStartOffset",
                         JsonValue(static_cast<std::int64_t>(glyph.text_start_offset))},
                        {"x", JsonValue(glyph.x)},
                        {"xPlacement", JsonValue(glyph.x_placement)},
                        {"yPlacement", JsonValue(glyph.y_placement)},
                    };
                    if (glyph.font_style_flags != 0U) {
                        fields.insert(
                            fields.begin() + 4,
                            JsonValue::ObjectEntry{
                                "fontStyleFlags",
                                JsonValue(static_cast<std::int64_t>(glyph.font_style_flags)),
                            });
                    }
                    if (glyph.y_advance != 0.0) {
                        fields.insert(
                            fields.end() - 1,
                            JsonValue::ObjectEntry{"yAdvance", JsonValue(glyph.y_advance)});
                    }
                    glyphs.emplace_back(std::move(fields));
                }
                return draw("text_run",
                            {
                                {"color", JsonValue(color_string(value.color))},
                                {"fontRasterization",
                                 JsonValue(font_rasterization_name(value.font_rasterization))},
                                {"glyphs", array(std::move(glyphs))},
                                {"origin", object({
                                               {"x", JsonValue(value.origin.x)},
                                               {"y", JsonValue(value.origin.y)},
                                           })},
                                {"pixelSize", JsonValue(value.pixel_size)},
                            });
            } else if constexpr (std::is_same_v<Type, CustomMeshRenderCommand>) {
                return draw(
                    "custom_mesh",
                    {
                        {"bounds", rectangle(value.bounds)},
                        {"indexCount",
                         JsonValue(static_cast<std::int64_t>(value.geometry.indices.size()))},
                        {"material",
                         value.material.has_value() ? material(*value.material) : JsonValue{}},
                        {"mesh", JsonValue(value.mesh)},
                        {"texture",
                         value.texture.has_value() ? JsonValue(*value.texture) : JsonValue{}},
                        {"vertexCount",
                         JsonValue(static_cast<std::int64_t>(value.geometry.vertices.size()))},
                    });
            } else if constexpr (std::is_same_v<Type, PathRenderCommand>) {
                JsonValue::Object fields{
                    {"bounds", rectangle(value.bounds)},
                    {"segmentCount",
                     JsonValue(static_cast<std::int64_t>(value.shape.path.segments().size()))},
                };
                if (value.shape.fill.has_value()) {
                    fields.emplace_back("fill", paint(*value.shape.fill));
                    fields.emplace_back("fillRule",
                                        JsonValue(value.shape.fill_rule == PathFillRule::evenodd
                                                      ? "evenodd"
                                                      : "nonzero"));
                }
                if (value.shape.stroke.has_value()) {
                    const StrokeStyle style = value.shape.stroke_style.value_or(StrokeStyle{});
                    fields.emplace_back("stroke", paint(*value.shape.stroke));
                    fields.emplace_back(
                        "strokeStyle",
                        object({
                            {"cap", JsonValue(std::string(path_cap_name(style.cap)))},
                            {"dashCount", JsonValue(static_cast<std::int64_t>(style.dash.size()))},
                            {"join", JsonValue(std::string(path_join_name(style.join)))},
                            {"width", JsonValue(style.width)},
                        }));
                }
                std::ranges::sort(fields, {}, &JsonValue::ObjectEntry::first);
                fields.insert(fields.begin(), JsonValue::ObjectEntry{"kind", JsonValue("path")});
                fields.emplace_back("hitTest", JsonValue{});
                return JsonValue(std::move(fields));
            } else if constexpr (std::is_same_v<Type, BlurRegionRenderCommand>) {
                return draw(
                    "blur_region",
                    {
                        {"bounds", rectangle(value.bounds)},
                        {"downsample", JsonValue(static_cast<std::int64_t>(value.downsample))},
                        {"radius", JsonValue(value.radius)},
                    });
            } else if constexpr (std::is_same_v<Type, ShadowRenderCommand>) {
                return draw("shadow", {
                                          {"bounds", rectangle(value.bounds)},
                                          {"color", JsonValue(color_string(value.color))},
                                          {"radii", radii(value.radii)},
                                          {"radius", JsonValue(value.radius)},
                                          {"spread", JsonValue(value.spread)},
                                      });
            } else if constexpr (std::is_same_v<Type, BackdropEffectRenderCommand>) {
                return draw("backdrop_effect", {
                                                   {"bounds", rectangle(value.bounds)},
                                                   {"effect", effect(value.effect)},
                                                   {"radii", radii(value.radii)},
                                               });
            } else if constexpr (std::is_same_v<Type, ContentEffectPushRenderCommand>) {
                return object({
                    {"bounds", rectangle(value.bounds)},
                    {"effect", effect(value.effect)},
                    {"kind", JsonValue("content_effect_push")},
                    {"radii", radii(value.radii)},
                });
            } else if constexpr (std::is_same_v<Type, ContentEffectPopRenderCommand>) {
                return object({{"kind", JsonValue("content_effect_pop")}});
            } else if constexpr (std::is_same_v<Type, ClipPushRenderCommand>) {
                return object({
                    {"kind", JsonValue("clip_push")},
                    {"radii", radii(value.radii)},
                    {"rect", rectangle(value.rect)},
                });
            } else if constexpr (std::is_same_v<Type, ClipPopRenderCommand>) {
                return object({{"kind", JsonValue("clip_pop")}});
            } else if constexpr (std::is_same_v<Type, TransformPushRenderCommand>) {
                return object({
                    {"kind", JsonValue("transform_push")},
                    {"transform", array({
                                      JsonValue(value.m00),
                                      JsonValue(value.m01),
                                      JsonValue(value.m02),
                                      JsonValue(value.m10),
                                      JsonValue(value.m11),
                                      JsonValue(value.m12),
                                  })},
                });
            } else if constexpr (std::is_same_v<Type, TransformPopRenderCommand>) {
                return object({{"kind", JsonValue("transform_pop")}});
            } else if constexpr (std::is_same_v<Type, MaterialPushRenderCommand>) {
                return object(
                    {{"kind", JsonValue("material_push")}, {"material", material(value.material)}});
            } else {
                static_assert(std::is_same_v<Type, MaterialPopRenderCommand>);
                return object({{"kind", JsonValue("material_pop")}});
            }
        },
        command);
}

JsonValue render_commands_json(const RenderCommandBuffer& commands) {
    std::vector<JsonValue> values;
    values.reserve(commands.size());
    for (const RenderCommand& command : commands.commands()) {
        values.push_back(render_command_json(command));
    }
    return array(std::move(values));
}

} // namespace strata::ui
