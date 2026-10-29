#include "resource/lottie.hpp"

#include "data/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <utility>

namespace strata::resource::lottie {
namespace {

using data::JsonValue;

/** Samples a spatial segment's arc is measured at; its speed is even to within their spacing. */
constexpr std::size_t arc_samples = 48U;
/** The deepest precomposition nesting and the most shape-group nesting a document may have. */
constexpr std::size_t maximum_nesting = 64U;

[[nodiscard]] std::optional<double> number(const JsonValue* const value) noexcept {
    if (value == nullptr)
        return std::nullopt;
    if (const double* real = value->number(); real != nullptr && std::isfinite(*real))
        return *real;
    if (const std::int64_t* integer = value->integer(); integer != nullptr)
        return static_cast<double>(*integer);
    if (const bool* flag = value->boolean(); flag != nullptr)
        return *flag ? 1.0 : 0.0;
    return std::nullopt;
}

[[nodiscard]] double number_or(const JsonValue& object, const std::string_view key,
                               const double fallback) noexcept {
    return number(object.find(key)).value_or(fallback);
}

[[nodiscard]] bool flag(const JsonValue& object, const std::string_view key) noexcept {
    const JsonValue* value = object.find(key);
    if (value == nullptr)
        return false;
    if (const bool* boolean = value->boolean(); boolean != nullptr)
        return *boolean;
    return number(value).value_or(0.0) != 0.0;
}

[[nodiscard]] const std::string* text(const JsonValue& object,
                                      const std::string_view key) noexcept {
    const JsonValue* value = object.find(key);
    return value != nullptr ? value->string() : nullptr;
}

/** A number or an array of numbers, as a list. */
[[nodiscard]] std::vector<double> numbers(const JsonValue* const value) {
    std::vector<double> result;
    if (value == nullptr)
        return result;
    if (const std::optional<double> single = number(value); single.has_value()) {
        result.push_back(*single);
        return result;
    }
    if (const JsonValue::Array* array = value->array(); array != nullptr) {
        result.reserve(array->size());
        for (const JsonValue& element : *array) {
            const std::optional<double> component = number(&element);
            if (!component.has_value())
                return {};
            result.push_back(*component);
        }
    }
    return result;
}

[[nodiscard]] EaseHandle ease_handle(const JsonValue* const value) {
    EaseHandle result;
    if (value == nullptr || value->object() == nullptr)
        return result;
    result.x = numbers(value->find("x"));
    result.y = numbers(value->find("y"));
    if (result.x.empty() || result.y.empty())
        return EaseHandle{};
    return result;
}

/** The keyframe objects of an animated property value, or null when it is not one. */
[[nodiscard]] const JsonValue::Array* keyframe_array(const JsonValue& k) noexcept {
    const JsonValue::Array* array = k.array();
    if (array == nullptr || array->empty())
        return nullptr;
    const JsonValue& first = array->front();
    if (first.object() == nullptr || first.find("t") == nullptr)
        return nullptr;
    return array;
}

[[nodiscard]] Vec2 cubic_point(const Vec2 a, const Vec2 b, const Vec2 c, const Vec2 d,
                               const double t) noexcept {
    const double u = 1.0 - t;
    const double w0 = u * u * u;
    const double w1 = 3.0 * u * u * t;
    const double w2 = 3.0 * u * t * t;
    const double w3 = t * t * t;
    return Vec2{w0 * a.x + w1 * b.x + w2 * c.x + w3 * d.x,
                w0 * a.y + w1 * b.y + w2 * c.y + w3 * d.y};
}

void measure_spatial(Keyframe& keyframe) {
    const Vec2 from{keyframe.start[0], keyframe.start[1]};
    const Vec2 to{keyframe.end[0], keyframe.end[1]};
    const Vec2 first{from.x + keyframe.spatial_out.x, from.y + keyframe.spatial_out.y};
    const Vec2 second{to.x + keyframe.spatial_in.x, to.y + keyframe.spatial_in.y};
    keyframe.arc_lengths.assign(arc_samples + 1U, 0.0);
    Vec2 previous = from;
    double length = 0.0;
    for (std::size_t sample = 1U; sample <= arc_samples; ++sample) {
        const Vec2 point =
            cubic_point(from, first, second, to, static_cast<double>(sample) / arc_samples);
        length += std::hypot(point.x - previous.x, point.y - previous.y);
        keyframe.arc_lengths[sample] = length;
        previous = point;
    }
    if (length <= 1.0e-9)
        keyframe.spatial = false;
}

/** Reads a property object (`{ a, k }`); a missing one is `fallback`, constant. */
[[nodiscard]] Property property(const JsonValue* const value, std::vector<double> fallback) {
    if (value == nullptr || value->object() == nullptr)
        return Property(std::move(fallback));
    const JsonValue* k = value->find("k");
    if (k == nullptr)
        return Property(std::move(fallback));
    const JsonValue::Array* frames = keyframe_array(*k);
    if (frames == nullptr) {
        std::vector<double> constant = numbers(k);
        if (constant.empty())
            return Property(std::move(fallback));
        return Property(std::move(constant));
    }
    Property result(std::move(fallback));
    result.keyframes_.reserve(frames->size());
    for (const JsonValue& frame : *frames) {
        if (frame.object() == nullptr)
            throw std::invalid_argument("a keyframe must be an object");
        Keyframe keyframe;
        keyframe.time = number_or(frame, "t", 0.0);
        keyframe.start = numbers(frame.find("s"));
        keyframe.end = numbers(frame.find("e"));
        keyframe.hold = flag(frame, "h");
        keyframe.out = ease_handle(frame.find("o"));
        keyframe.in = ease_handle(frame.find("i"));
        const std::vector<double> spatial_out = numbers(frame.find("to"));
        const std::vector<double> spatial_in = numbers(frame.find("ti"));
        if (spatial_out.size() >= 2U && spatial_in.size() >= 2U) {
            keyframe.spatial_out = Vec2{spatial_out[0], spatial_out[1]};
            keyframe.spatial_in = Vec2{spatial_in[0], spatial_in[1]};
            keyframe.spatial = keyframe.spatial_out != Vec2{} || keyframe.spatial_in != Vec2{};
        }
        result.keyframes_.push_back(std::move(keyframe));
    }
    std::ranges::stable_sort(result.keyframes_, {}, &Keyframe::time);
    // Older exports carry each segment's end (`e`); newer ones leave it to the next start. A
    // final keyframe may hold only its time.
    for (std::size_t index = 0U; index < result.keyframes_.size(); ++index) {
        Keyframe& keyframe = result.keyframes_[index];
        if (keyframe.start.empty() && index > 0U) {
            const Keyframe& previous = result.keyframes_[index - 1U];
            keyframe.start = previous.end.empty() ? previous.start : previous.end;
        }
    }
    for (std::size_t index = 0U; index < result.keyframes_.size(); ++index) {
        Keyframe& keyframe = result.keyframes_[index];
        if (keyframe.end.empty())
            keyframe.end = index + 1U < result.keyframes_.size()
                               ? result.keyframes_[index + 1U].start
                               : keyframe.start;
    }
    std::erase_if(result.keyframes_,
                  [](const Keyframe& keyframe) { return keyframe.start.empty(); });
    if (result.keyframes_.empty())
        return result;
    for (Keyframe& keyframe : result.keyframes_) {
        if (keyframe.end.size() != keyframe.start.size())
            keyframe.end = keyframe.start;
        if (keyframe.spatial && keyframe.start.size() >= 2U)
            measure_spatial(keyframe);
        else
            keyframe.spatial = false;
    }
    // The dimension is what the keyframes carry; a constant fallback of another size would
    // misreport it.
    result.constant_ = result.keyframes_.front().start;
    if (result.keyframes_.size() == 1U) {
        result.constant_ = result.keyframes_.front().start;
        result.keyframes_.clear();
    }
    return result;
}

[[nodiscard]] Vec2 vec2(const JsonValue& value) {
    const std::vector<double> components = numbers(&value);
    if (components.size() < 2U)
        throw std::invalid_argument("a shape vertex must have two coordinates");
    return Vec2{components[0], components[1]};
}

[[nodiscard]] BezierShape bezier(const JsonValue& value) {
    const JsonValue* shape = &value;
    // Keyframed shapes wrap each outline in a one-element array.
    if (const JsonValue::Array* wrapper = value.array(); wrapper != nullptr) {
        if (wrapper->empty())
            return {};
        shape = &wrapper->front();
    }
    if (shape->object() == nullptr)
        throw std::invalid_argument("a shape outline must be an object");
    BezierShape result;
    result.closed = flag(*shape, "c");
    const auto points = [&](const std::string_view key) {
        std::vector<Vec2> list;
        if (const JsonValue* field = shape->find(key);
            field != nullptr && field->array() != nullptr) {
            list.reserve(field->array()->size());
            for (const JsonValue& point : *field->array())
                list.push_back(vec2(point));
        }
        return list;
    };
    result.vertices = points("v");
    result.in_tangents = points("i");
    result.out_tangents = points("o");
    result.in_tangents.resize(result.vertices.size());
    result.out_tangents.resize(result.vertices.size());
    return result;
}

[[nodiscard]] ShapeProperty shape_property(const JsonValue* const value) {
    ShapeProperty result;
    if (value == nullptr || value->object() == nullptr)
        return result;
    const JsonValue* k = value->find("k");
    if (k == nullptr)
        return result;
    const JsonValue::Array* frames = keyframe_array(*k);
    if (frames == nullptr) {
        result.constant_ = bezier(*k);
        return result;
    }
    for (const JsonValue& frame : *frames) {
        ShapeKeyframe keyframe;
        keyframe.time = number_or(frame, "t", 0.0);
        keyframe.hold = flag(frame, "h");
        keyframe.out = ease_handle(frame.find("o"));
        keyframe.in = ease_handle(frame.find("i"));
        const JsonValue* start = frame.find("s");
        const JsonValue* end = frame.find("e");
        bool has_start = start != nullptr;
        if (has_start)
            keyframe.start = bezier(*start);
        if (end != nullptr)
            keyframe.end = bezier(*end);
        else
            keyframe.end.vertices.clear();
        if (!has_start && !result.keyframes_.empty()) {
            const ShapeKeyframe& previous = result.keyframes_.back();
            keyframe.start = previous.end.vertices.empty() ? previous.start : previous.end;
        }
        result.keyframes_.push_back(std::move(keyframe));
    }
    std::ranges::stable_sort(result.keyframes_, {}, &ShapeKeyframe::time);
    for (std::size_t index = 0U; index < result.keyframes_.size(); ++index) {
        ShapeKeyframe& keyframe = result.keyframes_[index];
        if (keyframe.end.vertices.empty())
            keyframe.end = index + 1U < result.keyframes_.size()
                               ? result.keyframes_[index + 1U].start
                               : keyframe.start;
    }
    if (!result.keyframes_.empty())
        result.constant_ = result.keyframes_.front().start;
    if (result.keyframes_.size() == 1U)
        result.keyframes_.clear();
    return result;
}

/** Colours exported as 0-255 channels (old Bodymovin) become 0-1 like the rest. */
void normalize_color(Property& color) {
    double largest = 0.0;
    for (const double channel : color.constant_)
        largest = std::max(largest, channel);
    for (const Keyframe& keyframe : color.keyframes_) {
        for (const double channel : keyframe.start)
            largest = std::max(largest, channel);
    }
    if (largest <= 1.0 + 1.0e-6)
        return;
    for (double& channel : color.constant_)
        channel /= 255.0;
    for (Keyframe& keyframe : color.keyframes_) {
        for (double& channel : keyframe.start)
            channel /= 255.0;
        for (double& channel : keyframe.end)
            channel /= 255.0;
    }
}

[[nodiscard]] Transform transform(const JsonValue* const value) {
    Transform result;
    if (value == nullptr || value->object() == nullptr)
        return result;
    result.anchor = property(value->find("a"), {0.0, 0.0});
    if (const JsonValue* position = value->find("p"); position != nullptr) {
        if (flag(*position, "s")) {
            result.split_position = true;
            result.position_x = property(position->find("x"), {0.0});
            result.position_y = property(position->find("y"), {0.0});
        } else {
            result.position = property(position, {0.0, 0.0});
        }
    }
    result.scale = property(value->find("s"), {100.0, 100.0});
    if (value->find("r") != nullptr)
        result.rotation = property(value->find("r"), {0.0});
    else
        result.rotation = property(value->find("rz"), {0.0});
    result.opacity = property(value->find("o"), {100.0});
    result.skew = property(value->find("sk"), {0.0});
    result.skew_axis = property(value->find("sa"), {0.0});
    result.start_opacity = property(value->find("so"), {100.0});
    result.end_opacity = property(value->find("eo"), {100.0});
    return result;
}

[[nodiscard]] StrokeStyle stroke_style(const JsonValue& item) {
    StrokeStyle style;
    style.width = property(item.find("w"), {1.0});
    const int cap = static_cast<int>(number_or(item, "lc", 1.0));
    style.cap = cap == 2 ? LineCap::round : cap == 3 ? LineCap::square : LineCap::butt;
    const int join = static_cast<int>(number_or(item, "lj", 1.0));
    style.join = join == 2 ? LineJoin::round : join == 3 ? LineJoin::bevel : LineJoin::miter;
    style.miter_limit = std::max(1.0, number_or(item, "ml", 4.0));
    if (const JsonValue* dashes = item.find("d"); dashes != nullptr && dashes->array() != nullptr) {
        for (const JsonValue& entry : *dashes->array()) {
            if (entry.object() == nullptr)
                continue;
            const std::string* name = text(entry, "n");
            Dash dash;
            dash.kind = name != nullptr && *name == "g"   ? Dash::Kind::gap
                        : name != nullptr && *name == "o" ? Dash::Kind::offset
                                                          : Dash::Kind::dash;
            dash.length = property(entry.find("v"), {0.0});
            style.dashes.push_back(std::move(dash));
        }
    }
    return style;
}

[[nodiscard]] int direction(const JsonValue& item) noexcept {
    return static_cast<int>(number_or(item, "d", 1.0));
}

void shape_items(const JsonValue* list, std::vector<ShapeItem>& output, std::size_t depth);

[[nodiscard]] std::optional<ShapeItem> shape_item(const JsonValue& item, const std::size_t depth) {
    if (item.object() == nullptr || flag(item, "hd"))
        return std::nullopt;
    const std::string* type = text(item, "ty");
    if (type == nullptr)
        return std::nullopt;
    if (*type == "gr") {
        GroupItem group;
        if (depth >= maximum_nesting)
            throw std::invalid_argument("shape groups nest too deeply");
        shape_items(item.find("it"), group.items, depth + 1U);
        if (const JsonValue* items = item.find("it");
            items != nullptr && items->array() != nullptr) {
            for (const JsonValue& child : *items->array()) {
                const std::string* child_type =
                    child.object() != nullptr ? text(child, "ty") : nullptr;
                if (child_type != nullptr && *child_type == "tr")
                    group.transform = transform(&child);
            }
        }
        return ShapeItem{std::move(group)};
    }
    if (*type == "sh")
        return ShapeItem{PathItem{shape_property(item.find("ks")), direction(item)}};
    if (*type == "rc") {
        return ShapeItem{RectItem{property(item.find("p"), {0.0, 0.0}),
                                  property(item.find("s"), {0.0, 0.0}),
                                  property(item.find("r"), {0.0}), direction(item)}};
    }
    if (*type == "el") {
        return ShapeItem{EllipseItem{property(item.find("p"), {0.0, 0.0}),
                                     property(item.find("s"), {0.0, 0.0}), direction(item)}};
    }
    if (*type == "sr") {
        PolystarItem star;
        star.star = number_or(item, "sy", 1.0) != 2.0;
        star.position = property(item.find("p"), {0.0, 0.0});
        star.points = property(item.find("pt"), {5.0});
        star.rotation = property(item.find("r"), {0.0});
        star.outer_radius = property(item.find("or"), {0.0});
        star.inner_radius = property(item.find("ir"), {0.0});
        star.outer_roundness = property(item.find("os"), {0.0});
        star.inner_roundness = property(item.find("is"), {0.0});
        star.direction = direction(item);
        return ShapeItem{std::move(star)};
    }
    if (*type == "fl" || *type == "st") {
        SolidPaintItem paint;
        paint.stroke = *type == "st";
        paint.color = property(item.find("c"), {0.0, 0.0, 0.0, 1.0});
        normalize_color(paint.color);
        paint.opacity = property(item.find("o"), {100.0});
        paint.rule = number_or(item, "r", 1.0) == 2.0 ? FillRule::even_odd : FillRule::nonzero;
        if (paint.stroke)
            paint.style = stroke_style(item);
        return ShapeItem{std::move(paint)};
    }
    if (*type == "gf" || *type == "gs") {
        GradientPaintItem paint;
        paint.stroke = *type == "gs";
        paint.radial = number_or(item, "t", 1.0) == 2.0;
        paint.start = property(item.find("s"), {0.0, 0.0});
        paint.end = property(item.find("e"), {0.0, 0.0});
        paint.opacity = property(item.find("o"), {100.0});
        paint.rule = number_or(item, "r", 1.0) == 2.0 ? FillRule::even_odd : FillRule::nonzero;
        if (const JsonValue* gradient = item.find("g"); gradient != nullptr && gradient->object()) {
            paint.color_stops =
                static_cast<std::size_t>(std::max(0.0, number_or(*gradient, "p", 0.0)));
            paint.colors = property(gradient->find("k"), {});
        }
        if (paint.color_stops == 0U)
            return std::nullopt;
        if (paint.stroke)
            paint.style = stroke_style(item);
        return ShapeItem{std::move(paint)};
    }
    if (*type == "tm") {
        TrimItem trim;
        trim.start = property(item.find("s"), {0.0});
        trim.end = property(item.find("e"), {100.0});
        trim.offset = property(item.find("o"), {0.0});
        trim.individually = number_or(item, "m", 1.0) == 2.0;
        return ShapeItem{std::move(trim)};
    }
    if (*type == "rp") {
        RepeaterItem repeater;
        repeater.copies = property(item.find("c"), {1.0});
        repeater.offset = property(item.find("o"), {0.0});
        repeater.above = number_or(item, "m", 1.0) != 2.0;
        repeater.transform = transform(item.find("tr"));
        return ShapeItem{std::move(repeater)};
    }
    if (*type == "rd")
        return ShapeItem{RoundCornersItem{property(item.find("r"), {0.0})}};
    if (*type == "mm")
        return ShapeItem{MergeItem{static_cast<int>(number_or(item, "mm", 1.0))}};
    return std::nullopt;
}

void shape_items(const JsonValue* const list, std::vector<ShapeItem>& output,
                 const std::size_t depth) {
    if (list == nullptr || list->array() == nullptr)
        return;
    output.reserve(list->array()->size());
    for (const JsonValue& item : *list->array()) {
        if (std::optional<ShapeItem> parsed = shape_item(item, depth); parsed.has_value())
            output.push_back(std::move(*parsed));
    }
}

[[nodiscard]] std::array<double, 3> hex_color(const std::string* const value) noexcept {
    std::array<double, 3> result{};
    if (value == nullptr || value->size() < 7U || (*value)[0] != '#')
        return result;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const std::string digits = value->substr(1U + channel * 2U, 2U);
        result[channel] = static_cast<double>(std::strtoul(digits.c_str(), nullptr, 16)) / 255.0;
    }
    return result;
}

[[nodiscard]] LayerType layer_type(const int type) noexcept {
    switch (type) {
    case 0:
        return LayerType::precomp;
    case 1:
        return LayerType::solid;
    case 2:
        return LayerType::image;
    case 3:
        return LayerType::null;
    case 4:
        return LayerType::shape;
    case 5:
        return LayerType::text;
    default:
        return LayerType::other;
    }
}

[[nodiscard]] std::optional<std::int64_t> integer(const JsonValue* const value) noexcept {
    const std::optional<double> real = number(value);
    if (!real.has_value())
        return std::nullopt;
    return static_cast<std::int64_t>(std::llround(*real));
}

[[nodiscard]] Layer layer(const JsonValue& value,
                          const std::map<std::string, std::size_t, std::less<>>& assets) {
    if (value.object() == nullptr)
        throw std::invalid_argument("a layer must be an object");
    Layer result;
    result.type = layer_type(static_cast<int>(number_or(value, "ty", -1.0)));
    result.index = integer(value.find("ind"));
    result.parent = integer(value.find("parent"));
    result.in_frame = number_or(value, "ip", 0.0);
    result.out_frame = number_or(value, "op", 0.0);
    result.start_time = number_or(value, "st", 0.0);
    result.stretch = number_or(value, "sr", 1.0);
    if (!(std::abs(result.stretch) > 1.0e-9))
        result.stretch = 1.0;
    result.transform = transform(value.find("ks"));
    result.auto_orient = flag(value, "ao");
    result.hidden = flag(value, "hd");
    result.matte_source = flag(value, "td");
    const int matte = static_cast<int>(number_or(value, "tt", 0.0));
    result.matte = matte >= 1 && matte <= 4 ? static_cast<MatteMode>(matte) : MatteMode::none;
    result.matte_parent = integer(value.find("tp"));
    if (const JsonValue* masks = value.find("masksProperties");
        masks != nullptr && masks->array() != nullptr) {
        for (const JsonValue& entry : *masks->array()) {
            if (entry.object() == nullptr)
                continue;
            Mask mask;
            const std::string* mode = text(entry, "mode");
            mask.mode = mode != nullptr && !mode->empty() ? (*mode)[0] : 'a';
            mask.inverted = flag(entry, "inv");
            mask.path = shape_property(entry.find("pt"));
            mask.opacity = property(entry.find("o"), {100.0});
            mask.expansion = property(entry.find("x"), {0.0});
            result.masks.push_back(std::move(mask));
        }
    }
    switch (result.type) {
    case LayerType::shape:
        shape_items(value.find("shapes"), result.shapes, 0U);
        break;
    case LayerType::precomp: {
        result.width = number_or(value, "w", 0.0);
        result.height = number_or(value, "h", 0.0);
        if (const std::string* reference = text(value, "refId"); reference != nullptr) {
            if (const auto found = assets.find(*reference); found != assets.end())
                result.precomp = found->second;
        }
        if (const JsonValue* remap = value.find("tm"); remap != nullptr)
            result.time_remap = property(remap, {0.0});
        break;
    }
    case LayerType::solid:
        result.solid_color = hex_color(text(value, "sc"));
        result.width = number_or(value, "sw", 0.0);
        result.height = number_or(value, "sh", 0.0);
        break;
    default:
        break;
    }
    return result;
}

[[nodiscard]] Composition
composition(const JsonValue* const layers,
            const std::map<std::string, std::size_t, std::less<>>& assets) {
    Composition result;
    if (layers == nullptr || layers->array() == nullptr)
        return result;
    result.layers.reserve(layers->array()->size());
    for (const JsonValue& entry : *layers->array())
        result.layers.push_back(layer(entry, assets));
    return result;
}

/** Whether precomposition `index` reaches itself through its layers. */
[[nodiscard]] bool cyclic(const Animation& animation, const std::size_t index,
                          std::vector<int>& state) {
    if (state[index] == 1)
        return true;
    if (state[index] == 2)
        return false;
    state[index] = 1;
    for (const Layer& entry : animation.precomps[index].layers) {
        if (entry.precomp.has_value() && cyclic(animation, *entry.precomp, state))
            return true;
    }
    state[index] = 2;
    return false;
}

[[nodiscard]] double cubic_axis(const double a, const double b, const double t) noexcept {
    const double u = 1.0 - t;
    return 3.0 * u * u * t * a + 3.0 * u * t * t * b + t * t * t;
}

[[nodiscard]] double handle(const std::vector<double>& values, const std::size_t dimension,
                            const double fallback) noexcept {
    if (values.empty())
        return fallback;
    return values[std::min(dimension, values.size() - 1U)];
}

/** The eased progress of a segment for one dimension, from its handles. */
[[nodiscard]] double eased(const EaseHandle& out, const EaseHandle& in, const std::size_t dimension,
                           const double t) noexcept {
    if (out.x.empty() || in.x.empty())
        return t;
    return cubic_ease(handle(out.x, dimension, 0.0), handle(out.y, dimension, 0.0),
                      handle(in.x, dimension, 1.0), handle(in.y, dimension, 1.0), t);
}

/** Where a spatial segment is at eased progress `progress`, moving evenly along its arc. */
[[nodiscard]] Vec2 spatial_point(const Keyframe& keyframe, const double progress) noexcept {
    const Vec2 from{keyframe.start[0], keyframe.start[1]};
    const Vec2 to{keyframe.end[0], keyframe.end[1]};
    const Vec2 first{from.x + keyframe.spatial_out.x, from.y + keyframe.spatial_out.y};
    const Vec2 second{to.x + keyframe.spatial_in.x, to.y + keyframe.spatial_in.y};
    const double length = keyframe.arc_lengths.back();
    // Overshooting easing (a bounce) carries on past the ends along their tangents.
    const auto beyond = [&](const Vec2 anchor, const Vec2 toward, const double distance) {
        double dx = toward.x - anchor.x;
        double dy = toward.y - anchor.y;
        const double norm = std::hypot(dx, dy);
        if (norm <= 1.0e-12)
            return anchor;
        dx /= norm;
        dy /= norm;
        return Vec2{anchor.x + dx * distance, anchor.y + dy * distance};
    };
    if (progress <= 0.0) {
        const Vec2 ahead = cubic_point(from, first, second, to, 1.0 / arc_samples);
        return beyond(from, ahead, progress * length);
    }
    if (progress >= 1.0) {
        const Vec2 behind = cubic_point(from, first, second, to, 1.0 - 1.0 / arc_samples);
        const Vec2 past = beyond(to, behind, -(progress - 1.0) * length);
        return past;
    }
    const double target = progress * length;
    const auto found = std::ranges::lower_bound(keyframe.arc_lengths, target);
    const std::size_t upper = std::clamp<std::size_t>(
        static_cast<std::size_t>(found - keyframe.arc_lengths.begin()), 1U, arc_samples);
    const double low = keyframe.arc_lengths[upper - 1U];
    const double high = keyframe.arc_lengths[upper];
    const double fraction = high > low ? (target - low) / (high - low) : 0.0;
    const double t = (static_cast<double>(upper - 1U) + fraction) / arc_samples;
    return cubic_point(from, first, second, to, t);
}

/** The segment `frame` falls in: its index, and the linear progress through it. */
template <typename Frames>
[[nodiscard]] std::pair<std::size_t, double> locate(const Frames& frames,
                                                    const double frame) noexcept {
    if (frame <= frames.front().time)
        return {0U, 0.0};
    if (frame >= frames.back().time)
        return {frames.size() - 1U, 0.0};
    const auto after = std::ranges::upper_bound(frames, frame, {},
                                                [](const auto& keyframe) { return keyframe.time; });
    const std::size_t index = static_cast<std::size_t>(after - frames.begin()) - 1U;
    const double duration = frames[index + 1U].time - frames[index].time;
    const double t = duration > 0.0 ? (frame - frames[index].time) / duration : 1.0;
    return {index, std::clamp(t, 0.0, 1.0)};
}

} // namespace

