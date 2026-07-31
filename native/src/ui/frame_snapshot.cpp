#include "ui/frame_snapshot.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/diagnostic_json.hpp"
#include "runtime/layer.hpp"
#include "ui/inspection.hpp"
#include "ui/render.hpp"
#include "ui/surface.hpp"

namespace strata::ui {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue focus_layers(const Surface& surface) {
    const std::optional<std::string_view> focused = surface.input().focused_key();
    std::vector<JsonValue> layers;
    for (const runtime::LayerSnapshot& layer : surface.layer_snapshot()) {
        layers.push_back(object({
            {"id", JsonValue(layer.id)},
            {"role", JsonValue(layer.role == runtime::LayerRole::screen ? "screen" : "overlay")},
        }));
    }
    return object({
        {"focusedKey", focused.has_value() ? JsonValue(std::string(*focused)) : JsonValue{}},
        {"layers", array(std::move(layers))},
    });
}

} // namespace

data::JsonValue surface_frame_snapshot(const Surface& surface, const SurfaceFrame& frame) {
    std::vector<JsonValue> events = frame.lifecycle_input.events;
    std::vector<JsonValue> outcomes = frame.lifecycle_input.action_outcomes;
    return object({
        {"actionOutcomes", array(std::move(outcomes))},
        {"diagnostics", runtime::diagnostics_json(frame.diagnostics)},
        {"events", array(std::move(events))},
        {"focusLayers", focus_layers(surface)},
        {"frameIndex", JsonValue(static_cast<std::int64_t>(frame.frame_index))},
        {"frameTimeNanos", JsonValue(frame.frame_time_nanos)},
        {"inspection", inspect_surface(surface)},
        {"operationCounters", inspect_operation_counters(frame)},
        {"protocol", JsonValue("strata.surface-frame")},
        {"renderCommands", render_commands_json(surface.render_commands())},
        {"semantics", surface.semantics().snapshot(surface.id())},
        {"state", inspect_state(surface.application())},
        {"surfaceId", JsonValue(surface.id())},
        {"version", JsonValue(std::int64_t{1})},
    });
}

} // namespace strata::ui
