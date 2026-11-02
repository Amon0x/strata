// Thread soak: the real showcase on real threads, driven at random the way a person and a window
// system would. Each window owns a threaded Presentation and asynchronous Sessions, and keeps
// switching tabs, scrolling from the render thread, resizing, minimizing, reloading, opening and
// closing an overlay, dropping preparations part way, and closing at any moment. Pass means nothing
// threw and nothing hung. It is meant to run under ThreadSanitizer and AddressSanitizer, which turn
// what would otherwise be a one-in-a-million crash into a report.
//
// usage: strata_thread_soak <resource-root> <scenario.json>
//            [--seconds=N] [--windows=N] [--workers=N] [--seed=N] [--scale=X] [--stall-limit=N] [--backend=NAME]

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <strata/presentation.hpp>
#include <strata/strata.hpp>

#include "application_host.hpp"
#include "capture_renderer.hpp"
#include "host/browser_model.hpp"
#include "host/resource_root.hpp"
#include "host/session.hpp"
#include "host_json.hpp"
#include "scenario.hpp"

namespace {
using namespace strata;
using Clock = std::chrono::steady_clock;

[[nodiscard]] std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();
}

struct Options final {
    std::filesystem::path resources;
    double seconds = 20.0;
    int windows = 2;
    std::uint64_t seed = 1U;
    double scale = 0.5;
    int stall_limit = 120;
    std::string backend = "reference";
};

/** What every window did; a run that did none of something proves nothing about it. */
struct Totals final {
    std::atomic<std::uint64_t> turns{0U};
    std::atomic<std::uint64_t> commits{0U};
    std::atomic<std::uint64_t> presented{0U};
    std::atomic<std::uint64_t> tab_clicks{0U};
    std::atomic<std::uint64_t> wheels_applied{0U};
    std::atomic<std::uint64_t> wheels_returned{0U};
    std::atomic<std::uint64_t> resizes{0U};
    std::atomic<std::uint64_t> minimizes{0U};
    std::atomic<std::uint64_t> prepares{0U};
    std::atomic<std::uint64_t> cancels{0U};
    std::atomic<std::uint64_t> adoptions{0U};
    std::atomic<std::uint64_t> retires{0U};
    std::atomic<std::uint64_t> overlays{0U};
    std::atomic<std::uint64_t> orderly_closes{0U};
    std::atomic<std::uint64_t> abandons{0U};
    std::mutex mutex;
    /** Each code's count and the first thing it said. */
    std::map<std::string, std::pair<std::uint64_t, std::string>> diagnostics;
};

/** Software pixels on the render thread: resources, draws and backdrop samples like a real driver. */
class Driver final : public host::PresentationDriver {
  public:
    explicit Driver(const std::string_view backend) : renderer_(headless::create_capture_renderer(backend)) {
        renderer_->set_clear_color({9U, 11U, 15U, 255U});
    }
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
    void release_programs(const std::string_view prefix) override {
        renderer_->release_programs(prefix);
    }
    void resize(const host::PresentationViewport& viewport) override {
        available_ = viewport.width != 0U && viewport.height != 0U;
        if (available_)
            renderer_->resize(viewport.width, viewport.height, viewport.logical_width, viewport.logical_height);
    }
    void set_pointer(std::optional<std::array<double, 2U>>) override {}
    void set_transparent(bool) override {}
    void consume_resources(const host::RenderPacket& packet) override {
        renderer_->consume_resources(packet);
    }
    void release_layer(std::string_view) noexcept override {}
    [[nodiscard]] bool begin_frame() override {
        drew_ = false;
        return available_;
    }
    [[nodiscard]] host::PresentationLayerTiming draw(
        std::string_view, const host::RenderPacket& packet) override {
        renderer_->render(packet, 0);
        drew_ = true;
        return {};
    }
    [[nodiscard]] std::vector<strata_backdrop_sample> take_backdrop_samples(std::string_view) override {
        return renderer_->take_backdrop_samples();
    }
    [[nodiscard]] host::PresentationResult end_frame() override {
        if (!drew_)
            renderer_->render(host::RenderPacket{}, 0);
        host::PresentationResult result;
        result.presented = true;
        return result;
    }
    void capture_next_frame() override {}
    [[nodiscard]] std::optional<host::PresentationCapture> take_capture() override { return std::nullopt; }

