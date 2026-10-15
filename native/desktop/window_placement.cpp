#include "window_placement.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>

#include <strata/host.hpp>

namespace strata::desktop::win32 {
namespace {

[[nodiscard]] std::uint64_t fingerprint(const std::string_view text) noexcept {
    std::uint64_t result = 14695981039346656037ULL;
    for (const char byte : text) {
        result ^= static_cast<std::uint8_t>(byte);
        result *= 1099511628211ULL;
    }
    return result;
}

[[nodiscard]] std::filesystem::path local_application_data() {
    wchar_t* local = nullptr;
    std::size_t size = 0U;
    const errno_t status = _wdupenv_s(&local, &size, L"LOCALAPPDATA");
    const std::filesystem::path root = status == 0 && local != nullptr
                                           ? std::filesystem::path(local)
                                           : std::filesystem::temp_directory_path();
    std::free(local);
    return root;
}

} // namespace

std::filesystem::path window_placement_path(const std::string_view identity) {
    std::ostringstream name;
    name << std::hex << fingerprint(identity) << ".json";
    return local_application_data() / "Strata" / "windows" / name.str();
}

bool restore_window_placement(const HWND window, const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
        return false;
    const std::string json{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    host::Value saved;
    try {
        saved = host::Value::parse(json);
    } catch (const std::exception&) {
        return false;
    }
    const auto integer = [&saved](const std::string_view field) -> const std::int64_t* {
        const host::Value* value = saved.find(field);
        return value != nullptr ? value->integer() : nullptr;
    };
    const std::int64_t* x = integer("x");
    const std::int64_t* y = integer("y");
    const std::int64_t* width = integer("width");
    const std::int64_t* height = integer("height");
    if (x == nullptr || y == nullptr || width == nullptr || height == nullptr || *width < 320 ||
        *height < 240 || *width > 16'384 || *height > 16'384 || std::abs(*x) > 1'000'000 ||
        std::abs(*y) > 1'000'000) {
        return false;
    }
    // The values came from GetWindowPlacement, which reports workspace coordinates (offset by a
    // taskbar on the left or top); SetWindowPlacement reads them back the same way and moves a
    // window that would land entirely off screen onto a connected monitor.
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (!GetWindowPlacement(window, &placement))
        return false;
    placement.flags = 0U;
    placement.showCmd = SW_HIDE;
    placement.rcNormalPosition = RECT{
        static_cast<LONG>(*x),
        static_cast<LONG>(*y),
        static_cast<LONG>(*x + *width),
        static_cast<LONG>(*y + *height),
    };
    if (!SetWindowPlacement(window, &placement))
        return false;
    const host::Value* maximized = saved.find("maximized");
    return maximized != nullptr && maximized->boolean() != nullptr && *maximized->boolean();
}

void save_window_placement(const HWND window, const std::filesystem::path& path) noexcept {
    try {
        WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
        if (!IsWindow(window) || !GetWindowPlacement(window, &placement))
            return;
        const RECT& normal = placement.rcNormalPosition;
        const std::string json = host::Value::object({
                                                         {"height", normal.bottom - normal.top},
                                                         {"maximized",
                                                          placement.showCmd == SW_SHOWMAXIMIZED},
                                                         {"width", normal.right - normal.left},
                                                         {"x", normal.left},
                                                         {"y", normal.top},
                                                     })
                                     .json();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        // Written beside the target and renamed over it, so a crash never leaves half a file.
        std::filesystem::path staging = path;
        staging += L".tmp";
        {
            std::ofstream output(staging, std::ios::binary | std::ios::trunc);
            if (!output.write(json.data(), static_cast<std::streamsize>(json.size())))
                return;
        }
        std::filesystem::rename(staging, path, error);
    } catch (...) {
    }
}

} // namespace strata::desktop::win32
