#include <strata/presentation.hpp>
#include <strata/strata.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "capture_renderer.hpp"

namespace {
using namespace strata;
using Clock = std::chrono::steady_clock;

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

/** Independent gates expose admission and terminal consumption without sleeping on races. */
struct Probe final {
    std::binary_semaphore factory_entered{0};
    std::binary_semaphore factory_proceed{0};
    std::binary_semaphore terminal_entered{0};
    std::binary_semaphore terminal_proceed{0};
    std::atomic<bool> park_frame{false};
    std::binary_semaphore frame_entered{0};
    std::binary_semaphore frame_proceed{0};
    std::binary_semaphore unavailable{0};
    std::counting_semaphore<128> wake{0};
    std::thread::id factory_thread;
    std::thread::id destroyed_thread;
};

/** Failure unwinding must release a parked consumer before its Presentation is destroyed. */
struct OpenGate final {
    std::binary_semaphore& gate;
    bool opened = false;
    ~OpenGate() { open(); }
    void open() noexcept {
        if (!opened) {
            opened = true;
            gate.release();
        }
    }
};

/** A real single-layer offscreen target. It draws/captures pixels but never claims scan-out. */
class OffscreenDriver final : public host::PresentationDriver {
  public:
    explicit OffscreenDriver(std::shared_ptr<Probe> probe, const std::string_view backend = "reference")
        : probe_(std::move(probe)), renderer_(headless::create_capture_renderer(backend)) {
        set_transparent(false);
    }
    ~OffscreenDriver() override { probe_->destroyed_thread = std::this_thread::get_id(); }
    [[nodiscard]] host::PresentationInfo info() const override {
        host::PresentationInfo result;
        result.adapter = renderer_->backend();
        result.vsync = false;
        return result;
    }
    void declare_material(const host::MaterialSource& material) override {
        renderer_->declare_material(material.declaration.id, material.source);
    }
    void declare_effect(const host::EffectSource& effect) override {
        const auto& pass = effect.declaration;
        renderer_->declare_effect_pass(pass.effect_id, pass.index,
            static_cast<std::uint32_t>(pass.kind), pass.radius, pass.downsample,
            pass.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
            pass.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE), effect.source);
    }
    void resize(const host::PresentationViewport& viewport) override {
        available_ = viewport.width != 0U && viewport.height != 0U;
        if (!available_)
            return;
        reported_unavailable_ = false;
        renderer_->resize(viewport.width, viewport.height, viewport.logical_width, viewport.logical_height);
    }
    void release_programs(const std::string_view prefix) override {
        renderer_->release_programs(prefix);
    }
    void set_pointer(std::optional<std::array<double, 2U>>) override {
        throw std::invalid_argument("the offscreen oracle has no platform pointer");
    }
    void set_transparent(const bool transparent) override {
        renderer_->set_clear_color(transparent ? std::array<std::uint8_t, 4U>{}
                                              : std::array<std::uint8_t, 4U>{9U, 11U, 15U, 255U});
    }
    void consume_resources(const host::RenderPacket& packet) override {
        if (std::ranges::any_of(packet.resources, [](const auto& operation) {
                return operation.kind == host::resource_release;
            })) {
            probe_->terminal_entered.release();
            probe_->terminal_proceed.acquire();
        }
        renderer_->consume_resources(packet);
    }
    void release_layer(const std::string_view id) noexcept override {
        if (layer_ == id)
            layer_.clear();
    }
    [[nodiscard]] bool begin_frame() override {
        if (!available_) {
            if (!reported_unavailable_) {
                reported_unavailable_ = true;
                probe_->unavailable.release();
            }
            return false;
        }
        if (probe_->park_frame.exchange(false)) {
            probe_->frame_entered.release();
            probe_->frame_proceed.acquire();
        }
        drew_ = false;
        return true;
    }
    [[nodiscard]] host::PresentationLayerTiming draw(
        const std::string_view id, const host::RenderPacket& packet) override {
        require(layer_.empty() || layer_ == id, "offscreen target requires retirement before reuse");
        layer_ = id;
        renderer_->render(packet, 0);
        drew_ = true;
        return {}; // This CPU oracle has no GPU timing or autonomous shader work.
    }
    [[nodiscard]] std::vector<strata_backdrop_sample> take_backdrop_samples(std::string_view) override {
        return renderer_->take_backdrop_samples();
    }
    [[nodiscard]] host::PresentationResult end_frame() override {
        if (!drew_)
            renderer_->render(host::RenderPacket{}, 0);
        if (capture_requested_) {
            capture_.emplace();
            capture_->width = renderer_->width();
            capture_->height = renderer_->height();
            const auto pixels = renderer_->pixels();
            capture_->rgba.assign(pixels.begin(), pixels.end());
            capture_requested_ = false;
        }
        return {};
    }
    void capture_next_frame() override { capture_requested_ = true; }
    [[nodiscard]] std::optional<host::PresentationCapture> take_capture() override {
        return std::exchange(capture_, std::nullopt);
    }

