#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <strata/strata.h>

struct HWND__;
using HWND = HWND__*;

namespace strata::host {
class ResourceRoot;
}

namespace strata::desktop {

/** Any-thread resource reads and owner-thread Win32 clipboard/IME services. */
class HostServices final {
  public:
    HostServices(HWND window, std::filesystem::path resource_root);
    ~HostServices();

    HostServices(const HostServices&) = delete;
    HostServices& operator=(const HostServices&) = delete;

    /** Reads one UTF-8 resource id below the resource root; see host::ResourceRoot. */
    [[nodiscard]] std::string text(std::string_view id) const;
    [[nodiscard]] std::vector<std::uint8_t> bytes(std::string_view id) const;
    /** Retains resource access independently of this window and its UI services. */
    [[nodiscard]] std::shared_ptr<host::ResourceRoot> resources() const noexcept;
    /** Where a resource id's file lies, for services that open files themselves (video). */
    [[nodiscard]] std::filesystem::path path(std::string_view id) const;
    [[nodiscard]] bool write_clipboard(std::string_view text) noexcept;
    [[nodiscard]] strata_resource_adapter resource_adapter() noexcept;
    [[nodiscard]] strata_clipboard_adapter clipboard_adapter() noexcept;
    [[nodiscard]] strata_ime_adapter ime_adapter(std::string owner);
    void set_surface_scale(double scale) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::desktop
