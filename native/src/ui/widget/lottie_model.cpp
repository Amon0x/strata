#include "ui/widget/lottie_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource/svg_image.hpp"
#include "ui/lottie/scene.hpp"
#include "ui/readiness.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/semantics.hpp"

namespace strata::ui {

/** One consumer's owner-only latest-frame state; the worker captures only a scene and scalar request. */
struct LottieFrameState final {
    struct Request final {
        std::uint64_t selection;
        double frame;
        Rect viewport;
        Rect clip;
        [[nodiscard]] bool compatible(const Request& other) const noexcept {
            return selection == other.selection && viewport == other.viewport && clip == other.clip;
        }
        [[nodiscard]] friend bool operator==(const Request&, const Request&) = default;
    };

    ~LottieFrameState() { pending.cancel(); }

    void select(const std::string_view next, base::TaskOwner& tasks) {
        if (selection != 0U && source == next)
            return;
        source = next;
        selection = tasks.next_resource_identity();
        pending.cancel();
        scene = {};
        desired.reset();
        completed.reset();
        commands = {};
        clock_time.reset();
        failed = false;
    }

    void adopt() {
        if (!in_flight.has_value() || pending.status() == base::PendingStatus::pending)
            return;
        if (const auto* result = pending.get();
            result != nullptr && desired.has_value() && in_flight->compatible(*desired)) {
            commands = *result;
            completed = in_flight;
        } else if (in_flight->selection == selection && pending.readiness().failure().error != nullptr) {
            failed = true;
        }
        in_flight.reset();
        pending = {};
    }

    void request(base::TaskOwner& tasks, const Request& next) {
        desired = next;
        if (completed.has_value() && !completed->compatible(next)) {
            completed.reset();
            commands = {};
        }
        if (in_flight.has_value() && !in_flight->compatible(next))
            pending.cancel();
        adopt();
        if (in_flight.has_value() || failed || (completed.has_value() && *completed == next))
            return;
        in_flight = next;
        pending = base::Pending<RenderCommandView>::start(
            tasks, base::TaskKind::lottie, base::TaskPriority::frame_critical, source,
            [prepared = scene, next](const base::TaskCancellation&) {
                std::vector<RenderCommand> projected;
                prepared.get()->append_frame(projected, next.frame, next.viewport, next.clip);
                RenderCommandBuffer output;
                output.append(std::move(projected));
                return output.commands();
            });
        adopt();
    }

    void observe(const RetainedNode& node) const {
        node.observe_preparation(scene.readiness());
        node.observe_preparation(pending.readiness());
    }