  private:
    std::shared_ptr<Probe> probe_;
    std::unique_ptr<headless::CaptureRenderer> renderer_;
    std::string layer_;
    bool available_ = false;
    bool reported_unavailable_ = false;
    bool drew_ = false;
    bool capture_requested_ = false;
    std::optional<host::PresentationCapture> capture_;
};

void wait_receipt(host::Presentation& presentation, const host::PresentationReceipt& receipt,
                  Probe& probe, const Clock::time_point deadline) {
    while (!receipt.ready()) {
        presentation.rethrow_failure();
        require(probe.wake.try_acquire_until(deadline), "presentation receipt did not complete");
    }
}

void ordered_packets_and_idle_feedback(const std::string_view backend) {
    RuntimeOptions options;
    options.task_execution = TaskExecution::deterministic;
    Runtime runtime(std::move(options));
    runtime.configure_application(ApplicationOptions{.id = "presentation.order"});
    require(runtime.activate(SourceActivation{
        .generation = 1U, .entry_source_id = "presentation.strata",
        .entry_text = R"(overlay Main {
            root Panel { Image(image: "test:photo", layout: { width: 32, height: 32 }) }
        })",
    }).activated(), "presentation image application did not activate");
    SurfaceOptions surface_options;
    surface_options.id = "presentation.order.surface";
    surface_options.root_role = SurfaceRootRole::overlay;
    surface_options.root_name = "Main";
    surface_options.environment.framebuffer_width = 32;
    surface_options.environment.framebuffer_height = 32;
    surface_options.environment.logical_width = 32;
    surface_options.environment.logical_height = 32;
    Surface surface = runtime.create_surface(surface_options);

    const auto probe = std::make_shared<Probe>();
    host::Presentation presentation([probe, backend = std::string(backend)] {
        probe->factory_thread = std::this_thread::get_id();
        probe->factory_entered.release();
        probe->factory_proceed.acquire();
        return std::make_unique<OffscreenDriver>(probe, backend);
    }, host::PresentationExecution::threaded, [probe] { probe->wake.release(); });
    OpenGate factory_gate{probe->factory_proceed};
    OpenGate terminal_gate{probe->terminal_proceed};
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    require(probe->factory_entered.try_acquire_until(deadline) &&
                probe->factory_thread != std::this_thread::get_id(),
            "presentation initialization blocked its producer");
    const auto stream = presentation.attach("image");
    presentation.resize({32U, 32U, 32.0, 32.0});
    std::int64_t time = 0;
    const auto commit = [&] {
        auto framed = host::frame_surface(surface.native_handle(), ++time);
        host::PresentationCommit next;
        next.order.push_back(stream);
        next.layers.push_back({stream, std::move(framed.encoded), framed.surface.unchanged != 0U});
        static_cast<void>(presentation.commit(std::move(next)));
        return framed.surface;
    };
    runtime.publish_image("test:photo", 1U, 1U, std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U});
    static_cast<void>(commit());
    runtime.publish_image("test:photo", 1U, 1U, std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U});
    static_cast<void>(commit());
    const auto capture_request = presentation.capture_next_frame();
    static_cast<void>(commit());
    factory_gate.open();

    std::vector<host::PresentationFrame> frames;
    std::optional<host::PresentationCapture> capture;
    while (!capture.has_value()) {
        presentation.rethrow_failure();
        presentation.take_frames(frames);
        for (auto& frame : frames) {
            if (std::ranges::find(frame.capture_requests, capture_request) != frame.capture_requests.end()) {
                require(frame.drawn && !frame.presented, "offscreen rendering claimed presentation");
                capture = std::move(frame.capture);
            }
        }
        if (!capture.has_value())
            require(probe->wake.try_acquire_until(deadline), "ordered presentation did not capture");
    }
    const std::size_t center = (16U * capture->width + 16U) * 4U;
    require(capture->width == 32U && capture->height == 32U &&
                capture->rgba[center] == 0U && capture->rgba[center + 1U] == 0U &&
                capture->rgba[center + 2U] == 255U && capture->rgba[center + 3U] == 255U,
            "skipped visual commits lost ordered image updates or captured an older scene");

    {
        OpenGate frame_gate{probe->frame_proceed};
        probe->park_frame.store(true);
        presentation.resize({32U, 32U, 32.0, 32.0});
        require(probe->frame_entered.try_acquire_until(deadline), "consumer did not park before coalescing");
        require(runtime.activate(SourceActivation{
            .generation = 2U, .entry_source_id = "presentation.strata",
            .entry_text = R"(overlay Main {
                root Panel { Image(image: "test:photo", layout: { width: 16, height: 32 }) }
            })",
        }).activated(), "resized image application did not activate");
        const auto resized_capture = presentation.capture_next_frame();
        static_cast<void>(commit());
        require(commit().unchanged != 0U, "resized image did not produce a compact successor");
        require(!presentation.ready_for_frame(),
                "the owner was told to frame while its last commit waited for the render thread");
        frame_gate.open();
        capture.reset();
        while (!capture.has_value()) {
            presentation.rethrow_failure();
            presentation.take_frames(frames);
            for (auto& frame : frames) {
                if (std::ranges::find(frame.capture_requests, resized_capture) != frame.capture_requests.end())
                    capture = std::move(frame.capture);
            }
            if (!capture.has_value())
                require(probe->wake.try_acquire_until(deadline), "coalesced geometry did not capture");
        }
        const std::size_t inside = (16U * capture->width + 8U) * 4U;
        const std::size_t outside = (16U * capture->width + 24U) * 4U;
        require(capture->rgba[inside] == 0U && capture->rgba[inside + 2U] == 255U &&
                    capture->rgba[outside] == 9U && capture->rgba[outside + 1U] == 11U &&
                    capture->rgba[outside + 2U] == 15U && capture->rgba[outside + 3U] == 255U,
                "compact successor discarded an undrawn geometry update");
        const auto ready_deadline = Clock::now() + std::chrono::seconds(10);
        while (!presentation.ready_for_frame()) {
            presentation.rethrow_failure();
            require(Clock::now() < ready_deadline, "a consumed commit never released the owner");
            std::this_thread::yield();
        }
    }

    presentation.resize({});
    const auto restored_capture = presentation.capture_next_frame();
    runtime.publish_image("test:photo", 1U, 1U, std::array<std::uint8_t, 4U>{0U, 255U, 0U, 255U});
    static_cast<void>(commit());
    require(probe->unavailable.try_acquire_until(deadline), "zero-size target was not deferred");
    presentation.take_frames(frames);
    for (const auto& frame : frames)
        require(std::ranges::find(frame.capture_requests, restored_capture) == frame.capture_requests.end(),
                "unavailable target completed a capture before drawing");
    presentation.resize({32U, 32U, 32.0, 32.0});
    capture.reset();
    while (!capture.has_value()) {
        presentation.rethrow_failure();
        presentation.take_frames(frames);
        for (auto& frame : frames) {
            if (std::ranges::find(frame.capture_requests, restored_capture) != frame.capture_requests.end())
                capture = std::move(frame.capture);
        }
        if (!capture.has_value())
            require(probe->wake.try_acquire_until(deadline), "restored target lost its pending capture");
    }
    const std::size_t restored_inside = (16U * capture->width + 8U) * 4U;
    require(capture->rgba[restored_inside] == 0U && capture->rgba[restored_inside + 1U] == 255U &&
                capture->rgba[restored_inside + 2U] == 0U,
            "restored target lost resources consumed while unavailable");

    const auto first_idle = commit();
    require(first_idle.unchanged != 0U && commit().unchanged != 0U && commit().unchanged != 0U,
            "static image fixture did not settle");
    const EncodedRenderPacket terminal = surface.prepare_release_packet();
    const auto retired = presentation.retire(stream, terminal);
    require(probe->terminal_entered.try_acquire_until(deadline) && !retired.ready(),
            "terminal admission acknowledged consumption before its resource operations ran");
    presentation.take_frames(frames);
    for (const auto& frame : frames) {
        for (const auto& layer : frame.layers) {
            require(layer.profiler.frame_index < first_idle.frame_index,
                    "unchanged resource-free commits generated a self-sustaining owner wake");
        }
    }
    bool refused = false;
    try {
        surface.close();
    } catch (const AbiError& error) {
        refused = error.status() == STRATA_STATUS_INVALID_ARGUMENT;
    }
    require(refused, "unconsumed terminal permitted Surface destruction");
    terminal_gate.open();
    wait_receipt(presentation, retired, *probe, deadline);
    retired.get();
    surface.acknowledge_release_packet();
    surface.close();
    runtime.close();
    presentation.stop();
    while (!presentation.stopped())
        require(probe->wake.try_acquire_until(deadline), "presentation did not stop on its owner");
    require(probe->destroyed_thread == probe->factory_thread && presentation.present_count() == 0U,
            "target destruction changed owners or an offscreen target claimed a Present");
}
/** The render thread moves content when the owner cannot, then gives the scroll back to it. */
void scroll_moves_without_the_owner(const std::string_view backend) {
    RuntimeOptions options;
    options.task_execution = TaskExecution::deterministic;
    Runtime runtime(std::move(options));
    runtime.configure_application(ApplicationOptions{.id = "presentation.scroll"});
    require(runtime.activate(SourceActivation{
        .generation = 1U, .entry_source_id = "scroll.strata",
        .entry_text = R"(style Red { background: #FF0000; border: null; radius: 0; }
        style Blue { background: #0000FF; border: null; radius: 0; }
        overlay Main {
          root Panel(layout: { kind: "COLUMN", width: 200, height: 400 }) {
            Scroll(key: "plain", vertical: true, horizontal: false,
                   layout: { width: 200, height: 200 }) {
              Panel(layout: { kind: "COLUMN", width: { weight: 1 }, height: "content" }) {
                Panel(style: Red, layout: { width: { weight: 1 }, height: 200 })
                Panel(style: Blue, layout: { width: { weight: 1 }, height: 200 })
              }
            }
            Scroll(key: "zoomed", vertical: true, horizontal: false,
                   layout: { width: 200, height: 200 }) {
              ZoomView(key: "view", layout: { width: 180, height: 400 }) {
                Panel(style: Red, layout: { width: 100, height: 100 })
              }
            }
          }
        })",
    }).activated(), "scroll application did not activate");
    SurfaceOptions surface_options;
    surface_options.id = "presentation.scroll";
    surface_options.root_role = SurfaceRootRole::overlay;
    surface_options.root_name = "Main";
    surface_options.environment.framebuffer_width = 200;
    surface_options.environment.framebuffer_height = 400;
    surface_options.environment.logical_width = 200;
    surface_options.environment.logical_height = 400;
    Surface surface = runtime.create_surface(surface_options);
    const auto probe = std::make_shared<Probe>();
    host::Presentation presentation([probe, backend = std::string(backend)] {
        return std::make_unique<OffscreenDriver>(probe, backend);
    }, host::PresentationExecution::caller);
    presentation.resize({200U, 400U, 200.0, 400.0});
    const auto stream = presentation.attach("scene");
    std::int64_t time = 0;
    std::uint64_t acknowledged = 0U;
    // The pixel near the top of the plain scroller, framed and drawn with the owner's latest state.
    const auto top_pixel = [&]() -> std::array<std::uint8_t, 3U> {
        const auto request = presentation.capture_next_frame();
        auto framed = host::frame_surface(surface.native_handle(), time += 16'000'000);
        host::PresentationCommit commit;
        commit.order.push_back(stream);
        commit.layers.push_back({stream, std::move(framed.encoded), framed.surface.unchanged != 0U,
                                 acknowledged});
        static_cast<void>(presentation.commit(std::move(commit)));
        std::vector<host::PresentationFrame> frames;
        presentation.take_frames(frames);
        for (const auto& frame : frames) {
            if (std::ranges::find(frame.capture_requests, request) != frame.capture_requests.end()) {
                const std::size_t at = (std::size_t{100U} * 200U + 100U) * 4U;
                return {frame.capture->rgba[at], frame.capture->rgba[at + 1U],
                        frame.capture->rgba[at + 2U]};
            }
        }
        throw std::runtime_error("the requested scroll capture was not presented");
    };
    const std::array<std::uint8_t, 3U> red{255U, 0U, 0U};
    const std::array<std::uint8_t, 3U> blue{0U, 0U, 255U};
    require(top_pixel() == red, "the scroll scene did not start at its first panel");

    // Eight notches pass the first panel, up to the limit. The owner has adopted nothing yet.
    std::vector<host::PresentationScroll> scrolls;
    presentation.wheel(stream, 100.0, 100.0, 0.0, -8.0);
    presentation.take_scrolls(scrolls);
    require(scrolls.size() == 1U && scrolls.front().applied && scrolls.front().delta_y > 0.0 &&
                scrolls.front().identity != 0U,
            "the render thread did not scroll an eligible scroller");
    require(top_pixel() == blue, "an applied scroll did not move the drawn content");
    require(top_pixel() == blue, "an applied scroll was lost by a later commit");

    // At its limit a wheel goes back to the owner, whose routing knows what else could take it.
    presentation.wheel(stream, 100.0, 100.0, 0.0, -8.0);
    presentation.take_scrolls(scrolls);
    require(scrolls.size() == 1U && !scrolls.front().applied,
            "a scroller at its limit did not hand back another wheel");

    // Adopting it moves the owner's offset, and the render thread stops adding it: not twice.
    const std::uint64_t serial = 1U;
    require(surface.adopt_scroll(1U << 30U, 0.0, 0.0) == false, "an unknown scroller adopted a scroll");
    presentation.wheel(stream, 100.0, 100.0, 0.0, 8.0);
    presentation.take_scrolls(scrolls);
    require(scrolls.size() == 1U && scrolls.front().applied && scrolls.front().delta_y < 0.0,
            "the render thread did not scroll back");
    require(top_pixel() == red, "scrolling back did not move the drawn content");
    presentation.wheel(stream, 100.0, 100.0, 0.0, -8.0);
    presentation.take_scrolls(scrolls);
    const auto identity = scrolls.front().identity;
    require(surface.adopt_scroll(identity, 0.0, scrolls.front().delta_y), "the owner did not adopt an applied scroll");
    acknowledged = scrolls.front().serial;
    require(acknowledged > serial, "scroll serials did not advance");
    require(top_pixel() == blue, "an adopted scroll was lost or applied twice");
    require(top_pixel() == blue, "an adopted scroll did not stay");

    // Something under the point takes the wheel itself: the owner has to route it.
    presentation.wheel(stream, 100.0, 300.0, 0.0, -1.0);
    presentation.take_scrolls(scrolls);
    require(scrolls.size() == 1U && !scrolls.front().applied && scrolls.front().notches_y == -1.0 &&
                scrolls.front().y == 300.0,
            "a wheel over a widget that consumes it was not handed back");
}

