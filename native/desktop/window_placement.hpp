#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <string_view>

namespace strata::desktop::win32 {

/**
 * Where a window's placement is kept between launches: one small file per identity (an application
 * id, or the showcase window's label) under %LOCALAPPDATA%\Strata\windows.
 */
[[nodiscard]] std::filesystem::path window_placement_path(std::string_view identity);

/**
 * Applies a remembered placement to a window that has not been shown yet: its restored bounds are
 * set while it stays hidden, and the result says whether it should then be shown maximized. A
 * missing, unreadable or implausible file leaves the window where it is and returns false.
 */
[[nodiscard]] bool restore_window_placement(HWND window, const std::filesystem::path& path);

/** Records the window's restored bounds and maximized state. Failures are ignored. */
void save_window_placement(HWND window, const std::filesystem::path& path) noexcept;

} // namespace strata::desktop::win32