    std::string source;
    std::uint64_t selection = 0U;
    base::Pending<lottie::Scene> scene;
    base::Pending<RenderCommandView> pending;
    std::optional<Request> desired;
    std::optional<Request> in_flight;
    std::optional<Request> completed;
    RenderCommandView commands;
    std::optional<std::int64_t> clock_time;
    bool failed = false;
};

namespace {

/** The retained bytes the playhead lives in. */
constexpr std::string_view playhead_name = "strata.lottie.playhead";
/** How far before an exclusive segment end its last frame is shown. */
constexpr double end_inset = 1.0e-3;

[[nodiscard]] std::optional<double> finite(const runtime::Value* const value) noexcept {
    if (value == nullptr || value->number() == nullptr || !std::isfinite(*value->number()))
        return std::nullopt;
    return *value->number();
}

[[nodiscard]] bool flag(const runtime::Value* const value, const bool fallback) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] const std::string* text(const runtime::Value* const value) noexcept {
    return value != nullptr ? value->string() : nullptr;
}

[[nodiscard]] std::shared_ptr<const resource::lottie::Animation>
animation(const resource::SvgImageRegistry* const images, const runtime::Value* const source,
          const RetainedNode& node) {
    const std::string* id = text(source);
    if (images == nullptr || images->lottie() == nullptr || id == nullptr || id->empty())
        return nullptr;
    const auto result = images->lottie()->find(*id);
    node.observe_preparation(result.readiness);
    return result.value.value_or(nullptr);
}

[[nodiscard]] LottieFrameState& frame_state(const RetainedNode& node,
                                            const runtime::Value* const source,
                                            lottie::SceneCache& scenes) {
    auto& state = node.lottie_frame_cache();
    if (state == nullptr) {
        state = std::make_shared<LottieFrameState>();
        node.add_cleanup([weak = std::weak_ptr<LottieFrameState>(state)] {
            if (const auto retired = weak.lock(); retired != nullptr) {
                retired->pending.cancel();
                retired->scene = {};
                retired->clock_time.reset();
            }
        });
    }
    const auto* id = text(source);
    state->select(id != nullptr ? std::string_view(*id) : std::string_view{}, scenes.tasks());
    return *state;
}

[[nodiscard]] lottie::SceneCache& scenes(const InputPresentation& input) {
    if (input.lottie_scenes() == nullptr)
        throw std::logic_error("Lottie requires the Surface scene preparation service");
    return *input.lottie_scenes();
}

[[nodiscard]] std::optional<LottiePlayhead> read_playhead(const std::span<const std::byte> bytes) {
    if (bytes.size() != sizeof(LottiePlayhead))
        return std::nullopt;
    LottiePlayhead result;
    std::memcpy(&result, bytes.data(), sizeof(result));
    return result;
}

/** The playhead's position when it starts: the segment's start, or its end playing backwards. */
[[nodiscard]] double start_position(const double speed) noexcept {
    return speed < 0.0 ? 1.0 : 0.0;
}

/** Whether `state` was measured against this source and segment. */
[[nodiscard]] bool current(const LottiePlayhead& state, const std::uint64_t source,
                           const LottieSegment segment) noexcept {
    return (state.flags & LottiePlayhead::started) != 0U && state.source == source &&
           state.first == segment.first && state.last == segment.last;
}

void lottie_defaults(WidgetLayoutDefaultsScope& scope) {
    scope.set("width", runtime::Value(160.0));
    scope.set("height", runtime::Value(160.0));
    scope.intrinsic(160.0, 160.0);
}

[[nodiscard]] double alignment(const runtime::Value* const value) noexcept {
    const std::string* name = text(value);
    if (name == nullptr)
        return 0.5;
    return *name == "START" ? 0.0 : *name == "END" ? 1.0 : 0.5;
}

/** Fits the composition without baking widget opacity into the worker's immutable command run. */
template <typename Scope>
[[nodiscard]] Rect frame_viewport(const Scope& scope, const resource::lottie::Animation& animation,
                                   const Rect bounds) {
    const std::string* fit = text(scope.property("fit"));
    if (fit != nullptr && *fit == "FILL")
        return bounds;
    const double contain = std::min(bounds.width / animation.width, bounds.height / animation.height);
    const double cover = std::max(bounds.width / animation.width, bounds.height / animation.height);
    double scale = contain;
    if (fit != nullptr && *fit == "COVER")
        scale = cover;
    else if (fit != nullptr && *fit == "NONE")
        scale = 1.0;
    else if (fit != nullptr && *fit == "SCALE_DOWN")
        scale = std::min(1.0, contain);
    const double width = animation.width * scale;
    const double height = animation.height * scale;
    return Rect{bounds.x + (bounds.width - width) * alignment(scope.property("alignX")),
                bounds.y + (bounds.height - height) * alignment(scope.property("alignY")),
                width, height};
}

/**
 * Moves the playhead by the time since the last frame, at `speed` times the composition's frame
 * rate, looping or stopping at the segment's end (`onComplete`), and says where it is now
 * (`onProgress`, only when bound). A `progress` that changes seeks
 * there, as scrubbing a video does, and play goes on from it; `playing` (or `autoplay` when
 * `playing` is not given) pauses it where it is and says where (`onPause`). Playing again after
 * the end starts over.
 */
void lottie_advance(WidgetInputScope& scope, const WidgetFrameInfo& frame) {
    const runtime::Value* source = scope.property("source");
    auto& cache = scenes(scope.input());
    auto& preparation = frame_state(scope.node(), source, cache);
    preparation.adopt();
    const std::shared_ptr<const resource::lottie::Animation> parsed =
        animation(scope.images(), source, scope.node());
    if (parsed == nullptr)
        return;
    if (preparation.scene.id() == 0U)
        preparation.scene = cache.find(parsed);
    preparation.observe(scope.node());
    if (preparation.scene.get() == nullptr || preparation.failed)
        return;
    const LottieSegment segment = lottie_segment(*parsed, scope.property("segment"));
    const double speed = finite(scope.property("speed")).value_or(1.0);
    const bool loop = flag(scope.property("loop"), true);
    const std::uint64_t key = preparation.selection;
    const std::optional<LottiePlayhead> stored = read_playhead(scope.retained_bytes(playhead_name));
    LottiePlayhead state = stored.value_or(LottiePlayhead{});
    if (!current(state, key, segment)) {
        state = LottiePlayhead{
            start_position(speed), segment.first, segment.last, key, LottiePlayhead::started, 0U};
    }
    const auto store = [&] {
        if (stored.has_value() && std::memcmp(&*stored, &state, sizeof(state)) == 0)
            return;
        scope.set_retained_bytes(std::string(playhead_name),
                                 std::as_bytes(std::span<const LottiePlayhead>(&state, 1U)),
                                 DirtyReason::paint);
    };
    if (const std::optional<double> progress = finite(scope.property("progress"));
        progress.has_value()) {
        const double target = std::clamp(*progress, 0.0, 1.0);
        if ((state.flags & LottiePlayhead::has_sought) == 0U || state.sought != target) {
            state.position = target;
            state.sought = target;
            state.flags |= LottiePlayhead::has_sought;
            state.flags &= ~LottiePlayhead::complete;
        }
    }
    if (!preparation.clock_time.has_value()) {
        if (const auto* layout = scope.layout(); layout != nullptr && !layout->content_bounds.empty()) {
            const Rect bounds = layout->content_bounds;
            preparation.request(
                cache.tasks(), LottieFrameState::Request{
                    preparation.selection, lottie_frame(segment, state.position),
                    frame_viewport(scope, *parsed, bounds), bounds});
        }
        preparation.observe(scope.node());
        if (preparation.completed.has_value())
            preparation.clock_time = frame.time_nanos;
    }
    const bool playing = flag(scope.property("playing"), flag(scope.property("autoplay"), true));
    const bool was_playing = (state.flags & LottiePlayhead::playing) != 0U;
    if (!playing) {
        state.flags &= ~LottiePlayhead::playing;
        store();
        if (was_playing) {
            using Fields = std::vector<std::pair<std::string, runtime::Value>>;
            scope.value_changed(
                "onPause", "pause",
                runtime::Value(Fields{
                    {"frame", runtime::Value(lottie_frame(segment, state.position))},
                    {"progress", runtime::Value(state.position)},
                }));
        }
        return;
    }
    if ((state.flags & LottiePlayhead::complete) != 0U &&
        (state.flags & LottiePlayhead::playing) == 0U) {
        state.position = start_position(speed);
        state.flags &= ~LottiePlayhead::complete;
    }
    state.flags |= LottiePlayhead::playing;
    if (!preparation.clock_time.has_value()) {
        store();
        return;
    }
    const std::int64_t elapsed = was_playing
        ? std::max<std::int64_t>(0, frame.time_nanos - *preparation.clock_time) : 0;
    preparation.clock_time = frame.time_nanos;
    bool completed = false;
    const double span = std::abs(segment.last - segment.first);
    const bool moving = (state.flags & LottiePlayhead::complete) == 0U && !frame.reduced_motion &&
                        speed != 0.0 && span > 0.0;
    if (moving) {
        const double step =
            static_cast<double>(elapsed) / 1.0e9 * parsed->frame_rate * speed / span;
        double next = state.position + step;
        if (loop) {
            next -= std::floor(next);
        } else if (speed > 0.0 ? next >= 1.0 : next <= 0.0) {
            next = std::clamp(next, 0.0, 1.0);
            state.flags |= LottiePlayhead::complete;
            completed = true;
        }
        state.position = next;
    }
    store();
    if (completed) {
        scope.value_changed("onComplete", "complete",
                            runtime::Value(lottie_frame(segment, state.position)));
    } else if (moving) {
        static_cast<void>(scope.request_frame(WidgetFrameCost::paint));
    }
    if (moving && elapsed > 0 && scope.has_action("onProgress")) {
        using Fields = std::vector<std::pair<std::string, runtime::Value>>;
        scope.value_changed("onProgress", "progress",
                            runtime::Value(Fields{
                                {"frame", runtime::Value(lottie_frame(segment, state.position))},
                                {"progress", runtime::Value(state.position)},
                            }));
    }
}

/** Draws the frame the playhead names (before it moves, `progress`), fitted to the content box. */
void lottie_content(WidgetRenderScope& scope) {
    if (scope.node().lifecycle() != RetainedLifecycle::attached) {
        const auto& held = scope.node().lottie_frame_cache();
        if (held != nullptr && held->completed.has_value())
            scope.append(held->commands);
        return;
    }
    const runtime::Value* source = scope.property("source");
    auto& cache = scenes(scope.input());
    auto& preparation = frame_state(scope.node(), source, cache);
    const std::shared_ptr<const resource::lottie::Animation> parsed =
        animation(scope.images(), source, scope.node());
    const Rect bounds = scope.layout().content_bounds;
    if (parsed == nullptr || bounds.empty())
        return;
    if (preparation.scene.id() == 0U)
        preparation.scene = cache.find(parsed);
    if (preparation.scene.get() == nullptr) {
        preparation.observe(scope.node());
        PreparationReadScope wake(scope.node(), PreparationUse::frame, WidgetFrameCost::paint);
        preparation.observe(scope.node());
        wake.commit();
        return;
    }
    const LottieSegment segment = lottie_segment(*parsed, scope.property("segment"));
    const double speed = finite(scope.property("speed")).value_or(1.0);
    double position = start_position(speed);
    if (const std::optional<LottiePlayhead> state =
            read_playhead(scope.node().retained_bytes(playhead_name));
        state.has_value() && current(*state, preparation.selection, segment)) {
        position = state->position;
    } else if (const std::optional<double> progress = finite(scope.property("progress"));
               progress.has_value()) {
        position = std::clamp(*progress, 0.0, 1.0);
    }
    const double frame = lottie_frame(segment, position);

    const Rect viewport = frame_viewport(scope, *parsed, bounds);
    preparation.request(cache.tasks(),
                        LottieFrameState::Request{preparation.selection, frame, viewport, bounds});
    preparation.observe(scope.node());
    {
        PreparationReadScope wake(scope.node(), PreparationUse::frame, WidgetFrameCost::paint);
        preparation.observe(scope.node());
        wake.commit();
    }
    if (preparation.completed.has_value()) {
        if (!preparation.clock_time.has_value())
            preparation.clock_time = scope.input().frame_time_nanos();
        scope.append(preparation.commands);
    }
}

} // namespace

