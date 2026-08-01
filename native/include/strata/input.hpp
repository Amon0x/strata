#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <strata/strata.h>

namespace strata {

struct Point final {
    double x = 0.0;
    double y = 0.0;
};

struct Rect final {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

enum class InputKind {
    pointer_move,
    pointer_press,
    pointer_release,
    pointer_cancel,
    scroll,
    key,
    text,
    ime_preedit,
    navigation,
};

enum class KeyAction { press, release, repeat };

enum class KeyModifiers : std::uint32_t {
    none = 0U,
    shift = STRATA_KEY_MODIFIER_SHIFT,
    control = STRATA_KEY_MODIFIER_CONTROL,
    alt = STRATA_KEY_MODIFIER_ALT,
    super_key = STRATA_KEY_MODIFIER_SUPER,
};

[[nodiscard]] constexpr KeyModifiers operator|(
    const KeyModifiers left,
    const KeyModifiers right
) noexcept {
    return static_cast<KeyModifiers>(
        static_cast<std::uint32_t>(left) | static_cast<std::uint32_t>(right)
    );
}

[[nodiscard]] constexpr bool has_modifier(
    const KeyModifiers value,
    const KeyModifiers modifier
) noexcept {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(modifier)) != 0U;
}

/** Owned input event. Conversion to the borrowed ABI record happens only during enqueue. */
struct InputEvent final {
    InputKind kind = InputKind::pointer_move;
    KeyModifiers modifiers = KeyModifiers::none;
    std::int32_t pointer_id = 0;
    std::int32_t button = 0;
    Point position;
    Point delta;
    std::string text;
    std::uint64_t selection_start = 0U;
    std::uint64_t selection_end = 0U;
    std::int64_t timestamp_nanoseconds = 0;
    KeyAction key_action = KeyAction::press;

    [[nodiscard]] static InputEvent pointer(
        InputKind kind,
        Point position,
        std::int32_t pointer_id = 0,
        std::int32_t button = 0,
        KeyModifiers modifiers = KeyModifiers::none,
        std::int64_t timestamp_nanoseconds = 0
    );
    [[nodiscard]] static InputEvent scroll(
        Point position,
        Point delta,
        KeyModifiers modifiers = KeyModifiers::none,
        std::int64_t timestamp_nanoseconds = 0
    );
    [[nodiscard]] static InputEvent key(
        std::string key,
        KeyAction action = KeyAction::press,
        KeyModifiers modifiers = KeyModifiers::none,
        std::int64_t timestamp_nanoseconds = 0
    );
    [[nodiscard]] static InputEvent committed_text(
        std::string text,
        std::int64_t timestamp_nanoseconds = 0
    );
    [[nodiscard]] static InputEvent preedit(
        std::string text,
        std::uint64_t selection_start,
        std::uint64_t selection_end,
        std::int64_t timestamp_nanoseconds = 0
    );
    [[nodiscard]] static InputEvent navigation(
        std::string direction,
        KeyAction action = KeyAction::press,
        KeyModifiers modifiers = KeyModifiers::none,
        std::int64_t timestamp_nanoseconds = 0
    );