double cubic_ease(const double out_x, const double out_y, const double in_x, const double in_y,
                  const double t) noexcept {
    if (t <= 0.0)
        return 0.0;
    if (t >= 1.0)
        return 1.0;
    const double x1 = std::clamp(out_x, 0.0, 1.0);
    const double x2 = std::clamp(in_x, 0.0, 1.0);
    if (x1 == out_y && x2 == in_y)
        return t;
    // x(u) is monotonic for handles in [0, 1]: Newton's method from a good start, with bisection
    // keeping it inside the bracket.
    double low = 0.0;
    double high = 1.0;
    double u = t;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const double x = cubic_axis(x1, x2, u) - t;
        if (std::abs(x) < 1.0e-10)
            break;
        if (x > 0.0)
            high = u;
        else
            low = u;
        const double v = 1.0 - u;
        const double slope = 3.0 * v * v * x1 + 6.0 * v * u * (x2 - x1) + 3.0 * u * u * (1.0 - x2);
        double next = slope > 1.0e-9 ? u - x / slope : (low + high) * 0.5;
        if (!(next > low && next < high))
            next = (low + high) * 0.5;
        u = next;
    }
    return cubic_axis(out_y, in_y, u);
}

Property::Property(std::vector<double> constant) : constant_(std::move(constant)) {}

