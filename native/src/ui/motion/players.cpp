#include "ui/motion/players.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace strata::ui::motion_detail {
namespace {

[[nodiscard]] MotionDirection opposite(const MotionDirection direction) noexcept {
    return direction == MotionDirection::forward
               ? MotionDirection::reverse
               : MotionDirection::forward;
}

[[nodiscard]] MotionDirection iteration_direction(
    const MotionDirection requested,
    const bool alternating,
    const std::uint64_t iteration
) noexcept {
    return alternating && (iteration & 1U) != 0U ? opposite(requested) : requested;
}

[[nodiscard]] bool visible_before(const MotionFillMode fill) noexcept {
    return fill == MotionFillMode::backwards || fill == MotionFillMode::both;
}

[[nodiscard]] bool visible_after(const MotionFillMode fill) noexcept {
    return fill == MotionFillMode::forwards || fill == MotionFillMode::both;
}

[[nodiscard]] std::int64_t saturating_multiply(
    const std::int64_t value,
    const std::uint32_t count
) noexcept {
    if (count == 0U) return 0;
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    if (value > maximum / static_cast<std::int64_t>(count)) return maximum;
    return value * static_cast<std::int64_t>(count);
}

[[nodiscard]] MotionValue sample_track(const MotionTrack& track, const double progress) {
    if (track.keyframes.size() == 1U || progress <= track.keyframes.front().offset) {
        return track.keyframes.front().value;
    }
    if (progress >= track.keyframes.back().offset) return track.keyframes.back().value;
    const auto upper = std::ranges::upper_bound(track.keyframes, progress, {}, &MotionKeyframe::offset);
    const MotionKeyframe& next = *upper;
    const MotionKeyframe& previous = *(upper - 1);
    const double span = next.offset - previous.offset;
    const double raw = span > 0.0 ? (progress - previous.offset) / span : 1.0;
    const double eased = previous.easing.has_value()
                             ? motion_easing(*previous.easing, raw)
                             : raw;
    return interpolate_motion_value(previous.value, next.value, eased);
}

[[nodiscard]] TimelineSample evaluate(
    const CompiledMotion& animation,
    const std::int64_t elapsed_nanos,
    const MotionDirection direction
) {
    const std::int64_t elapsed = std::max<std::int64_t>(0, elapsed_nanos);
    const bool delayed = elapsed < animation.timing.delay_nanos;
    const std::int64_t active_elapsed = delayed ? 0 : elapsed - animation.timing.delay_nanos;
    const bool forever = animation.timing.repeat.kind == MotionRepeatKind::forever;
    const std::uint32_t iterations = animation.timing.repeat.kind == MotionRepeatKind::count
                                         ? std::max(1U, animation.timing.repeat.iterations)
                                         : 1U;
    const std::int64_t active_duration = saturating_multiply(
        animation.timing.duration_nanos, iterations
    );
    const bool finished = !delayed && !forever && active_elapsed >= active_duration;
    std::uint64_t iteration = 0U;
    double raw_forward = 0.0;
    bool visible = true;
    if (delayed) {
        visible = visible_before(animation.timing.fill_mode);
    } else if (finished) {
        iteration = static_cast<std::uint64_t>(iterations - 1U);
        raw_forward = 1.0;
        visible = visible_after(animation.timing.fill_mode);
    } else {
        iteration = static_cast<std::uint64_t>(active_elapsed / animation.timing.duration_nanos);
        const std::int64_t iteration_elapsed = active_elapsed % animation.timing.duration_nanos;
        raw_forward = static_cast<double>(iteration_elapsed) /
                      static_cast<double>(animation.timing.duration_nanos);
    }
    const MotionDirection directed_iteration = iteration_direction(
        direction, animation.timing.reverse, iteration
    );
    const double directed = finished
                                ? motion_terminal_progress(animation, direction)
                                : directed_iteration == MotionDirection::reverse
                                      ? 1.0 - raw_forward
                                      : raw_forward;
    const double progress = motion_easing(animation.timing.easing, directed);
    MotionComputedValues computed;
    computed.progress = progress;
    if (visible) {
        for (const MotionTrack& track : animation.tracks) {
            computed.values.insert_or_assign(track.property, sample_track(track, progress));
        }
    }
    return TimelineSample{std::move(computed), directed, !finished, finished, direction};
}

[[nodiscard]] std::int64_t terminal_elapsed(const CompiledMotion& animation) noexcept {
    const std::uint32_t iterations = animation.timing.repeat.kind == MotionRepeatKind::count
                                         ? std::max(1U, animation.timing.repeat.iterations)
                                         : 1U;
    const std::int64_t active = saturating_multiply(animation.timing.duration_nanos, iterations);
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    return animation.timing.delay_nanos > maximum - active
               ? maximum
               : animation.timing.delay_nanos + active;
}

[[nodiscard]] std::int64_t start_for_direction(
    const CompiledMotion& animation,
    const std::int64_t now_nanos,
    const MotionDirection direction,
    const double progress,
    const bool preserve
) noexcept {
    const double clamped = std::clamp(progress, 0.0, 1.0);
    const double active_fraction = direction == MotionDirection::reverse ? 1.0 - clamped : clamped;
    const auto active_offset = static_cast<std::int64_t>(
        static_cast<double>(animation.timing.duration_nanos) * active_fraction
    );
    return now_nanos - active_offset - (preserve ? animation.timing.delay_nanos : 0);
}

[[nodiscard]] std::uint32_t packed(const runtime::ColorValue& color) noexcept {
    return (static_cast<std::uint32_t>(color.red) << 24U) |
           (static_cast<std::uint32_t>(color.green) << 16U) |
           (static_cast<std::uint32_t>(color.blue) << 8U) |
           static_cast<std::uint32_t>(color.alpha);
}

[[nodiscard]] std::string number_text(const double value) {
    std::array<char, 64U> buffer{};
    const auto result = std::to_chars(
        buffer.data(), buffer.data() + buffer.size(), value, std::chars_format::general
    );
    std::string text(buffer.data(), result.ptr);
    if (text.find_first_of(".eE") == std::string::npos) text += ".0";
    return text;
}

} // namespace