  private:
    std::unique_ptr<headless::CaptureRenderer> renderer_;
    bool available_ = false;
    bool drew_ = false;
};

struct Scene final {
    std::unique_ptr<host::Session> session;
    host::PresentationStream stream = 0U;
    std::optional<host::PresentationReceipt> programs;
    std::optional<host::PresentationReceipt> release;
    std::uint64_t scroll_serial = 0U;
    std::uint64_t scroll_ack = 0U;
    bool scroll_awaits_packet = false;
};

/** One thing on screen: the showcase itself, or an overlay over it, each reloadable on its own. */
struct Slot final {
    std::optional<host::SessionPreparation> preparation;
    std::unique_ptr<host::Bindings> bindings;
    std::uint64_t prepared_environment = 0U;
    std::unique_ptr<Scene> candidate;
    std::unique_ptr<Scene> live;
};

class Window final {
  public:
    Window(const Options& options, const headless::Scenario& scenario,
           const std::vector<std::pair<double, double>>& tabs, Totals& totals, const std::uint64_t seed)
        : options_(options), scenario_(scenario), tabs_(tabs), totals_(totals), random_(seed) {}

    [[nodiscard]] std::int64_t heartbeat() const noexcept { return beat_.load(std::memory_order_relaxed); }
    [[nodiscard]] const char* doing() const noexcept { return doing_.load(std::memory_order_relaxed); }

    /** Lives one after another until the deadline: each opens a window and closes it at random. */
    void run(const Clock::time_point deadline) {
        while (Clock::now() < deadline)
            life(deadline);
    }

  private:
    void note(const char* const what) noexcept {
        doing_.store(what, std::memory_order_relaxed);
        beat_.store(now(), std::memory_order_relaxed);
    }

    [[nodiscard]] bool chance(const int per_mille) {
        return std::uniform_int_distribution<int>(0, 999)(random_) < per_mille;
    }
    [[nodiscard]] double between(const double low, const double high) {
        return std::uniform_real_distribution<double>(low, high)(random_);
    }

    [[nodiscard]] strata_surface_environment make_environment() {
        strata_surface_environment result{};
        result.struct_size = sizeof(result);
        result.generation = ++environment_generation_;
        result.scale = options_.scale;
        result.logical_width = width_;
        result.logical_height = height_;
        result.framebuffer_width = std::max(1, static_cast<int>(width_ * options_.scale));
        result.framebuffer_height = std::max(1, static_cast<int>(height_ * options_.scale));
        result.point_snapping = STRATA_POINT_SNAP_NEAREST;
        result.rectangle_snapping = STRATA_RECTANGLE_SNAP_OUTWARD;
        result.density = STRATA_SURFACE_DENSITY_COMFORTABLE;
        result.pointer_precision = STRATA_POINTER_PRECISION_FINE;
        result.input_capabilities = STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD;
        result.text_antialiasing = STRATA_TEXT_ANTIALIASING_GRAYSCALE;
        return result;
    }

    void resize_target() {
        presentation_->resize({static_cast<std::uint32_t>(environment_.framebuffer_width),
                               static_cast<std::uint32_t>(environment_.framebuffer_height),
                               environment_.logical_width, environment_.logical_height});
    }

