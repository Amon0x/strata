#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "ui/motion/model.hpp"

namespace strata::ui::motion_detail {

struct TimelineSample final {
    MotionComputedValues computed;
    double raw_progress = 0.0;
    bool running = false;
    bool finished = false;
    MotionDirection direction = MotionDirection::forward;
};

class TimelinePlayer final {
public:
    [[nodiscard]] TimelineSample advance(
        const CompiledMotion& animation,
        bool active,
        MotionDirection active_direction,
        std::int64_t now_nanos,
        bool reduced_motion
    );
    [[nodiscard]] TimelineSample snap(
        const CompiledMotion& animation,
        bool active,
        MotionDirection active_direction,
        std::int64_t now_nanos
    );
    [[nodiscard]] bool initialized() const noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] double progress() const noexcept;
    [[nodiscard]] MotionDirection direction() const noexcept;
    [[nodiscard]] TimelinePlayer continued() const;

private:
    const CompiledMotion* animation_ = nullptr;
    std::optional<std::int64_t> started_at_nanos_;
    bool initialized_ = false;
    bool active_ = false;
    bool running_ = false;
    double raw_progress_ = 0.0;
    MotionDirection direction_ = MotionDirection::forward;
    bool continuation_pending_ = false;
};

struct MotionTimelineSample final {
    double progress = 0.0;
    bool running = false;
};

/** Retained property-neutral clock. Reduced motion pauses the presented value without catch-up. */
class MotionTimelinePlayer final {
public:
    [[nodiscard]] MotionTimelineSample advance(
        const MotionTimelineSpec& spec,
        std::int64_t now_nanos,
        bool reduced_motion
    );
    [[nodiscard]] const MotionTimelineSample& sample() const noexcept;

private:
    std::optional<MotionTimelineSpec> spec_;
    std::optional<std::int64_t> last_frame_nanos_;
    MotionTimelineSample sample_;
};

struct TargetSample final {
    MotionValue value = 0.0;
    MotionValue target = 0.0;
    double progress = 1.0;
    bool running = false;
    bool snapped_by_reduced_motion = false;
};

class TargetPlayer final {
public:
    [[nodiscard]] TargetSample advance(
        const MotionValue& target,
        const MotionTiming& timing,
        std::int64_t now_nanos,
        bool reduced_motion
    );
    [[nodiscard]] const TargetSample& sample() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;

private:
    [[nodiscard]] TargetSample sample_at(std::int64_t now_nanos) const;
    void snap(const MotionValue& target, bool reduced_motion);

    std::optional<MotionValue> start_;
    std::optional<MotionValue> target_;
    std::optional<std::int64_t> started_at_nanos_;
    MotionTiming timing_;
    TargetSample sample_;
};

[[nodiscard]] std::string motion_value_text(const MotionValue& value);

} // namespace strata::ui::motion_detail