void Property::evaluate(const double frame, const std::span<double> out) const noexcept {
    const auto write = [&out](const std::vector<double>& values) {
        for (std::size_t index = 0U; index < out.size(); ++index)
            out[index] = index < values.size() ? values[index] : 0.0;
    };
    if (keyframes_.empty()) {
        write(constant_);
        return;
    }
    const auto [index, t] = locate(keyframes_, frame);
    const Keyframe& keyframe = keyframes_[index];
    // Before the first keyframe its value holds, and after the last one the last value does.
    if (index + 1U >= keyframes_.size() || frame <= keyframes_.front().time) {
        write(keyframe.start);
        return;
    }
    if (keyframe.hold) {
        write(keyframe.start);
        return;
    }
    if (keyframe.spatial && out.size() >= 2U) {
        const Vec2 point = spatial_point(keyframe, eased(keyframe.out, keyframe.in, 0U, t));
        out[0] = point.x;
        out[1] = point.y;
        for (std::size_t dimension = 2U; dimension < out.size(); ++dimension) {
            const double from = dimension < keyframe.start.size() ? keyframe.start[dimension] : 0.0;
            const double to = dimension < keyframe.end.size() ? keyframe.end[dimension] : 0.0;
            out[dimension] = from + (to - from) * eased(keyframe.out, keyframe.in, dimension, t);
        }
        return;
    }
    for (std::size_t dimension = 0U; dimension < out.size(); ++dimension) {
        const double from = dimension < keyframe.start.size() ? keyframe.start[dimension] : 0.0;
        const double to = dimension < keyframe.end.size() ? keyframe.end[dimension] : 0.0;
        out[dimension] = from + (to - from) * eased(keyframe.out, keyframe.in, dimension, t);
    }
}