    void prepare(Slot& slot) {
        note("prepare");
        if (slot.preparation.has_value())
            ++totals_.cancels;
        slot.preparation.reset();
        slot.bindings = std::make_unique<host::Bindings>(scenario_.application_id);
        for (const std::string& action : scenario_.actions)
            slot.bindings->on(action, [](const host::ActionEvent&) { return host::ActionResult::handled; });
        for (const auto& snapshot : scenario_.snapshots) {
            slot.bindings->snapshot(snapshot.id, [this] { return model_.value(); },
                                    [values = host::from_json_value(snapshot.values)] { return values; });
        }
        host::SessionPrepareOptions prepare;
        prepare.resources = std::make_shared<host::ResourceRoot>(options_.resources);
        prepare.schemas_resource = scenario_.schemas.generic_string();
        prepare.module_resource = scenario_.module.generic_string();
        prepare.program_language = "hlsl";
        prepare.generation = ++generation_;
        prepare.time_nanoseconds = now();
        prepare.session.application_id = scenario_.application_id;
        prepare.session.task_execution = TaskExecution::asynchronous;
        prepare.session.extension_packages = scenario_.packages;
        prepare.session.extension_search_paths = scenario_.extension_search_paths;
        prepare.session.frame_slicing = true;
        prepare.session.frame_wake = [this] { wakes_.fetch_add(1U, std::memory_order_relaxed); };
        // A long frame hands the wheel over, as the desktop host's message poll does.
        prepare.session.busy_poll = [this] { pass_wheel(); };
        prepare.session.on_diagnostic = [this](const strata_diagnostic& diagnostic) {
            const std::scoped_lock lock(totals_.mutex);
            auto& entry = totals_.diagnostics[std::string(diagnostic.code.data, diagnostic.code.size)];
            if (entry.first++ == 0U)
                entry.second.assign(diagnostic.message.data, diagnostic.message.size);
        };
        prepare.snapshots = slot.bindings->capture_snapshots();
        prepare.actions = slot.bindings->capture_actions();
        prepare.surface.id = scenario_.surface_id;
        prepare.surface.root_role = scenario_.root_role == "screen" ? STRATA_SURFACE_ROOT_SCREEN
                                                                    : STRATA_SURFACE_ROOT_OVERLAY;
        prepare.surface.root_name = scenario_.root;
        prepare.surface.environment = environment_;
        slot.prepared_environment = environment_.generation;
        for (const auto& font : scenario_.fonts)
            prepare.surface.fonts.push_back({font.id, font.resource, font.system});
        for (const auto& image : scenario_.images)
            prepare.surface.images.push_back({image.id, image.resource, image.sampling});
        slot.preparation.emplace(host::Session::prepare(std::move(prepare)));
        ++totals_.prepares;
    }

    void pass_wheel() {
        if (presentation_ == nullptr || !chance(50))
            return;
        presentation_->wheel(interactive_stream_.load(std::memory_order_relaxed), between(0.0, width_),
                             between(0.0, height_), 0.0, between(-3.0, 3.0));
    }

    void retire(std::unique_ptr<Scene>& scene) {
        if (scene == nullptr)
            return;
        note("retire");
        scene->release = presentation_->retire(scene->stream, scene->session->prepare_release());
        retiring_.push_back(std::move(scene));
        ++totals_.retires;
    }

    void collect_retired() {
        for (auto scene = retiring_.begin(); scene != retiring_.end();) {
            if (!(*scene)->release->ready()) {
                ++scene;
                continue;
            }
            note("collect retired");
            (*scene)->release->get();
            (*scene)->session->acknowledge_release();
            (*scene)->session->close({});
            scene = retiring_.erase(scene);
        }
    }

    [[nodiscard]] Scene* scene_of(const host::PresentationStream stream) noexcept {
        for (Slot& slot : slots_) {
            if (slot.live != nullptr && slot.live->stream == stream)
                return slot.live.get();
            if (slot.candidate != nullptr && slot.candidate->stream == stream)
                return slot.candidate.get();
        }
        return nullptr;
    }

    void enqueue(strata_input_event event) {
        Slot& slot = slots_[slots_[1U].live != nullptr ? 1U : 0U];
        if (slot.live == nullptr)
            return;
        event.struct_size = sizeof(event);
        event.version = STRATA_INPUT_EVENT_VERSION_2;
        event.timestamp_nanoseconds = now();
        slot.live->session->enqueue(std::span<const strata_input_event>(&event, 1U));
    }

