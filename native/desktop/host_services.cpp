#include "host_services.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <strata/strata.hpp>

namespace strata::desktop {
namespace {

[[nodiscard]] std::string copy(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("could not open desktop resource: " + path.string());
    return std::vector<std::uint8_t>(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
    );
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr
    );
    if (size <= 0) throw std::runtime_error("Win32 text could not be converted to UTF-8");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr
        ) != size) {
        throw std::runtime_error("Win32 UTF-8 conversion was incomplete");
    }
    return result;
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0
    );
    if (size <= 0) throw std::runtime_error("UTF-8 clipboard text is invalid");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size
        ) != size) {
        throw std::runtime_error("UTF-8 clipboard conversion was incomplete");
    }
    return result;
}

} // namespace

struct HostServices::Impl final {
    Impl(HWND window, std::filesystem::path root)
        : window(window), root(std::move(root)) {
        if (window == nullptr) throw std::invalid_argument("desktop services require a window");
        if (!std::filesystem::is_directory(this->root)) {
            throw std::invalid_argument("desktop resource root is not a directory");
        }
    }

    [[nodiscard]] std::filesystem::path resource_path(
        const std::filesystem::path& relative
    ) const {
        if (relative.empty() || relative.is_absolute()) {
            throw std::invalid_argument("desktop resource id must be relative");
        }
        for (const std::filesystem::path& component : relative) {
            if (component == "..") {
                throw std::invalid_argument("desktop resource id escapes its root");
            }
        }
        return root / relative;
    }

    [[nodiscard]] std::string text(const std::filesystem::path& relative) const {
        const std::vector<std::uint8_t> bytes = read_bytes(resource_path(relative));
        return std::string(bytes.begin(), bytes.end());
    }

    [[nodiscard]] std::vector<std::uint8_t> bytes(
        const std::filesystem::path& relative
    ) const {
        return read_bytes(resource_path(relative));
    }

    static strata_status load_resource(
        void* const user_data,
        const strata_string_view id,
        strata_bytes_view* const output
    ) noexcept {
        if (output == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
        try {
            auto& self = *static_cast<Impl*>(user_data);
            const std::string resource_id = copy(id);
            const std::filesystem::path relative(resource_id);
            auto [found, inserted] = self.resource_cache.try_emplace(resource_id);
            if (inserted) found->second = read_bytes(self.resource_path(relative));
            output->data = found->second.data();
            output->size = found->second.size();
            return output->size == 0U ? STRATA_STATUS_NOT_FOUND : STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_NOT_FOUND;
        }
    }

    static strata_status clipboard_read(
        void* const user_data,
        strata_string_view* const output
    ) noexcept {
        if (output == nullptr) return STRATA_STATUS_INVALID_ARGUMENT;
        auto& self = *static_cast<Impl*>(user_data);
        if (!OpenClipboard(self.window)) return STRATA_STATUS_SERVICE_UNAVAILABLE;
        const HANDLE data = GetClipboardData(CF_UNICODETEXT);
        if (data == nullptr) {
            CloseClipboard();
            return STRATA_STATUS_NOT_FOUND;
        }
        const auto* const text = static_cast<const wchar_t*>(GlobalLock(data));
        if (text == nullptr) {
            CloseClipboard();
            return STRATA_STATUS_SERVICE_UNAVAILABLE;
        }
        try {
            self.clipboard_text = wide_to_utf8(text);
            GlobalUnlock(data);
            CloseClipboard();
            *output = strata::view(self.clipboard_text);
            return STRATA_STATUS_OK;
        } catch (...) {
            GlobalUnlock(data);
            CloseClipboard();
            return STRATA_STATUS_INVALID_UTF8;
        }
    }

    static strata_status clipboard_write(
        void* const user_data,
        const strata_string_view input
    ) noexcept {
        auto& self = *static_cast<Impl*>(user_data);
        try {
            const std::wstring text = utf8_to_wide(copy(input));
            if (!OpenClipboard(self.window)) return STRATA_STATUS_SERVICE_UNAVAILABLE;
            if (!EmptyClipboard()) {
                CloseClipboard();
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            }
            const std::size_t bytes = (text.size() + 1U) * sizeof(wchar_t);
            HGLOBAL allocation = GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (allocation == nullptr) {
                CloseClipboard();
                return STRATA_STATUS_OUT_OF_MEMORY;
            }
            void* const destination = GlobalLock(allocation);
            if (destination == nullptr) {
                GlobalFree(allocation);
                CloseClipboard();
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            }
            std::memcpy(destination, text.c_str(), bytes);
            GlobalUnlock(allocation);
            if (SetClipboardData(CF_UNICODETEXT, allocation) == nullptr) {
                GlobalFree(allocation);
                CloseClipboard();
                return STRATA_STATUS_SERVICE_UNAVAILABLE;
            }
            CloseClipboard();
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INVALID_UTF8;
        }
    }

    HWND window = nullptr;
    std::filesystem::path root;
    std::map<std::string, std::vector<std::uint8_t>, std::less<>> resource_cache;
    std::string clipboard_text;
};

HostServices::HostServices(HWND window, std::filesystem::path resource_root)
    : impl_(std::make_unique<Impl>(window, std::move(resource_root))) {}
HostServices::~HostServices() = default;

std::string HostServices::text(const std::filesystem::path& relative) const {
    return impl_->text(relative);
}

std::vector<std::uint8_t> HostServices::bytes(
    const std::filesystem::path& relative
) const {
    return impl_->bytes(relative);
}

bool HostServices::write_clipboard(const std::string_view text) noexcept {
    return Impl::clipboard_write(impl_.get(), strata::view(text)) == STRATA_STATUS_OK;
}

strata_resource_adapter HostServices::resource_adapter() noexcept {
    return strata_resource_adapter{
        sizeof(strata_resource_adapter), impl_.get(), 1U, &Impl::load_resource,
    };
}

strata_clipboard_adapter HostServices::clipboard_adapter() noexcept {
    return strata_clipboard_adapter{
        sizeof(strata_clipboard_adapter), impl_.get(), &Impl::clipboard_read, &Impl::clipboard_write,
    };
}

} // namespace strata::desktop