LottieSegment lottie_segment(const resource::lottie::Animation& animation,
                             const runtime::Value* const segment) noexcept {
    LottieSegment result{animation.in_frame, animation.out_frame};
    if (segment == nullptr || segment->list() == nullptr || segment->list()->values.size() != 2U)
        return result;
    const std::optional<double> first = finite(&segment->list()->values[0]);
    const std::optional<double> last = finite(&segment->list()->values[1]);
    if (!first.has_value() || !last.has_value())
        return result;
    const double a = std::clamp(*first, animation.in_frame, animation.out_frame);
    const double b = std::clamp(*last, animation.in_frame, animation.out_frame);
    if (a == b)
        return result;
    return LottieSegment{a, b};
}

double lottie_frame(const LottieSegment segment, const double position) noexcept {
    const double low = std::min(segment.first, segment.last);
    const double high = std::max(segment.first, segment.last);
    const double frame =
        segment.first + (segment.last - segment.first) * std::clamp(position, 0.0, 1.0);
    return std::clamp(frame, low, std::max(low, high - end_inset));
}

void register_lottie_widget(WidgetRegistry& registry) {
    registry.register_describe_phase("Lottie",
                                     WidgetDescribePhase{.layout_defaults = &lottie_defaults});
    registry.register_semantics_phase(
        "Lottie", WidgetSemanticsPhase{"image", {}, &label_or_key, false, false});
    registry.register_present_phase(
        "Lottie", WidgetPresentPhase{&lottie_content, nullptr, nullptr, nullptr, false});
    registry.register_style_schema("Lottie", ThemeStyleSchema{.chromeless = true});
    registry.register_frame_phase("Lottie", WidgetFramePhase{&lottie_advance, true});
}

} // namespace strata::ui