void failed_candidate_keeps_live_programs(const std::string_view backend) {
    if (backend == "reference")
        return;
    RuntimeOptions options;
    options.task_execution = TaskExecution::deterministic;
    Runtime runtime(std::move(options));
    runtime.configure_application(ApplicationOptions{
        .id = "presentation.programs",
        .schemas_json = R"({"widgets":{"definitions":[]},"actions":{"definitions":[]},"host":[],
            "materials":{"definitions":[{"id":"test:shared","blendMode":"straight_alpha",
            "fallback":"strata:rounded_rect","parameters":[],"shaders":{"hlsl":"shared.hlsl"}}]}})",
    });
    require(runtime.activate(SourceActivation{
        .generation = 1U, .entry_source_id = "programs.strata",
        .entry_text = R"(style Fill {
            width: { weight: 1 }; height: { weight: 1 }; background: #FFFFFFFF;
        }
        overlay Main { root Panel(style: Fill, material: material("test:shared")) })",
    }).activated(), "program isolation application did not activate");
    SurfaceOptions surface_options;
    surface_options.id = "presentation.programs.current";
    surface_options.root_role = SurfaceRootRole::overlay;
    surface_options.root_name = "Main";
    surface_options.environment.framebuffer_width = 32;
    surface_options.environment.framebuffer_height = 32;
    surface_options.environment.logical_width = 32;
    surface_options.environment.logical_height = 32;
    Surface current = runtime.create_surface(surface_options);
    surface_options.id = "presentation.programs.candidate";
    Surface candidate = runtime.create_surface(surface_options);
    const auto probe = std::make_shared<Probe>();
    host::Presentation presentation([probe, backend] {
        return std::make_unique<OffscreenDriver>(probe, backend);
    }, host::PresentationExecution::caller);
    presentation.resize({32U, 32U, 32.0, 32.0});
    const auto old_stream = presentation.attach("scene");
    const auto new_stream = presentation.attach("scene");
    const auto material = [](const std::string_view id, const std::string_view source) {
        host::MaterialSource result;
        result.declaration.id = id;
        result.source = source;
        return result;
    };
    presentation.declare_programs(old_stream, {{
        material("test:shared", "float4 material(PixelInput input) { return float4(1,0,0,1); }"),
    }, {}}).get();
    std::int64_t time = 0;
    const auto capture_current = [&] {
        const auto request = presentation.capture_next_frame();
        auto framed = host::frame_surface(current.native_handle(), ++time);
        host::PresentationCommit commit;
        commit.order.push_back(old_stream);
        commit.layers.push_back({old_stream, std::move(framed.encoded), framed.surface.unchanged != 0U});
        static_cast<void>(presentation.commit(std::move(commit)));
        std::vector<host::PresentationFrame> frames;
        presentation.take_frames(frames);
        for (auto& frame : frames) {
            if (std::ranges::find(frame.capture_requests, request) != frame.capture_requests.end())
                return std::move(frame.capture);
        }
        return std::optional<host::PresentationCapture>{};
    };
    auto capture = capture_current();
    const std::size_t center = (16U * 32U + 16U) * 4U;
    require(capture.has_value() && capture->rgba[center] == 255U &&
                capture->rgba[center + 2U] == 0U, "initial authored material did not render");
    const auto failed = presentation.declare_programs(new_stream, {{
        material("test:shared", "float4 material(PixelInput input) { return float4(0,0,1,1); }"),
        material("test:invalid", "invalid shader source"),
    }, {}});
    bool rejected = false;
    try {
        failed.get();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "invalid candidate shader was accepted");
    const auto repeated = presentation.declare_programs(new_stream, {{
        material("test:invalid", "invalid shader source"),
    }, {}});
    rejected = false;
    try {
        repeated.get();
    } catch (const std::exception&) {
        rejected = true;
    }
    require(rejected, "failed compilation left an apparently usable cached program");
    for (unsigned attempt = 0U; attempt != 2U; ++attempt) {
        host::EffectSource invalid;
        invalid.declaration.effect_id = "test:invalid";
        invalid.declaration.kind = EffectPassKind::shader;
        invalid.source = "invalid effect source";
        host::PresentationPrograms effects;
        effects.effects.push_back(std::move(invalid));
        const auto effect_failure = presentation.declare_programs(new_stream, std::move(effects));
        rejected = false;
        try {
            effect_failure.get();
        } catch (const std::exception&) {
            rejected = true;
        }
        require(rejected, "failed effect compilation became accepted on retry");
    }
    capture = capture_current();
    require(capture.has_value() && capture->rgba[center] == 255U &&
                capture->rgba[center + 2U] == 0U,
            "partially failed candidate declarations changed the live scene's shader");
    presentation.retire(new_stream, candidate.prepare_release_packet()).get();
    candidate.acknowledge_release_packet();
    candidate.close();
    candidate = runtime.create_surface(surface_options);
    const auto ready_stream = presentation.attach("scene");
    presentation.declare_programs(ready_stream, {{
        material("test:shared", "float4 material(PixelInput input) { return float4(0,0,1,1); }"),
    }, {}}).get();
    const auto retired_current = presentation.retire(old_stream, current.prepare_release_packet());
    retired_current.get();
    current.acknowledge_release_packet();
    current.close();
    const auto promoted_capture = presentation.capture_next_frame();
    auto promoted = host::frame_surface(candidate.native_handle(), ++time);
    host::PresentationCommit promotion;
    promotion.order.push_back(ready_stream);
    promotion.layers.push_back({ready_stream, std::move(promoted.encoded), false});
    static_cast<void>(presentation.commit(std::move(promotion)));
    std::vector<host::PresentationFrame> frames;
    presentation.take_frames(frames);
    capture.reset();
    for (auto& frame : frames) {
        if (std::ranges::find(frame.capture_requests, promoted_capture) != frame.capture_requests.end())
            capture = std::move(frame.capture);
    }
    require(capture.has_value() && capture->rgba[center] == 0U &&
                capture->rgba[center + 2U] == 255U,
            "retiring the previous scene removed the candidate's separately owned program");
    const auto retired_candidate = presentation.retire(ready_stream, candidate.prepare_release_packet());
    retired_candidate.get();
    candidate.acknowledge_release_packet();
    candidate.close();
    runtime.close();
}
} // namespace

int main(const int argc, char** argv) {
    try {
        ordered_packets_and_idle_feedback(argc > 1 ? std::string_view(argv[1]) : "reference");
        failed_candidate_keeps_live_programs(argc > 1 ? std::string_view(argv[1]) : "reference");
        scroll_moves_without_the_owner(argc > 1 ? std::string_view(argv[1]) : "reference");
        std::cout << "presentation ownership tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "presentation ownership tests failed: " << error.what() << '\n';
        return 1;
    }
}
