#include "ui/widget/lottie_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource/svg_image.hpp"
#include "ui/lottie/scene.hpp"
#include "ui/tree.hpp"
#include "ui/widget/description.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/presentation.hpp"
#include "ui/widget/semantics.hpp"

namespace strata::ui {
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
animation(const resource::SvgImageRegistry* const images, const runtime::Value* const source) {
    const std::string* id = text(source);
    if (images == nullptr || images->lottie() == nullptr || id == nullptr || id->empty())
        return nullptr;
    return images->lottie()->find(*id);
}

[[nodiscard]] std::uint64_t source_key(const runtime::Value* const source) noexcept {
    const std::string* id = text(source);
    return id != nullptr ? std::hash<std::string_view>{}(*id) : 0U;
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

/**
 * Moves the playhead by the time since the last frame, at `speed` times the composition's frame
 * rate, looping or stopping at the segment's end (`onComplete`). A bound `progress` places it
 * instead; `playing` (or `autoplay` when `playing` is not given) pauses it where it is. Playing
 * again after the end starts over.
 */
void lottie_advance(WidgetInputScope& scope, const WidgetFrameInfo& frame) {
    const runtime::Value* source = scope.property("source");
    const std::shared_ptr<const resource::lottie::Animation> parsed =
        animation(scope.images(), source);
    if (parsed == nullptr)
        return;
    const LottieSegment segment = lottie_segment(*parsed, scope.property("segment"));
    const double speed = finite(scope.property("speed")).value_or(1.0);
    const bool loop = flag(scope.property("loop"), true);
    const std::uint64_t key = source_key(source);
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
        state.position = std::clamp(*progress, 0.0, 1.0);
        state.flags &= ~(LottiePlayhead::complete | LottiePlayhead::playing);
        store();
        return;
    }
    const bool playing = flag(scope.property("playing"), flag(scope.property("autoplay"), true));
    if (!playing) {
        state.flags &= ~LottiePlayhead::playing;
        store();
        return;
    }
    if ((state.flags & LottiePlayhead::complete) != 0U &&
        (state.flags & LottiePlayhead::playing) == 0U) {
        state.position = start_position(speed);
        state.flags &= ~LottiePlayhead::complete;
    }
    state.flags |= LottiePlayhead::playing;
    bool completed = false;
    const double span = std::abs(segment.last - segment.first);
    const bool moving = (state.flags & LottiePlayhead::complete) == 0U && !frame.reduced_motion &&
                        speed != 0.0 && span > 0.0;
    if (moving) {
        const double step =
            static_cast<double>(frame.delta_nanos) / 1.0e9 * parsed->frame_rate * speed / span;
        double next = state.position + step;
        if (loop) {
            next -= std::floor(next);
        } else if (next >= 1.0 || next <= 0.0) {
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
}

[[nodiscard]] double alignment(const runtime::Value* const value) noexcept {
    const std::string* name = text(value);
    if (name == nullptr)
        return 0.5;
    return *name == "START" ? 0.0 : *name == "END" ? 1.0 : 0.5;
}

/** Draws the frame the playhead (or a bound `progress`) names, fitted to the content box. */
void lottie_content(WidgetRenderScope& scope) {
    const runtime::Value* source = scope.property("source");
    const std::shared_ptr<const resource::lottie::Animation> parsed =
        animation(scope.images(), source);
    const Rect bounds = scope.layout().content_bounds;
    if (parsed == nullptr || bounds.empty())
        return;
    const LottieSegment segment = lottie_segment(*parsed, scope.property("segment"));
    const double speed = finite(scope.property("speed")).value_or(1.0);
    double position = start_position(speed);
    if (const std::optional<double> progress = finite(scope.property("progress"));
        progress.has_value()) {
        position = std::clamp(*progress, 0.0, 1.0);
    } else if (const std::optional<LottiePlayhead> state =
                   read_playhead(scope.node().retained_bytes(playhead_name));
               state.has_value() && current(*state, source_key(source), segment)) {
        position = state->position;
    }
    const double frame = lottie_frame(segment, position);

    // `fit` as Image has it: FILL stretches, CONTAIN shows all, COVER fills by cropping, NONE is
    // the natural size and SCALE_DOWN never enlarges.
    const std::string* fit = text(scope.property("fit"));
    const double contain = std::min(bounds.width / parsed->width, bounds.height / parsed->height);
    const double cover = std::max(bounds.width / parsed->width, bounds.height / parsed->height);
    Rect viewport = bounds;
    if (fit == nullptr || *fit != "FILL") {
        double scale = contain;
        if (fit != nullptr && *fit == "COVER")
            scale = cover;
        else if (fit != nullptr && *fit == "NONE")
            scale = 1.0;
        else if (fit != nullptr && *fit == "SCALE_DOWN")
            scale = std::min(1.0, contain);
        const double width = parsed->width * scale;
        const double height = parsed->height * scale;
        viewport = Rect{bounds.x + (bounds.width - width) * alignment(scope.property("alignX")),
                        bounds.y + (bounds.height - height) * alignment(scope.property("alignY")),
                        width, height};
    }
    std::vector<RenderCommand> commands;
    lottie::append_frame(commands, *parsed, frame, viewport, bounds);
    for (RenderCommand& command : commands)
        scope.append(std::move(command));
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
