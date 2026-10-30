#include "ui/render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string_view>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(const std::initializer_list<data::JsonEntry> entries) {
    return data::json_object(entries);
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
        {"kind", JsonValue(std::string(gradient_kind_name(gradient.kind)))},
        {"stops", array(std::move(stops))},
    };
    if (gradient.rotation_degrees != 0.0)
        fields.emplace_back("rotation", JsonValue(gradient.rotation_degrees));
    if (gradient.kind == GradientKind::conic) {
        fields.emplace_back("angle", JsonValue(gradient.angle_degrees.value_or(0.0)));
        fields.emplace_back("center", point(gradient.center));
    } else if (gradient.kind == GradientKind::linear) {
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

/** A border's paint: `color` for a solid one, as it always was, or `paint` for a gradient. */
[[nodiscard]] data::JsonEntry border_paint(const RenderBorder& value) {
    if (const RenderColor* solid = value.paint.color(); solid != nullptr)
        return {"color", JsonValue(color_string(*solid))};
    return {"paint", paint(value.paint)};
}

[[nodiscard]] JsonValue border(const RenderBorder& value) {
    return object({
        border_paint(value),
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
    const std::string_view backdrop_source =
        value.backdrop_source == EffectBackdropSource::surface ? "SURFACE" : "CURRENT";
    JsonValue::Object result;
    data::append_json_entries(result,
                              {
                                  {"backdropSource", JsonValue(std::string(backdrop_source))},
                                  {"id", JsonValue(value.id)},
                                  {"input", JsonValue(std::string(input))},
                                  {"opacity", JsonValue(value.opacity)},
                                  {"refreshRate", JsonValue(value.refresh_rate)},
                                  {"parameters", array(std::move(parameters))},
                              });
    // A layer's built-in processing appears only when it has any, so plain effects read as before.
    if (value.composite != LayerComposite::source_over || !value.filters.empty()) {
        std::vector<JsonValue> filters;
        filters.reserve(value.filters.size());
        for (const LayerFilter& filter : value.filters) {
            std::vector<JsonValue> values;
            values.reserve(filter.values.size());
            for (const double entry : filter.values)
                values.emplace_back(entry);
            filters.push_back(object({
                {"kind", JsonValue(static_cast<std::int64_t>(filter.kind))},
                {"values", array(std::move(values))},
            }));
        }
        result.emplace_back("composite", JsonValue(static_cast<std::int64_t>(value.composite)));
        result.emplace_back("filters", array(std::move(filters)));
    }
    return JsonValue(std::move(result));
}

[[nodiscard]] JsonValue draw(const std::string& kind,
                             const std::initializer_list<data::JsonEntry> fields) {
    JsonValue::Object result;
    result.reserve(fields.size() + 2U);
    result.emplace_back("kind", JsonValue(kind));
    data::append_json_entries(result, fields);
    result.emplace_back("hitTest", JsonValue{});
    return JsonValue(std::move(result));
}

} // namespace

CornerRadii CornerRadii::all(const double value) noexcept {
    return CornerRadii{value, value, value, value};
}

double CornerRadii::largest() const noexcept {
    return std::max({top_left, top_right, bottom_right, bottom_left});
}

std::optional<CornerRadii> corner_radii_value(const runtime::Value* value) noexcept {
    const auto length = [](const runtime::Value* candidate) -> std::optional<double> {
        if (candidate == nullptr || candidate->number() == nullptr)
            return std::nullopt;
        const double number = *candidate->number();
        return std::isfinite(number) ? std::max(0.0, number) : 0.0;
    };
    if (value == nullptr)
        return std::nullopt;
    if (const std::optional<double> all = length(value); all.has_value())
        return CornerRadii::all(*all);
    if (value->object() == nullptr)
        return std::nullopt;
    const auto corner = [&](const std::string_view name, const std::string_view vertical,
                            const std::string_view horizontal) {
        for (const std::string_view field : {name, vertical, horizontal, std::string_view("all")}) {
            if (const std::optional<double> resolved = length(value->field(field));
                resolved.has_value()) {
                return *resolved;
            }
        }
        return 0.0;
    };
    return CornerRadii{
        corner("topLeft", "top", "left"),
        corner("topRight", "top", "right"),
        corner("bottomRight", "bottom", "right"),
        corner("bottomLeft", "bottom", "left"),
    };
}

const RenderCommand& RenderCommandView::locate(const std::size_t index,
                                               std::size_t& run) const noexcept {
    const auto holds = [this, index](const std::size_t at) {
        return at < segments_.size() && index >= offsets_[at] &&
               index < offsets_[at] + segments_[at].size();
    };
    if (!holds(run)) {
        if (holds(run + 1U)) {
            ++run;
        } else {
            run = static_cast<std::size_t>(
                std::upper_bound(offsets_.begin(), offsets_.end(), index) - offsets_.begin() - 1);
        }
    }
    const Segment& segment = segments_[run];
    return (*segment.storage)[segment.begin + (index - offsets_[run])];
}

void RenderCommandView::push(Segment segment) {
    const std::size_t added = segment.size();
    if (added == 0U)
        return;
    // Continuing the last run where it ends extends it.
    if (!segments_.empty() && segments_.back().storage == segment.storage &&
        segments_.back().end == segment.begin) {
        segments_.back().end = segment.end;
    } else {
        offsets_.push_back(size_);
        segments_.push_back(std::move(segment));
    }
    size_ += added;
}

std::vector<RenderCommandView::Segment> RenderCommandView::slice(const std::size_t begin,
                                                                 const std::size_t end) const {
    std::vector<Segment> result;
    if (begin >= end)
        return result;
    std::size_t run = static_cast<std::size_t>(
        std::upper_bound(offsets_.begin(), offsets_.end(), begin) - offsets_.begin() - 1);
    for (; run < segments_.size() && offsets_[run] < end; ++run) {
        const Segment& segment = segments_[run];
        const std::size_t from = std::max(begin, offsets_[run]) - offsets_[run];
        const std::size_t to = std::min(end, offsets_[run] + segment.size()) - offsets_[run];
        if (from < to)
            result.push_back(Segment{segment.storage, segment.begin + from, segment.begin + to});
    }
    return result;
}

std::vector<RenderCommand> RenderCommandView::to_vector() const {
    std::vector<RenderCommand> result;
    result.reserve(size_);
    for (const Segment& segment : segments_) {
        result.insert(result.end(),
                      segment.storage->begin() + static_cast<std::ptrdiff_t>(segment.begin),
                      segment.storage->begin() + static_cast<std::ptrdiff_t>(segment.end));
    }
    return result;
}

namespace {

/** Walks two streams of one length run by run, calling `visit(index, run length, same)`. */
template <typename Visit>
void walk_runs(const RenderCommandView& left, const RenderCommandView& right, Visit&& visit) {
    const auto& a = left.segments();
    const auto& b = right.segments();
    std::size_t index = 0U;
    std::size_t sa = 0U;
    std::size_t sb = 0U;
    std::size_t ia = 0U;
    std::size_t ib = 0U;
    while (sa < a.size() && sb < b.size()) {
        const std::size_t run = std::min(a[sa].size() - ia, b[sb].size() - ib);
        const bool same = a[sa].storage == b[sb].storage && a[sa].begin + ia == b[sb].begin + ib;
        if (!visit(index, run, same, a[sa].begin + ia, *a[sa].storage, b[sb].begin + ib,
                   *b[sb].storage))
            return;
        index += run;
        ia += run;
        ib += run;
        if (ia == a[sa].size()) {
            ++sa;
            ia = 0U;
        }
        if (ib == b[sb].size()) {
            ++sb;
            ib = 0U;
        }
    }
}

} // namespace

std::vector<std::size_t> RenderCommandView::differences(const RenderCommandView& other) const {
    std::vector<std::size_t> result;
    if (size_ != other.size_)
        return result;
    walk_runs(*this, other, [&](const std::size_t index, const std::size_t run, const bool same,
                                const std::size_t from_a, const std::vector<RenderCommand>& a,
                                const std::size_t from_b, const std::vector<RenderCommand>& b) {
        if (same)
            return true;
        for (std::size_t offset = 0U; offset < run; ++offset) {
            if (!(a[from_a + offset] == b[from_b + offset]))
                result.push_back(index + offset);
        }
        return true;
    });
    return result;
}

bool operator==(const RenderCommandView& left, const RenderCommandView& right) {
    if (left.size_ != right.size_)
        return false;
    bool equal = true;
    walk_runs(left, right, [&](const std::size_t, const std::size_t run, const bool same,
                               const std::size_t from_a, const std::vector<RenderCommand>& a,
                               const std::size_t from_b, const std::vector<RenderCommand>& b) {
        if (same)
            return true;
        for (std::size_t offset = 0U; offset < run; ++offset) {
            if (!(a[from_a + offset] == b[from_b + offset])) {
                equal = false;
                return false;
            }
        }
        return true;
    });
    return equal;
}

bool operator==(const RenderCommandView& left, const std::vector<RenderCommand>& right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0U; index < right.size(); ++index) {
        if (!(left[index] == right[index]))
            return false;
    }
    return true;
}

RenderCommandBuffer::RenderCommandBuffer(const RenderCommandBuffer& other)
    : view_(other.view_), groups_(other.groups_), reserve_hint_(other.size()) {}

RenderCommandBuffer& RenderCommandBuffer::operator=(const RenderCommandBuffer& other) {
    if (this == &other)
        return *this;
    view_ = other.view_;
    tail_.reset();
    tail_open_ = false;
    groups_ = other.groups_;
    reserve_hint_ = other.size();
    return *this;
}

void RenderCommandBuffer::open_tail() {
    if (tail_ == nullptr) {
        // A frame's stream is usually as long as the last one, so start at that size rather than
        // regrowing a vector of large variants from empty.
        tail_ = std::make_shared<std::vector<RenderCommand>>();
        tail_->reserve(reserve_hint_);
    }
    if (!tail_open_) {
        view_.offsets_.push_back(view_.size_);
        view_.segments_.push_back(
            RenderCommandView::Segment{tail_, tail_->size(), tail_->size()});
        tail_open_ = true;
    }
}

void RenderCommandBuffer::append(RenderCommand command) {
    open_tail();
    tail_->push_back(std::move(command));
    ++view_.segments_.back().end;
    ++view_.size_;
}

void RenderCommandBuffer::append(const std::vector<RenderCommand>& commands) {
    if (commands.empty())
        return;
    open_tail();
    tail_->insert(tail_->end(), commands.begin(), commands.end());
    view_.segments_.back().end += commands.size();
    view_.size_ += commands.size();
}

void RenderCommandBuffer::append(const std::span<const RenderCommandView::Segment> segments) {
    for (const RenderCommandView::Segment& segment : segments) {
        if (segment.size() == 0U)
            continue;
        tail_open_ = false;
        view_.push(segment);
    }
}

void RenderCommandBuffer::clear() noexcept {
    view_ = RenderCommandView{};
    tail_open_ = false;
    if (tail_ != nullptr && tail_.use_count() == 1L) {
        tail_->clear();
    } else {
        // Read by a retained frame or subtree, so the next stream needs storage of its own.
        reserve_hint_ = tail_ != nullptr ? tail_->size() : reserve_hint_;
        tail_.reset();
    }
    groups_.reset();
}

void RenderCommandBuffer::set_groups(std::vector<RenderGroup> groups) {
    groups_ = groups.empty() ? nullptr
                             : std::make_shared<const std::vector<RenderGroup>>(std::move(groups));
}

const std::vector<RenderGroup>& RenderCommandBuffer::groups() const noexcept {
    static const std::vector<RenderGroup> empty;
    return groups_ != nullptr ? *groups_ : empty;
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
                                          border_paint(value.border),
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
                        {"fontId", JsonValue(glyph.font_id.str())},
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
                if (!value.joined.empty()) {
                    fields.emplace_back(
                        "joined", JsonValue(static_cast<std::int64_t>(value.joined.size())));
                }
                if (value.art.has_value()) {
                    fields.emplace_back(
                        "art",
                        object({
                            {"closed", JsonValue(value.art->closed)},
                            {"count", JsonValue(static_cast<std::int64_t>(value.art->count))},
                            {"segments", JsonValue(value.art->segments)},
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
                                          {"offset", point(value.offset)},
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
            } else if constexpr (std::is_same_v<Type, MaterialPopRenderCommand>) {
                return object({{"kind", JsonValue("material_pop")}});
            } else if constexpr (std::is_same_v<Type, OpacityPushRenderCommand>) {
                return object(
                    {{"kind", JsonValue("opacity_push")}, {"opacity", JsonValue(value.opacity)}});
            } else if constexpr (std::is_same_v<Type, OpacityPopRenderCommand>) {
                return object({{"kind", JsonValue("opacity_pop")}});
            } else if constexpr (std::is_same_v<Type, GroupPushRenderCommand>) {
                return object({{"kind", JsonValue("group_push")},
                               {"group", JsonValue(static_cast<std::int64_t>(value.group))}});
            } else {
                static_assert(std::is_same_v<Type, GroupPopRenderCommand>);
                return object({{"kind", JsonValue("group_pop")}});
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
