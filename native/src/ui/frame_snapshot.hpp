#pragma once

#include "data/json.hpp"

namespace strata::ui {

class Surface;
struct SurfaceFrame;

/** Canonical platform-neutral projection of one completed Surface frame. */
[[nodiscard]] data::JsonValue surface_frame_snapshot(
    const Surface& surface,
    const SurfaceFrame& frame
);

} // namespace strata::ui
