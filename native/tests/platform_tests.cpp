#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.hpp>

#include "platform.hpp"

namespace {
using namespace strata;

void require(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

/** A hidden real window gives platform services their normal Win32 and OLE lifetime. */
struct Window final {
    HWND handle = CreateWindowExW(0, L"STATIC", L"Strata platform capture", WS_OVERLAPPED,
                                   0, 0, 32, 32, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Window() { require(handle != nullptr, "platform test window creation failed"); }
    ~Window() { DestroyWindow(handle); }
};

void request_capture(desktop::PlatformServices& services, const std::uint64_t request) {
    const auto adapter = services.effect_adapter("capture.client");
    const std::string payload = "{\"kind\":\"node.capture\",\"request\":" +
        std::to_string(request) +
        ",\"rect\":{\"x\":1,\"y\":0,\"width\":1,\"height\":2},\"image\":\"captured:image\"}";
    require(adapter.emit(adapter.user_data, view("strata.platform"), view(payload)) == STRATA_STATUS_OK,
            "capture effect admission failed");
    require(services.pump().empty() && services.wants_frame(), "capture did not wait for its frame");
}

void closed_capture_cannot_answer_reopened_client() {
    Window window;
    desktop::PlatformServices services(window.handle,
        [](std::string_view) { return std::vector<std::uint8_t>{}; });
    request_capture(services, 0U);
    services.close_client("capture.client");
    require(!services.wants_frame(), "a closed client retained unsubmitted captures");
    request_capture(services, 1U);
    services.frame_requested(10U);
    services.close_client("capture.client");
    request_capture(services, 1U);
    constexpr std::array<std::uint8_t, 16U> pixels{
        255U, 0U, 0U, 255U, 0U, 0U, 255U, 255U,
        255U, 0U, 0U, 255U, 0U, 255U, 0U, 255U};
    services.frame_presented(10U, 2U, 2U, pixels);
    require(services.pump().empty() && services.wants_frame(),
            "a retired capture answered or consumed the reopened textual client's request");
    services.frame_requested(11U);
    services.frame_presented(11U, 2U, 2U, pixels);
    const auto answers = services.pump();
    require(answers.size() == 1U && answers[0].image.has_value(),
            "the reopened client lost its own capture");
    const auto& image = *answers[0].image;
    require(image.width == 1U && image.height == 2U &&
                image.rgba == std::vector<std::uint8_t>{0U, 0U, 255U, 255U, 0U, 255U, 0U, 255U},
            "capture cropped the wrong client region");
}

void deferred_window_answer_retains_client_lifetime() {
    Window window;
    desktop::PlatformServices services(window.handle,
        [](std::string_view) { return std::vector<std::uint8_t>{}; });
    const auto request = [&] {
        const auto adapter = services.effect_adapter("window.client");
        require(adapter.emit(adapter.user_data, view("strata.platform"),
                    view(R"({"kind":"window.close","request":1,"id":"missing"})")) == STRATA_STATUS_OK,
                "window effect admission failed");
        require(services.pump().empty(), "window request bypassed its host");
        auto windows = services.take_window_requests();
        require(windows.size() == 1U, "window request was not deferred");
        return windows.front();
    };
    const auto old = request();
    services.close_client("window.client");
    const auto current = request();
    services.answer(old, R"({"closed":false})");
    require(services.pump().empty(), "a late window reply crossed client lifetimes");
    services.answer(current, R"({"closed":false})");
    require(services.pump().size() == 1U, "the current window request lost its answer");
}
} // namespace

int main() {
    try {
        closed_capture_cannot_answer_reopened_client();
        deferred_window_answer_retains_client_lifetime();
        std::cout << "platform lifetime tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "platform lifetime tests failed: " << error.what() << '\n';
        return 1;
    }
}
