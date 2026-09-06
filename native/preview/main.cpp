#include <SDL.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <strata/strata.hpp>
#include <string>
#include <string_view>

#include "desktop/launch.hpp"
#include "headless/application_host.hpp"
#include "headless/png.hpp"
#include "input.hpp"
#include "services.hpp"

namespace {
using namespace strata;

void check_sdl(const bool success, const std::string_view operation) {
    if (!success)
        throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
struct Video final {
    Video() {
        SDL_SetHint(SDL_HINT_IME_SHOW_UI, "1");
        SDL_SetHint(SDL_HINT_IME_SUPPORT_EXTENDED_TEXT, "1");
        check_sdl(SDL_Init(SDL_INIT_VIDEO) == 0, "initialize SDL video");
    }
    ~Video() {
        SDL_Quit();
    }
};
std::filesystem::path resources() {
    std::unique_ptr<char, decltype(&SDL_free)> base(SDL_GetBasePath(), &SDL_free);
    if (base) {
        const auto installed =
            std::filesystem::path(base.get()).parent_path().parent_path() / "share";
        if (std::filesystem::is_directory(installed / "assets/strata"))
            return installed;
    }
    return STRATA_PREVIEW_RESOURCES;
}
std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
void observations(headless::ApplicationHost& app) {
    for (const auto& diagnostic : app.diagnostics())
        std::cerr << diagnostic.code << ": " << diagnostic.message << '\n';
    for (const auto& action : app.actions())
        std::cout << "STRATA ACTION " << action.id << ' ' << action.payload << '\n';
    app.clear_observations();
}
} // namespace

int main(const int argc, char** argv) {
    try {
        std::filesystem::path manifest_path, resource_override, screenshot;
        std::string backend;
        bool smoke = false;
        bool uncapped = false;
        for (int i = 1; i < argc; ++i) {
            const std::string_view arg(argv[i]);
            const auto next = [&]() -> std::string {
                if (++i >= argc)
                    throw std::invalid_argument(std::string(arg) + " requires a value");
                return argv[i];
            };
            if (arg == "--help") {
                std::cout << "usage: strata_preview [app.strata-app.json] [--resources directory]\n"
                             "                      [--backend vulkan|reference] [--smoke] "
                             "[--screenshot file.png] [--uncapped]\n"
                             "With no manifest, opens the bundled primitives gallery. F5 reloads "
                             "the application.\n";
                return 0;
            }
            if (arg == "--resources")
                resource_override = next();
            else if (arg == "--backend")
                backend = next();
            else if (arg == "--screenshot")
                screenshot = next();
            else if (arg == "--smoke")
                smoke = true;
            else if (arg == "--uncapped")
                uncapped = true;
            else if (!arg.starts_with('-') && manifest_path.empty())
                manifest_path = arg;
            else
                throw std::invalid_argument("unknown argument: " + std::string(arg));
        }
        Video video;
        if (manifest_path.empty()) {
            const auto root = resource_override.empty() ? resources() : resource_override;
            manifest_path = root / "assets/strata/samples/primitives.strata-app.json";
        }
        auto manifest = desktop::load_launch_manifest(manifest_path);
        if (manifest.kind != desktop::LaunchKind::generic)
            throw std::invalid_argument(
                "strata_preview requires a generic manifest; run custom host executables directly");
        if (manifest.watch)
            throw std::invalid_argument(
                "automatic watch is unavailable in strata_preview; F5 reloads the application");
        if (!resource_override.empty())
            manifest.resource_root = resource_override;
        if (backend.empty()) {
#ifdef STRATA_HAS_VULKAN
            backend = "vulkan";
#else
            backend = "reference";
#endif
        }
        if (backend != "vulkan" && backend != "reference")
            throw std::invalid_argument("preview backend must be vulkan or reference");
        manifest.application.render_backend = backend;
        const auto& scenario = manifest.application;
        const Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI |
                             (smoke ? SDL_WINDOW_HIDDEN : SDL_WINDOW_SHOWN);
        std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
            SDL_CreateWindow(manifest.title.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             static_cast<int>(std::ceil(scenario.width * scenario.scale)),
                             static_cast<int>(std::ceil(scenario.height * scenario.scale)), flags),
            &SDL_DestroyWindow);
        check_sdl(window != nullptr, "create preview window");
        SDL_SetWindowMinimumSize(window.get(), 480, 360);
        if (uncapped)
            SDL_SetHintWithPriority(SDL_HINT_RENDER_VSYNC, "0", SDL_HINT_OVERRIDE);
        std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)> renderer(
            SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_ACCELERATED), &SDL_DestroyRenderer);
        if (!renderer)
            renderer.reset(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE));
        check_sdl(renderer != nullptr, "create preview presenter");
        preview::Services services;
        services.coordinate_scale = scenario.scale;
        headless::ApplicationHostOptions options;
        options.clipboard = services.clipboard();
        options.ime = services.ime();
        options.capture_frames = false;
        auto app =
            std::make_unique<headless::ApplicationHost>(scenario, manifest.resource_root, options);
        std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> texture(nullptr,
                                                                            &SDL_DestroyTexture);
        int previous_width = 0, previous_height = 0, previous_pixel_width = 0,
            previous_pixel_height = 0;
        Point pointer;
        bool running = true;
        while (running) {
            const auto frame_start = now();
            SDL_Event event;
            while (SDL_PollEvent(&event) != 0) {
                std::unique_ptr<char, decltype(&SDL_free)> editing_text(
                    event.type == SDL_TEXTEDITING_EXT ? event.editExt.text : nullptr, &SDL_free);
                if (event.type == SDL_QUIT) {
                    running = false;
                    break;
                }
                if (event.type == SDL_WINDOWEVENT) {
                    if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                        running = false;
                        break;
                    }
                    if (event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                        app->cancel_interactions();
                        SDL_CaptureMouse(SDL_FALSE);
                    }
                    if (event.window.event == SDL_WINDOWEVENT_LEAVE &&
                        SDL_GetMouseState(nullptr, nullptr) == 0) {
                        const auto leave =
                            InputEvent::pointer(InputKind::pointer_move, Point{-1, -1}).native();
                        app->enqueue({&leave, 1});
                    }
                }
                if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_F5 &&
                    event.key.repeat == 0) {
                    try {
                        // Build the replacement first, so compiler/resource failures retain the
                        // last good UI.
                        auto replacement = std::make_unique<headless::ApplicationHost>(
                            scenario, manifest.resource_root, options);
                        app = std::move(replacement);
                        previous_width = 0;
                    } catch (const std::exception& error) {
                        std::cerr << "STRATA PREVIEW: " << error.what() << '\n';
                    }
                    continue;
                }
                auto translated = preview::translate_input(event, services.coordinate_scale,
                                                           pointer, frame_start);
                if (!translated)
                    continue;
                if (translated->kind == InputKind::pointer_move ||
                    translated->kind == InputKind::pointer_press ||
                    translated->kind == InputKind::pointer_release)
                    pointer = translated->position;
                if (event.type == SDL_MOUSEBUTTONDOWN)
                    SDL_CaptureMouse(SDL_TRUE);
                if (event.type == SDL_MOUSEBUTTONUP && SDL_GetMouseState(nullptr, nullptr) == 0)
                    SDL_CaptureMouse(SDL_FALSE);
                const auto input = translated->native();
                app->enqueue({&input, 1});
            }
            if (!running)
                break;
            if ((SDL_GetWindowFlags(window.get()) & SDL_WINDOW_MINIMIZED) != 0) {
                SDL_Delay(30);
                continue;
            }
            int width = 0, height = 0, pixel_width = 0, pixel_height = 0;
            SDL_GetWindowSize(window.get(), &width, &height);
            check_sdl(SDL_GetRendererOutputSize(renderer.get(), &pixel_width, &pixel_height) == 0,
                      "read framebuffer size");
            if (width <= 0 || height <= 0 || pixel_width <= 0 || pixel_height <= 0) {
                SDL_Delay(16);
                continue;
            }
            if (width != previous_width || height != previous_height ||
                pixel_width != previous_pixel_width || pixel_height != previous_pixel_height) {
                const double scale = scenario.scale * static_cast<double>(pixel_width) / width;
                app->resize(pixel_width / scale, pixel_height / scale, scale, frame_start);
                texture.reset(SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_RGBA32,
                                                SDL_TEXTUREACCESS_STREAMING,
                                                static_cast<int>(app->framebuffer_width()),
                                                static_cast<int>(app->framebuffer_height())));
                check_sdl(texture != nullptr, "create frame texture");
                check_sdl(SDL_SetTextureBlendMode(texture.get(), SDL_BLENDMODE_NONE) == 0,
                          "set frame blend mode");
                previous_width = width;
                previous_height = height;
                previous_pixel_width = pixel_width;
                previous_pixel_height = pixel_height;
            } else
                app->frame(frame_start);
            observations(*app);
            check_sdl(SDL_UpdateTexture(texture.get(), nullptr, app->pixels().data(),
                                        static_cast<int>(app->framebuffer_width() * 4)) == 0,
                      "upload preview frame");
            check_sdl(SDL_RenderCopy(renderer.get(), texture.get(), nullptr, nullptr) == 0,
                      "draw preview frame");
            SDL_RenderPresent(renderer.get());
            if (!screenshot.empty()) {
                headless::write_png(screenshot, app->framebuffer_width(), app->framebuffer_height(),
                                    app->pixels());
                screenshot.clear();
            }
            if (smoke) {
                std::cout << "STRATA_PREVIEW_READY " << backend << '\n';
                break;
            }
            if (!uncapped) {
                const auto remaining = 16'666'667 - (now() - frame_start);
                if (remaining > 0)
                    SDL_Delay(static_cast<Uint32>(remaining / 1'000'000));
            }
        }
        app->close();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_preview: " << error.what() << '\n';
        return 1;
    }
}
