#include "session.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <strata/strata.hpp>

#include "application_host.hpp"
#include "data/json.hpp"
#include "png.hpp"

namespace strata::headless {
namespace {

using data::JsonValue;

[[nodiscard]] JsonValue object(std::initializer_list<JsonValue::ObjectEntry> entries) {
    return JsonValue(JsonValue::Object(entries));
}

[[nodiscard]] JsonValue array(std::vector<JsonValue> values = {}) {
    return JsonValue(JsonValue::Array(std::move(values)));
}

[[nodiscard]] JsonValue captured_actions(const ApplicationHost& host) {
    std::vector<JsonValue> values;
    for (const CapturedAction& action : host.actions()) {
        values.push_back(object({
            {"eventKind", JsonValue(action.event_kind)},
            {"eventValue", JsonValue(action.event_value)},
            {"id", JsonValue(action.id)},
            {"payload", JsonValue(action.payload)},
            {"sourceKey", JsonValue(action.source_key)},
        }));
    }
    return array(std::move(values));
}

[[nodiscard]] JsonValue captured_diagnostics(const ApplicationHost& host) {
    std::vector<JsonValue> values;
    for (const CapturedDiagnostic& diagnostic : host.diagnostics()) {
        values.push_back(object({
            {"code", JsonValue(diagnostic.code)},
            {"componentPath", JsonValue(diagnostic.component_path)},
            {"expected", JsonValue(diagnostic.expected)},
            {"id", JsonValue(static_cast<std::int64_t>(diagnostic.id))},
            {"message", JsonValue(diagnostic.message)},
            {"severity", JsonValue(static_cast<std::int64_t>(diagnostic.severity))},
            {"source", JsonValue(diagnostic.source)},
        }));
    }
    return array(std::move(values));
}

[[nodiscard]] JsonValue captured_effects(const ApplicationHost& host) {
    std::vector<JsonValue> values;
    for (const CapturedEffect& effect : host.effects()) {
        values.push_back(object({
            {"id", JsonValue(effect.id)},
            {"payload", JsonValue(effect.payload)},
        }));
    }
    return array(std::move(values));
}

[[nodiscard]] JsonValue captured_async_requests(const ApplicationHost& host) {
    std::vector<JsonValue> values;
    for (const CapturedAsyncRequest& request : host.async_requests()) {
        values.push_back(object({
            {"binding", JsonValue(request.binding)},
            {"cancelled", JsonValue(request.cancelled)},
            {"id", JsonValue(static_cast<std::int64_t>(request.id))},
            {"owner", JsonValue(request.owner)},
            {"payload", JsonValue(request.payload)},
        }));
    }
    return array(std::move(values));
}

[[nodiscard]] JsonValue material_fallbacks(const ApplicationHost& host) {
    std::vector<JsonValue> values;
    for (const std::string& material : host.material_fallbacks())
        values.emplace_back(material);
    return array(std::move(values));
}

} // namespace

struct Session::Impl final {
    Impl(const Scenario& scenario, std::filesystem::path resource_root,
         std::filesystem::path output_root)
        : scenario_(scenario), output_root_(std::move(output_root)),
          host_(scenario, std::move(resource_root)), viewport_width_(scenario.width),
          viewport_height_(scenario.height), viewport_scale_(scenario.scale) {
        std::filesystem::create_directories(output_root_);
    }

    void execute_step(const ScenarioStep& step) {
        std::visit([this](const auto& value) { execute(value); }, step);
    }

    void frame() {
        host_.frame(time_nanoseconds_);
    }

    void ensure_frame() {
        if (!host_.has_frame())
            frame();
    }

    void advance_clock(const std::int64_t delta = 16'666'667) {
        if (delta < 0 || time_nanoseconds_ > std::numeric_limits<std::int64_t>::max() - delta) {
            throw std::overflow_error("headless scenario clock exhausted");
        }
        time_nanoseconds_ += delta;
    }

