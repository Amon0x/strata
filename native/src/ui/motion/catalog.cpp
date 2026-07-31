#include "ui/motion/catalog.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace strata::ui {
namespace {

using JsonValue = data::JsonView;
using JsonArray = data::JsonArrayView;

[[nodiscard]] std::string operator+(const char* const left, const std::string_view right) {
    std::string result(left);
    result.append(right);
    return result;
}

[[nodiscard]] JsonValue required(
    const JsonValue value,
    const std::string_view field,
    const std::string_view context
) {
    const JsonValue result = value.find(field);
    if (!result) {
        throw std::runtime_error(
            "portable animation " + std::string(context) + " is missing '" + std::string(field) + "'"
        );
    }
    return result;
}

[[nodiscard]] std::string_view string_field(
    const JsonValue value,
    const std::string_view field,
    const std::string_view context
) {
    const std::optional<std::string_view> result = required(value, field, context).string();
    if (!result.has_value()) {
        throw std::runtime_error(
            "portable animation " + std::string(context) + " field '" + std::string(field) +
            "' must be a string"
        );
    }
    return *result;
}

[[nodiscard]] JsonArray array_field(
    const JsonValue value,
    const std::string_view field,
    const std::string_view context
) {
    const std::optional<JsonArray> result = required(value, field, context).array();
    if (!result.has_value()) {
        throw std::runtime_error(
            "portable animation " + std::string(context) + " field '" + std::string(field) +
            "' must be an array"
        );
    }
    return *result;
}

[[nodiscard]] double number(const JsonValue value, const std::string_view context) {
    if (const std::optional<double> result = value.number();
        result.has_value() && std::isfinite(*result)) return *result;
    if (const std::optional<std::int64_t> result = value.integer(); result.has_value()) {
        return static_cast<double>(*result);
    }
    throw std::runtime_error("portable animation " + std::string(context) + " must be numeric");
}

[[nodiscard]] std::int64_t integer(const JsonValue value, const std::string_view context) {
    if (const std::optional<std::int64_t> result = value.integer(); result.has_value()) return *result;
    const double parsed = number(value, context);
    if (parsed < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        parsed > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        throw std::runtime_error("portable animation " + std::string(context) + " is outside int64 range");
    }
    return static_cast<std::int64_t>(parsed);
}

[[nodiscard]] std::uint8_t hex_byte(const std::string_view value, const std::size_t offset) {
    unsigned parsed = 0U;
    const auto result = std::from_chars(
        value.data() + static_cast<std::ptrdiff_t>(offset),
        value.data() + static_cast<std::ptrdiff_t>(offset + 2U),
        parsed,
        16
    );
    if (result.ec != std::errc{} || result.ptr != value.data() + static_cast<std::ptrdiff_t>(offset + 2U) ||
        parsed > 255U) {
        throw std::runtime_error("portable animation color contains invalid hexadecimal channels");
    }
    return static_cast<std::uint8_t>(parsed);
}

[[nodiscard]] runtime::ColorValue color(const std::string_view rgba) {
    if (rgba.size() != 8U) throw std::runtime_error("portable animation color must contain RGBA8");
    return runtime::ColorValue{
        hex_byte(rgba, 0U), hex_byte(rgba, 2U), hex_byte(rgba, 4U), hex_byte(rgba, 6U),
    };
}

[[nodiscard]] MotionValue motion_value(const JsonValue value) {
    const std::string_view kind = string_field(value, "kind", "keyframe value");
    const JsonValue payload = required(value, "value", "keyframe value");
    if (kind == "number") return number(payload, "numeric keyframe value");
    if (kind == "color") {
        const std::optional<std::string_view> rgba = payload.string();
        if (!rgba.has_value()) throw std::runtime_error("portable animation color value must be a string");
        return color(*rgba);
    }
    if (kind == "boolean") {
        const std::optional<bool> boolean = payload.boolean();
        if (!boolean.has_value()) throw std::runtime_error("portable animation boolean value must be boolean");
        return *boolean;
    }
    throw std::runtime_error("portable animation keyframe has unsupported value kind '" + kind + "'");
}

[[nodiscard]] std::string normalized_token(std::string value);

[[nodiscard]] std::optional<MotionEasing> easing(const JsonValue value) {
    if (value.is_null()) return std::nullopt;
    if (const std::optional<std::string_view> direct = value.string(); direct.has_value()) {
        try {
            return MotionEasing(std::string(*direct));
        } catch (const std::invalid_argument& error) {
            throw std::runtime_error(error.what());
        }
    }
    if (!value.object().has_value()) {
        throw std::runtime_error("portable animation easing must be null, a name, or an object");
    }
    const std::string_view kind = string_field(value, "kind", "easing");
    if (normalized_token(std::string(kind)) == "cubicbezier") {
        return MotionEasing::cubic_bezier(
            number(required(value, "x1", "easing"), "easing x1"),
            number(required(value, "y1", "easing"), "easing y1"),
            number(required(value, "x2", "easing"), "easing x2"),
            number(required(value, "y2", "easing"), "easing y2")
        );
    }
    try {
        return MotionEasing(std::string(kind));
    } catch (const std::invalid_argument& error) {
        throw std::runtime_error(error.what());
    }
}

[[nodiscard]] std::string normalized_token(std::string value) {
    std::erase_if(value, [](const unsigned char character) {
        return character == '-' || character == '_';
    });
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] MotionFillMode fill_mode(const JsonValue value) {
    const std::optional<std::string_view> token = value.string();
    if (!token.has_value()) throw std::runtime_error("portable animation fillMode must be a string");
    const std::string normalized = normalized_token(std::string(*token));
    if (normalized == "none") return MotionFillMode::none;
    if (normalized == "forwards") return MotionFillMode::forwards;
    if (normalized == "backwards") return MotionFillMode::backwards;
    if (normalized == "both") return MotionFillMode::both;
    throw std::runtime_error("portable animation fillMode has unknown value '" + *token + "'");
}

[[nodiscard]] MotionRepeat repeat(const JsonValue value) {
    if (!value.object().has_value()) {
        throw std::runtime_error("portable animation repeat must be an object");
    }
    const std::string_view kind = string_field(value, "kind", "repeat");
    const std::string normalized = normalized_token(std::string(kind));
    if (normalized == "none") return MotionRepeat{};
    if (normalized == "forever") return MotionRepeat{MotionRepeatKind::forever, 1U};
    if (normalized != "count") {
        throw std::runtime_error("portable animation repeat has unknown kind '" + kind + "'");
    }
    JsonValue count = value.find("count");
    if (!count) count = value.find("iterations");
    if (!count) throw std::runtime_error("portable animation count repeat is missing 'count'");
    const std::int64_t iterations = integer(count, "repeat count");
    if (iterations <= 0 ||
        iterations > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        throw std::runtime_error("portable animation repeat count must be a positive uint32");
    }
    return MotionRepeat{MotionRepeatKind::count, static_cast<std::uint32_t>(iterations)};
}

[[nodiscard]] CompiledMotion compile_motion(const JsonValue declaration) {
    CompiledMotion result;
    result.name = string_field(declaration, "name", "declaration");
    if (const std::optional<std::string_view> declared_trigger =
            declaration.find("trigger").string(); declared_trigger.has_value()) {
        const std::optional<MotionTrigger> parsed = motion_trigger(*declared_trigger);
        if (!parsed.has_value()) {
            throw std::runtime_error("portable animation trigger is unknown");
        }
        result.trigger = *parsed;
    }
    const JsonValue animation = required(declaration, "animation", "declaration");
    const JsonValue timing = required(animation, "timing", "payload");
    result.timing.duration_nanos = integer(
        required(timing, "durationNanos", "timing"), "durationNanos"
    );
    result.timing.delay_nanos = integer(required(timing, "delayNanos", "timing"), "delayNanos");
    if (result.timing.duration_nanos <= 0 || result.timing.delay_nanos < 0) {
        throw std::runtime_error("portable animation timing must have positive duration and non-negative delay");
    }
    const JsonValue timing_easing = required(timing, "easing", "timing");
    result.timing.easing = easing(timing_easing).value_or(MotionEasing{});
    result.timing.fill_mode = fill_mode(required(timing, "fillMode", "timing"));
    result.timing.repeat = repeat(required(timing, "repeat", "timing"));
    if (const std::optional<bool> reverse = timing.find("reverse").boolean();
        reverse.has_value()) {
        result.timing.reverse = *reverse;
    }
    std::set<MotionProperty> track_properties;
    for (const JsonValue track_value : array_field(animation, "tracks", "payload")) {
        const std::string_view property_name = string_field(track_value, "property", "track");
        const std::optional<MotionProperty> property = motion_property(property_name);
        if (!property.has_value()) {
            throw std::runtime_error("portable animation uses unknown property '" + property_name + "'");
        }
        if (!track_properties.insert(*property).second) {
            throw std::runtime_error("portable animation contains duplicate property tracks");
        }
        MotionTrack track;
        track.property = *property;
        for (const JsonValue frame : array_field(track_value, "keyframes", "track")) {
            MotionKeyframe keyframe;
            keyframe.offset = number(required(frame, "offset", "keyframe"), "keyframe offset");
            if (!std::isfinite(keyframe.offset) || keyframe.offset < 0.0 || keyframe.offset > 1.0) {
                throw std::runtime_error("portable animation keyframe offset must be normalized");
            }
            keyframe.value = motion_value(required(frame, "value", "keyframe"));
            keyframe.easing = easing(required(frame, "easing", "keyframe"));
            track.keyframes.push_back(std::move(keyframe));
        }
        std::ranges::sort(track.keyframes, {}, &MotionKeyframe::offset);
        if (track.keyframes.empty()) continue;
        for (std::size_t index = 0U; index < track.keyframes.size(); ++index) {
            const MotionKeyframe& frame = track.keyframes[index];
            if (!motion_property_accepts(*property, frame.value) ||
                (index != 0U && frame.value.index() != track.keyframes.front().value.index()) ||
                (index != 0U && frame.offset == track.keyframes[index - 1U].offset)) {
                throw std::runtime_error(
                    "portable animation track has incompatible values or duplicate offsets"
                );
            }
        }
        result.tracks.push_back(std::move(track));
    }
    if (const std::optional<JsonArray> authored_order =
            animation.find("$authoredTrackOrder").array(); authored_order.has_value()) {
        std::map<std::string, std::size_t, std::less<>> ranks;
        for (std::size_t index = 0U; index < authored_order->size(); ++index) {
            const std::optional<std::string_view> name = (*authored_order)[index].string();
            if (name.has_value()) ranks.emplace(std::string(*name), index);
        }
        std::ranges::stable_sort(result.tracks, [&ranks](const MotionTrack& left, const MotionTrack& right) {
            const auto left_rank = ranks.find(std::string(motion_property_name(left.property)));
            const auto right_rank = ranks.find(std::string(motion_property_name(right.property)));
            const std::size_t fallback = ranks.size();
            return (left_rank != ranks.end() ? left_rank->second : fallback) <
                   (right_rank != ranks.end() ? right_rank->second : fallback);
        });
    }
    if (result.tracks.empty()) {
        throw std::runtime_error("portable animation '" + result.name + "' has no supported tracks");
    }
    return result;
}

} // namespace

