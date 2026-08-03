#include "ui/render/packet.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "data/json.hpp"
#include "runtime/value.hpp"
#include "ui/render.hpp"
#include "ui/render/packet_internal.hpp"

namespace strata::ui {
namespace {

using Bytes = std::vector<std::uint8_t>;

template <typename Integer>
    requires std::is_unsigned_v<Integer>
void integer(Bytes& output, const Integer value) {
    for (std::size_t byte = 0U; byte < sizeof(Integer); ++byte) {
        output.push_back(static_cast<std::uint8_t>(value >> (byte * 8U)));
    }
}

void number(Bytes& output, const double value) {
    integer(output, std::bit_cast<std::uint64_t>(value));
}

void text(Bytes& output, const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("render packet string exceeds the v1 length field");
    }
    integer(output, static_cast<std::uint32_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void boolean(Bytes& output, const bool value) {
    output.push_back(value ? std::uint8_t{1U} : std::uint8_t{0U});
}

void point(Bytes& output, const Point& value) {
    number(output, value.x);
    number(output, value.y);
}

void rect(Bytes& output, const Rect& value) {
    number(output, value.x);
    number(output, value.y);
    number(output, value.width);
    number(output, value.height);
}

void edges(Bytes& output, const Edges& value) {
    number(output, value.left);
    number(output, value.top);
    number(output, value.right);
    number(output, value.bottom);
}

void radii(Bytes& output, const CornerRadii& value) {
    number(output, value.top_left);
    number(output, value.top_right);
    number(output, value.bottom_right);
    number(output, value.bottom_left);
}

void color(Bytes& output, const RenderColor& value) {
    output.push_back(value.red);
    output.push_back(value.green);
    output.push_back(value.blue);
    output.push_back(value.alpha);
}

void paint(Bytes& output, const Paint& value) {
    if (const RenderColor* solid = value.color(); solid != nullptr) {
        integer(output, std::uint32_t{0U});
        color(output, *solid);
        return;
    }
    const Gradient& gradient = *value.gradient();
    integer(output, static_cast<std::uint32_t>(gradient.kind) + 1U);
    integer(output, static_cast<std::uint32_t>(gradient.extend));
    const auto [start, end] = gradient.axis(Size{1.0, 1.0});
    point(output, start);
    point(output, end);
    point(output, gradient.center);
    number(output, gradient.radius_x);
    number(output, gradient.radius_y);
    integer(output, static_cast<std::uint32_t>(gradient.stops.size()));
    for (const GradientStop& stop : gradient.stops) {
        number(output, stop.offset);
        color(output, stop.color);
    }
}

void border(Bytes& output, const RenderBorder& value) {
    number(output, value.width);
    color(output, value.color);
    boolean(output, value.inside);
}

void texture_region(Bytes& output, const TextureRegion& value) {
    number(output, value.u);
    number(output, value.v);
    number(output, value.width);
    number(output, value.height);
}

[[nodiscard]] std::uint32_t material_value_kind(const runtime::ValueKind kind) {
    switch (kind) {
    case runtime::ValueKind::null_value: return 0U;
    case runtime::ValueKind::boolean: return 1U;
    case runtime::ValueKind::number: return 2U;
    case runtime::ValueKind::duration: return 3U;
    case runtime::ValueKind::string: return 4U;
    case runtime::ValueKind::color: return 5U;
    case runtime::ValueKind::image: return 6U;
    case runtime::ValueKind::key: return 7U;
    case runtime::ValueKind::theme_token: return 8U;
    case runtime::ValueKind::list: return 9U;
    case runtime::ValueKind::object: return 10U;
    }
    throw std::logic_error("render packet material value has no stable kind");
}

void material(Bytes& output, const MaterialState& value) {
    text(output, value.id);
    text(output, value.blend_mode);
    number(output, value.opacity);
    if (value.parameters.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("render packet material parameter count exceeds v1");
    }
    integer(output, static_cast<std::uint32_t>(value.parameters.size()));
    for (const MaterialParameter& parameter : value.parameters) {
        text(output, parameter.name);
        integer(output, material_value_kind(parameter.value.kind()));
        text(output, data::encode_canonical_json(runtime::value_to_json(parameter.value)));
    }
}

void glyphs(Bytes& output, const TextRunRenderCommand& value) {
    std::vector<std::string_view> fonts;
    for (const LogicalGlyph& glyph : value.glyphs) {
        if (!std::ranges::contains(fonts, std::string_view(glyph.font_id))) {
            fonts.emplace_back(glyph.font_id);
        }
    }
    if (fonts.size() > std::numeric_limits<std::uint32_t>::max() ||
        value.glyphs.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("render packet text run exceeds v1 table limits");
    }
    integer(output, static_cast<std::uint32_t>(fonts.size()));
    for (const std::string_view font : fonts) text(output, font);
    integer(output, static_cast<std::uint32_t>(value.glyphs.size()));
    for (const LogicalGlyph& glyph : value.glyphs) {
        const auto font = std::ranges::find(fonts, std::string_view(glyph.font_id));
        integer(output, static_cast<std::uint32_t>(font - fonts.begin()));
        integer(output, glyph.glyph_id);
        integer(output, glyph.code_point);
        integer(output, static_cast<std::uint64_t>(glyph.text_start_offset));
        integer(output, static_cast<std::uint64_t>(glyph.text_end_offset));
        number(output, glyph.x);
        number(output, glyph.baseline);
        number(output, glyph.advance);
        number(output, glyph.x_placement);
        number(output, glyph.y_placement);
    }
}

void command_payload(Bytes& output, const RenderCommand& command) {
    std::visit([&output](const auto& value) {
        using Command = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<Command, SolidRectRenderCommand>) {
            rect(output, value.bounds);
            paint(output, value.fill);
        } else if constexpr (std::is_same_v<Command, RoundedRectRenderCommand>) {
            rect(output, value.bounds);
            radii(output, value.radii);
            paint(output, value.fill);
            boolean(output, value.border.has_value());
            if (value.border.has_value()) border(output, *value.border);
            number(output, value.softness);
        } else if constexpr (std::is_same_v<Command, BorderRenderCommand>) {
            rect(output, value.bounds);
            border(output, value.border);
            radii(output, value.radii);
        } else if constexpr (std::is_same_v<Command, ImageRenderCommand>) {
            rect(output, value.bounds);
            text(output, value.texture);
            texture_region(output, value.source);
            color(output, value.tint);
        } else if constexpr (std::is_same_v<Command, NinePatchRenderCommand>) {
            rect(output, value.bounds);
            text(output, value.texture);
            texture_region(output, value.source);
            edges(output, value.source_insets);
            edges(output, value.destination_insets);
            color(output, value.tint);
        } else if constexpr (std::is_same_v<Command, TextRunRenderCommand>) {
            point(output, value.origin);
            color(output, value.color);
            number(output, value.pixel_size);
            integer(output, static_cast<std::uint32_t>(value.font_rasterization));
            glyphs(output, value);
        } else if constexpr (std::is_same_v<Command, CustomMeshRenderCommand>) {
            rect(output, value.bounds);
            text(output, value.mesh);
            integer(output, static_cast<std::uint64_t>(value.geometry.vertices.size()));
            integer(output, static_cast<std::uint64_t>(value.geometry.indices.size()));
            boolean(output, value.texture.has_value());
            if (value.texture.has_value()) text(output, *value.texture);
            boolean(output, value.material.has_value());
            if (value.material.has_value()) material(output, *value.material);
        } else if constexpr (std::is_same_v<Command, PathRenderCommand>) {
            rect(output, value.bounds);
            integer(output, static_cast<std::uint32_t>(value.shape.path.segments().size()));
            for (const PathSegment& segment : value.shape.path.segments()) {
                integer(output, static_cast<std::uint32_t>(segment.verb));
                point(output, segment.control_a);
                point(output, segment.control_b);
                point(output, segment.to);
            }
            integer(output, static_cast<std::uint32_t>(value.shape.fill_rule));
            boolean(output, value.shape.fill.has_value());
            if (value.shape.fill.has_value()) paint(output, *value.shape.fill);
            boolean(output, value.shape.stroke.has_value());
            if (value.shape.stroke.has_value()) {
                const StrokeStyle style = value.shape.stroke_style.value_or(StrokeStyle{});
                paint(output, *value.shape.stroke);
                number(output, style.width);
                integer(output, static_cast<std::uint32_t>(style.cap));
                integer(output, static_cast<std::uint32_t>(style.join));
                number(output, style.miter_limit);
                number(output, style.dash_offset);
                integer(output, static_cast<std::uint32_t>(style.dash.size()));
                for (const double entry : style.dash) number(output, entry);
            }
        } else if constexpr (std::is_same_v<Command, BlurRegionRenderCommand>) {
            rect(output, value.bounds);
            number(output, value.radius);
            integer(output, static_cast<std::uint64_t>(value.downsample));
        } else if constexpr (std::is_same_v<Command, ShadowRenderCommand>) {
            rect(output, value.bounds);
            radii(output, value.radii);
            color(output, value.color);
            number(output, value.radius);
            number(output, value.spread);
        } else if constexpr (std::is_same_v<Command, ClipPushRenderCommand>) {
            rect(output, value.rect);
            radii(output, value.radii);
        } else if constexpr (std::is_same_v<Command, TransformPushRenderCommand>) {
            number(output, value.m00);
            number(output, value.m01);
            number(output, value.m02);
            number(output, value.m10);
            number(output, value.m11);
            number(output, value.m12);
        } else if constexpr (std::is_same_v<Command, MaterialPushRenderCommand>) {
            material(output, value.material);
        }
    }, command);
}

} // namespace

namespace detail {

std::vector<std::uint8_t> encode_command_payload_v2(const RenderCommand& command) {
    Bytes output;
    command_payload(output, command);
    return output;
}

} // namespace detail

std::vector<std::uint8_t> encode_render_packet(
    const RenderCommandBuffer& commands,
    const std::uint64_t frame_index
) {
    if (commands.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("render command count exceeds the packet-v3 limit");
    }
    Bytes output;
    constexpr std::string_view magic = "STRATARP";
    output.insert(output.end(), magic.begin(), magic.end());
    integer(output, std::uint32_t{3U});
    integer(output, static_cast<std::uint32_t>(commands.size()));
    integer(output, frame_index);
    for (const RenderCommand& command : commands.commands()) {
        Bytes payload = detail::encode_command_payload_v2(command);
        if (payload.size() > std::numeric_limits<std::uint32_t>::max()) {
            throw std::length_error("render command payload exceeds the packet-v3 limit");
        }
        integer(output, static_cast<std::uint32_t>(command.index()));
        integer(output, static_cast<std::uint32_t>(payload.size()));
        output.insert(output.end(), payload.begin(), payload.end());
    }
    return output;
}

} // namespace strata::ui