    void next_frame(const std::int64_t delta = 16'666'667) {
        advance_clock(delta);
        frame();
    }

    void enqueue(const strata_input_event& event) {
        host_.enqueue(std::span(&event, 1U));
    }

    [[nodiscard]] BrowserModel browser_model() const {
        if (!host_.has_frame())
            throw std::logic_error("headless browser requires a current frame");
        return BrowserModel::build(host_.frame_document(), viewport_width_, viewport_height_);
    }

    [[nodiscard]] std::pair<double, double> resolve(const Selector& selector) {
        ensure_frame();
        return browser_model().resolve(selector);
    }

    [[nodiscard]] strata_input_event pointer_event(const std::uint32_t kind, const double x,
                                                   const double y,
                                                   const std::int32_t button = 0) const {
        strata_input_event event{};
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.kind = kind;
        event.pointer_id = 0;
        event.button = button;
        event.x = x;
        event.y = y;
        event.timestamp_nanoseconds = time_nanoseconds_;
        return event;
    }

    void execute(const AdvanceStep& step) {
        const std::int64_t base = time_nanoseconds_;
        for (std::uint32_t frame_index = 0U; frame_index < step.frames; ++frame_index) {
            const long double fraction =
                static_cast<long double>(frame_index + 1U) / static_cast<long double>(step.frames);
            const std::int64_t offset = static_cast<std::int64_t>(
                std::llround(static_cast<long double>(step.duration_nanoseconds) * fraction));
            if (offset > std::numeric_limits<std::int64_t>::max() - base) {
                throw std::overflow_error("headless scenario clock exhausted");
            }
            time_nanoseconds_ = base + offset;
            frame();
        }
    }

    void write_snapshot(const std::string_view name) const {
        write_png(output_root_ / (std::string(name) + ".png"), host_.framebuffer_width(),
                  host_.framebuffer_height(), host_.pixels());
        std::ofstream json(output_root_ / (std::string(name) + ".json"),
                           std::ios::binary | std::ios::trunc);
        json << host_.frame_json();
        if (!json)
            throw std::runtime_error("could not write headless frame JSON");
    }

    void execute(const CaptureStep& step) {
        ensure_frame();
        write_snapshot(step.name);
        captures_.push_back(step.name);
    }

    void execute(const MoveStep& step) {
        const auto [x, y] = resolve(step.target);
        advance_clock();
        enqueue(pointer_event(STRATA_INPUT_POINTER_MOVE, x, y));
        frame();
    }

    void execute(const PointerDragStep& step) {
        const auto [start_x, start_y] = resolve(step.from);
        const auto [end_x, end_y] = resolve(step.to);
        advance_clock();
        enqueue(pointer_event(STRATA_INPUT_POINTER_MOVE, start_x, start_y));
        frame();
        advance_clock(1'000'000);
        enqueue(pointer_event(STRATA_INPUT_POINTER_PRESS, start_x, start_y, step.button));
        frame();
        const std::int64_t base = time_nanoseconds_;
        for (std::uint32_t index = 0U; index < step.steps; ++index) {
            const long double fraction =
                static_cast<long double>(index + 1U) / static_cast<long double>(step.steps);
            time_nanoseconds_ =
                base + static_cast<std::int64_t>(std::llround(
                           static_cast<long double>(step.duration_nanoseconds) * fraction));
            const double x = start_x + (end_x - start_x) * static_cast<double>(fraction);
            const double y = start_y + (end_y - start_y) * static_cast<double>(fraction);
            enqueue(pointer_event(STRATA_INPUT_POINTER_MOVE, x, y, step.button));
            frame();
        }
        advance_clock(1'000'000);
        enqueue(pointer_event(STRATA_INPUT_POINTER_RELEASE, end_x, end_y, step.button));
        frame();
    }

