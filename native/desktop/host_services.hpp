#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.h>

struct HWND__;
using HWND = HWND__*;

namespace strata::desktop {

/** Win32 implementations of the resource and clipboard services exposed through the C ABI. */
class HostServices final {
public:
    HostServices(HWND window, std::filesystem::path resource_root);
    ~HostServices();

    HostServices(const HostServices&) = delete;
    HostServices& operator=(const HostServices&) = delete;

    [[nodiscard]] std::string text(const std::filesystem::path& relative) const;
    [[nodiscard]] std::vector<std::uint8_t> bytes(
        const std::filesystem::path& relative
    ) const;
    [[nodiscard]] bool write_clipboard(std::string_view text) noexcept;
    [[nodiscard]] strata_resource_adapter resource_adapter() noexcept;
    [[nodiscard]] strata_clipboard_adapter clipboard_adapter() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