    void pointer(const std::uint32_t kind, const double x, const double y) {
        strata_input_event event{};
        event.kind = kind;
        event.x = x;
        event.y = y;
        enqueue(event);
    }

    void adopt_feedback() {
        note("adopt feedback");
        presentation_->rethrow_failure();
        presentation_->take_scrolls(scrolls_);
        for (const host::PresentationScroll& scroll : scrolls_) {
            Scene* const scene = scene_of(scroll.stream);
            if (scene == nullptr || scene->release.has_value())
                continue;
            if (!scroll.applied) {
                ++totals_.wheels_returned;
                strata_input_event event{};
                event.struct_size = sizeof(event);
                event.version = STRATA_INPUT_EVENT_VERSION_2;
                event.kind = STRATA_INPUT_SCROLL;
                event.x = scroll.x;
                event.y = scroll.y;
                event.delta_x = scroll.notches_x;
                event.delta_y = scroll.notches_y;
                event.timestamp_nanoseconds = now();
                scene->session->enqueue(std::span<const strata_input_event>(&event, 1U));
                continue;
            }
            ++totals_.wheels_applied;
            scene->scroll_serial = scroll.serial;
            if (scene->session->surface().adopt_scroll(scroll.identity, scroll.delta_x, scroll.delta_y))
                scene->scroll_awaits_packet = true;
        }
        presentation_->take_frames(rendered_);
        for (const host::PresentationFrame& frame : rendered_) {
            if (frame.presented)
                ++totals_.presented;
            for (const host::PresentationLayerFeedback& feedback : frame.layers) {
                Scene* const scene = scene_of(feedback.stream);
                if (scene == nullptr || scene->release.has_value())
                    continue;
                if (!feedback.backdrop.empty())
                    scene->session->surface().report_backdrop(feedback.backdrop);
                scene->session->surface().record_host_frame(feedback.profiler);
            }
        }
    }

    void adopt_prepared(Slot& slot) {
        if (slot.preparation.has_value() && slot.preparation->ready()) {
            note("adopt prepared");
            auto scene = std::make_unique<Scene>();
            auto prepared = slot.preparation->take({}, std::move(slot.bindings));
            slot.preparation.reset();
            scene->session = std::move(prepared.session);
            if (slot.prepared_environment != environment_.generation &&
                !scene->session->adopt_environment(environment_))
                throw std::runtime_error("a prepared Session rejected the current environment");
            scene->stream = presentation_->attach(scenario_.surface_id);
            scene->programs = presentation_->declare_programs(
                scene->stream, {std::move(prepared.materials), std::move(prepared.effects)});
            retire(slot.candidate);
            slot.candidate = std::move(scene);
            ++totals_.adoptions;
        }
        if (slot.candidate != nullptr && slot.candidate->programs.has_value() &&
            slot.candidate->programs->ready()) {
            slot.candidate->programs->get();
            slot.candidate->programs.reset();
        }
    }

    /** Frames each scene into one commit; a ready candidate replaces what it reloads atomically. */
    void frame_scenes() {
        note("frame");
        host::PresentationCommit commit;
        bool idle = true;
        const std::int64_t time = now();
        std::vector<std::unique_ptr<Scene>> replaced;
        for (Slot& slot : slots_) {
            if (slot.candidate != nullptr && !slot.candidate->programs.has_value()) {
                if (slot.live != nullptr)
                    replaced.push_back(std::move(slot.live));
                slot.live = std::move(slot.candidate);
            }
            if (slot.live == nullptr)
                continue;
            Scene& scene = *slot.live;
            const std::uint64_t adopted = scene.scroll_serial;
            auto framed = scene.session->frame(time);
            idle = idle && framed.surface.idle != 0U;
            if (!scene.scroll_awaits_packet || framed.surface.unchanged == 0U) {
                scene.scroll_awaits_packet = false;
                scene.scroll_ack = adopted;
            }
            commit.order.push_back(scene.stream);
            commit.layers.push_back({scene.stream, std::move(framed.encoded), framed.surface.unchanged != 0U,
                                     scene.scroll_ack});
        }
        Slot& front = slots_[slots_[1U].live != nullptr ? 1U : 0U];
        if (front.live != nullptr)
            interactive_stream_.store(front.live->stream, std::memory_order_relaxed);
        commit.animate = !idle;
        note("commit");
        if (!commit.layers.empty()) {
            static_cast<void>(presentation_->commit(std::move(commit)));
            ++totals_.commits;
        }
        for (std::unique_ptr<Scene>& scene : replaced)
            retire(scene);
    }