double Property::scalar(const double frame) const noexcept {
    double value = 0.0;
    evaluate(frame, std::span<double>(&value, 1U));
    return value;
}

Vec2 Property::point(const double frame) const noexcept {
    std::array<double, 2> value{};
    evaluate(frame, value);
    return Vec2{value[0], value[1]};
}

std::vector<double> Property::values(const double frame) const {
    std::size_t size = constant_.size();
    if (!keyframes_.empty()) {
        const auto [index, t] = locate(keyframes_, frame);
        static_cast<void>(t);
        size = keyframes_[index].start.size();
    }
    std::vector<double> result(size);
    evaluate(frame, result);
    return result;
}

BezierShape ShapeProperty::evaluate(const double frame) const {
    if (keyframes_.empty())
        return constant_;
    const auto [index, t] = locate(keyframes_, frame);
    const ShapeKeyframe& keyframe = keyframes_[index];
    if (index + 1U >= keyframes_.size() || frame <= keyframes_.front().time || keyframe.hold)
        return keyframe.start;
    const BezierShape& from = keyframe.start;
    const BezierShape& to = keyframe.end;
    // Outlines of different vertex counts cannot be blended; the first holds.
    if (from.vertices.size() != to.vertices.size())
        return from;
    const double progress = eased(keyframe.out, keyframe.in, 0U, t);
    const auto mix = [progress](const Vec2 a, const Vec2 b) {
        return Vec2{a.x + (b.x - a.x) * progress, a.y + (b.y - a.y) * progress};
    };
    BezierShape result;
    result.closed = from.closed;
    const std::size_t count = from.vertices.size();
    result.vertices.resize(count);
    result.in_tangents.resize(count);
    result.out_tangents.resize(count);
    for (std::size_t vertex = 0U; vertex < count; ++vertex) {
        result.vertices[vertex] = mix(from.vertices[vertex], to.vertices[vertex]);
        result.in_tangents[vertex] = mix(from.in_tangents[vertex], to.in_tangents[vertex]);
        result.out_tangents[vertex] = mix(from.out_tangents[vertex], to.out_tangents[vertex]);
    }
    return result;
}

