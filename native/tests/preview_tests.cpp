#include "headless/application_host.hpp"
#include "headless/scenario.hpp"
#include "host/browser_model.hpp"
#include "preview/input.hpp"
#include "preview/services.hpp"
#include <SDL.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using namespace strata;
void check(const bool value, const char* message) {
    if (!value)
        throw std::runtime_error(message);
}
double number(const data::JsonValue& value) {
    if (value.number())
        return *value.number();
    if (value.integer())
        return static_cast<double>(*value.integer());
    throw std::runtime_error("expected numeric state");
}
void translation() {
    SDL_Event event{};
    event.type = SDL_MOUSEBUTTONDOWN;
    event.button.button = SDL_BUTTON_RIGHT;
    event.button.x = 120;
    event.button.y = 80;
    auto result = preview::translate_input(event, 2.0, {}, 123);
    check(result && result->position.x == 60 && result->position.y == 40 && result->button == 1 &&
              result->timestamp_nanoseconds == 123,
          "scaled pointer or right-button translation failed");
    event = {};
    event.type = SDL_KEYDOWN;
    event.key.keysym.sym = SDLK_LEFT;
    event.key.keysym.mod = KMOD_CTRL | KMOD_SHIFT;
    event.key.repeat = 1;
    result = preview::translate_input(event, 1, {}, 0);
    check(result && result->text == "left" && result->key_action == KeyAction::repeat &&
              has_modifier(result->modifiers, KeyModifiers::control) &&
              has_modifier(result->modifiers, KeyModifiers::shift),
          "repeat/modifier translation failed");
    event.type = SDL_KEYUP;
    result = preview::translate_input(event, 1, {}, 0);
    check(result && result->key_action == KeyAction::release, "key release translation failed");
    event = {};
    event.type = SDL_TEXTEDITING;
    std::strcpy(event.edit.text, "aé界");
    event.edit.start = 1;
    event.edit.length = 2;
    result = preview::translate_input(event, 1, {}, 0);
    check(result && result->selection_start == 1 && result->selection_end == 6,
          "IME character selection was not converted to UTF-8 bytes");
    event = {};
    event.type = SDL_MOUSEWHEEL;
    event.wheel.preciseY = 0.25F;
    event.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    result = preview::translate_input(event, 1, {20, 30}, 0);
    check(result && result->delta.y == -10 && result->position.y == 30,
          "precise wheel direction or pointer anchoring failed");
}
void application(const std::filesystem::path& resources) {
    auto scenario =
        headless::load_scenario(resources / "assets/strata/samples/primitives.strata-app.json");
    scenario.render_backend = "reference";
    preview::Services services;
    headless::ApplicationHostOptions options;
    options.clipboard = services.clipboard();
    options.ime = services.ime();
    options.capture_frames = false;
    headless::ApplicationHost app(scenario, resources, options);
    std::int64_t time = 0;
    const auto frame = [&] { app.frame(time += 16'666'667); };
    frame();
    const auto document = [&] { return data::parse_json(app.inspect()); };
    const auto bounds = [&](const std::string_view key, const std::string_view role) {
        const auto model = host::BrowserModel::build(document(), scenario.width, scenario.height);
        host::Selector selector;
        selector.key = key;
        selector.role = role;
        return model.resolve_bounds(selector);
    };
    const auto send = [&](const SDL_Event& event) {
        const auto input = preview::translate_input(event, 1, {}, time);
        check(input.has_value(), "preview rejected an input event");
        const auto native = input->native();
        app.enqueue({&native, 1});
        frame();
    };
    const auto click = [&](const std::string_view key, const std::string_view role,
                           const double fraction = 0.5) {
        const auto target = bounds(key, role);
        SDL_Event event{};
        event.type = SDL_MOUSEBUTTONDOWN;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.x = static_cast<int>(std::lround(target.x + target.width * fraction));
        event.button.y = static_cast<int>(std::lround(target.y + target.height * 0.5));
        send(event);
        event.type = SDL_MOUSEBUTTONUP;
        send(event);
    };
    const auto key = [&](const SDL_Keycode code, const Uint16 modifiers = 0) {
        SDL_Event event{};
        event.type = SDL_KEYDOWN;
        event.key.keysym.sym = code;
        event.key.keysym.mod = modifiers;
        send(event);
        event.type = SDL_KEYUP;
        send(event);
    };
    const auto state = [&](const std::string_view name) {
        const auto value = document();
        for (const auto& entry : *value.find("state")->array()) {
            if (*entry.find("name")->string() == name)
                return *entry.find("value");
        }
        throw std::runtime_error("missing gallery state");
    };
    const auto brightness = [&](const std::string_view key, const double fraction) {
        const auto target = bounds(key, "slider");
        const auto x = static_cast<std::size_t>(target.x + 7.0 + (target.width - 14.0) * fraction);
        const auto y = static_cast<std::size_t>(target.y + target.height * 0.5);
        const auto index = (y * app.framebuffer_width() + x) * 4;
        return static_cast<int>(app.pixels()[index]);
    };
    const int enabled_thumb = brightness("gallery.slider", 0.64);
    const int disabled_thumb = brightness("gallery.disabled-slider", 0.45);
    check(enabled_thumb > disabled_thumb + 40, "disabled control lost its dimmed presentation");
    frame();
    check(brightness("gallery.disabled-slider", 0.45) == disabled_thumb,
          "cached frame changed the disabled presentation");
    click("gallery.primary", "button");
    check(number(state("clicks")) == 1, "SDL click did not invoke the authored action");
    click("gallery.slider", "slider", 0.8);
    check(number(state("level")) >= 79, "SDL pointer did not adjust slider");
    key(SDLK_HOME);
    check(number(state("level")) == 0, "slider keyboard minimum failed");
    key(SDLK_END);
    check(number(state("level")) == 100, "slider keyboard maximum failed");
    click("gallery.select", "combo_box");
    key(SDLK_DOWN);
    key(SDLK_RETURN);
    check(state("quality").string() && *state("quality").string() == "maximum",
          "dropdown keyboard navigation did not commit a choice");
    click("gallery.name", "text_field");
    key(SDLK_a, KMOD_CTRL);
    SDL_Event text{};
    text.type = SDL_TEXTINPUT;
    std::strcpy(text.text.text, "Linux ✓");
    send(text);
    check(state("name").string() && *state("name").string() == "Linux ✓",
          "UTF-8 text input did not replace selected text");
    check(SDL_SetClipboardText("Pasted from Linux") == 0, "SDL clipboard write failed");
    key(SDLK_a, KMOD_CTRL);
    key(SDLK_v, KMOD_CTRL);
    check(state("name").string() && *state("name").string() == "Pasted from Linux",
          "native clipboard paste did not reach the editor");
    app.cancel_interactions();
    frame();
    check(app.frames().empty() && app.frame_json().empty(),
          "live preview retained per-frame inspection history");
    app.clear_observations();
    check(app.actions().empty() && app.diagnostics().empty(), "preview observations did not drain");
    app.resize(800, 600, 1.5, time += 16'666'667);
    check(app.framebuffer_width() == 1200 && app.framebuffer_height() == 900,
          "preview resize did not update physical framebuffer dimensions");
    app.close();
}
} // namespace
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::invalid_argument("expected resource root");
        check(SDL_Init(SDL_INIT_VIDEO) == 0, "SDL initialization failed");
        translation();
        application(argv[1]);
        SDL_Quit();
        std::cout << "strata_preview_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        SDL_Quit();
        std::cerr << "strata_preview_tests: " << error.what() << '\n';
        return 1;
    }
}