TimelineSample TimelinePlayer::advance(
    const CompiledMotion& animation,
    const bool active,
    const MotionDirection active_direction,
    const std::int64_t now_nanos,
    const bool reduced_motion
) {
    if (now_nanos < 0) throw std::invalid_argument("motion clock must be non-negative");
    if (reduced_motion) return snap(animation, active, active_direction, now_nanos);
    const bool signature_changed = animation_ == nullptr || *animation_ != animation;
    if (signature_changed) {
        animation_ = &animation;
        if (continuation_pending_) {
            continuation_pending_ = false;
        } else {
            started_at_nanos_.reset();
            initialized_ = false;
            active_ = false;
            running_ = false;
            raw_progress_ = 0.0;
        }
    } else if (continuation_pending_) {
        continuation_pending_ = false;
    }
    const bool became_active = active && !active_;
    const bool became_inactive = !active && active_;
    active_ = active;
    if (!active && !became_inactive && !started_at_nanos_.has_value()) {
        initialized_ = true;
        direction_ = opposite(active_direction);
        return TimelineSample{MotionComputedValues{}, raw_progress_, false, false, direction_};
    }
    if (became_active || became_inactive || !started_at_nanos_.has_value()) {
        const bool preserve = started_at_nanos_.has_value() && (running_ || raw_progress_ > 0.0);
        direction_ = active ? active_direction : opposite(active_direction);
        const double start_progress = !preserve && direction_ == MotionDirection::reverse
                                          ? 1.0
                                          : raw_progress_;
        started_at_nanos_ = start_for_direction(
            animation, now_nanos, direction_, start_progress, preserve
        );
        running_ = true;
    }
    initialized_ = true;
    TimelineSample result = evaluate(animation, now_nanos - *started_at_nanos_, direction_);
    raw_progress_ = result.raw_progress;
    running_ = result.running;
    return result;
}

TimelineSample TimelinePlayer::snap(
    const CompiledMotion& animation,
    const bool active,
    const MotionDirection active_direction,
    const std::int64_t now_nanos
) {
    animation_ = &animation;
    active_ = active;
    initialized_ = true;
    running_ = false;
    direction_ = active ? active_direction : opposite(active_direction);
    const std::int64_t elapsed = terminal_elapsed(animation);
    started_at_nanos_ = now_nanos - elapsed;
    continuation_pending_ = false;
    TimelineSample result = evaluate(animation, elapsed, direction_);
    raw_progress_ = result.raw_progress;
    result.running = false;
    result.finished = true;
    return result;
}

bool TimelinePlayer::initialized() const noexcept { return initialized_; }
bool TimelinePlayer::running() const noexcept { return running_; }
double TimelinePlayer::progress() const noexcept { return raw_progress_; }
MotionDirection TimelinePlayer::direction() const noexcept { return direction_; }

TimelinePlayer TimelinePlayer::continued() const {
    TimelinePlayer result = *this;
    result.active_ = false;
    result.continuation_pending_ = true;
    return result;
}