Vec2 Transform::position_at(const double frame) const noexcept {
    if (split_position)
        return Vec2{position_x.scalar(frame), position_y.scalar(frame)};
    return position.point(frame);
}

Animation parse(const std::string_view json) {
    JsonValue document;
    try {
        document = data::parse_json(json);
    } catch (const data::JsonError& error) {
        throw std::invalid_argument("invalid JSON at byte " + std::to_string(error.offset()) +
                                    ": " + error.what());
    }
    if (document.object() == nullptr)
        throw std::invalid_argument("a Lottie document must be a JSON object");
    Animation result;
    result.width = number_or(document, "w", 0.0);
    result.height = number_or(document, "h", 0.0);
    result.frame_rate = number_or(document, "fr", 0.0);
    result.in_frame = number_or(document, "ip", 0.0);
    result.out_frame = number_or(document, "op", 0.0);
    if (const std::string* name = text(document, "nm"); name != nullptr)
        result.name = *name;
    if (!(result.width > 0.0) || !(result.height > 0.0))
        throw std::invalid_argument("a Lottie composition needs a positive width and height");
    if (!(result.frame_rate > 0.0))
        throw std::invalid_argument("a Lottie composition needs a positive frame rate");
    if (!(result.out_frame > result.in_frame))
        throw std::invalid_argument("a Lottie composition's out point must follow its in point");
    if (document.find("layers") == nullptr || document.find("layers")->array() == nullptr)
        throw std::invalid_argument("a Lottie composition needs a layers array");
    // Precompositions may refer to each other, so every id is known before any is read.
    std::map<std::string, std::size_t, std::less<>> assets;
    std::vector<const JsonValue*> precomp_sources;
    if (const JsonValue* list = document.find("assets");
        list != nullptr && list->array() != nullptr) {
        for (const JsonValue& asset : *list->array()) {
            if (asset.object() == nullptr || asset.find("layers") == nullptr)
                continue;
            const std::string* id = text(asset, "id");
            if (id == nullptr || assets.contains(*id))
                continue;
            assets.emplace(*id, precomp_sources.size());
            precomp_sources.push_back(asset.find("layers"));
        }
    }
    result.precomps.reserve(precomp_sources.size());
    for (const JsonValue* layers : precomp_sources)
        result.precomps.push_back(composition(layers, assets));
    result.root = composition(document.find("layers"), assets);
    std::vector<int> state(result.precomps.size(), 0);
    for (std::size_t index = 0U; index < result.precomps.size(); ++index) {
        if (cyclic(result, index, state))
            throw std::invalid_argument("a Lottie precomposition contains itself");
    }
    return result;
}

} // namespace strata::resource::lottie

namespace strata::resource {

LottieLibrary::LottieLibrary(Loader loader) : loader_(std::move(loader)) {}

std::shared_ptr<const lottie::Animation> LottieLibrary::find(const std::string_view id) const {
    const std::scoped_lock lock(mutex_);
    if (const auto found = entries_.find(id); found != entries_.end())
        return found->second.animation;
    Entry entry;
    if (id.empty() || !loader_) {
        entry.error = "no Lottie source";
    } else {
        try {
            const std::vector<std::uint8_t> bytes = loader_(id);
            entry.animation = std::make_shared<const lottie::Animation>(lottie::parse(
                std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())));
        } catch (const std::exception& error) {
            entry.error = std::string(id) + ": " + error.what();
        }
    }
    std::shared_ptr<const lottie::Animation> animation = entry.animation;
    entries_.emplace(std::string(id), std::move(entry));
    return animation;
}

std::string LottieLibrary::error(const std::string_view id) const {
    const std::scoped_lock lock(mutex_);
    const auto found = entries_.find(id);
    return found != entries_.end() ? found->second.error : std::string{};
}

} // namespace strata::resource