    void execute(const ClickStep& step) {
        const auto [x, y] = resolve(step.target);
        advance_clock();
        enqueue(pointer_event(STRATA_INPUT_POINTER_MOVE, x, y));
        frame();
        advance_clock(1'000'000);
        enqueue(pointer_event(STRATA_INPUT_POINTER_PRESS, x, y, step.button));
        frame();
        advance_clock(1'000'000);
        enqueue(pointer_event(STRATA_INPUT_POINTER_RELEASE, x, y, step.button));
        frame();
    }

    void execute(const ScrollStep& step) {
        const auto [x, y] = resolve(step.target);
        advance_clock();
        strata_input_event event = pointer_event(STRATA_INPUT_SCROLL, x, y);
        event.delta_x = step.delta_x;
        event.delta_y = step.delta_y;
        enqueue(event);
        frame();
    }

    void execute(const KeyStep& step) {
        ensure_frame();
        advance_clock();
        strata_input_event event{};
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.kind = STRATA_INPUT_KEY;
        event.modifiers = step.modifiers;
        event.text = strata::view(step.key);
        event.timestamp_nanoseconds = time_nanoseconds_;
        event.key_action = STRATA_KEY_PRESS;
        enqueue(event);
        frame();
        advance_clock(1'000'000);
        event.timestamp_nanoseconds = time_nanoseconds_;
        event.key_action = STRATA_KEY_RELEASE;
        enqueue(event);
        frame();
    }

    void execute(const TextStep& step) {
        ensure_frame();
        advance_clock();
        strata_input_event event{};
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.kind = STRATA_INPUT_TEXT;
        event.text = strata::view(step.text);
        event.timestamp_nanoseconds = time_nanoseconds_;
        enqueue(event);
        frame();
    }

    void execute(const ResizeStep& step) {
        if (!std::isfinite(step.width) || !std::isfinite(step.height) ||
            !std::isfinite(step.scale) || step.width <= 0.0 || step.height <= 0.0 ||
            step.scale <= 0.0 ||
            step.width * step.scale >
                static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
            step.height * step.scale >
                static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
            throw std::invalid_argument("headless resize dimensions must be positive and finite");
        }
        advance_clock();
        host_.resize(step.width, step.height, step.scale, time_nanoseconds_);
        viewport_width_ = step.width;
        viewport_height_ = step.height;
        viewport_scale_ = step.scale;
    }

    void execute(const PublishStep& step) {
        host_.publish(step.snapshot);
        next_frame();
    }

    void write_current() {
        ensure_frame();
        write_snapshot("current");
    }

    [[nodiscard]] JsonValue interactive_state(const std::string_view event) const {
        if (!host_.has_frame() || host_.frames().empty()) {
            throw std::logic_error("interactive state requires a current frame");
        }
        const CapturedFrame& frame = host_.frames().back();
        const JsonValue browser = browser_model().document();
        const JsonValue* elements = browser.find("elements");
        return object({
            {"artifacts", object({
                              {"image", JsonValue((output_root_ / "current.png").string())},
                              {"snapshot", JsonValue((output_root_ / "current.json").string())},
                          })},
            {"backend", JsonValue(std::string(host_.render_backend()))},
            {"elements", elements != nullptr ? *elements : array()},
            {"event", JsonValue(std::string(event))},
            {"frame",
             object({
                 {"actionOutcomes", JsonValue(static_cast<std::int64_t>(frame.actions))},
                 {"commands", JsonValue(static_cast<std::int64_t>(frame.commands))},
                 {"emittedEvents", JsonValue(static_cast<std::int64_t>(frame.emitted_events))},
                 {"index", JsonValue(static_cast<std::int64_t>(frame.index))},
                 {"inputEvents", JsonValue(static_cast<std::int64_t>(frame.input_events))},
                 {"timeNanos", JsonValue(frame.time)},
             })},
            {"host", object({
                         {"actions", captured_actions(host_)},
                         {"asyncRequests", captured_async_requests(host_)},
                         {"diagnostics", captured_diagnostics(host_)},
                         {"effects", captured_effects(host_)},
                         {"materialFallbacks", material_fallbacks(host_)},
                     })},
            {"protocol", JsonValue("strata.headless.interactive")},
            {"surfaceId", JsonValue(scenario_.surface_id)},
            {"version", JsonValue(std::int64_t{1})},
            {"viewport", object({
                             {"framebufferHeight",
                              JsonValue(static_cast<std::int64_t>(host_.framebuffer_height()))},
                             {"framebufferWidth",
                              JsonValue(static_cast<std::int64_t>(host_.framebuffer_width()))},
                             {"height", JsonValue(viewport_height_)},
                             {"scale", JsonValue(viewport_scale_)},
                             {"width", JsonValue(viewport_width_)},
                         })},
        });
    }

