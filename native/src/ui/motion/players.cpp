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
    const MotionDirection direction,
    const std::int64_t extra_delay_nanos = 0
) {
    const std::int64_t elapsed = std::max<std::int64_t>(0, elapsed_nanos);
    const std::int64_t maximum = std::numeric_limits<std::int64_t>::max();
    const std::int64_t delay = animation.timing.delay_nanos > maximum - extra_delay_nanos
                                   ? maximum
                                   : animation.timing.delay_nanos + extra_delay_nanos;
    const bool delayed = elapsed < delay;
    const std::int64_t active_elapsed = delayed ? 0 : elapsed - delay;
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
    const bool reduced_motion,
    const std::int64_t start_delay_nanos
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
        // Only a start from rest waits for its stagger; reversals continue from their progress.
        start_delay_nanos_ = preserve ? 0 : std::max<std::int64_t>(0, start_delay_nanos);
        running_ = true;
    }
    initialized_ = true;
    TimelineSample result = evaluate(
        animation, now_nanos - *started_at_nanos_, direction_, start_delay_nanos_
    );
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
    start_delay_nanos_ = 0;
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

namespace {

/** A value's numbers, as a spring moves them; nothing for a value a spring cannot move. */
[[nodiscard]] std::optional<std::vector<double>> spring_components(const MotionValue& value) {
    if (const double* number = std::get_if<double>(&value))
        return std::vector<double>{*number};
    if (const auto* color = std::get_if<runtime::ColorValue>(&value)) {
        return std::vector<double>{
            static_cast<double>(color->red), static_cast<double>(color->green),
            static_cast<double>(color->blue), static_cast<double>(color->alpha)};
    }
    if (const auto* layout = std::get_if<MotionLayoutValue>(&value))
        return std::vector<double>{layout->value};
    if (const auto* numbers = std::get_if<MotionNumbers>(&value);
        numbers != nullptr && numbers->values != nullptr)
        return *numbers->values;
    return std::nullopt;
}

/** The value of `target`'s kind at `components`. */
[[nodiscard]] MotionValue spring_value(const MotionValue& target,
                                       const std::vector<double>& components) {
    if (std::holds_alternative<double>(target))
        return components[0];
    if (std::holds_alternative<runtime::ColorValue>(target)) {
        const auto channel = [&components](const std::size_t index) {
            return static_cast<std::uint8_t>(std::clamp(std::round(components[index]), 0.0, 255.0));
        };
        return runtime::ColorValue{channel(0U), channel(1U), channel(2U), channel(3U)};
    }
    if (const auto* layout = std::get_if<MotionLayoutValue>(&target))
        return MotionLayoutValue{layout->unit, components[0]};
    return MotionNumbers{std::make_shared<const std::vector<double>>(components)};
}

} // namespace

void spring_step(const double k, const double c, const double seconds, double& offset,
                 double& velocity) noexcept {
    const double omega = std::sqrt(k);
    const double zeta = c / (2.0 * omega);
    const double x0 = offset;
    const double v0 = velocity;
    if (zeta < 1.0) {
        const double damped = omega * std::sqrt(1.0 - zeta * zeta);
        const double decay = std::exp(-zeta * omega * seconds);
        const double cosine = std::cos(damped * seconds);
        const double sine = std::sin(damped * seconds);
        offset = decay * (x0 * cosine + (v0 + zeta * omega * x0) / damped * sine);
        velocity = decay * (v0 * cosine - (zeta * omega * v0 + omega * omega * x0) / damped * sine);
    } else if (zeta == 1.0) {
        const double decay = std::exp(-omega * seconds);
        const double rate = v0 + omega * x0;
        offset = (x0 + rate * seconds) * decay;
        velocity = (v0 - omega * rate * seconds) * decay;
    } else {
        const double root = omega * std::sqrt(zeta * zeta - 1.0);
        const double fast = -zeta * omega - root;
        const double slow = -zeta * omega + root;
        const double a = (v0 - slow * x0) / (fast - slow);
        const double b = x0 - a;
        const double fast_decay = std::exp(fast * seconds);
        const double slow_decay = std::exp(slow * seconds);
        offset = a * fast_decay + b * slow_decay;
        velocity = a * fast * fast_decay + b * slow * slow_decay;
    }
}

