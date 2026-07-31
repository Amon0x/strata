#include "ui/behavior/registry.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"
#include "ui/behavior/input.hpp"
#include "ui/tree.hpp"
#include "ui/widget/input.hpp"

namespace strata::ui {
namespace {

constexpr std::string_view activate_session = "strata.activate.session";
constexpr std::string_view activate_click = "strata.activate.click";
constexpr std::string_view movement_offset = "strata.movement.offset";
constexpr std::string_view movement_session = "strata.movement.session";
constexpr std::string_view runtime_size = "strata.gesture.runtimeSize";
constexpr std::string_view resize_session = "strata.gesture.resizeSession";
constexpr std::string_view split_ratio = "strata.gesture.splitRatio";
constexpr std::string_view split_session = "strata.gesture.splitSession";

[[nodiscard]] runtime::Value object(
    std::initializer_list<std::pair<std::string, runtime::Value>> fields
) {
    return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>(fields));
}

[[nodiscard]] const runtime::Value* field(
    const runtime::Value* value,
    const std::string_view name
) noexcept {
    return value != nullptr ? value->field(name) : nullptr;
}

[[nodiscard]] double number(
    const runtime::Value* value,
    const double fallback
) noexcept {
    return value != nullptr && value->number() != nullptr && std::isfinite(*value->number())
        ? *value->number()
        : fallback;
}

[[nodiscard]] std::int64_t integer(
    const runtime::Value* value,
    const std::int64_t fallback
) noexcept {
    const double resolved = number(value, static_cast<double>(fallback));
    if (resolved < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        resolved > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return fallback;
    }
    return static_cast<std::int64_t>(std::trunc(resolved));
}

[[nodiscard]] bool boolean(
    const runtime::Value* value,
    const bool fallback
) noexcept {
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

[[nodiscard]] std::string text(
    const runtime::Value* value,
    std::string fallback = {}
) {
    if (value != nullptr && value->string() != nullptr) return *value->string();
    if (value != nullptr && value->key() != nullptr) return value->key()->value;
    return fallback;
}

[[nodiscard]] std::string normalized(std::string value) {
    std::ranges::transform(value, value.begin(), [](const unsigned char character) {
        return character == '_' ? '-' : static_cast<char>(std::tolower(character));
    });
    return value;
}

[[nodiscard]] std::int32_t pointer_button(const BehaviorInputScope& scope) {
    const std::string value = normalized(text(scope.option("button"), "left"));
    if (value == "right") return 1;
    if (value == "middle") return 2;
    return 0;
}

[[nodiscard]] bool modifiers_match(const BehaviorInputScope& scope) noexcept {
    const runtime::Value* required = scope.option("modifiers");
    const KeyModifiers& actual = scope.modifiers();
    return actual.shift == boolean(field(required, "shift"), false) &&
           actual.control == boolean(field(required, "control"), false) &&
           actual.alt == boolean(field(required, "alt"), false) &&
           actual.super_key == boolean(field(required, "super"), false);
}

[[nodiscard]] bool pointer_policy(const BehaviorInputScope& scope) {
    return normalized(text(scope.option("policy"), "pointer-and-keyboard")) != "keyboard-only";
}

[[nodiscard]] bool keyboard_policy(const BehaviorInputScope& scope) {
    return normalized(text(scope.option("policy"), "pointer-and-keyboard")) != "pointer-only";
}

[[nodiscard]] std::optional<std::int64_t> long_press_duration(
    const BehaviorInputScope& scope
) noexcept {
    const runtime::Value* value = scope.option("longPress");
    if (value == nullptr || value->kind() == runtime::ValueKind::null_value) return std::nullopt;
    if (value->duration() != nullptr && value->duration()->nanoseconds > 0) {
        return value->duration()->nanoseconds;
    }
    const std::int64_t nanos = integer(value, 0);
    return nanos > 0 ? std::optional<std::int64_t>(nanos) : std::nullopt;
}

[[nodiscard]] bool activation_pointer(BehaviorInputScope& scope) {
    const PointerInputEvent* event = scope.pointer();
    if (event == nullptr || !pointer_policy(scope)) return false;
    const bool include_descendants = boolean(scope.option("includeDescendants"), false);
    const bool passive_descendant = scope.passive_pointer_descendant();
    const bool accepted_phase = scope.phase() == BehaviorInputEventPhase::target ||
        (include_descendants && scope.phase() == BehaviorInputEventPhase::capture) ||
        (!include_descendants && passive_descendant &&
         scope.phase() == BehaviorInputEventPhase::bubble);
    if (!accepted_phase || event->button != pointer_button(scope) || !modifiers_match(scope)) {
        return false;
    }
    const std::optional<std::int64_t> duration = long_press_duration(scope);
    if (event->type == PointerEventType::press) {
        if (duration.has_value()) {
            scope.set_retained(
                std::string(activate_session),
                object({
                    {"pointerId", runtime::Value(static_cast<double>(event->pointer_id))},
                    {"startedAt", runtime::Value(static_cast<double>(
                        event->timestamp_nanos > 0 ? event->timestamp_nanos : scope.widget_scope().frame_time_nanos()
                    ))},
                    {"emitted", runtime::Value(false)},
                }),
                DirtyReason::input
            );
        }
        // The router must still establish generic capture/press state after behavior routing.
        return false;
    }
    if (event->type == PointerEventType::move) {
        if (duration.has_value() && scope.press_moved_beyond_slop()) {
            scope.set_retained(std::string(activate_session), runtime::Value{}, DirtyReason::input);
        }
        return false;
    }
    if (event->type == PointerEventType::cancel) {
        scope.set_retained(std::string(activate_session), runtime::Value{}, DirtyReason::input);
        return false;
    }
    if (event->type != PointerEventType::release || duration.has_value() ||
        !scope.press_matches(include_descendants || passive_descendant) ||
        scope.press_moved_beyond_slop() ||
        scope.long_press_emitted() ||
        scope.gesture_claim_state() != GestureClaimState::unclaimed) {
        return false;
    }

    const std::int64_t now = event->timestamp_nanos > 0
        ? event->timestamp_nanos
        : scope.widget_scope().frame_time_nanos();
    const runtime::Value* previous = scope.retained(activate_click);
    const double previous_x = number(field(previous, "x"), std::numeric_limits<double>::infinity());
    const double previous_y = number(field(previous, "y"), std::numeric_limits<double>::infinity());
    const std::int64_t previous_time = integer(field(previous, "time"), 0);
    const std::int64_t elapsed = now >= previous_time ? now - previous_time
                                                       : std::numeric_limits<std::int64_t>::max();
    const bool same_run = previous_time > 0 && elapsed <= 500'000'000 &&
        std::abs(event->position.x - previous_x) <= 4.0 &&
        std::abs(event->position.y - previous_y) <= 4.0;
    const std::int64_t previous_count = integer(field(previous, "count"), 0);
    const std::int64_t click_count = same_run ? previous_count % 3 + 1 : 1;
    scope.set_retained(
        std::string(activate_click),
        object({
            {"x", runtime::Value(event->position.x)},
            {"y", runtime::Value(event->position.y)},
            {"time", runtime::Value(static_cast<double>(now))},
            {"count", runtime::Value(static_cast<double>(click_count))},
        }),
        DirtyReason::input
    );
    const std::int64_t required_clicks = std::clamp<std::int64_t>(
        integer(scope.option("clickCount"), 1), 1, 3
    );
    return click_count == required_clicks && scope.emit("activated");
}

[[nodiscard]] bool activation_key(BehaviorInputScope& scope) {
    if (scope.phase() != BehaviorInputEventPhase::target || !keyboard_policy(scope) ||
        !modifiers_match(scope)) {
        return false;
    }
    return (scope.key() == "enter" || scope.key() == "space") && scope.emit("activated");
}

[[nodiscard]] bool activation_advance(BehaviorInputScope& scope) {
    const std::optional<std::int64_t> duration = long_press_duration(scope);
    const runtime::Value* session = scope.retained(activate_session);
    if (!duration.has_value() || session == nullptr || session->object() == nullptr ||
        boolean(field(session, "emitted"), false)) {
        return false;
    }
    if (scope.press_moved_beyond_slop() ||
        scope.gesture_claim_state() != GestureClaimState::unclaimed) {
        scope.set_retained(std::string(activate_session), runtime::Value{}, DirtyReason::input);
        return false;
    }
    const std::int64_t started = integer(field(session, "startedAt"), 0);
    const std::int64_t now = scope.widget_scope().frame_time_nanos();
    if (started <= 0 || now < started || now - started < *duration) return false;
    scope.set_retained(
        std::string(activate_session),
        object({
            {"pointerId", runtime::Value(number(field(session, "pointerId"), 0.0))},
            {"startedAt", runtime::Value(static_cast<double>(started))},
            {"emitted", runtime::Value(true)},
        }),
        DirtyReason::input
    );
    scope.mark_long_press_emitted();
    return scope.emit("activated");
}

[[nodiscard]] Point retained_point(
    const runtime::Value* value,
    const Point fallback = {}
) noexcept {
    return Point{number(field(value, "x"), fallback.x), number(field(value, "y"), fallback.y)};
}

[[nodiscard]] Point clamp_movement(
    BehaviorInputScope& scope,
    Point offset
) noexcept {
    if (!boolean(scope.option("clampToBounds"), true) ||
        normalized(text(scope.option("bounds"), "parent")) == "none") {
        return offset;
    }
    const LayoutRecord* node_layout = scope.layout();
    RetainedNode* parent = scope.node().parent();
    const LayoutRecord* parent_layout = parent != nullptr ? scope.layout(*parent) : nullptr;
    if (node_layout == nullptr || parent_layout == nullptr) return offset;
    const Rect bounds = parent_layout->viewport.value_or(parent_layout->content_bounds);
    const double min_x = bounds.x - node_layout->bounds.x;
    const double max_x = bounds.right() - node_layout->bounds.right();
    const double min_y = bounds.y - node_layout->bounds.y;
    const double max_y = bounds.bottom() - node_layout->bounds.bottom();
    offset.x = std::clamp(offset.x, std::min(min_x, max_x), std::max(min_x, max_x));
    offset.y = std::clamp(offset.y, std::min(min_y, max_y), std::max(min_y, max_y));
    return offset;
}

[[nodiscard]] bool movement_phase(const BehaviorInputScope& scope) {
    const std::string handle_key = text(scope.option("handleKey"));
    if (handle_key.empty()) {
        return scope.phase() == BehaviorInputEventPhase::target ||
            (scope.phase() == BehaviorInputEventPhase::capture &&
             boolean(scope.option("includeDescendants"), true));
    }
    if (scope.phase() != BehaviorInputEventPhase::capture) return false;
    WidgetInputScope widget = scope.widget_scope();
    for (RetainedNode* current = widget.pointer_target(); current != nullptr && current != &scope.node();
         current = current->parent()) {
        if (current->description().key == handle_key) return true;
    }
    return false;
}

[[nodiscard]] bool movable_pointer(BehaviorInputScope& scope) {
    const PointerInputEvent* event = scope.pointer();
    if (event == nullptr || !boolean(scope.option("enabled"), true)) return false;
    const runtime::Value* session = scope.retained(movement_session);
    if (event->type == PointerEventType::press) {
        if (!movement_phase(scope) || event->button != 0) return false;
        const Point start_offset = retained_point(scope.retained(movement_offset));
        scope.set_retained(
            std::string(movement_session),
            object({
                {"pointerId", runtime::Value(static_cast<double>(event->pointer_id))},
                {"startX", runtime::Value(event->position.x)},
                {"startY", runtime::Value(event->position.y)},
                {"offsetX", runtime::Value(start_offset.x)},
                {"offsetY", runtime::Value(start_offset.y)},
                {"dragged", runtime::Value(false)},
            }),
            DirtyReason::input
        );
        return false;
    }
    if (session == nullptr || session->object() == nullptr ||
        integer(field(session, "pointerId"), -1) != event->pointer_id) {
        return false;
    }
    if (event->type == PointerEventType::cancel) {
        static_cast<void>(scope.cancel_gesture());
        scope.set_retained(
            std::string(movement_offset),
            object({
                {"x", runtime::Value(number(field(session, "offsetX"), 0.0))},
                {"y", runtime::Value(number(field(session, "offsetY"), 0.0))},
            }),
            DirtyReason::input
        );
        scope.set_retained(std::string(movement_session), runtime::Value{}, DirtyReason::input);
        return true;
    }
    if (event->type == PointerEventType::move) {
        if (!scope.press_moved_beyond_slop()) return false;
        if (!scope.claim_gesture()) return false;
        Point next{
            number(field(session, "offsetX"), 0.0) + event->position.x - number(field(session, "startX"), event->position.x),
            number(field(session, "offsetY"), 0.0) + event->position.y - number(field(session, "startY"), event->position.y),
        };
        next = clamp_movement(scope, next);
        scope.set_retained(
            std::string(movement_offset),
            object({{"x", runtime::Value(next.x)}, {"y", runtime::Value(next.y)}}),
            DirtyReason::input
        );
        scope.set_retained(
            std::string(movement_session),
            object({
                {"pointerId", runtime::Value(static_cast<double>(event->pointer_id))},
                {"startX", runtime::Value(number(field(session, "startX"), event->position.x))},
                {"startY", runtime::Value(number(field(session, "startY"), event->position.y))},
                {"offsetX", runtime::Value(number(field(session, "offsetX"), 0.0))},
                {"offsetY", runtime::Value(number(field(session, "offsetY"), 0.0))},
                {"dragged", runtime::Value(true)},
            }),
            DirtyReason::input
        );
        return true;
    }
    if (event->type == PointerEventType::release) {
        const bool dragged = boolean(field(session, "dragged"), false);
        scope.set_retained(std::string(movement_session), runtime::Value{}, DirtyReason::input);
        return dragged;
    }
    return false;
}

struct ResizeSpec final {
    bool horizontal = true;
    bool vertical = true;
    double min_width = 32.0;
    double min_height = 24.0;
    double max_width = std::numeric_limits<double>::infinity();
    double max_height = std::numeric_limits<double>::infinity();
};

[[nodiscard]] ResizeSpec resize_spec(const BehaviorInputScope& scope) noexcept {
    ResizeSpec spec;
    spec.horizontal = boolean(scope.option("horizontal"), true);
    spec.vertical = boolean(scope.option("vertical"), true);
    spec.min_width = std::max(0.0, number(scope.option("minWidth"), 32.0));
    spec.min_height = std::max(0.0, number(scope.option("minHeight"), 24.0));
    spec.max_width = std::max(spec.min_width, number(scope.option("maxWidth"), spec.max_width));
    spec.max_height = std::max(spec.min_height, number(scope.option("maxHeight"), spec.max_height));
    return spec;
}

void set_runtime_size(BehaviorInputScope& scope, const double width, const double height) {
    scope.set_retained(
        std::string(runtime_size),
        object({{"width", runtime::Value(width)}, {"height", runtime::Value(height)}}),
        DirtyReason::layout
    );
    static_cast<void>(scope.emit(
        "resized",
        object({{"width", runtime::Value(width)}, {"height", runtime::Value(height)}})
    ));
}

[[nodiscard]] bool resize_pointer(BehaviorInputScope& scope) {
    const bool accepted_phase = scope.phase() == BehaviorInputEventPhase::target ||
        (scope.phase() == BehaviorInputEventPhase::capture &&
         boolean(scope.option("includeDescendants"), true));
    if (!accepted_phase || scope.pointer() == nullptr) return false;
    const PointerInputEvent& event = *scope.pointer();
    const runtime::Value* session = scope.retained(resize_session);
    if (event.type == PointerEventType::press) {
        if (event.button != 0) return false;
        const LayoutRecord* layout = scope.layout();
        if (layout == nullptr) return false;
        const runtime::Value* previous = scope.retained(runtime_size);
        scope.set_retained(
            std::string(resize_session),
            object({
                {"pointerId", runtime::Value(static_cast<double>(event.pointer_id))},
                {"startX", runtime::Value(event.position.x)},
                {"startY", runtime::Value(event.position.y)},
                {"width", runtime::Value(number(field(previous, "width"), layout->bounds.width))},
                {"height", runtime::Value(number(field(previous, "height"), layout->bounds.height))},
                {"hadPrevious", runtime::Value(previous != nullptr && previous->object() != nullptr)},
                {"previousWidth", runtime::Value(number(field(previous, "width"), layout->bounds.width))},
                {"previousHeight", runtime::Value(number(field(previous, "height"), layout->bounds.height))},
            }),
            DirtyReason::input
        );
        return false;
    }
    if (session == nullptr || session->object() == nullptr ||
        integer(field(session, "pointerId"), -1) != event.pointer_id) return false;
    if (event.type == PointerEventType::move) {
        if (!scope.press_moved_beyond_slop()) return false;
        if (!scope.claim_gesture()) return false;
        const ResizeSpec spec = resize_spec(scope);
        const double width = spec.horizontal
            ? std::clamp(number(field(session, "width"), 0.0) + event.position.x - number(field(session, "startX"), event.position.x), spec.min_width, spec.max_width)
            : number(field(session, "width"), 0.0);
        const double height = spec.vertical
            ? std::clamp(number(field(session, "height"), 0.0) + event.position.y - number(field(session, "startY"), event.position.y), spec.min_height, spec.max_height)
            : number(field(session, "height"), 0.0);
        set_runtime_size(scope, width, height);
        return true;
    }
    if (event.type == PointerEventType::cancel) {
        static_cast<void>(scope.cancel_gesture());
        if (boolean(field(session, "hadPrevious"), false)) {
            set_runtime_size(
                scope,
                number(field(session, "previousWidth"), 0.0),
                number(field(session, "previousHeight"), 0.0)
            );
        } else {
            scope.set_retained(std::string(runtime_size), runtime::Value{}, DirtyReason::layout);
        }
        scope.set_retained(std::string(resize_session), runtime::Value{}, DirtyReason::input);
        return true;
    }
    if (event.type == PointerEventType::release) {
        const bool claimed = scope.gesture_claim_state() == GestureClaimState::claimed;
        scope.set_retained(std::string(resize_session), runtime::Value{}, DirtyReason::input);
        return claimed;
    }
    return false;
}

[[nodiscard]] bool resize_key(BehaviorInputScope& scope) {
    if (scope.phase() != BehaviorInputEventPhase::target) return false;
    if (scope.key() == "escape") {
        if (scope.retained(resize_session) == nullptr) return false;
        scope.set_retained(std::string(resize_session), runtime::Value{}, DirtyReason::input);
        return true;
    }
    const ResizeSpec spec = resize_spec(scope);
    const LayoutRecord* layout = scope.layout();
    if (layout == nullptr) return false;
    const runtime::Value* retained = scope.retained(runtime_size);
    double width = number(field(retained, "width"), layout->bounds.width);
    double height = number(field(retained, "height"), layout->bounds.height);
    if (spec.horizontal && scope.key() == "left") width -= 4.0;
    else if (spec.horizontal && scope.key() == "right") width += 4.0;
    else if (spec.vertical && scope.key() == "up") height -= 4.0;
    else if (spec.vertical && scope.key() == "down") height += 4.0;
    else return false;
    width = std::clamp(width, spec.min_width, spec.max_width);
    height = std::clamp(height, spec.min_height, spec.max_height);
    set_runtime_size(scope, width, height);
    return true;
}

[[nodiscard]] RetainedNode* split_pane(BehaviorInputScope& scope) noexcept {
    return scope.find_key(text(scope.option("paneKey")));
}

[[nodiscard]] bool split_vertical(const RetainedNode& pane) noexcept {
    const auto found = pane.description().properties.find("axis");
    const runtime::Value* value = found != pane.description().properties.end()
        ? found->second.value()
        : nullptr;
    return normalized(text(value, "horizontal")) == "vertical";
}

[[nodiscard]] double pane_number(
    const RetainedNode& pane,
    const std::string_view name,
    const double fallback
) noexcept {
    const auto found = pane.description().properties.find(name);
    return number(found != pane.description().properties.end() ? found->second.value() : nullptr, fallback);
}

[[nodiscard]] double current_split_ratio(const RetainedNode& pane) noexcept {
    const auto controlled = pane.description().properties.find("ratio");
    if (controlled != pane.description().properties.end() && controlled->second.value() != nullptr &&
        controlled->second.value()->number() != nullptr) {
        return *controlled->second.value()->number();
    }
    return number(pane.retained_value(split_ratio), pane_number(pane, "defaultRatio", 0.5));
}

void change_split(BehaviorInputScope& scope, RetainedNode& pane, double value) {
    const double minimum = pane_number(pane, "minRatio", 0.1);
    const double maximum = pane_number(pane, "maxRatio", 0.9);
    value = std::clamp(value, std::min(minimum, maximum), std::max(minimum, maximum));
    const auto controlled = pane.description().properties.find("ratio");
    const bool is_controlled = controlled != pane.description().properties.end() &&
        controlled->second.value() != nullptr && controlled->second.value()->number() != nullptr;
    if (!is_controlled) {
        scope.set_retained(pane, std::string(split_ratio), runtime::Value(value), DirtyReason::layout);
    }
    static_cast<void>(scope.emit("number-changed", runtime::Value(value)));
}

[[nodiscard]] bool split_pointer(BehaviorInputScope& scope) {
    if (scope.phase() != BehaviorInputEventPhase::target || scope.pointer() == nullptr) return false;
    RetainedNode* pane = split_pane(scope);
    if (pane == nullptr) return false;
    const PointerInputEvent& event = *scope.pointer();
    const runtime::Value* session = scope.retained(split_session);
    if (event.type == PointerEventType::press) {
        if (event.button != 0) return false;
        scope.set_retained(
            std::string(split_session),
            object({
                {"pointerId", runtime::Value(static_cast<double>(event.pointer_id))},
                {"startX", runtime::Value(event.position.x)},
                {"startY", runtime::Value(event.position.y)},
                {"ratio", runtime::Value(current_split_ratio(*pane))},
            }),
            DirtyReason::input
        );
        return false;
    }
    if (session == nullptr || session->object() == nullptr ||
        integer(field(session, "pointerId"), -1) != event.pointer_id) return false;
    if (event.type == PointerEventType::move) {
        if (!scope.press_moved_beyond_slop()) return false;
        if (!scope.claim_gesture()) return false;
        const LayoutRecord* layout = scope.layout(*pane);
        if (layout == nullptr) return false;
        const bool vertical = split_vertical(*pane);
        const double extent = vertical ? layout->bounds.height : layout->bounds.width;
        if (extent <= 0.0) return true;
        const double delta = vertical
            ? event.position.y - number(field(session, "startY"), event.position.y)
            : event.position.x - number(field(session, "startX"), event.position.x);
        change_split(scope, *pane, number(field(session, "ratio"), 0.5) + delta / extent);
        return true;
    }
    if (event.type == PointerEventType::cancel) {
        static_cast<void>(scope.cancel_gesture());
        change_split(scope, *pane, number(field(session, "ratio"), current_split_ratio(*pane)));
        scope.set_retained(std::string(split_session), runtime::Value{}, DirtyReason::input);
        return true;
    }
    if (event.type == PointerEventType::release) {
        const bool claimed = scope.gesture_claim_state() == GestureClaimState::claimed;
        scope.set_retained(std::string(split_session), runtime::Value{}, DirtyReason::input);
        return claimed;
    }
    return false;
}

[[nodiscard]] bool split_key(BehaviorInputScope& scope) {
    if (scope.phase() != BehaviorInputEventPhase::target) return false;
    RetainedNode* pane = split_pane(scope);
    if (pane == nullptr) return false;
    if (scope.key() == "escape") {
        scope.set_retained(std::string(split_session), runtime::Value{}, DirtyReason::input);
        return true;
    }
    const bool vertical = split_vertical(*pane);
    double direction = 0.0;
    if (!vertical && scope.key() == "left") direction = -1.0;
    else if (!vertical && scope.key() == "right") direction = 1.0;
    else if (vertical && scope.key() == "up") direction = -1.0;
    else if (vertical && scope.key() == "down") direction = 1.0;
    else return false;
    change_split(scope, *pane, current_split_ratio(*pane) + direction * 0.02);
    return true;
}

} // namespace

void register_builtin_behavior_inputs(BehaviorRegistry& registry) {
    registry.register_input_phase(
        "strata.activate",
        BehaviorInputPhase{
            .pointer = &activation_pointer,
            .key = &activation_key,
            .advance = &activation_advance,
            .focusable = true,
            .accepts_pointer = true,
        }
    );
    registry.register_input_phase(
        "strata.movable",
        BehaviorInputPhase{
            .pointer = &movable_pointer,
            .focusable = true,
            .accepts_pointer = true,
        }
    );
    registry.register_input_phase(
        "strata.resize",
        BehaviorInputPhase{
            .pointer = &resize_pointer,
            .key = &resize_key,
            .focusable = true,
            .accepts_pointer = true,
        }
    );
    registry.register_input_phase(
        "strata.split-handle",
        BehaviorInputPhase{
            .pointer = &split_pointer,
            .key = &split_key,
            .focusable = true,
            .accepts_pointer = true,
        }
    );
}

} // namespace strata::ui