void MotionCatalog::bind(std::shared_ptr<const runtime::RuntimeUnit> unit) {
    if (unit_ == unit) return;
    unit_ = std::move(unit);
    compiled_.clear();
    timed_.clear();
}

void MotionCatalog::set_supplemental(
    std::map<std::string, CompiledMotion, std::less<>> motions
) {
    if (supplemental_ == motions) return;
    supplemental_ = std::move(motions);
    timed_.clear();
}

const CompiledMotion* MotionCatalog::find(const std::string_view name) {
    if (const auto supplemental = supplemental_.find(name); supplemental != supplemental_.end()) {
        return &supplemental->second;
    }
    if (unit_ == nullptr) return nullptr;
    if (const auto found = compiled_.find(name); found != compiled_.end()) return &found->second;
    const data::JsonView declaration = unit_->animation(name);
    if (!declaration) return nullptr;
    CompiledMotion compiled = compile_motion(declaration);
    return &compiled_.emplace(std::string(name), std::move(compiled)).first->second;
}

const CompiledMotion* MotionCatalog::timed(
    const std::string_view name,
    const std::int64_t duration_nanos,
    const std::int64_t delay_nanos
) {
    if (duration_nanos <= 0 || delay_nanos < 0) return nullptr;
    const CompiledMotion* source = find(name);
    if (source == nullptr) return nullptr;
    const std::string key = std::to_string(name.size()) + "#" + std::string(name) +
                            std::to_string(duration_nanos) + "#" +
                            std::to_string(delay_nanos);
    if (const auto found = timed_.find(key); found != timed_.end()) return &found->second;
    CompiledMotion result = *source;
    result.timing.duration_nanos = duration_nanos;
    result.timing.delay_nanos = delay_nanos;
    return &timed_.emplace(key, std::move(result)).first->second;
}

void MotionCatalog::clear() noexcept {
    unit_.reset();
    compiled_.clear();
    timed_.clear();
    supplemental_.clear();
}

} // namespace strata::ui