    /** About one tick a millisecond, a busy person's input; a window turning faster adds nothing. */
    void chaos() {
        note("chaos");
        const std::int64_t time = now();
        if (time < next_chaos_)
            return;
        next_chaos_ = time + 1'000'000;
        // Now and then a burst past what the input queue holds, which must not cost the application.
        if (chance(1)) {
            for (int index = 0; index < 600; ++index) {
                strata_input_event event{};
                event.kind = STRATA_INPUT_KEY;
                event.text = strata_string_view{"Shift", 5U};
                event.key_action = index % 2 == 0 ? STRATA_KEY_PRESS : STRATA_KEY_RELEASE;
                enqueue(event);
            }
        }
        if (chance(200))
            pointer(STRATA_INPUT_POINTER_MOVE, between(0.0, width_), between(0.0, height_));
        if (chance(30)) {
            const double x = between(0.0, width_);
            const double y = between(0.0, height_);
            pointer(STRATA_INPUT_POINTER_PRESS, x, y);
            pointer(STRATA_INPUT_POINTER_RELEASE, x, y);
        }
        if (chance(10) && !tabs_.empty()) {
            const auto tab = tabs_[std::uniform_int_distribution<std::size_t>(0U, tabs_.size() - 1U)(random_)];
            pointer(STRATA_INPUT_POINTER_MOVE, tab.first, tab.second);
            pointer(STRATA_INPUT_POINTER_PRESS, tab.first, tab.second);
            pointer(STRATA_INPUT_POINTER_RELEASE, tab.first, tab.second);
            ++totals_.tab_clicks;
        }
        if (chance(100)) {
            strata_input_event event{};
            event.kind = STRATA_INPUT_SCROLL;
            event.x = between(0.0, width_);
            event.y = between(0.0, height_);
            event.delta_y = between(-4.0, 4.0);
            enqueue(event);
        }
        if (chance(10)) {
            static constexpr std::array<std::string_view, 4U> keys{"Tab", "ArrowDown", "Enter", "Escape"};
            const std::string_view key =
                keys[std::uniform_int_distribution<std::size_t>(0U, keys.size() - 1U)(random_)];
            strata_input_event event{};
            event.kind = STRATA_INPUT_KEY;
            event.text = strata_string_view{key.data(), key.size()};
            event.key_action = STRATA_KEY_PRESS;
            enqueue(event);
            event.key_action = STRATA_KEY_RELEASE;
            enqueue(event);
        }
        if (chance(10))
            model_.changed();
        if (chance(5))
            resize();
        if (chance(1)) {
            ++totals_.minimizes;
            presentation_->resize({});
            minimized_turns_ = std::uniform_int_distribution<int>(1, 60)(random_);
        }
        if (chance(1))
            prepare(slots_[0U]);
        if (chance(1)) {
            Slot& overlay = slots_[1U];
            if (overlay.live != nullptr || overlay.candidate != nullptr || overlay.preparation.has_value()) {
                if (overlay.preparation.has_value())
                    ++totals_.cancels;
                overlay.preparation.reset();
                retire(overlay.candidate);
                retire(overlay.live);
            } else {
                ++totals_.overlays;
                prepare(overlay);
            }
        }
        if (chance(1) && chance(300)) {
            // Opening and changing one's mind over and over, each ticket dropped part way.
            for (int index = 0; index < 4; ++index)
                prepare(slots_[1U].live != nullptr ? slots_[1U] : slots_[0U]);
        }
        close_requested_ = close_requested_ || (chance(1) && chance(200));
    }