    [[nodiscard]] strata_input_event native() const noexcept;
};

inline InputEvent InputEvent::pointer(
    const InputKind kind,
    const Point position,
    const std::int32_t pointer_id,
    const std::int32_t button,
    const KeyModifiers modifiers,
    const std::int64_t timestamp_nanoseconds
) {
    if (kind != InputKind::pointer_move && kind != InputKind::pointer_press &&
        kind != InputKind::pointer_release && kind != InputKind::pointer_cancel) {
        throw std::invalid_argument("pointer input requires a pointer event kind");
    }
    InputEvent result;
    result.kind = kind;
    result.position = position;
    result.pointer_id = pointer_id;
    result.button = button;
    result.modifiers = modifiers;
    result.timestamp_nanoseconds = timestamp_nanoseconds;
    return result;
}

inline InputEvent InputEvent::scroll(
    const Point position,
    const Point delta,
    const KeyModifiers modifiers,
    const std::int64_t timestamp_nanoseconds
) {
    InputEvent result;
    result.kind = InputKind::scroll;
    result.position = position;
    result.delta = delta;
    result.modifiers = modifiers;
    result.timestamp_nanoseconds = timestamp_nanoseconds;
    return result;
}

inline InputEvent InputEvent::key(
    std::string key,
    const KeyAction action,
    const KeyModifiers modifiers,
    const std::int64_t timestamp_nanoseconds
) {
    InputEvent result;
    result.kind = InputKind::key;
    result.text = std::move(key);
    result.key_action = action;
    result.modifiers = modifiers;
    result.timestamp_nanoseconds = timestamp_nanoseconds;
    return result;
}

inline InputEvent InputEvent::committed_text(
    std::string text,
    const std::int64_t timestamp_nanoseconds
) {
    InputEvent result;
    result.kind = InputKind::text;
    result.text = std::move(text);
    result.timestamp_nanoseconds = timestamp_nanoseconds;
    return result;
}

inline InputEvent InputEvent::preedit(
    std::string text,
    const std::uint64_t selection_start,
    const std::uint64_t selection_end,
    const std::int64_t timestamp_nanoseconds
) {
    InputEvent result;
    result.kind = InputKind::ime_preedit;
    result.text = std::move(text);
    result.selection_start = selection_start;
    result.selection_end = selection_end;
    result.timestamp_nanoseconds = timestamp_nanoseconds;
    return result;
}

inline InputEvent InputEvent::navigation(
    std::string direction,
    const KeyAction action,
    const KeyModifiers modifiers,
    const std::int64_t timestamp_nanoseconds
) {
    InputEvent result;
    result.kind = InputKind::navigation;
    result.text = std::move(direction);
    result.key_action = action;
    result.modifiers = modifiers;
    result.timestamp_nanoseconds = timestamp_nanoseconds;
    return result;
}

inline strata_input_event InputEvent::native() const noexcept {
    const auto native_kind = [this] {
        switch (kind) {
        case InputKind::pointer_move: return STRATA_INPUT_POINTER_MOVE;
        case InputKind::pointer_press: return STRATA_INPUT_POINTER_PRESS;
        case InputKind::pointer_release: return STRATA_INPUT_POINTER_RELEASE;
        case InputKind::pointer_cancel: return STRATA_INPUT_POINTER_CANCEL;
        case InputKind::scroll: return STRATA_INPUT_SCROLL;
        case InputKind::key: return STRATA_INPUT_KEY;
        case InputKind::text: return STRATA_INPUT_TEXT;
        case InputKind::ime_preedit: return STRATA_INPUT_IME_PREEDIT;
        case InputKind::navigation: return STRATA_INPUT_NAVIGATION;
        }
        return STRATA_INPUT_POINTER_CANCEL;
    }();
    const auto native_key_action = [this] {
        switch (key_action) {
        case KeyAction::press: return STRATA_KEY_PRESS;
        case KeyAction::release: return STRATA_KEY_RELEASE;
        case KeyAction::repeat: return STRATA_KEY_REPEAT;
        }
        return STRATA_KEY_PRESS;
    }();
    return strata_input_event{
        sizeof(strata_input_event),
        STRATA_INPUT_EVENT_VERSION_2,
        native_kind,
        static_cast<strata_key_modifiers>(modifiers),
        pointer_id,
        button,
        position.x,
        position.y,
        delta.x,
        delta.y,
        strata_string_view{text.data(), text.size()},
        selection_start,
        selection_end,
        timestamp_nanoseconds,
        native_key_action,
        0U,
    };
}

struct InputBatchInfo final {
    std::uint64_t accepted_event_count = 0U;
    std::uint64_t queued_event_count = 0U;
};

enum class SurfaceDensity { compact, comfortable };
enum class PointerPrecision { none, coarse, fine };
enum class PointSnap { none, nearest };
enum class RectangleSnap { none, nearest, outward };

/** Owned, portable Surface environment adopted atomically by the runtime. */
struct SurfaceEnvironment final {
    std::uint64_t generation = 1U;
    std::int64_t framebuffer_width = 0;
    std::int64_t framebuffer_height = 0;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double scale = 1.0;
    double safe_inset_left = 0.0;
    double safe_inset_top = 0.0;
    double safe_inset_right = 0.0;
    double safe_inset_bottom = 0.0;
    PointSnap point_snapping = PointSnap::nearest;
    RectangleSnap rectangle_snapping = RectangleSnap::nearest;
    SurfaceDensity density = SurfaceDensity::comfortable;
    PointerPrecision pointer_precision = PointerPrecision::fine;
    strata_surface_input_capabilities input_capabilities =
        STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD;
    bool reduced_motion = false;

    [[nodiscard]] strata_surface_environment native() const noexcept;
};

inline strata_surface_environment SurfaceEnvironment::native() const noexcept {
    const auto point_snap = point_snapping == PointSnap::nearest
        ? STRATA_POINT_SNAP_NEAREST
        : STRATA_POINT_SNAP_NONE;
    const auto rectangle_snap = [this] {
        switch (rectangle_snapping) {
        case RectangleSnap::none: return STRATA_RECTANGLE_SNAP_NONE;
        case RectangleSnap::nearest: return STRATA_RECTANGLE_SNAP_NEAREST;
        case RectangleSnap::outward: return STRATA_RECTANGLE_SNAP_OUTWARD;
        }
        return STRATA_RECTANGLE_SNAP_NONE;
    }();
    const auto native_density = density == SurfaceDensity::compact
        ? STRATA_SURFACE_DENSITY_COMPACT
        : STRATA_SURFACE_DENSITY_COMFORTABLE;
    const auto native_precision = [this] {
        switch (pointer_precision) {
        case PointerPrecision::none: return STRATA_POINTER_PRECISION_NONE;
        case PointerPrecision::coarse: return STRATA_POINTER_PRECISION_COARSE;
        case PointerPrecision::fine: return STRATA_POINTER_PRECISION_FINE;
        }
        return STRATA_POINTER_PRECISION_NONE;
    }();
    return strata_surface_environment{
        sizeof(strata_surface_environment),
        generation,
        framebuffer_width,
        framebuffer_height,
        logical_width,
        logical_height,
        scale,
        safe_inset_left,
        safe_inset_top,
        safe_inset_right,
        safe_inset_bottom,
        point_snap,
        rectangle_snap,
        native_density,
        native_precision,
        input_capabilities,
        reduced_motion ? 1U : 0U,
        0U,
    };
}

} // namespace strata