    void write_result() const {
        std::vector<JsonValue> captures;
        for (const std::string& capture : captures_)
            captures.emplace_back(capture);
        std::vector<JsonValue> frames;
        for (const CapturedFrame& frame : host_.frames()) {
            frames.push_back(object({
                {"actionOutcomes", JsonValue(static_cast<std::int64_t>(frame.actions))},
                {"commands", JsonValue(static_cast<std::int64_t>(frame.commands))},
                {"emittedEvents", JsonValue(static_cast<std::int64_t>(frame.emitted_events))},
                {"frameIndex", JsonValue(static_cast<std::int64_t>(frame.index))},
                {"inputEvents", JsonValue(static_cast<std::int64_t>(frame.input_events))},
                {"packetBytes", JsonValue(static_cast<std::int64_t>(frame.packet_bytes))},
                {"timeNanos", JsonValue(frame.time)},
            }));
        }
        const JsonValue result = object({
            {"actions", captured_actions(host_)},
            {"asyncRequests", captured_async_requests(host_)},
            {"backend", JsonValue(std::string(host_.render_backend()))},
            {"captures", array(std::move(captures))},
            {"diagnostics", captured_diagnostics(host_)},
            {"effects", captured_effects(host_)},
            {"frames", array(std::move(frames))},
            {"materialFallbacks", material_fallbacks(host_)},
            {"protocol", JsonValue("strata.headless-result")},
            {"surfaceId", JsonValue(scenario_.surface_id)},
            {"version", JsonValue(std::int64_t{1})},
        });
        std::ofstream output(output_root_ / "result.json", std::ios::binary | std::ios::trunc);
        output << data::encode_canonical_json(result);
        if (!output)
            throw std::runtime_error("could not write headless result JSON");
    }

    const Scenario& scenario_;
    std::filesystem::path output_root_;
    ApplicationHost host_;
    std::vector<std::string> captures_;
    std::int64_t time_nanoseconds_ = 0;
    double viewport_width_ = 0.0;
    double viewport_height_ = 0.0;
    double viewport_scale_ = 1.0;
    bool closed_ = false;
};

Session::Session(const Scenario& scenario, std::filesystem::path resource_root,
                 std::filesystem::path output_root)
    : impl_(std::make_unique<Impl>(scenario, std::move(resource_root), std::move(output_root))) {}
Session::~Session() = default;

void Session::execute(const ScenarioStep& step) {
    impl_->execute_step(step);
}
void Session::ensure_frame() {
    impl_->ensure_frame();
}
void Session::write_current() {
    impl_->write_current();
}
void Session::write_result() const {
    impl_->write_result();
}
void Session::close() {
    if (impl_->closed_)
        return;
    impl_->host_.close();
    impl_->closed_ = true;
}
BrowserModel Session::browser_model() const {
    return impl_->browser_model();
}
JsonValue Session::interactive_state(const std::string_view event) const {
    return impl_->interactive_state(event);
}
std::int64_t Session::time_nanoseconds() const noexcept {
    return impl_->time_nanoseconds_;
}

} // namespace strata::headless