    void resize() {
        ++totals_.resizes;
        width_ = between(320.0, 1600.0);
        height_ = between(240.0, 1000.0);
        environment_ = make_environment();
        resize_target();
        for (Slot& slot : slots_) {
            for (Scene* const scene : {slot.live.get(), slot.candidate.get()}) {
                if (scene != nullptr && !scene->session->adopt_environment(environment_))
                    throw std::runtime_error("a Session rejected a new environment");
            }
        }
    }

    /** Stops the wheel thread even when the life unwinds, before what it posts to goes away. */
    struct WheelThread final {
        std::atomic<bool> running{true};
        std::thread thread;
        void stop() {
            running.store(false, std::memory_order_relaxed);
            if (thread.joinable())
                thread.join();
        }
        ~WheelThread() { stop(); }
    };

    void life(const Clock::time_point deadline) {
        note("open");
        width_ = 1280.0;
        height_ = 800.0;
        minimized_turns_ = 0;
        close_requested_ = false;
        environment_ = make_environment();
        slots_ = {};
        retiring_.clear();
        interactive_stream_.store(0U, std::memory_order_relaxed);
        host::Presentation presentation([backend = options_.backend] { return std::make_unique<Driver>(backend); },
                                        host::PresentationExecution::threaded,
                                        [this] { wakes_.fetch_add(1U, std::memory_order_relaxed); });
        presentation_ = &presentation;
        resize_target();
        prepare(slots_[0U]);
        // Wheels from a thread of their own: the render thread's entry point has to take that.
        WheelThread wheels;
        wheels.thread = std::thread([&wheels, &presentation, this, seed = random_()] {
            std::mt19937_64 random(seed);
            std::uniform_real_distribution<double> x(0.0, 1280.0);
            std::uniform_real_distribution<double> y(0.0, 800.0);
            std::uniform_real_distribution<double> notches(-3.0, 3.0);
            while (wheels.running.load(std::memory_order_relaxed)) {
                presentation.wheel(interactive_stream_.load(std::memory_order_relaxed), x(random), y(random),
                                   0.0, notches(random));
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
        bool closing = false;
        while (!closing) {
            adopt_feedback();
            collect_retired();
            ++totals_.turns;
            if (slots_[0U].live != nullptr)
                chaos();
            if (minimized_turns_ > 0) {
                note("minimized");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (--minimized_turns_ == 0)
                    resize_target();
            } else if (!presentation.ready_for_frame()) {
                std::this_thread::yield();
            } else {
                for (Slot& slot : slots_)
                    adopt_prepared(slot);
                frame_scenes();
            }
            closing = Clock::now() >= deadline || close_requested_;
        }
        const bool orderly = Clock::now() >= deadline || chance(500);
        wheels.stop();
        if (orderly) {
            note("orderly close");
            for (Slot& slot : slots_) {
                if (slot.preparation.has_value())
                    ++totals_.cancels;
                slot.preparation.reset();
                retire(slot.candidate);
                retire(slot.live);
            }
            while (!retiring_.empty()) {
                note("waiting for releases");
                presentation.rethrow_failure();
                collect_retired();
                std::this_thread::yield();
            }
            ++totals_.orderly_closes;
        } else {
            // A crash-path close: tickets and Sessions go with work still running behind them.
            note("abandon");
            for (Slot& slot : slots_) {
                slot.preparation.reset();
                for (Scene* const scene : {slot.live.get(), slot.candidate.get()}) {
                    if (scene != nullptr)
                        scene->session->abandon();
                }
                slot.candidate.reset();
                slot.live.reset();
            }
            for (std::unique_ptr<Scene>& scene : retiring_)
                scene->session->abandon();
            retiring_.clear();
            ++totals_.abandons;
        }
        note("stop");
        presentation.stop();
        presentation_ = nullptr;
        slots_ = {};
    }

    const Options& options_;
    const headless::Scenario& scenario_;
    const std::vector<std::pair<double, double>>& tabs_;
    Totals& totals_;
    std::mt19937_64 random_;
    host::Presentation* presentation_ = nullptr;
    std::array<Slot, 2U> slots_;
    std::vector<std::unique_ptr<Scene>> retiring_;
    std::vector<host::PresentationScroll> scrolls_;
    std::vector<host::PresentationFrame> rendered_;
    strata_surface_environment environment_{};
    std::uint64_t environment_generation_ = 0U;
    std::uint64_t generation_ = 0U;
    host::Revision model_;
    double width_ = 1280.0;
    double height_ = 800.0;
    int minimized_turns_ = 0;
    std::int64_t next_chaos_ = 0;
    bool close_requested_ = false;
    std::atomic<host::PresentationStream> interactive_stream_{0U};
    std::atomic<std::uint64_t> wakes_{0U};
    std::atomic<std::int64_t> beat_{0};
    std::atomic<const char*> doing_{"starting"};
};

/** Every showcase tab's centre, from one deterministic frame at the window's opening size. */
[[nodiscard]] std::vector<std::pair<double, double>> locate_tabs(headless::Scenario scenario,
                                                                 const std::filesystem::path& resources) {
    scenario.render_backend = "none";
    scenario.steps.clear();
    scenario.width = 1280.0;
    scenario.height = 800.0;
    headless::ApplicationHost host(scenario, resources);
    host.set_capture_frames(true);
    host.frame(16'000'000);
    host.frame(32'000'000);
    const host::BrowserModel model = host::BrowserModel::build(host.frame_document(), scenario.width, scenario.height);
    std::vector<std::pair<double, double>> result;
    for (const char* const name :
         {"MENU", "BLUEPRINT", "INTERACT", "TYPE", "LAYOUT", "DRAW", "STATE", "CONTROL", "FORMS", "MANIPULATE",
          "ADAPT", "INSPECT", "DATA", "SHELL", "SYSTEM", "MEDIA", "DESKTOP", "NET", "LOTTIE", "CONTENT", "EXTEND"}) {
        result.push_back(model.resolve(host::Selector{std::nullopt, std::nullopt, std::string(name), std::string("tab")}));
    }
    host.close();
    return result;
}

[[nodiscard]] std::optional<std::string_view> flag(const std::string_view argument, const std::string_view name) {
    return argument.starts_with(name) ? std::optional(argument.substr(name.size())) : std::nullopt;
}

} // namespace

int main(const int argc, char** argv) {
    try {
        if (argc < 3) {
            std::fprintf(stderr, "usage: strata_thread_soak <resource-root> <scenario.json> [--seconds=N] "
                                 "[--windows=N] [--workers=N] [--seed=N] [--scale=X] [--stall-limit=N]\n");
            return 2;
        }
        Options options;
        options.resources = argv[1];
        for (int index = 3; index < argc; ++index) {
            const std::string_view argument = argv[index];
            if (const auto value = flag(argument, "--seconds="))
                options.seconds = std::atof(std::string(*value).c_str());
            else if (const auto windows = flag(argument, "--windows="))
                options.windows = std::atoi(std::string(*windows).c_str());
            else if (const auto workers = flag(argument, "--workers="))
                static_cast<void>(strata_limit_task_workers(static_cast<std::uint32_t>(std::atoi(std::string(*workers).c_str()))));
            else if (const auto seed = flag(argument, "--seed="))
                options.seed = std::strtoull(std::string(*seed).c_str(), nullptr, 10);
            else if (const auto scale = flag(argument, "--scale="))
                options.scale = std::atof(std::string(*scale).c_str());
            else if (const auto limit = flag(argument, "--stall-limit="))
                options.stall_limit = std::atoi(std::string(*limit).c_str());
            else if (const auto backend = flag(argument, "--backend="))
                options.backend = *backend;
            else
                throw std::invalid_argument("unknown argument " + std::string(argument));
        }
        const headless::Scenario scenario = headless::load_scenario(argv[2]);
        const auto tabs = locate_tabs(scenario, options.resources);
        std::printf("seed %llu, %d windows, %.0f s, %zu tabs\n", static_cast<unsigned long long>(options.seed),
                    options.windows, options.seconds, tabs.size());
        std::fflush(stdout);

        Totals totals;
        std::vector<std::unique_ptr<Window>> windows;
        for (int index = 0; index < options.windows; ++index) {
            windows.push_back(std::make_unique<Window>(options, scenario, tabs, totals,
                                                       options.seed * 1'000'003U + static_cast<std::uint64_t>(index)));
        }
        const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                                  std::chrono::duration<double>(options.seconds));
        std::mutex failure_mutex;
        std::vector<std::string> failures;
        std::atomic<int> running{options.windows};
        std::vector<std::thread> threads;
        for (const std::unique_ptr<Window>& window : windows) {
            threads.emplace_back([&, target = window.get()] {
                try {
                    target->run(deadline);
                } catch (const std::exception& error) {
                    const std::scoped_lock lock(failure_mutex);
                    failures.emplace_back(std::string(target->doing()) + ": " + error.what());
                }
                running.fetch_sub(1, std::memory_order_relaxed);
            });
        }
        // A window that stops turning is a deadlock or a lost wake: say where, and leave a dump.
        while (running.load(std::memory_order_relaxed) != 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            const std::int64_t limit = std::int64_t{options.stall_limit} * 1'000'000'000;
            for (const std::unique_ptr<Window>& window : windows) {
                const std::int64_t beat = window->heartbeat();
                if (beat != 0 && now() - beat > limit && running.load(std::memory_order_relaxed) != 0 &&
                    Clock::now() < deadline + std::chrono::seconds(options.stall_limit)) {
                    std::fprintf(stderr, "STALL: a window has not turned for %d s; last doing: %s\n",
                                 options.stall_limit, window->doing());
                    std::fflush(stderr);
                    std::abort();
                }
            }
        }
        for (std::thread& thread : threads)
            thread.join();

        std::printf("turns %llu, commits %llu, presented %llu, tab clicks %llu, wheels applied %llu, returned %llu\n",
                    static_cast<unsigned long long>(totals.turns.load()), static_cast<unsigned long long>(totals.commits.load()),
                    static_cast<unsigned long long>(totals.presented.load()), static_cast<unsigned long long>(totals.tab_clicks.load()),
                    static_cast<unsigned long long>(totals.wheels_applied.load()), static_cast<unsigned long long>(totals.wheels_returned.load()));
        std::printf("resizes %llu, minimizes %llu, prepares %llu, cancels %llu, adoptions %llu, retires %llu, overlays %llu, closes %llu, abandons %llu\n",
                    static_cast<unsigned long long>(totals.resizes.load()), static_cast<unsigned long long>(totals.minimizes.load()),
                    static_cast<unsigned long long>(totals.prepares.load()), static_cast<unsigned long long>(totals.cancels.load()),
                    static_cast<unsigned long long>(totals.adoptions.load()), static_cast<unsigned long long>(totals.retires.load()),
                    static_cast<unsigned long long>(totals.overlays.load()), static_cast<unsigned long long>(totals.orderly_closes.load()),
                    static_cast<unsigned long long>(totals.abandons.load()));
        for (const auto& [code, entry] : totals.diagnostics) {
            std::printf("diagnostic %s x %llu: %s\n", code.c_str(), static_cast<unsigned long long>(entry.first),
                        entry.second.c_str());
        }
        for (const std::string& failure : failures)
            std::fprintf(stderr, "FAILED during %s\n", failure.c_str());
        return failures.empty() ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