MotionTimelineSample MotionTimelinePlayer::advance(
    const MotionTimelineSpec& spec,
    const std::int64_t now_nanos,
    const bool reduced_motion
) {
    if (now_nanos < 0 || spec.duration_nanos <= 0) {
        throw std::invalid_argument("motion timeline requires a positive duration and clock");
    }
    if (!spec_.has_value() || *spec_ != spec) {
        spec_ = spec;
        last_frame_nanos_.reset();
        sample_ = MotionTimelineSample{};
    }
    if (reduced_motion || !spec.running) {
        // A property-neutral or looping clock has no meaningful terminal value. Forgetting the
        // origin freezes its presented frame and prevents reduced-motion time from catching up.
        last_frame_nanos_.reset();
        sample_.running = false;
        return sample_;
    }
    if (!spec.loop && sample_.progress >= 1.0) {
        last_frame_nanos_.reset();
        sample_.running = false;
        return sample_;
    }
    const std::optional<std::int64_t> previous = last_frame_nanos_;
    last_frame_nanos_ = now_nanos;
    if (previous.has_value()) {
        const std::int64_t delta = std::max<std::int64_t>(0, now_nanos - *previous);
        if (delta > 0) {
            const double next = sample_.progress +
                                static_cast<double>(delta) /
                                    static_cast<double>(spec.duration_nanos);
            sample_.progress = spec.loop ? std::fmod(next, 1.0) : std::min(1.0, next);
        }
    }
    sample_.running = spec.loop || sample_.progress < 1.0;
    return sample_;
}

const MotionTimelineSample& MotionTimelinePlayer::sample() const noexcept { return sample_; }

TargetSample TargetPlayer::advance(
    const MotionValue& next_target,
    const MotionTiming& timing,
    const std::int64_t now_nanos,
    const bool reduced_motion
) {
    if (now_nanos < 0 || timing.duration_nanos <= 0 || timing.delay_nanos < 0) {
        throw std::invalid_argument("target motion requires valid timing and a non-negative clock");
    }
    if (!target_.has_value()) {
        timing_ = timing;
        snap(next_target, reduced_motion);
        return sample_;
    }
    if (reduced_motion) {
        timing_ = timing;
        snap(next_target, true);
        return sample_;
    }
    if (*target_ != next_target || timing_ != timing) {
        const TargetSample presented = sample_at(now_nanos);
        start_ = presented.value;
        target_ = next_target;
        started_at_nanos_ = now_nanos;
        timing_ = timing;
    }
    sample_ = sample_at(now_nanos);
    if (!sample_.running) {
        start_ = target_;
        started_at_nanos_.reset();
        sample_.progress = 1.0;
    }
    return sample_;
}

const TargetSample& TargetPlayer::sample() const noexcept { return sample_; }
bool TargetPlayer::initialized() const noexcept { return target_.has_value(); }

TargetSample TargetPlayer::sample_at(const std::int64_t now_nanos) const {
    const MotionValue& from = start_.has_value() ? *start_ : *target_;
    const MotionValue& to = *target_;
    const std::int64_t started = started_at_nanos_.value_or(now_nanos);
    const std::int64_t elapsed = std::max<std::int64_t>(0, now_nanos - started);
    const double raw = elapsed <= timing_.delay_nanos
                           ? 0.0
                           : std::clamp(
                                 static_cast<double>(elapsed - timing_.delay_nanos) /
                                     static_cast<double>(timing_.duration_nanos),
                                 0.0,
                                 1.0
                             );
    return TargetSample{
        interpolate_motion_value(from, to, motion_easing(timing_.easing, raw)),
        to,
        raw,
        raw < 1.0 && from != to,
        false,
    };
}

void TargetPlayer::snap(const MotionValue& target, const bool reduced_motion) {
    start_ = target;
    target_ = target;
    started_at_nanos_.reset();
    sample_ = TargetSample{target, target, 1.0, false, reduced_motion};
}

std::string motion_value_text(const MotionValue& value) {
    if (const double* number = std::get_if<double>(&value)) {
        return "Number(value=" + number_text(*number) + ")";
    }
    if (const auto* color = std::get_if<runtime::ColorValue>(&value)) {
        return "Color(value=RgbaColor(rgba=" + std::to_string(packed(*color)) + "))";
    }
    if (const bool* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "BooleanValue(value=true)" : "BooleanValue(value=false)";
    }
    const MotionLayoutValue& layout = std::get<MotionLayoutValue>(value);
    const std::string_view unit = layout.unit == MotionLayoutUnit::percent
        ? "percent"
        : layout.unit == MotionLayoutUnit::fill ? "fill" : "fixed";
    return "LayoutValue(unit=" + std::string(unit) +
        ", value=" + number_text(layout.value) + ")";
}

} // namespace strata::ui::motion_detail
