#include "vulkan.hpp"

#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <strata/vulkan_presentation.hpp>

#include "desktop/launch.hpp"
#include "headless/png.hpp"
#include "host/resource_root.hpp"
#include "host/session.hpp"
#include "host_json.hpp"
#include "input.hpp"
#include "services.hpp"

namespace strata::preview {
namespace {
void check_sdl(const bool success, const std::string_view operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
void wake(const Uint32 type) {
    SDL_Event event{};
    event.type = type;
    check_sdl(SDL_PushEvent(&event) == 1, "wake Vulkan preview");
}

/** SDL owns platform surface creation; the GPU owner waits only for this one startup handoff. */
class SurfaceBridge final {
  public:
    SurfaceBridge(SDL_Window* window, const Uint32 event)
        : window_(window), event_(event), owner_(std::this_thread::get_id()) {}
    [[nodiscard]] VkSurfaceKHR create(const VkInstance instance) {
        if (std::this_thread::get_id() == owner_)
            return create_on_owner(instance);
        std::unique_lock lock(mutex_);
        if (closed_)
            throw std::runtime_error("preview closed before Vulkan surface creation");
        instance_ = instance;
        requested_ = true;
        wake(event_);
        ready_.wait(lock, [this] { return completed_ || closed_; });
        if (completed_) {
            if (failure_)
                std::rethrow_exception(failure_);
            return surface_;
        }
        throw std::runtime_error("preview cancelled Vulkan surface creation");
    }
    void pump() noexcept {
        VkInstance instance;
        {
            const std::scoped_lock lock(mutex_);
            if (!requested_ || completed_ || closed_)
                return;
            instance = instance_;
            requested_ = false;
        }
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        std::exception_ptr failure;
        try {
            surface = create_on_owner(instance);
        } catch (...) {
            failure = std::current_exception();
        }
        {
            const std::scoped_lock lock(mutex_);
            surface_ = surface;
            failure_ = std::move(failure);
            completed_ = true;
        }
        ready_.notify_one();
    }
    void close() noexcept {
        {
            const std::scoped_lock lock(mutex_);
            closed_ = true;
        }
        ready_.notify_one();
    }

  private:
    [[nodiscard]] VkSurfaceKHR create_on_owner(const VkInstance instance) const {
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        check_sdl(SDL_Vulkan_CreateSurface(window_, instance, &surface) == SDL_TRUE,
                  "create SDL Vulkan surface");
        return surface;
    }
    SDL_Window* window_;
    Uint32 event_;
    std::thread::id owner_;
    std::mutex mutex_;
    std::condition_variable ready_;
    VkInstance instance_{};
    VkSurfaceKHR surface_{};
    std::exception_ptr failure_;
    bool requested_ = false;
    bool completed_ = false;
    bool closed_ = false;
};

/** Failure teardown keeps the SDL window alive and pumps its platform messages until GPU exit. */
struct StopPresentation final {
    host::Presentation& presentation;
    SurfaceBridge& surface;
    ~StopPresentation() {
        surface.close();
        presentation.stop();
        while (!presentation.stopped()) {
            SDL_PumpEvents();
            SDL_Delay(1U);
        }
    }
};

/** Runtime state is main-thread-only; terminal receipts retain it until ordered GPU consumption. */
struct Scene final {
    Services services{false};
    std::unique_ptr<host::Session> session;
    host::PresentationStream stream = 0U;
    std::optional<host::PresentationReceipt> programs;
    std::optional<host::PresentationReceipt> release;
};

strata_surface_environment environment(const headless::Scenario& scenario,
                                       const std::uint64_t generation, const int width,
                                       const int pixel_width, const int pixel_height) {
    strata_surface_environment result{};
    result.struct_size = sizeof(result);
    result.generation = generation;
    result.framebuffer_width = pixel_width;
    result.framebuffer_height = pixel_height;
    result.scale = scenario.scale * static_cast<double>(pixel_width) / width;
    result.logical_width = pixel_width / result.scale;
    result.logical_height = pixel_height / result.scale;
    result.point_snapping = STRATA_POINT_SNAP_NEAREST;
    result.rectangle_snapping = STRATA_RECTANGLE_SNAP_OUTWARD;
    result.density = STRATA_SURFACE_DENSITY_COMFORTABLE;
    result.pointer_precision = STRATA_POINTER_PRECISION_FINE;
    result.input_capabilities = STRATA_SURFACE_INPUT_POINTER | STRATA_SURFACE_INPUT_KEYBOARD |
        STRATA_SURFACE_INPUT_TOUCH | STRATA_SURFACE_INPUT_IME | STRATA_SURFACE_INPUT_CLIPBOARD;
    result.reduced_motion = scenario.reduced_motion ? 1U : 0U;
    result.text_antialiasing = scenario.text_antialiasing == "subpixel" ? STRATA_TEXT_ANTIALIASING_SUBPIXEL_RGB
        : scenario.text_antialiasing == "grayscale"                     ? STRATA_TEXT_ANTIALIASING_GRAYSCALE
                                                                        : STRATA_TEXT_ANTIALIASING_SYSTEM;
    result.text_rasterizer = STRATA_TEXT_RASTERIZER_SYSTEM;
    return result;
}
} // namespace

int run_vulkan(const desktop::LaunchManifest& manifest, const bool smoke, const bool uncapped,
               std::filesystem::path screenshot) {
    const auto& scenario = manifest.application;
    const Uint32 flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                         (smoke ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
    std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow(manifest.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                         static_cast<int>(std::ceil(scenario.width * scenario.scale)),
                         static_cast<int>(std::ceil(scenario.height * scenario.scale)), flags),
        &SDL_DestroyWindow);
    check_sdl(window != nullptr, "create Vulkan preview window");
    SDL_SetWindowMinimumSize(window.get(), 480, 360);
    unsigned extension_count = 0U;
    check_sdl(SDL_Vulkan_GetInstanceExtensions(window.get(), &extension_count, nullptr) == SDL_TRUE,
              "query SDL Vulkan extensions");
    std::vector<const char*> extensions(extension_count);
    check_sdl(SDL_Vulkan_GetInstanceExtensions(window.get(), &extension_count, extensions.data()) == SDL_TRUE,
              "read SDL Vulkan extensions");
    const Uint32 wake_event = SDL_RegisterEvents(1);
    check_sdl(wake_event != static_cast<Uint32>(-1), "register Vulkan preview wake");
    const auto surface = std::make_shared<SurfaceBridge>(window.get(), wake_event);
    vulkan::WindowSurface target;
    for (const char* extension : extensions)
        target.instance_extensions.emplace_back(extension);
    target.create = [surface](const VkInstance instance) { return surface->create(instance); };
    vulkan::WindowOptions target_options;
    target_options.vsync = !uncapped;
    target_options.validation = SDL_getenv("STRATA_VULKAN_VALIDATION") != nullptr;
    target_options.clear_color = scenario.clear_color;
    host::Presentation presentation(
        [target = std::move(target), target_options]() mutable {
            return vulkan::create_window_driver(std::move(target), target_options);
        }, smoke || uncapped ? host::PresentationExecution::caller : host::PresentationExecution::threaded,
        [wake_event] { wake(wake_event); });
    std::unique_ptr<Scene> current;
    std::unique_ptr<Scene> candidate;
    std::vector<std::unique_ptr<Scene>> retiring;
    std::unique_ptr<host::Bindings> bindings;
    std::optional<host::SessionPreparation> preparation;
    StopPresentation stop{presentation, *surface};
    std::vector<host::PresentationFrame> feedback;
    std::optional<std::uint64_t> capture_request;
    std::uint64_t generation = 0U;
    std::uint64_t prepared_environment = 0U;
    std::uint64_t environment_generation = 1U;
    int width = 0, height = 0, pixel_width = 0, pixel_height = 0;
    SDL_GetWindowSize(window.get(), &width, &height);
    SDL_Vulkan_GetDrawableSize(window.get(), &pixel_width, &pixel_height);
    if (width <= 0 || pixel_width <= 0 || pixel_height <= 0)
        throw std::runtime_error("Vulkan preview has no initial drawable extent");
    auto viewport = environment(scenario, environment_generation, width, pixel_width, pixel_height);
    const auto resize_target = [&] {
        presentation.resize({static_cast<std::uint32_t>(viewport.framebuffer_width),
                             static_cast<std::uint32_t>(viewport.framebuffer_height),
                             viewport.logical_width, viewport.logical_height});
    };
    resize_target();
    const auto retire = [&](std::unique_ptr<Scene>& scene) {
        if (scene == nullptr)
            return;
        if (scene->session == nullptr) {
            scene.reset();
            return;
        }
        scene->services.set_ime_owner(false);
        scene->release = presentation.retire(scene->stream, scene->session->prepare_release());
        retiring.push_back(std::move(scene));
    };
    const auto collect_retired = [&] {
        for (auto scene = retiring.begin(); scene != retiring.end();) {
            if (!(*scene)->release->ready()) {
                ++scene;
                continue;
            }
            (*scene)->release->get();
            (*scene)->session->acknowledge_release();
            (*scene)->session->close({});
            scene = retiring.erase(scene);
        }
    };
    const auto prepare = [&] {
        preparation.reset();
        retire(candidate);
        bindings = std::make_unique<host::Bindings>(scenario.application_id);
        for (const std::string& action : scenario.actions) {
            bindings->on(action, [](const host::ActionEvent& event) {
                std::cout << "STRATA ACTION " << event.id << ' ' << event.payload.json() << '\n';
                return host::ActionResult::handled;
            });
        }
        host::SessionPrepareOptions options;
        options.resources = std::make_shared<host::ResourceRoot>(manifest.resource_root);
        options.schemas_resource = scenario.schemas.generic_string();
        options.module_resource = scenario.module.generic_string();
        options.program_language = "hlsl";
        options.generation = ++generation;
        options.time_nanoseconds = now();
        options.session.application_id = scenario.application_id;
        options.session.task_execution = smoke ? TaskExecution::deterministic : TaskExecution::asynchronous;
        options.session.extension_packages = scenario.packages;
        options.session.extension_search_paths = scenario.extension_search_paths;
        options.session.frame_slicing = true;
        options.session.frame_wake = [wake_event] { wake(wake_event); };
        options.session.on_diagnostic = [](const strata_diagnostic& diagnostic) {
            std::cerr << std::string_view(diagnostic.code.data, diagnostic.code.size) << ": "
                      << std::string_view(diagnostic.message.data, diagnostic.message.size) << '\n';
        };
        options.actions = bindings->capture_actions();
        options.snapshots.reserve(scenario.snapshots.size());
        for (const auto& snapshot : scenario.snapshots)
            options.snapshots.push_back({{snapshot.id, 0U}, host::from_json_value(snapshot.values)});
        options.surface.id = scenario.surface_id;
        options.surface.root_role = scenario.root_role == "screen" ? STRATA_SURFACE_ROOT_SCREEN
                                                                   : STRATA_SURFACE_ROOT_OVERLAY;
        options.surface.root_name = scenario.root;
        options.surface.environment = viewport;
        prepared_environment = viewport.generation;
        for (const auto& font : scenario.fonts)
            options.surface.fonts.push_back({font.id, font.resource, font.system});
        for (const auto& image : scenario.images)
            options.surface.images.push_back({image.id, image.resource, image.sampling});
        preparation.emplace(host::Session::prepare(std::move(options)));
    };
    prepare();
    Point pointer;
    bool running = true;
    bool drawn = false;
    bool minimized = false;
    while (running) {
        const auto frame_start = now();
        surface->pump();
        presentation.rethrow_failure();
        SDL_Event event;
        while (SDL_PollEvent(&event) != 0) {
            std::unique_ptr<char, decltype(&SDL_free)> editing(
                event.type == SDL_TEXTEDITING_EXT ? event.editExt.text : nullptr, &SDL_free);
            if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT &&
                                          event.window.event == SDL_WINDOWEVENT_CLOSE)) {
                running = false;
                break;
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                if (current != nullptr)
                    current->session->surface().cancel_interactions();
                SDL_CaptureMouse(SDL_FALSE);
            }
            if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_LEAVE &&
                SDL_GetMouseState(nullptr, nullptr) == 0U) {
                presentation.set_pointer(std::nullopt);
                if (current != nullptr) {
                    const auto leave = InputEvent::pointer(InputKind::pointer_move, {-1, -1}).native();
                    current->session->enqueue({&leave, 1U});
                }
            }
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F5 && event.key.repeat == 0) {
                prepare();
                continue;
            }
            auto input = translate_input(event, scenario.scale, pointer, frame_start);
            if (!input)
                continue;
            if (input->kind == InputKind::pointer_move || input->kind == InputKind::pointer_press ||
                input->kind == InputKind::pointer_release) {
                pointer = input->position;
                presentation.set_pointer(std::array<double, 2U>{pointer.x * viewport.scale,
                                                                pointer.y * viewport.scale});
            }
            if (event.type == SDL_MOUSEBUTTONDOWN)
                SDL_CaptureMouse(SDL_TRUE);
            if (event.type == SDL_MOUSEBUTTONUP && SDL_GetMouseState(nullptr, nullptr) == 0U)
                SDL_CaptureMouse(SDL_FALSE);
            if (current != nullptr) {
                const auto native = input->native();
                current->session->enqueue({&native, 1U});
            }
        }
        if (!running)
            break;
        const bool hidden = (SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) != 0U;
        if (hidden != minimized) {
            minimized = hidden;
            if (minimized)
                presentation.resize({});
            else
                resize_target();
        }
        if (minimized) {
            static_cast<void>(SDL_WaitEventTimeout(nullptr, 30));
            continue;
        }
        if (!presentation.ready_for_frame()) {
            static_cast<void>(SDL_WaitEventTimeout(nullptr, 4));
            continue;
        }
        int next_width = 0, next_height = 0, next_pixel_width = 0, next_pixel_height = 0;
        SDL_GetWindowSize(window.get(), &next_width, &next_height);
        SDL_Vulkan_GetDrawableSize(window.get(), &next_pixel_width, &next_pixel_height);
        if (next_width <= 0 || next_height <= 0 || next_pixel_width <= 0 || next_pixel_height <= 0) {
            static_cast<void>(SDL_WaitEventTimeout(nullptr, 16));
            continue;
        }
        if (next_width != width || next_height != height || next_pixel_width != pixel_width ||
            next_pixel_height != pixel_height) {
            width = next_width;
            height = next_height;
            pixel_width = next_pixel_width;
            pixel_height = next_pixel_height;
            viewport = environment(scenario, ++environment_generation, width, pixel_width, pixel_height);
            resize_target();
            if (current != nullptr && !current->session->adopt_environment(viewport))
                throw std::runtime_error("preview environment adoption failed");
            if (candidate != nullptr && !candidate->session->adopt_environment(viewport))
                throw std::runtime_error("preview candidate environment adoption failed");
        }
        std::optional<host::SurfacePacketCommit> candidate_commit;
        try {
            if (preparation.has_value() && preparation->ready()) {
                candidate = std::make_unique<Scene>();
                candidate->services.coordinate_scale = scenario.scale;
                host::SessionAdapters adapters;
                adapters.clipboard = candidate->services.clipboard();
                adapters.ime = candidate->services.ime();
                auto prepared = preparation->take(adapters, std::move(bindings));
                preparation.reset();
                candidate->session = std::move(prepared.session);
                if (prepared_environment != viewport.generation &&
                    !candidate->session->adopt_environment(viewport))
                    throw std::runtime_error("prepared preview environment adoption failed");
                candidate->stream = presentation.attach(scenario.surface_id);
                candidate->programs = presentation.declare_programs(
                    candidate->stream, {std::move(prepared.materials), std::move(prepared.effects)});
            }
            if (candidate != nullptr && candidate->programs->ready()) {
                candidate->programs->get();
                candidate->programs.reset();
                candidate_commit.emplace(candidate->session->frame(frame_start));
            }
        } catch (const std::exception& error) {
            if (current == nullptr)
                throw;
            std::cerr << "STRATA PREVIEW: " << error.what() << '\n';
            preparation.reset();
            retire(candidate);
        }
        if (candidate_commit.has_value()) {
            host::PresentationCommit commit;
            commit.order.push_back(candidate->stream);
            commit.layers.push_back({candidate->stream, std::move(candidate_commit->encoded),
                                     candidate_commit->surface.unchanged != 0U});
            commit.animate = candidate_commit->surface.idle == 0U;
            if (!screenshot.empty() && !capture_request.has_value())
                capture_request = presentation.capture_next_frame();
            static_cast<void>(presentation.commit(std::move(commit)));
            retire(current);
            current = std::move(candidate);
            current->services.set_ime_owner(true);
            drawn = false;
        } else if (current != nullptr) {
            auto frame = current->session->frame(frame_start);
            host::PresentationCommit commit;
            commit.order.push_back(current->stream);
            commit.layers.push_back({current->stream, std::move(frame.encoded), frame.surface.unchanged != 0U});
            commit.animate = frame.surface.idle == 0U;
            static_cast<void>(presentation.commit(std::move(commit)));
        }
        presentation.take_frames(feedback);
        for (const auto& frame : feedback) {
            for (const auto& layer : frame.layers) {
                if (current == nullptr || layer.stream != current->stream)
                    continue;
                current->session->surface().record_host_frame(layer.profiler);
                drawn = drawn || (frame.drawn && frame.presented);
            }
            if (capture_request.has_value() && frame.capture.has_value() &&
                std::ranges::find(frame.capture_requests, *capture_request) != frame.capture_requests.end()) {
                headless::write_png(screenshot, frame.capture->width, frame.capture->height, frame.capture->rgba);
                screenshot.clear();
                capture_request.reset();
            }
        }
        collect_retired();
        if (smoke && drawn && screenshot.empty()) {
            std::cout << "STRATA_PREVIEW_READY vulkan\n";
            break;
        }
        if (!uncapped)
            static_cast<void>(SDL_WaitEventTimeout(nullptr, 50));
    }
    preparation.reset();
    retire(candidate);
    retire(current);
    while (!retiring.empty()) {
        surface->pump();
        SDL_PumpEvents();
        presentation.rethrow_failure();
        collect_retired();
        if (!retiring.empty())
            SDL_Delay(1U);
    }
    return 0;
}
} // namespace strata::preview
