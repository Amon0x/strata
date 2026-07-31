#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "ui/motion/model.hpp"
#include "ui/tree.hpp"

namespace strata::runtime { class RuntimeUnit; }

namespace strata::ui {

class InputRouter;
struct LayoutResult;

struct DisclosureMotionSpec final {
    bool expanded = false;
    double collapsed_extent = 0.0;
    MotionTiming timing{
        180'000'000, 0, "cubic-in-out", {}, false, MotionFillMode::both,
    };
};

struct NormalizedMotionSample final {
    double current = 0.0;
    double target = 0.0;
    double progress = 1.0;
    bool running = false;
    bool snapped_by_reduced_motion = false;
};

struct MotionInspectionChannel final {
    std::string id;
    std::string source;
    MotionDirection direction = MotionDirection::forward;
    double progress = 0.0;
    std::optional<std::string> current_value;
    std::optional<std::string> target_value;
    std::vector<std::string> affected_properties;
    bool running = false;
    bool snapped_by_reduced_motion = false;
    std::optional<MotionTrigger> trigger;
    std::optional<MotionInteraction> interaction;
};

struct MotionFrameCounters final {
    std::size_t evaluated_nodes = 0U;
    std::size_t mutated_nodes = 0U;
    std::size_t running_players = 0U;
};

struct MotionMoveOrigin final {
    std::uint64_t identity = 0U;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double ancestor_scroll_x = 0.0;
    double ancestor_scroll_y = 0.0;
};

/** Returns the effective retained disclosure contract for any widget that declares one. */
[[nodiscard]] std::optional<DisclosureMotionSpec> disclosure_motion(const RetainedNode& node);

/** Descendants of a collapsed disclosure are disabled; the disclosure header itself remains live. */
[[nodiscard]] bool motion_input_eligible(const RetainedNode& node) noexcept;

/**
 * Surface-owned typed motion graph. Portable declarations are compiled once per active unit;
 * retained players are keyed by node identity and channel id, making retargeting interruptible.
 */
class MotionRuntime final {
public:
    MotionRuntime();
    ~MotionRuntime();
    MotionRuntime(const MotionRuntime&) = delete;
    MotionRuntime& operator=(const MotionRuntime&) = delete;

    /** Binds the immutable declaration catalog before reconciliation asks lifecycle questions. */
    void bind(const std::shared_ptr<const runtime::RuntimeUnit>& unit);
    void set_supplemental(std::map<std::string, CompiledMotion, std::less<>> motions);
    /** True when this removal boundary contains at least one executable exit attachment. */
    [[nodiscard]] bool should_retain_for_exit(const RetainedNode& node);
    /** True after every exit attachment in an EXITING subtree reached its terminal sample. */
    [[nodiscard]] bool exit_finished(const RetainedNode& node);
    [[nodiscard]] std::vector<MotionMoveOrigin> capture_move_origins(
        const RetainedTree& tree,
        const LayoutResult& layout
    );
    /** Applies FLIP-style move tracks from the previous and newly arranged retained bounds. */
    void apply_move_transitions(
        RetainedTree& tree,
        const std::vector<MotionMoveOrigin>& origins,
        const LayoutResult& after,
        std::int64_t frame_time_nanos,
        bool reduced_motion
    );

    [[nodiscard]] MotionFrameCounters advance(
        RetainedTree& tree,
        const std::shared_ptr<const runtime::RuntimeUnit>& unit,
        const InputRouter& input,
        std::int64_t frame_time_nanos,
        bool reduced_motion
    );
    /**
     * Initializes or retargets only nodes whose retained dirty generation was not sampled yet.
     * Existing active clocks are not advanced; the supplied timestamp is the current frame sample.
     */
    [[nodiscard]] MotionFrameCounters discover(
        RetainedTree& tree,
        const std::shared_ptr<const runtime::RuntimeUnit>& unit,
        const InputRouter& input,
        std::int64_t frame_time_nanos,
        bool reduced_motion
    );
    [[nodiscard]] const MotionComputedValues* computed_values(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] const std::vector<MotionInspectionChannel>* inspection_channels(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] const NormalizedMotionSample* disclosure_sample(
        std::uint64_t identity
    ) const noexcept;
    [[nodiscard]] std::size_t active_count() const noexcept;
    void clear() noexcept;

private:
    [[nodiscard]] MotionFrameCounters evaluate(
        RetainedTree& tree,
        const std::shared_ptr<const runtime::RuntimeUnit>& unit,
        const InputRouter& input,
        std::int64_t frame_time_nanos,
        bool reduced_motion,
        bool advance_active_players
    );
    struct Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace strata::ui