bool TargetPlayer::advance_spring(const MotionValue& next_target, const std::int64_t now_nanos) {
    const std::optional<std::vector<double>> goal = spring_components(next_target);
    // Layout values in different units do not blend: they take the curve, which snaps.
    const auto* target_layout = std::get_if<MotionLayoutValue>(&next_target);
    const auto* shown_layout = std::get_if<MotionLayoutValue>(&sample_.value);
    if (!goal.has_value() || (target_layout != nullptr && shown_layout != nullptr &&
                              target_layout->unit != shown_layout->unit)) {
        spring_.reset();
        return false;
    }
    const bool retarget = !spring_.has_value() || spring_->target != *goal ||
                          !target_.has_value() || *target_ != next_target;
    if (!spring_.has_value() || spring_->position.size() != goal->size()) {
        // Starting from where the value shows now, at rest.
        const std::optional<std::vector<double>> from = spring_components(sample_.value);
        if (!from.has_value() || from->size() != goal->size()) {
            spring_.reset();
            return false;
        }
        spring_ = Spring{*from, std::vector<double>(goal->size(), 0.0), *goal, 0.0, 0.0, now_nanos};
    }
    Spring& spring = *spring_;
    if (retarget) {
        spring.target = *goal;
        double distance = 0.0;
        for (std::size_t index = 0U; index < goal->size(); ++index)
            distance = std::max(distance, std::abs(spring.position[index] - spring.target[index]));
        spring.travel = std::max(distance, 1.0e-9);
        spring.rest = std::max(distance, 1.0) * 1.0e-3;
    }
    target_ = next_target;
    start_ = next_target;
    started_at_nanos_.reset();
    // Long gaps are stepped in pieces, so a stalled frame cannot wind the spring up.
    double remaining =
        static_cast<double>(std::max<std::int64_t>(0, now_nanos - spring.last_nanos)) / 1.0e9;
    spring.last_nanos = now_nanos;
    const double k = timing_.easing.x1;
    // A colour settles without overshoot: past its target it would pass through other hues.
    const double c = std::holds_alternative<runtime::ColorValue>(next_target)
                         ? std::max(timing_.easing.y1, 2.0 * std::sqrt(k))
                         : timing_.easing.y1;
    while (remaining > 0.0) {
        const double step = std::min(remaining, 0.05);
        remaining -= step;
        for (std::size_t index = 0U; index < goal->size(); ++index) {
            double offset = spring.position[index] - spring.target[index];
            spring_step(k, c, step, offset, spring.velocity[index]);
            spring.position[index] = spring.target[index] + offset;
        }
    }
    double distance = 0.0;
    double speed = 0.0;
    for (std::size_t index = 0U; index < goal->size(); ++index) {
        distance = std::max(distance, std::abs(spring.position[index] - spring.target[index]));
        speed = std::max(speed, std::abs(spring.velocity[index]));
    }
    // At rest within a thousandth of its travel and barely moving: there, exactly.
    if (distance <= spring.rest && speed <= spring.rest * 10.0) {
        spring_.reset();
        sample_ = TargetSample{next_target, next_target, 1.0, false, false};
        return true;
    }
    sample_ = TargetSample{spring_value(next_target, spring.position), next_target,
                           std::clamp(1.0 - distance / spring.travel, 0.0, 1.0), true, false};
    return true;
}

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
        spring_.reset();
        snap(next_target, true);
        return sample_;
    }
    if (timing.easing.kind == MotionEasingKind::spring && timing.delay_nanos == 0) {
        // A spring already at its target stays at rest until the target moves.
        if (!spring_.has_value() && *target_ == next_target && timing_ == timing &&
            !sample_.running) {
            return sample_;
        }
        timing_ = timing;
        if (advance_spring(next_target, now_nanos))
            return sample_;
    } else {
        spring_.reset();
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
    spring_.reset();
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
    if (const auto* numbers = std::get_if<MotionNumbers>(&value)) {
        return "Numbers(count=" +
               std::to_string(numbers->values != nullptr ? numbers->values->size() : 0U) + ")";
    }
    const MotionLayoutValue& layout = std::get<MotionLayoutValue>(value);
    const std::string_view unit = layout.unit == MotionLayoutUnit::percent
        ? "percent"
        : layout.unit == MotionLayoutUnit::fill ? "fill" : "fixed";
    return "LayoutValue(unit=" + std::string(unit) +
        ", value=" + number_text(layout.value) + ")";
}

} // namespace strata::ui::motion_detail
