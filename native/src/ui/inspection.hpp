#pragma once

#include "data/json.hpp"
#include "ui/surface.hpp"

namespace strata::ui {

/** Canonical protocol-v1 retained/layout inspection for a completed surface frame. */
[[nodiscard]] data::JsonValue inspect_surface(const Surface& surface);

/** Canonical protocol-v1 operation counters, including zeroes for stages not yet active. */
[[nodiscard]] data::JsonValue inspect_operation_counters(const SurfaceFrame& frame);

/** Canonical declaration-owned application state snapshot. */
[[nodiscard]] data::JsonValue inspect_state(runtime::ApplicationContext& application);

/** Compact live selection projection for host tooling and extension-owned inspectors. */
[[nodiscard]] data::JsonValue inspect_selection(const Surface& surface);

} // namespace strata::ui
