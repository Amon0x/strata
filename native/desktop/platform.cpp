#include "platform.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>
#include <dxgiformat.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include "data/json.hpp"
#include "media_events.hpp"
#include "platform_io.hpp"
#include "window_control.hpp"

namespace strata::desktop {
namespace {

using data::JsonValue;
using Microsoft::WRL::ComPtr;

/** The tray icon's callback message. */
constexpr UINT tray_message = WM_APP + 0x51;
constexpr UINT tray_icon_id = 1U;
// DWM attributes newer than some SDKs name.
constexpr DWORD dark_mode_attribute = 20U;
constexpr DWORD backdrop_attribute = 38U;

[[nodiscard]] std::string utf8(const std::wstring_view value) {
    if (value.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(std::max(size, 0)), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                            size, nullptr, nullptr);
    return result;
}

[[nodiscard]] std::wstring wide(const std::string_view value) {
    if (value.empty())
        return {};
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(std::max(size, 0)), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(),
                            size);
    return result;
}

[[nodiscard]] std::string text_field(const JsonValue& payload, const std::string_view name,
                                     std::string fallback = {}) {
    const JsonValue* value = payload.find(name);
    return value != nullptr && value->string() != nullptr ? *value->string() : std::move(fallback);
}

[[nodiscard]] bool flag_field(const JsonValue& payload, const std::string_view name,
                              const bool fallback) {
    const JsonValue* value = payload.find(name);
    return value != nullptr && value->boolean() != nullptr ? *value->boolean() : fallback;
}

/** Copies `text` into `target`, cut to fit with its terminator. */
template <std::size_t Size> void copy_text(wchar_t (&target)[Size], const std::wstring_view text) {
    const std::size_t count = std::min(text.size(), Size - 1U);
    std::copy_n(text.data(), count, target);
    target[count] = L'\0';
}

/** Decoded straight-alpha RGBA8 pixels of any image WIC reads. */
struct Pixels final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> rgba;
};

[[nodiscard]] ComPtr<IWICImagingFactory> imaging_factory() {
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory)))) {
        throw std::runtime_error("the Windows imaging component is unavailable");
    }
    return factory;
}

[[nodiscard]] Pixels decode(const std::span<const std::uint8_t> bytes) {
    if (bytes.empty() || bytes.size() > 0x7FFFFFFFU)
        throw std::runtime_error("no image bytes");
    const ComPtr<IWICImagingFactory> factory = imaging_factory();
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> converted;
    if (FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                            static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0U, &frame)) ||
        FAILED(WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.Get(), &converted))) {
        throw std::runtime_error("the image could not be decoded");
    }
    Pixels result;
    UINT width = 0U;
    UINT height = 0U;
    converted->GetSize(&width, &height);
    if (width == 0U || height == 0U || static_cast<std::uint64_t>(width) * height > 64'000'000U)
        throw std::runtime_error("the image is empty or too large");
    result.width = width;
    result.height = height;
    result.rgba.resize(static_cast<std::size_t>(width) * height * 4U);
    if (FAILED(converted->CopyPixels(nullptr, width * 4U, static_cast<UINT>(result.rgba.size()),
                                     result.rgba.data()))) {
        throw std::runtime_error("the image pixels could not be read");
    }
    return result;
}

[[nodiscard]] std::vector<std::uint8_t> encode_png(const Pixels& pixels) {
    const ComPtr<IWICImagingFactory> factory = imaging_factory();
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(pixels.width, pixels.height))) {
        throw std::runtime_error("the PNG encoder is unavailable");
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) || format != GUID_WICPixelFormat32bppBGRA)
        throw std::runtime_error("the PNG encoder refused BGRA pixels");
    std::vector<std::uint8_t> bgra = pixels.rgba;
    for (std::size_t index = 0U; index + 3U < bgra.size(); index += 4U)
        std::swap(bgra[index], bgra[index + 2U]);
    if (FAILED(frame->WritePixels(pixels.height, pixels.width * 4U, static_cast<UINT>(bgra.size()),
                                  bgra.data())) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        throw std::runtime_error("the PNG could not be written");
    }
    HGLOBAL memory = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &memory)))
        throw std::runtime_error("the PNG could not be read back");
    STATSTG stat{};
    stream->Stat(&stat, STATFLAG_NONAME);
    const auto* const data = static_cast<const std::uint8_t*>(GlobalLock(memory));
    std::vector<std::uint8_t> result(data, data + stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return result;
}

[[nodiscard]] std::optional<double> number_field(const JsonValue& payload,
                                                 const std::string_view name) {
    const JsonValue* const value = payload.find(name);
    if (value == nullptr)
        return std::nullopt;
    if (value->number() != nullptr && std::isfinite(*value->number()))
        return *value->number();
    if (value->integer() != nullptr)
        return static_cast<double>(*value->integer());
    return std::nullopt;
}

/** `pixels` resampled to `width` x `height`. */
[[nodiscard]] Pixels scaled(const Pixels& pixels, const std::uint32_t width,
                            const std::uint32_t height) {
    if (width == pixels.width && height == pixels.height)
        return pixels;
    if (width == 0U || height == 0U || static_cast<std::uint64_t>(width) * height > 64'000'000U)
        throw std::runtime_error("the scaled image is empty or too large");
    const ComPtr<IWICImagingFactory> factory = imaging_factory();
    ComPtr<IWICBitmap> source;
    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(factory->CreateBitmapFromMemory(pixels.width, pixels.height,
                                               GUID_WICPixelFormat32bppRGBA, pixels.width * 4U,
                                               static_cast<UINT>(pixels.rgba.size()),
                                               const_cast<BYTE*>(pixels.rgba.data()), &source)) ||
        FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(source.Get(), width, height,
                                  WICBitmapInterpolationModeHighQualityCubic))) {
        throw std::runtime_error("the image could not be scaled");
    }
    Pixels result{width, height, std::vector<std::uint8_t>(std::size_t{width} * height * 4U)};
    if (FAILED(scaler->CopyPixels(nullptr, width * 4U, static_cast<UINT>(result.rgba.size()),
                                  result.rgba.data()))) {
        throw std::runtime_error("the scaled image could not be read");
    }
    return result;
}

/** An icon of `pixels` at `size` x `size`, for the tray; the caller destroys it. */
[[nodiscard]] HICON make_icon(const Pixels& pixels, const std::uint32_t size) {
    const Pixels fitted = scaled(pixels, size, size);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = static_cast<LONG>(size);
    info.bmiHeader.biHeight = -static_cast<LONG>(size);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HBITMAP color = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0U);
    if (color == nullptr || bits == nullptr)
        throw std::runtime_error("the tray icon bitmap could not be made");
    auto* const target = static_cast<std::uint8_t*>(bits);
    for (std::size_t index = 0U; index + 3U < fitted.rgba.size(); index += 4U) {
        target[index] = fitted.rgba[index + 2U];
        target[index + 1U] = fitted.rgba[index + 1U];
        target[index + 2U] = fitted.rgba[index];
        target[index + 3U] = fitted.rgba[index + 3U];
    }
    const HBITMAP mask =
        CreateBitmap(static_cast<int>(size), static_cast<int>(size), 1U, 1U, nullptr);
    ICONINFO parts{TRUE, 0U, 0U, mask, color};
    const HICON icon = CreateIconIndirect(&parts);
    DeleteObject(mask);
    DeleteObject(color);
    if (icon == nullptr)
        throw std::runtime_error("the tray icon could not be made");
    return icon;
}

/** A known folder's path, or nothing where the system has none. */
[[nodiscard]] std::string known_folder(REFKNOWNFOLDERID folder) {
    PWSTR path = nullptr;
    std::string result;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, KF_FLAG_DEFAULT, nullptr, &path)))
        result = utf8(path);
    CoTaskMemFree(path);
    return result;
}

[[nodiscard]] std::filesystem::path temporary_directory() {
    std::wstring path(MAX_PATH + 1U, L'\0');
    const DWORD size = GetTempPathW(static_cast<DWORD>(path.size()), path.data());
    path.resize(size <= path.size() ? size : 0U);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(path);
}

/** The look the user chose: the app theme, and the accent colour Windows paints with. */
[[nodiscard]] SystemAppearance read_appearance() {
    SystemAppearance result;
    DWORD value = 0U;
    DWORD size = sizeof(value);
    if (RegGetValueW(
            HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        result.dark = value == 0U;
    }
    size = sizeof(value);
    // AccentColor is 0xAABBGGRR; without it, the colorization colour (0xAARRGGBB) is the accent.
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor",
                     RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS) {
        result.accent = strata_color{static_cast<std::uint8_t>(value & 0xFFU),
                                     static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                                     static_cast<std::uint8_t>((value >> 16U) & 0xFFU), 255U};
    } else if (BOOL opaque = FALSE; SUCCEEDED(DwmGetColorizationColor(&value, &opaque))) {
        result.accent = strata_color{static_cast<std::uint8_t>((value >> 16U) & 0xFFU),
                                     static_cast<std::uint8_t>((value >> 8U) & 0xFFU),
                                     static_cast<std::uint8_t>(value & 0xFFU), 255U};
    }
    // ClearType: font smoothing on, of the ClearType kind, in the display's subpixel order.
    BOOL smoothing = FALSE;
    UINT type = 0U;
    UINT orientation = FE_FONTSMOOTHINGORIENTATIONRGB;
    if (SystemParametersInfoW(SPI_GETFONTSMOOTHING, 0U, &smoothing, 0U) && smoothing &&
        SystemParametersInfoW(SPI_GETFONTSMOOTHINGTYPE, 0U, &type, 0U) &&
        type == FE_FONTSMOOTHINGCLEARTYPE) {
        static_cast<void>(
            SystemParametersInfoW(SPI_GETFONTSMOOTHINGORIENTATION, 0U, &orientation, 0U));
        result.text_antialiasing = orientation == FE_FONTSMOOTHINGORIENTATIONBGR
                                       ? STRATA_TEXT_ANTIALIASING_SUBPIXEL_BGR
                                       : STRATA_TEXT_ANTIALIASING_SUBPIXEL_RGB;
    }
    return result;
}

/** Whether the primary mouse button (the left, unless swapped) is down now. */
[[nodiscard]] bool primary_button_down() noexcept {
    const int button = GetSystemMetrics(SM_SWAPBUTTON) != 0 ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(button) & 0x8000) != 0;
}

/** A movable global holding `bytes`, for the clipboard. */
[[nodiscard]] HGLOBAL global_copy(const void* const bytes, const std::size_t size) {
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size);
    if (memory == nullptr)
        return nullptr;
    void* const target = GlobalLock(memory);
    std::memcpy(target, bytes, size);
    GlobalUnlock(memory);
    return memory;
}

/** The paths a shell drag carries (CF_HDROP), one per line. */
[[nodiscard]] std::string dropped_paths(IDataObject* const data) {
    FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    if (data == nullptr || FAILED(data->GetData(&format, &medium)))
        return {};
    std::string result;
    const auto drop = static_cast<HDROP>(GlobalLock(medium.hGlobal));
    if (drop != nullptr) {
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFFU, nullptr, 0U);
        for (UINT index = 0U; index < count; ++index) {
            const UINT length = DragQueryFileW(drop, index, nullptr, 0U);
            std::wstring path(length + 1U, L'\0');
            DragQueryFileW(drop, index, path.data(), length + 1U);
            path.resize(length);
            if (!result.empty())
                result += '\n';
            result += utf8(path);
        }
        GlobalUnlock(medium.hGlobal);
    }
    ReleaseStgMedium(&medium);
    return result;
}

/** Reports a shell drag over the window as file input. */
class DropTarget final : public IDropTarget {
  public:
    DropTarget(HWND window, PlatformServices::FileSink sink)
        : window_(window), sink_(std::move(sink)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (out == nullptr)
            return E_POINTER;
        if (id == IID_IUnknown || id == IID_IDropTarget) {
            *out = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references_;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --references_;
        if (left == 0U)
            delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL point,
                                        DWORD* effect) override {
        paths_ = dropped_paths(data);
        return over(STRATA_INPUT_FILE_DRAG, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL point, DWORD* effect) override {
        return over(STRATA_INPUT_FILE_DRAG, point, effect);
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        report(STRATA_INPUT_FILE_DRAG_LEAVE, POINTL{});
        paths_.clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point, DWORD* effect) override {
        if (const std::string dropped = dropped_paths(data); !dropped.empty())
            paths_ = dropped;
        const HRESULT result = over(STRATA_INPUT_FILE_DROP, point, effect);
        paths_.clear();
        return result;
    }

  private:
    HRESULT over(const std::uint32_t kind, const POINTL point, DWORD* const effect) {
        if (effect != nullptr)
            *effect = paths_.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
        if (!paths_.empty())
            report(kind, point);
        return S_OK;
    }
    void report(const std::uint32_t kind, const POINTL point) {
        POINT client{point.x, point.y};
        ScreenToClient(window_, &client);
        try {
            sink_(kind, static_cast<double>(client.x), static_cast<double>(client.y), paths_);
        } catch (...) {
            // Nothing may unwind into OLE.
        }
    }

    HWND window_;
    PlatformServices::FileSink sink_;
    std::string paths_;
    ULONG references_ = 1U;
};

} // namespace

struct PlatformServices::Impl final : PlatformOutbox {
    struct Client final {
        Impl* platform = nullptr;
        std::string id;
        /** Raised when the session closes; a session opened again under the id gets a new one. */
        std::shared_ptr<std::atomic<bool>> closed = std::make_shared<std::atomic<bool>>(false);
    };
    using Request = PlatformRequest;

    /** One video playing into a runtime image. */
    struct Video final {
        std::string client;
        std::string image;
        ComPtr<IMFMediaEngine> engine;
        MediaEvents* events = nullptr;
        ComPtr<IWICBitmap> target;
        UINT width = 0U;
        UINT height = 0U;
        /** The play request, answered once the video's size and length are known. */
        std::optional<Request> waiting;
        bool fresh = false;
        PlatformImage frame;
        /** Whether a node has shown it, and whether it was paused because none does now. */
        bool seen = false;
        bool hidden = false;
    };

    /** An item of the tray icon's menu. */
    struct TrayItem final {
        std::string id;
        std::string label;
        bool checked = false;
        bool enabled = true;
        bool separator = false;
    };

    Impl(HWND window, ResourceReader read_resource, ResourcePath resource_path)
        : window(window), read_resource(std::move(read_resource)),
          resource_path(std::move(resource_path)), control(window) {
        if (window == nullptr)
            throw std::invalid_argument("platform services require a window");
        ole = SUCCEEDED(OleInitialize(nullptr));
        system_appearance = read_appearance();
        services.push_back(make_network_service(*this));
        services.push_back(make_file_service(*this, this->resource_path));
        services.push_back(make_audio_service(*this, this->resource_path));
    }

    ~Impl() override {
        // Their threads answer through this; they end first.
        services.clear();
        for (auto& [id, video] : videos)
            release(video);
        videos.clear();
        if (media_started)
            MFShutdown();
        if (drop_target != nullptr) {
            RevokeDragDrop(window);
            drop_target->Release();
        }
        remove_tray();
        if (ole)
            OleUninitialize();
    }

    static strata_status emit(void* const user_data, const strata_string_view effect,
                              const strata_string_view payload) noexcept {
        auto* const client = static_cast<Client*>(user_data);
        if (client == nullptr)
            return STRATA_STATUS_INVALID_ARGUMENT;
        if (std::string_view(effect.data != nullptr ? effect.data : "", effect.size) !=
            "strata.platform") {
            return STRATA_STATUS_SERVICE_UNAVAILABLE;
        }
        try {
            JsonValue parsed = data::parse_json(
                std::string_view(payload.data != nullptr ? payload.data : "", payload.size));
            const JsonValue* number = parsed.find("request");
            const JsonValue* kind = parsed.find("kind");
            if (number == nullptr || number->integer() == nullptr || kind == nullptr ||
                kind->string() == nullptr) {
                return STRATA_STATUS_INVALID_ARGUMENT;
            }
            const std::scoped_lock lock(client->platform->mutex);
            client->platform->requests.push_back(
                Request{client->id, static_cast<std::uint64_t>(*number->integer()), *kind->string(),
                        std::move(parsed), client->closed});
            return STRATA_STATUS_OK;
        } catch (...) {
            return STRATA_STATUS_INVALID_ARGUMENT;
        }
    }

    [[nodiscard]] PlatformAnswer answer(const Request& request, JsonValue::Object fields) const {
        return PlatformAnswer{request.client, request.number,
                              data::encode_canonical_json(JsonValue(std::move(fields))),
                              std::nullopt};
    }

    // ---- what services say from their threads ----

    void reply(const Request& request, JsonValue::Object fields) override {
        PlatformAnswer said = answer(request, std::move(fields));
        const std::scoped_lock lock(mutex);
        if (request.closed == nullptr || !request.closed->load())
            answered.push_back(std::move(said));
    }

    void say(const Request& request, const std::string& channel, JsonValue data) override {
        PlatformAnswer said{request.client, 0U, data::encode_canonical_json(data), std::nullopt,
                            channel};
        const std::scoped_lock lock(mutex);
        if (request.closed == nullptr || !request.closed->load())
            answered.push_back(std::move(said));
    }

    /** Forgets a session: what it asked is dropped, and what it started stops. */
    void close_client(const std::string_view id) {
        {
            const std::scoped_lock lock(mutex);
            if (const auto found = clients.find(id); found != clients.end()) {
                found->second->closed->store(true);
                found->second->closed = std::make_shared<std::atomic<bool>>(false);
            }
            std::erase_if(requests, [id](const Request& request) { return request.client == id; });
            std::erase_if(answered, [id](const PlatformAnswer& said) { return said.client == id; });
        }
        for (const std::unique_ptr<PlatformService>& service : services)
            service->close_client(id);
        for (auto entry = videos.begin(); entry != videos.end();) {
            if (entry->second.client == id) {
                release(entry->second);
                entry = videos.erase(entry);
            } else {
                ++entry;
            }
        }
    }

    // ---- dialogs ----

    [[nodiscard]] JsonValue::Object file_dialog(const Request& request, const bool save) {
        ComPtr<IFileDialog> dialog;
        const HRESULT created = save
                                    ? CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))
                                    : CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                                       CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
        if (FAILED(created))
            return {{"cancelled", JsonValue(true)},
                    {"path", JsonValue("")},
                    {"paths", JsonValue(JsonValue::Array{})}};
        const bool multiple = !save && flag_field(request.payload, "multiple", false);
        const bool folder = !save && flag_field(request.payload, "folder", false);
        DWORD options = 0U;
        dialog->GetOptions(&options);
        options |= FOS_FORCEFILESYSTEM;
        if (multiple)
            options |= FOS_ALLOWMULTISELECT;
        if (folder)
            options |= FOS_PICKFOLDERS;
        dialog->SetOptions(options);
        if (const std::string title = text_field(request.payload, "title"); !title.empty())
            dialog->SetTitle(wide(title).c_str());
        if (save) {
            if (const std::string name = text_field(request.payload, "name"); !name.empty())
                dialog->SetFileName(wide(name).c_str());
        }
        // Filters: { name, patterns } objects (patterns a string or a list), or "Name|*.a;*.b".
        std::vector<std::pair<std::wstring, std::wstring>> filters;
        if (const JsonValue* list = request.payload.find("filters");
            list != nullptr && list->array() != nullptr && !folder) {
            for (const JsonValue& entry : *list->array()) {
                std::string name;
                std::string patterns;
                if (entry.string() != nullptr) {
                    const std::size_t bar = entry.string()->find('|');
                    name = entry.string()->substr(0U, bar);
                    patterns = bar == std::string::npos ? *entry.string()
                                                        : entry.string()->substr(bar + 1U);
                } else if (entry.object() != nullptr) {
                    name = text_field(entry, "name");
                    if (const JsonValue* given = entry.find("patterns"); given != nullptr) {
                        if (given->string() != nullptr) {
                            patterns = *given->string();
                        } else if (given->array() != nullptr) {
                            for (const JsonValue& pattern : *given->array()) {
                                if (pattern.string() == nullptr)
                                    continue;
                                if (!patterns.empty())
                                    patterns += ';';
                                patterns += *pattern.string();
                            }
                        }
                    }
                }
                if (!patterns.empty())
                    filters.emplace_back(wide(name.empty() ? patterns : name), wide(patterns));
            }
        }
        std::vector<COMDLG_FILTERSPEC> specs;
        for (const auto& [name, patterns] : filters)
            specs.push_back(COMDLG_FILTERSPEC{name.c_str(), patterns.c_str()});
        if (!specs.empty())
            dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
        JsonValue::Array paths;
        if (SUCCEEDED(dialog->Show(window))) {
            const auto add = [&paths](IShellItem* const item) {
                PWSTR path = nullptr;
                if (item != nullptr && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                    paths.emplace_back(utf8(path));
                    CoTaskMemFree(path);
                }
            };
            ComPtr<IFileOpenDialog> open;
            ComPtr<IShellItemArray> items;
            if (!save && SUCCEEDED(dialog.As(&open)) && SUCCEEDED(open->GetResults(&items))) {
                DWORD count = 0U;
                items->GetCount(&count);
                for (DWORD index = 0U; index < count; ++index) {
                    ComPtr<IShellItem> item;
                    if (SUCCEEDED(items->GetItemAt(index, &item)))
                        add(item.Get());
                }
            } else {
                ComPtr<IShellItem> item;
                if (SUCCEEDED(dialog->GetResult(&item)))
                    add(item.Get());
            }
        }
        JsonValue first = paths.empty() ? JsonValue("") : paths.front();
        const bool cancelled = paths.empty();
        return {{"cancelled", JsonValue(cancelled)},
                {"path", std::move(first)},
                {"paths", JsonValue(std::move(paths))}};
    }

    // ---- notifications ----

    void ensure_tray() {
        if (tray)
            return;
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = tray_icon_id;
        icon.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        icon.uCallbackMessage = tray_message;
        icon.hIcon = reinterpret_cast<HICON>(SendMessageW(window, WM_GETICON, ICON_SMALL2, 0));
        if (icon.hIcon == nullptr)
            icon.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        std::wstring title(256U, L'\0');
        title.resize(static_cast<std::size_t>(GetWindowTextW(window, title.data(), 256)));
        copy_text(icon.szTip, title.empty() ? std::wstring(L"Strata") : title);
        tray = Shell_NotifyIconW(NIM_ADD, &icon) != FALSE;
        icon.uVersion = NOTIFYICON_VERSION_4;
        tray_version_4 = Shell_NotifyIconW(NIM_SETVERSION, &icon) != FALSE;
    }

    void remove_tray() noexcept {
        if (tray) {
            NOTIFYICONDATAW icon{};
            icon.cbSize = sizeof(icon);
            icon.hWnd = window;
            icon.uID = tray_icon_id;
            Shell_NotifyIconW(NIM_DELETE, &icon);
        }
        tray = false;
        tray_menu = false;
        tray_notified = false;
        tray_for_hiding = false;
        tray_items.clear();
        if (tray_icon != nullptr)
            DestroyIcon(tray_icon);
        tray_icon = nullptr;
    }

    /** `system.tray`: the icon, its tooltip and its menu, or no icon at all. */
    [[nodiscard]] JsonValue::Object set_tray(const Request& request) {
        if (!flag_field(request.payload, "visible", true)) {
            // A notification or a hidden window still needs the icon; only the menu goes.
            tray_menu = false;
            tray_items.clear();
            if (!tray_notified && !control.hidden())
                remove_tray();
            return {{"visible", JsonValue(tray)}};
        }
        ensure_tray();
        if (!tray)
            return {{"error", JsonValue("the tray icon could not be added")},
                    {"visible", JsonValue(false)}};
        tray_menu = true;
        tray_client = request.client;
        tray_items.clear();
        if (const JsonValue* items = request.payload.find("items");
            items != nullptr && items->array() != nullptr) {
            for (const JsonValue& entry : *items->array()) {
                if (entry.object() == nullptr)
                    continue;
                TrayItem item;
                item.id = text_field(entry, "id");
                item.label = text_field(entry, "label", item.id);
                item.checked = flag_field(entry, "checked", false);
                item.enabled = flag_field(entry, "enabled", true);
                item.separator = flag_field(entry, "separator", false);
                if (item.separator || !item.label.empty())
                    tray_items.push_back(std::move(item));
            }
        }
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = tray_icon_id;
        icon.uFlags = NIF_TIP | NIF_SHOWTIP;
        std::wstring tooltip = wide(text_field(request.payload, "tooltip"));
        if (tooltip.empty()) {
            tooltip.resize(256U);
            tooltip.resize(static_cast<std::size_t>(GetWindowTextW(window, tooltip.data(), 256)));
        }
        copy_text(icon.szTip, tooltip.empty() ? std::wstring(L"Strata") : tooltip);
        std::string failure;
        HICON made = nullptr;
        if (const std::string source = text_field(request.payload, "icon"); !source.empty()) {
            try {
                const UINT dpi = GetDpiForWindow(window);
                made = make_icon(decode(read_resource(source)),
                                 static_cast<std::uint32_t>(
                                     GetSystemMetricsForDpi(SM_CXSMICON, dpi == 0U ? 96U : dpi)));
                icon.uFlags |= NIF_ICON;
                icon.hIcon = made;
            } catch (const std::exception& error) {
                failure = error.what();
            }
        }
        Shell_NotifyIconW(NIM_MODIFY, &icon);
        if (made != nullptr) {
            if (tray_icon != nullptr)
                DestroyIcon(tray_icon);
            tray_icon = made;
        }
        JsonValue::Object result{{"items", JsonValue(static_cast<std::int64_t>(tray_items.size()))},
                                 {"visible", JsonValue(true)}};
        if (!failure.empty())
            result.emplace_back("error", JsonValue("the tray icon image: " + failure));
        return result;
    }

    /** The tray icon's menu, at `anchor`; a chosen item is said on the "tray" channel. */
    void show_tray_menu(const POINT anchor) {
        if (!tray_menu || tray_items.empty())
            return;
        const HMENU menu = CreatePopupMenu();
        if (menu == nullptr)
            return;
        for (std::size_t index = 0U; index < tray_items.size(); ++index) {
            const TrayItem& item = tray_items[index];
            if (item.separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0U, nullptr);
                continue;
            }
            const UINT flags = MF_STRING | (item.checked ? MF_CHECKED : MF_UNCHECKED) |
                               (item.enabled ? MF_ENABLED : MF_GRAYED);
            AppendMenuW(menu, flags, index + 1U, wide(item.label).c_str());
        }
        // The menu closes when the user clicks elsewhere only if the window is foreground.
        SetForegroundWindow(window);
        const UINT alignment =
            GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0 ? TPM_RIGHTALIGN : TPM_LEFTALIGN;
        const auto chosen = static_cast<std::size_t>(TrackPopupMenuEx(
            menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | alignment,
            anchor.x, anchor.y, window, nullptr));
        PostMessageW(window, WM_NULL, 0, 0);
        DestroyMenu(menu);
        if (chosen == 0U || chosen > tray_items.size())
            return;
        const std::string said = data::encode_canonical_json(
            JsonValue(JsonValue::Object{{"id", JsonValue(tray_items[chosen - 1U].id)}}));
        const std::scoped_lock lock(mutex);
        answered.push_back(
            PlatformAnswer{tray_client, 0U, said, std::nullopt, std::string("tray")});
    }

    /** Shows the window again; an icon made only to bring it back goes with it. */
    void show_window() {
        control.show();
        if (tray_for_hiding && !tray_menu && !tray_notified)
            remove_tray();
        tray_for_hiding = false;
    }

    [[nodiscard]] JsonValue::Object hide_to_tray() {
        if (!tray) {
            ensure_tray();
            tray_for_hiding = tray;
        }
        // Without an icon to click, a hidden window could never come back.
        if (!tray)
            return {{"error", JsonValue("the tray icon could not be added")},
                    {"hidden", JsonValue(false)}};
        control.hide();
        return {{"hidden", JsonValue(true)}};
    }

    [[nodiscard]] bool notify(const Request& request) {
        ensure_tray();
        if (!tray)
            return false;
        tray_notified = true;
        NOTIFYICONDATAW icon{};
        icon.cbSize = sizeof(icon);
        icon.hWnd = window;
        icon.uID = tray_icon_id;
        icon.uFlags = NIF_INFO;
        icon.dwInfoFlags = NIIF_INFO;
        copy_text(icon.szInfoTitle, wide(text_field(request.payload, "title")));
        copy_text(icon.szInfo, wide(text_field(request.payload, "body", " ")));
        return Shell_NotifyIconW(NIM_MODIFY, &icon) != FALSE;
    }

    // ---- window chrome ----

    [[nodiscard]] JsonValue::Object set_chrome(const Request& request) {
        const std::string title_bar = text_field(request.payload, "titleBar", "system");
        const std::string backdrop = text_field(request.payload, "backdrop", "none");
        const BOOL dark = flag_field(request.payload, "dark", true) ? TRUE : FALSE;
        DwmSetWindowAttribute(window, dark_mode_attribute, &dark, sizeof(dark));
        // DWMSBT_NONE, MAINWINDOW (mica), TRANSIENTWINDOW (acrylic), TABBEDWINDOW.
        const int backdrop_type = backdrop == "mica"      ? 2
                                  : backdrop == "acrylic" ? 3
                                  : backdrop == "tabbed"  ? 4
                                                          : 1;
        const bool applied = SUCCEEDED(DwmSetWindowAttribute(
            window, backdrop_attribute, &backdrop_type, sizeof(backdrop_type)));
        transparent_backdrop = backdrop_type != 1 && applied;
        // The backdrop shows wherever frames leave the window transparent.
        const MARGINS margins =
            transparent_backdrop ? MARGINS{-1, -1, -1, -1} : MARGINS{0, 0, 0, 0};
        DwmExtendFrameIntoClientArea(window, &margins);
        custom_title = title_bar == "custom";
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return {{"backdrop", JsonValue(transparent_backdrop ? backdrop : std::string("none"))},
                {"titleBar", JsonValue(custom_title ? "custom" : "system")}};
    }

    [[nodiscard]] int frame_thickness() const {
        const UINT dpi = GetDpiForWindow(window);
        return GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
               GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    }

    // ---- clipboard images ----

    [[nodiscard]] JsonValue::Object copy_image(const Request& request) {
        const std::string id = text_field(request.payload, "image");
        Pixels pixels;
        if (const auto held = pasted.find(id); held != pasted.end()) {
            pixels = held->second;
        } else {
            const std::vector<std::uint8_t> bytes = read_resource(id);
            pixels = decode(bytes);
        }
        const bool copied = put_on_clipboard(pixels);
        return {{"copied", JsonValue(copied)},
                {"height", JsonValue(static_cast<std::int64_t>(pixels.height))},
                {"width", JsonValue(static_cast<std::int64_t>(pixels.width))}};
    }

    /** Puts `pixels` on the clipboard as a DIB and a PNG. */
    [[nodiscard]] bool put_on_clipboard(const Pixels& pixels) {
        const std::vector<std::uint8_t> png = encode_png(pixels);
        // CF_DIB for everything, bottom-up BGRA; PNG beside it keeps the alpha for apps that read
        // it.
        BITMAPINFOHEADER header{};
        header.biSize = sizeof(header);
        header.biWidth = static_cast<LONG>(pixels.width);
        header.biHeight = static_cast<LONG>(pixels.height);
        header.biPlanes = 1;
        header.biBitCount = 32;
        header.biCompression = BI_RGB;
        std::vector<std::uint8_t> dib(sizeof(header) + pixels.rgba.size());
        std::memcpy(dib.data(), &header, sizeof(header));
        const std::size_t row = static_cast<std::size_t>(pixels.width) * 4U;
        for (std::uint32_t y = 0U; y < pixels.height; ++y) {
            const std::uint8_t* const source = pixels.rgba.data() + (pixels.height - 1U - y) * row;
            std::uint8_t* const target = dib.data() + sizeof(header) + y * row;
            for (std::size_t x = 0U; x < row; x += 4U) {
                target[x] = source[x + 2U];
                target[x + 1U] = source[x + 1U];
                target[x + 2U] = source[x];
                target[x + 3U] = source[x + 3U];
            }
        }
        bool copied = false;
        if (OpenClipboard(window)) {
            EmptyClipboard();
            if (HGLOBAL memory = global_copy(dib.data(), dib.size()); memory != nullptr) {
                if (SetClipboardData(CF_DIB, memory) == nullptr)
                    GlobalFree(memory);
                else
                    copied = true;
            }
            if (HGLOBAL memory = global_copy(png.data(), png.size()); memory != nullptr) {
                if (SetClipboardData(RegisterClipboardFormatW(L"PNG"), memory) == nullptr)
                    GlobalFree(memory);
            }
            CloseClipboard();
        }
        return copied;
    }

    // ---- node captures ----

    /** A capture's answer: every field, always, so a document can keep it whole in state. */
    [[nodiscard]] static JsonValue::Object captured(const bool ok, std::string error,
                                                    const std::uint32_t width,
                                                    const std::uint32_t height, std::string path,
                                                    std::string image, const bool copied) {
        return {{"copied", JsonValue(copied)},
                {"error", JsonValue(std::move(error))},
                {"height", JsonValue(static_cast<std::int64_t>(height))},
                {"image", JsonValue(std::move(image))},
                {"ok", JsonValue(ok)},
                {"path", JsonValue(std::move(path))},
                {"width", JsonValue(static_cast<std::int64_t>(width))}};
    }

    /** Cuts a `node.capture`'s rectangle out of the frame and writes it where it asked. */
    [[nodiscard]] PlatformAnswer capture(const Request& request, const std::uint32_t width,
                                         const std::uint32_t height,
                                         const std::span<const std::uint8_t> rgba) {
        const JsonValue* const area = request.payload.find("rect");
        if (area == nullptr || area->object() == nullptr)
            return answer(request, captured(false, "no node with that key is on screen", 0U, 0U, {},
                                            {}, false));
        const auto edge = [area](const std::string_view name) {
            return std::max<std::int64_t>(
                0, static_cast<std::int64_t>(std::lround(number_field(*area, name).value_or(0.0))));
        };
        const std::int64_t left = std::min<std::int64_t>(edge("x"), width);
        const std::int64_t top = std::min<std::int64_t>(edge("y"), height);
        const std::int64_t right = std::min<std::int64_t>(left + edge("width"), width);
        const std::int64_t bottom = std::min<std::int64_t>(top + edge("height"), height);
        if (right <= left || bottom <= top || rgba.size() < std::size_t{width} * height * 4U)
            return answer(request,
                          captured(false, "the node is outside the window", 0U, 0U, {}, {}, false));
        Pixels pixels{
            static_cast<std::uint32_t>(right - left), static_cast<std::uint32_t>(bottom - top), {}};
        pixels.rgba.resize(std::size_t{pixels.width} * pixels.height * 4U);
        for (std::uint32_t row = 0U; row < pixels.height; ++row) {
            const std::size_t from =
                (static_cast<std::size_t>(top + row) * width + static_cast<std::size_t>(left)) * 4U;
            std::memcpy(pixels.rgba.data() + std::size_t{row} * pixels.width * 4U,
                        rgba.data() + from, std::size_t{pixels.width} * 4U);
        }
        if (const std::optional<double> factor = number_field(request.payload, "scale");
            factor.has_value() && *factor > 0.0 && *factor != 1.0) {
            pixels =
                scaled(pixels,
                       std::max<std::uint32_t>(
                           1U, static_cast<std::uint32_t>(std::lround(pixels.width * *factor))),
                       std::max<std::uint32_t>(
                           1U, static_cast<std::uint32_t>(std::lround(pixels.height * *factor))));
        }
        std::string path = text_field(request.payload, "path");
        const bool clipboard = flag_field(request.payload, "clipboard", false);
        const std::string image = text_field(request.payload, "image");
        // Asked for nothing else, it goes to a file of its own.
        std::filesystem::path file;
        if (!path.empty()) {
            file = std::filesystem::path(wide(path));
        } else if (!clipboard && image.empty()) {
            file = temporary_directory() / L"Strata" /
                   (L"capture-" + std::to_wstring(++capture_count) + L".png");
        }
        if (!file.empty()) {
            file.make_preferred();
            if (file.has_parent_path())
                std::filesystem::create_directories(file.parent_path());
            const std::vector<std::uint8_t> png = encode_png(pixels);
            std::ofstream output(file, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(png.data()),
                         static_cast<std::streamsize>(png.size()));
            if (!output)
                throw std::runtime_error("the capture could not be written to " +
                                         utf8(file.wstring()));
            path = utf8(file.wstring());
        }
        const bool copied = clipboard && put_on_clipboard(pixels);
        PlatformAnswer result =
            answer(request, captured(true, {}, pixels.width, pixels.height, path, image, copied));
        if (!image.empty())
            result.image =
                PlatformImage{image, pixels.width, pixels.height, std::move(pixels.rgba)};
        return result;
    }

    void frame_presented(const std::uint32_t width, const std::uint32_t height,
                         const std::span<const std::uint8_t> rgba) {
        std::vector<Request> due;
        due.swap(captures);
        std::vector<PlatformAnswer> finished;
        for (const Request& request : due) {
            try {
                finished.push_back(capture(request, width, height, rgba));
            } catch (const std::exception& error) {
                finished.push_back(
                    answer(request, captured(false, error.what(), 0U, 0U, {}, {}, false)));
            }
        }
        const std::scoped_lock lock(mutex);
        for (PlatformAnswer& done : finished)
            answered.push_back(std::move(done));
    }

    // ---- dragging out ----

    /**
     * `system.dragOut`: the shell's drag loop carrying files (CF_HDROP) and text
     * (CF_UNICODETEXT) to wherever the user drops them; it runs until the button is released.
     */
    [[nodiscard]] JsonValue::Object drag_out(const Request& request) {
        // The press may have ended before this frame came round: then there is nothing to drag.
        if (!primary_button_down())
            return {{"effect", JsonValue("none")}};
        std::vector<std::wstring> files;
        if (const JsonValue* listed = request.payload.find("files");
            listed != nullptr && listed->array() != nullptr) {
            for (const JsonValue& entry : *listed->array()) {
                if (entry.string() == nullptr)
                    continue;
                std::filesystem::path file(wide(*entry.string()));
                file.make_preferred();
                if (GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
                    files.push_back(file.wstring());
            }
        }
        const std::wstring text = wide(text_field(request.payload, "text"));
        if (files.empty() && text.empty())
            return {{"effect", JsonValue("none")},
                    {"error", JsonValue("none of the files to drag exist")}};
        ComPtr<IDataObject> data;
        if (FAILED(SHCreateDataObject(nullptr, 0U, nullptr, nullptr, IID_PPV_ARGS(&data))))
            throw std::runtime_error("the drag data could not be made");
        const auto put = [&data](const CLIPFORMAT format, const HGLOBAL memory) {
            if (memory == nullptr)
                return;
            FORMATETC described{format, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
            STGMEDIUM medium{};
            medium.tymed = TYMED_HGLOBAL;
            medium.hGlobal = memory;
            if (FAILED(data->SetData(&described, &medium, TRUE)))
                GlobalFree(memory);
        };
        if (!files.empty()) {
            // DROPFILES, then each path and its terminator, then one more terminator.
            std::vector<wchar_t> list;
            for (const std::wstring& file : files) {
                list.insert(list.end(), file.begin(), file.end());
                list.push_back(L'\0');
            }
            list.push_back(L'\0');
            std::vector<std::uint8_t> drop(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
            DROPFILES header{};
            header.pFiles = sizeof(DROPFILES);
            header.fWide = TRUE;
            std::memcpy(drop.data(), &header, sizeof(header));
            std::memcpy(drop.data() + sizeof(header), list.data(), list.size() * sizeof(wchar_t));
            put(CF_HDROP, global_copy(drop.data(), drop.size()));
        }
        if (!text.empty())
            put(CF_UNICODETEXT, global_copy(text.c_str(), (text.size() + 1U) * sizeof(wchar_t)));
        const DWORD allowed =
            DROPEFFECT_COPY | (flag_field(request.payload, "move", false) ? DROPEFFECT_MOVE : 0U);
        DWORD effect = DROPEFFECT_NONE;
        const HRESULT dropped = SHDoDragDrop(window, data.Get(), nullptr, allowed, &effect);
        // The drag loop took the release of the press that began it.
        window_moved = true;
        if (dropped != DRAGDROP_S_DROP)
            return {{"effect", JsonValue("none")}};
        // A target that moved the files itself reports what it did here, not in `effect`.
        FORMATETC logical{
            static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"Logical Performed DropEffect")),
            nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM performed{};
        if (SUCCEEDED(data->GetData(&logical, &performed))) {
            if (const auto* const value = static_cast<const DWORD*>(GlobalLock(performed.hGlobal));
                value != nullptr) {
                effect = *value;
                GlobalUnlock(performed.hGlobal);
            }
            ReleaseStgMedium(&performed);
        }
        const char* const name = (effect & DROPEFFECT_MOVE) != 0U   ? "move"
                                 : (effect & DROPEFFECT_COPY) != 0U ? "copy"
                                 : (effect & DROPEFFECT_LINK) != 0U ? "copy"
                                                                    : "none";
        return {{"effect", JsonValue(name)}};
    }

    // ---- folders ----

    [[nodiscard]] static JsonValue::Object paths() {
        return {{"appData", JsonValue(known_folder(FOLDERID_RoamingAppData))},
                {"desktop", JsonValue(known_folder(FOLDERID_Desktop))},
                {"documents", JsonValue(known_folder(FOLDERID_Documents))},
                {"downloads", JsonValue(known_folder(FOLDERID_Downloads))},
                {"home", JsonValue(known_folder(FOLDERID_Profile))},
                {"pictures", JsonValue(known_folder(FOLDERID_Pictures))},
                {"temp", JsonValue(utf8(temporary_directory().wstring()))}};
    }

    // ---- windows ----

    /** `window.open`, and `window.close` naming another window: the embedder's to carry out. */
    void request_window(const Request& request, const WindowRequest::Kind kind) {
        WindowRequest window_request;
        window_request.kind = kind;
        window_request.client = request.client;
        window_request.request = request.number;
        window_request.id = text_field(request.payload, "id");
        window_request.root = text_field(request.payload, "root");
        window_request.title = text_field(request.payload, "title");
        window_request.width = number_field(request.payload, "width");
        window_request.height = number_field(request.payload, "height");
        window_request.x = number_field(request.payload, "x");
        window_request.y = number_field(request.payload, "y");
        window_requests.push_back(std::move(window_request));
    }

    [[nodiscard]] std::optional<Pixels> clipboard_image() {
        if (!OpenClipboard(window))
            return std::nullopt;
        std::optional<Pixels> result;
        const auto read = [](const HANDLE data) {
            std::vector<std::uint8_t> bytes;
            if (data == nullptr)
                return bytes;
            const auto* const source = static_cast<const std::uint8_t*>(GlobalLock(data));
            if (source != nullptr) {
                bytes.assign(source, source + GlobalSize(data));
                GlobalUnlock(data);
            }
            return bytes;
        };
        try {
            if (const UINT png = RegisterClipboardFormatW(L"PNG");
                IsClipboardFormatAvailable(png)) {
                result = decode(read(GetClipboardData(png)));
            } else if (IsClipboardFormatAvailable(CF_DIB)) {
                // A DIB is a bitmap file without its file header: put one in front and decode.
                const std::vector<std::uint8_t> dib = read(GetClipboardData(CF_DIB));
                if (dib.size() >= sizeof(BITMAPINFOHEADER)) {
                    BITMAPINFOHEADER header{};
                    std::memcpy(&header, dib.data(), sizeof(header));
                    std::size_t palette = header.biClrUsed != 0U    ? header.biClrUsed
                                          : header.biBitCount <= 8U ? (1U << header.biBitCount)
                                                                    : 0U;
                    std::size_t masks = header.biSize == sizeof(BITMAPINFOHEADER) &&
                                                header.biCompression == BI_BITFIELDS
                                            ? 12U
                                            : 0U;
                    BITMAPFILEHEADER file{};
                    file.bfType = 0x4D42;
                    file.bfOffBits =
                        static_cast<DWORD>(sizeof(file) + header.biSize + masks + palette * 4U);
                    file.bfSize = static_cast<DWORD>(sizeof(file) + dib.size());
                    std::vector<std::uint8_t> bitmap(sizeof(file) + dib.size());
                    std::memcpy(bitmap.data(), &file, sizeof(file));
                    std::memcpy(bitmap.data() + sizeof(file), dib.data(), dib.size());
                    Pixels decoded = decode(bitmap);
                    // A 32-bit DIB whose alpha is all zero means opaque.
                    bool clear = true;
                    for (std::size_t index = 3U; index < decoded.rgba.size() && clear; index += 4U)
                        clear = decoded.rgba[index] == 0U;
                    if (clear) {
                        for (std::size_t index = 3U; index < decoded.rgba.size(); index += 4U)
                            decoded.rgba[index] = 255U;
                    }
                    result = std::move(decoded);
                }
            }
        } catch (const std::exception&) {
            result.reset();
        }
        CloseClipboard();
        return result;
    }

    // ---- video ----

    static void release(Video& video) {
        if (video.engine != nullptr)
            video.engine->Shutdown();
        video.engine.Reset();
        if (video.events != nullptr)
            video.events->Release();
        video.events = nullptr;
    }

    [[nodiscard]] Video* video(const Request& request) {
        const auto found = videos.find(text_field(request.payload, "image"));
        return found != videos.end() ? &found->second : nullptr;
    }

    /** Starts a video; its answer waits until it knows its size (see video_frames). */
    void play(const Request& request) {
        if (!media_started) {
            if (FAILED(MFStartup(MF_VERSION)))
                throw std::runtime_error("Media Foundation is unavailable");
            media_started = true;
        }
        const std::string image = text_field(request.payload, "image");
        if (image.empty())
            throw std::runtime_error("media.play needs an image to play into");
        if (const auto found = videos.find(image); found != videos.end()) {
            release(found->second);
            videos.erase(found);
        }
        // A link or an absolute path opens as it is; anything else is a resource id.
        // The source named, or what the action's event brings: a dialog's path, a drop's file.
        std::string source = text_field(request.payload, "source");
        if (const JsonValue* event = request.payload.find("event");
            source.empty() && event != nullptr) {
            if (event->string() != nullptr) {
                source = *event->string();
            } else if (const JsonValue* path = event->find("path");
                       path != nullptr && path->string() != nullptr) {
                source = *path->string();
            } else if (const JsonValue* files = event->find("files");
                       files != nullptr && files->array() != nullptr && !files->array()->empty() &&
                       files->array()->front().string() != nullptr) {
                source = *files->array()->front().string();
            }
        }
        if (source.empty())
            throw std::runtime_error("media.play has no source");
        std::wstring url = wide(source);
        const bool absolute = source.find("://") != std::string::npos ||
                              (source.size() > 2U && source[1] == ':') ||
                              source.starts_with("\\\\");
        if (!absolute && resource_path)
            url = resource_path(source);
        ComPtr<IMFMediaEngineClassFactory> factory;
        ComPtr<IMFAttributes> attributes;
        if (FAILED(CoCreateInstance(CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory))) ||
            FAILED(MFCreateAttributes(&attributes, 2U))) {
            throw std::runtime_error("the media engine is unavailable");
        }
        Video playing;
        playing.client = request.client;
        playing.image = image;
        playing.events = new MediaEvents();
        attributes->SetUnknown(MF_MEDIA_ENGINE_CALLBACK, playing.events);
        // Frame-server mode: no window of its own, frames handed over on request.
        attributes->SetUINT32(MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT, DXGI_FORMAT_B8G8R8A8_UNORM);
        if (FAILED(factory->CreateInstance(0U, attributes.Get(), &playing.engine))) {
            release(playing);
            throw std::runtime_error("the media engine could not start");
        }
        BSTR location = SysAllocString(url.c_str());
        const HRESULT opened = playing.engine->SetSource(location);
        SysFreeString(location);
        if (FAILED(opened)) {
            release(playing);
            throw std::runtime_error("the video could not be opened");
        }
        playing.engine->SetLoop(flag_field(request.payload, "loop", true) ? TRUE : FALSE);
        playing.engine->SetMuted(flag_field(request.payload, "muted", false) ? TRUE : FALSE);
        if (const JsonValue* volume = request.payload.find("volume");
            volume != nullptr && volume->number() != nullptr) {
            playing.engine->SetVolume(std::clamp(*volume->number(), 0.0, 1.0));
        } else if (volume != nullptr && volume->integer() != nullptr) {
            playing.engine->SetVolume(
                std::clamp(static_cast<double>(*volume->integer()), 0.0, 1.0));
        }
        playing.engine->Play();
        playing.waiting = request;
        videos.insert_or_assign(image, std::move(playing));
    }

    [[nodiscard]] JsonValue::Object status(const Video& video) const {
        const double duration = video.engine->GetDuration();
        return {{"duration", JsonValue(std::isfinite(duration) ? duration : 0.0)},
                {"ended", JsonValue(video.engine->IsEnded() != FALSE)},
                {"height", JsonValue(static_cast<std::int64_t>(video.height))},
                {"image", JsonValue(video.image)},
                {"paused", JsonValue(video.engine->IsPaused() != FALSE)},
                {"position", JsonValue(video.engine->GetCurrentTime())},
                {"width", JsonValue(static_cast<std::int64_t>(video.width))}};
    }

    /** Answers videos that have loaded, and copies out each one's newest frame. */
    void tick_videos(std::vector<PlatformAnswer>& answers) {
        for (auto entry = videos.begin(); entry != videos.end();) {
            Video& video = entry->second;
            if (video.events->failed) {
                if (video.waiting.has_value())
                    answers.push_back(answer(*video.waiting,
                                             {{"error", JsonValue("the video could not be played")},
                                              {"image", JsonValue(video.image)}}));
                release(video);
                entry = videos.erase(entry);
                continue;
            }
            if (video.target == nullptr && video.events->loaded) {
                DWORD native_width = 0U;
                DWORD native_height = 0U;
                video.engine->GetNativeVideoSize(&native_width, &native_height);
                if (native_width != 0U && native_height != 0U) {
                    video.width = native_width;
                    video.height = native_height;
                    imaging_factory()->CreateBitmap(video.width, video.height,
                                                    GUID_WICPixelFormat32bppBGRA,
                                                    WICBitmapCacheOnLoad, &video.target);
                }
                if (video.waiting.has_value()) {
                    answers.push_back(answer(*video.waiting, status(video)));
                    video.waiting.reset();
                }
            }
            LONGLONG presentation = 0;
            if (video.target != nullptr && video.engine->OnVideoStreamTick(&presentation) == S_OK) {
                const MFVideoNormalizedRect whole{0.0F, 0.0F, 1.0F, 1.0F};
                const RECT into{0, 0, static_cast<LONG>(video.width),
                                static_cast<LONG>(video.height)};
                const MFARGB border{0U, 0U, 0U, 255U};
                if (SUCCEEDED(video.engine->TransferVideoFrame(video.target.Get(), &whole, &into,
                                                               &border))) {
                    ComPtr<IWICBitmapLock> lock;
                    const WICRect area{0, 0, static_cast<INT>(video.width),
                                       static_cast<INT>(video.height)};
                    if (SUCCEEDED(video.target->Lock(&area, WICBitmapLockRead, &lock))) {
                        UINT stride = 0U;
                        UINT size = 0U;
                        BYTE* pixels = nullptr;
                        lock->GetStride(&stride);
                        lock->GetDataPointer(&size, &pixels);
                        video.frame.id = video.image;
                        video.frame.width = video.width;
                        video.frame.height = video.height;
                        video.frame.rgba.resize(static_cast<std::size_t>(video.width) *
                                                video.height * 4U);
                        for (UINT y = 0U; y < video.height; ++y) {
                            const BYTE* source = pixels + static_cast<std::size_t>(y) * stride;
                            std::uint8_t* target = video.frame.rgba.data() +
                                                   static_cast<std::size_t>(y) * video.width * 4U;
                            for (UINT x = 0U; x < video.width; ++x) {
                                target[x * 4U] = source[x * 4U + 2U];
                                target[x * 4U + 1U] = source[x * 4U + 1U];
                                target[x * 4U + 2U] = source[x * 4U];
                                target[x * 4U + 3U] = 255U;
                            }
                        }
                        video.fresh = true;
                    }
                }
            }
            ++entry;
        }
    }

    // ---- the queue ----

    [[nodiscard]] std::vector<PlatformAnswer> pump() {
        std::deque<Request> due;
        std::vector<PlatformAnswer> answers;
        {
            const std::scoped_lock lock(mutex);
            due.swap(requests);
            // Dialogs that closed since the last frame.
            answers.swap(answered);
        }
        for (const Request& request : due) {
            try {
                run(request, answers);
            } catch (const std::exception& error) {
                answers.push_back(
                    answer(request, {{"error", JsonValue(std::string(error.what()))}}));
            }
        }
        tick_videos(answers);
        for (const std::unique_ptr<PlatformService>& service : services)
            service->tick();
        return answers;
    }

    void run(const Request& request, std::vector<PlatformAnswer>& answers) {
        const std::string& kind = request.kind;
        if (kind == "system.openFile" || kind == "system.saveFile") {
            // The dialog runs on a thread of its own: it is modal to the window, while the app
            // behind it goes on drawing and playing.
            const bool save = kind == "system.saveFile";
            dialogs.emplace_back([this, request, save] {
                const HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                PlatformAnswer finished;
                try {
                    finished = answer(request, file_dialog(request, save));
                } catch (const std::exception& error) {
                    finished = answer(request, {{"error", JsonValue(std::string(error.what()))}});
                }
                if (SUCCEEDED(apartment))
                    CoUninitialize();
                const std::scoped_lock lock(mutex);
                answered.push_back(std::move(finished));
            });
        } else if (kind == "system.notify") {
            answers.push_back(answer(request, {{"shown", JsonValue(notify(request))}}));
        } else if (kind == "system.open") {
            const std::wstring target = wide(text_field(request.payload, "target"));
            const auto opened = reinterpret_cast<std::intptr_t>(
                ShellExecuteW(window, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
            answers.push_back(answer(request, {{"opened", JsonValue(opened > 32)}}));
        } else if (kind == "window.minimize") {
            ShowWindow(window, SW_MINIMIZE);
        } else if (kind == "window.toggleMaximize") {
            ShowWindow(window, IsZoomed(window) ? SW_RESTORE : SW_MAXIMIZE);
        } else if (kind == "window.close") {
            if (text_field(request.payload, "id").empty())
                PostMessageW(window, WM_CLOSE, 0, 0);
            else
                request_window(request, WindowRequest::Kind::close);
        } else if (kind == "window.open") {
            if (text_field(request.payload, "id").empty() ||
                text_field(request.payload, "root").empty()) {
                answers.push_back(answer(
                    request, {{"error", JsonValue("window.open needs an id and a root overlay")},
                              {"opened", JsonValue(false)}}));
                return;
            }
            request_window(request, WindowRequest::Kind::open);
        } else if (kind == "window.set") {
            answers.push_back(answer(request, control.set(request.payload)));
        } else if (kind == "window.show") {
            show_window();
            answers.push_back(answer(request, control.state()));
        } else if (kind == "window.hideToTray") {
            answers.push_back(answer(request, hide_to_tray()));
        } else if (kind == "system.tray") {
            answers.push_back(answer(request, set_tray(request)));
        } else if (kind == "system.dragOut") {
            answers.push_back(answer(request, drag_out(request)));
        } else if (kind == "system.paths") {
            answers.push_back(answer(request, paths()));
        } else if (kind == "node.capture") {
            // The next frame's pixels hold it; the host hands them over once presented.
            captures.push_back(request);
        } else if (kind == "window.drag") {
            // The system's own move loop: snapping, shaking and dragging off a maximized window.
            POINT cursor{};
            GetCursorPos(&cursor);
            ReleaseCapture();
            SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(cursor.x, cursor.y));
            window_moved = true;
        } else if (kind == "media.play") {
            play(request);
        } else if (kind.starts_with("media.")) {
            Video* const playing = video(request);
            if (playing == nullptr) {
                answers.push_back(
                    answer(request, {{"error", JsonValue("no video plays into that image")},
                                     {"image", JsonValue(text_field(request.payload, "image"))}}));
                return;
            }
            if (kind == "media.set") {
                // Muting, volume and looping change while it plays.
                if (const JsonValue* muted = request.payload.find("muted");
                    muted != nullptr && muted->boolean() != nullptr) {
                    playing->engine->SetMuted(*muted->boolean() ? TRUE : FALSE);
                }
                if (const JsonValue* volume = request.payload.find("volume"); volume != nullptr) {
                    const double level = volume->number() != nullptr ? *volume->number()
                                         : volume->integer() != nullptr
                                             ? static_cast<double>(*volume->integer())
                                             : 1.0;
                    playing->engine->SetVolume(std::clamp(level, 0.0, 1.0));
                }
                if (const JsonValue* loop = request.payload.find("loop");
                    loop != nullptr && loop->boolean() != nullptr) {
                    playing->engine->SetLoop(*loop->boolean() ? TRUE : FALSE);
                }
            } else if (kind == "media.pause") {
                playing->engine->Pause();
                playing->hidden = false;
            } else if (kind == "media.resume") {
                playing->engine->Play();
                playing->hidden = false;
            } else if (kind == "media.seek") {
                const JsonValue* seconds = request.payload.find("seconds");
                if (seconds == nullptr)
                    seconds = request.payload.find("event");
                const double to = seconds == nullptr             ? 0.0
                                  : seconds->number() != nullptr ? *seconds->number()
                                  : seconds->integer() != nullptr
                                      ? static_cast<double>(*seconds->integer())
                                      : 0.0;
                playing->engine->SetCurrentTime(std::max(0.0, to));
            } else if (kind == "media.stop") {
                const std::string image = playing->image;
                release(*playing);
                videos.erase(image);
                answers.push_back(answer(request, {{"stopped", JsonValue(true)}}));
                return;
            }
            answers.push_back(answer(request, status(*playing)));
        } else if (kind == "window.setChrome") {
            answers.push_back(answer(request, set_chrome(request)));
        } else if (kind == "clipboard.copyImage") {
            answers.push_back(answer(request, copy_image(request)));
        } else if (kind == "clipboard.readImage") {
            std::optional<Pixels> pixels = clipboard_image();
            if (!pixels.has_value()) {
                answers.push_back(answer(request, {{"height", JsonValue(std::int64_t{0})},
                                                   {"image", JsonValue("")},
                                                   {"width", JsonValue(std::int64_t{0})}}));
                return;
            }
            const std::string id = "clipboard:image-" + std::to_string(++pasted_count);
            PlatformAnswer result =
                answer(request, {{"height", JsonValue(static_cast<std::int64_t>(pixels->height))},
                                 {"image", JsonValue(id)},
                                 {"width", JsonValue(static_cast<std::int64_t>(pixels->width))}});
            result.image = PlatformImage{id, pixels->width, pixels->height, pixels->rgba};
            // Only the latest paste is kept to copy back.
            pasted.clear();
            pasted.emplace(id, std::move(*pixels));
            answers.push_back(std::move(result));
        } else if (const auto service = std::ranges::find_if(
                       services,
                       [&kind](const auto& candidate) { return candidate->serves(kind); });
                   service != services.end()) {
            // Requests, files, sockets and sounds answer later, through reply.
            (*service)->run(request);
        } else {
            answers.push_back(
                answer(request, {{"error", JsonValue("unknown platform request " + kind)}}));
        }
    }

    // ---- messages ----

    [[nodiscard]] std::optional<std::intptr_t> window_message(const UINT message, const WPARAM word,
                                                              const LPARAM long_value) {
        if (message == tray_message) {
            const UINT event = LOWORD(long_value);
            if (event == WM_LBUTTONUP || event == NIN_SELECT || event == NIN_KEYSELECT ||
                event == NIN_BALLOONUSERCLICK) {
                show_window();
            } else if (event == WM_CONTEXTMENU || (!tray_version_4 && event == WM_RBUTTONUP)) {
                // Version 4 anchors the menu where the icon was invoked, keyboard or mouse.
                POINT anchor{GET_X_LPARAM(word), GET_Y_LPARAM(word)};
                if (!tray_version_4)
                    GetCursorPos(&anchor);
                show_tray_menu(anchor);
            }
            return 0;
        }
        // The user changed the app theme or the accent colour: documents that follow them follow.
        if ((message == WM_SETTINGCHANGE && long_value != 0 &&
             std::wstring_view(reinterpret_cast<const wchar_t*>(long_value)) ==
                 L"ImmersiveColorSet") ||
            // ClearType turned on or off, or its subpixel order changed.
            (message == WM_SETTINGCHANGE &&
             (word == SPI_SETFONTSMOOTHING || word == SPI_SETFONTSMOOTHINGTYPE ||
              word == SPI_SETFONTSMOOTHINGORIENTATION || word == SPI_SETFONTSMOOTHINGCONTRAST)) ||
            message == WM_DWMCOLORIZATIONCOLORCHANGED || message == WM_THEMECHANGED) {
            if (SystemAppearance next = read_appearance(); !(next == system_appearance)) {
                system_appearance = next;
                appearance_changed = true;
            }
            return std::nullopt;
        }
        if (const std::optional<std::intptr_t> handled =
                control.window_message(message, word, long_value);
            handled.has_value()) {
            return handled;
        }
        // Fullscreen has no chrome, custom or not.
        if (!custom_title || control.fullscreen())
            return std::nullopt;
        if (message == WM_NCCALCSIZE && word == TRUE) {
            // The title bar is the document's: the client reaches the top edge, while the sides
            // and bottom keep the system's resize borders.
            auto* const parameters = reinterpret_cast<NCCALCSIZE_PARAMS*>(long_value);
            RECT& client = parameters->rgrc[0];
            const int frame = frame_thickness();
            client.left += frame;
            client.right -= frame;
            client.bottom -= frame;
            if (IsZoomed(window))
                client.top += frame;
            return 0;
        }
        if (message == WM_NCHITTEST) {
            const LRESULT hit = DefWindowProcW(window, message, word, long_value);
            if (hit != HTCLIENT || IsZoomed(window))
                return hit;
            POINT point{GET_X_LPARAM(long_value), GET_Y_LPARAM(long_value)};
            ScreenToClient(window, &point);
            return point.y < frame_thickness() ? HTTOP : HTCLIENT;
        }
        return std::nullopt;
    }

    HWND window = nullptr;
    ResourceReader read_resource;
    ResourcePath resource_path;
    bool media_started = false;
    std::map<std::string, Video, std::less<>> videos;
    /** Answers from dialogs that closed on their own threads, waiting for the next pump. */
    std::vector<PlatformAnswer> answered;
    /** Declared last: joined first, while everything a dialog thread uses is still alive. */
    std::vector<std::jthread> dialogs;
    bool ole = false;
    std::mutex mutex;
    std::deque<Request> requests;
    std::map<std::string, std::unique_ptr<Client>, std::less<>> clients;
    DropTarget* drop_target = nullptr;
    bool tray = false;
    bool custom_title = false;
    bool transparent_backdrop = false;
    bool window_moved = false;
    std::uint64_t pasted_count = 0U;
    std::map<std::string, Pixels, std::less<>> pasted;
    WindowControl control;
    /** The tray icon's menu and the session that set it, which hears what is chosen. */
    std::vector<TrayItem> tray_items;
    std::string tray_client;
    HICON tray_icon = nullptr;
    bool tray_version_4 = false;
    /** Why the tray icon is there: its menu, a notification, a window hidden to it. */
    bool tray_menu = false;
    bool tray_notified = false;
    bool tray_for_hiding = false;
    std::vector<WindowRequest> window_requests;
    /** `node.capture` requests waiting for the next presented frame. */
    std::vector<Request> captures;
    std::uint64_t capture_count = 0U;
    SystemAppearance system_appearance;
    bool appearance_changed = false;
    /** Network, files and sound: they finish later, on threads of their own. */
    std::vector<std::unique_ptr<PlatformService>> services;
};

PlatformServices::PlatformServices(HWND window, ResourceReader read_resource,
                                   ResourcePath resource_path)
    : impl_(std::make_unique<Impl>(window, std::move(read_resource), std::move(resource_path))) {}

std::vector<std::pair<std::string, PlatformImage>> PlatformServices::video_frames() {
    std::vector<std::pair<std::string, PlatformImage>> frames;
    for (auto& [id, video] : impl_->videos) {
        if (!video.fresh)
            continue;
        video.fresh = false;
        frames.emplace_back(video.client, video.frame);
    }
    return frames;
}

void PlatformServices::follow_video_use(
    const std::function<bool(std::string_view client, std::string_view image)>& shown) {
    for (auto& [id, video] : impl_->videos) {
        const bool now = shown(video.client, video.image);
        if (now && !video.seen) {
            video.seen = true;
        } else if (!now && video.seen && !video.hidden && video.engine->IsPaused() == FALSE) {
            video.engine->Pause();
            video.hidden = true;
        } else if (now && video.hidden) {
            video.engine->Play();
            video.hidden = false;
        }
    }
}

PlatformServices::~PlatformServices() = default;

strata_effect_adapter PlatformServices::effect_adapter(std::string client) {
    auto [entry, inserted] = impl_->clients.try_emplace(client);
    if (inserted)
        entry->second =
            std::make_unique<Impl::Client>(Impl::Client{impl_.get(), std::move(client)});
    return strata_effect_adapter{sizeof(strata_effect_adapter), entry->second.get(), &Impl::emit};
}

std::vector<PlatformAnswer> PlatformServices::pump() {
    return impl_->pump();
}

void PlatformServices::post_event(std::string client, std::string channel, std::string data_json) {
    const std::scoped_lock lock(impl_->mutex);
    impl_->answered.push_back(PlatformAnswer{std::move(client), 0U, std::move(data_json),
                                             std::nullopt, std::move(channel)});
}

std::string PlatformServices::event_json(const PlatformAnswer& event) {
    JsonValue data;
    try {
        data = data::parse_json(event.json);
    } catch (const std::exception&) {
        data = JsonValue(event.json);
    }
    return data::encode_canonical_json(JsonValue(JsonValue::Object{
        {"channel", JsonValue(event.channel.value_or(std::string{}))},
        {"data", std::move(data)},
    }));
}

void PlatformServices::answer(std::string client, const std::uint64_t request, std::string json) {
    const std::scoped_lock lock(impl_->mutex);
    impl_->answered.push_back(
        PlatformAnswer{std::move(client), request, std::move(json), std::nullopt, std::nullopt});
}

std::vector<WindowRequest> PlatformServices::take_window_requests() {
    return std::exchange(impl_->window_requests, {});
}

const SystemAppearance& PlatformServices::appearance() const noexcept {
    return impl_->system_appearance;
}

bool PlatformServices::take_appearance_changed() noexcept {
    return std::exchange(impl_->appearance_changed, false);
}

bool PlatformServices::wants_frame() const noexcept {
    return !impl_->captures.empty();
}

void PlatformServices::frame_presented(const std::uint32_t width, const std::uint32_t height,
                                       const std::span<const std::uint8_t> rgba) {
    impl_->frame_presented(width, height, rgba);
}

void PlatformServices::close_client(const std::string_view client) noexcept {
    try {
        impl_->close_client(client);
    } catch (...) {
        // Hosts close sessions while tearing down: nothing may escape.
    }
}

bool PlatformServices::take_window_moved() noexcept {
    return std::exchange(impl_->window_moved, false);
}

void PlatformServices::accept_files(FileSink sink) {
    if (impl_->drop_target != nullptr || !impl_->ole)
        return;
    auto* const target = new DropTarget(impl_->window, std::move(sink));
    if (SUCCEEDED(RegisterDragDrop(impl_->window, target)))
        impl_->drop_target = target;
    else
        target->Release();
}

bool PlatformServices::transparent() const noexcept {
    return impl_->transparent_backdrop;
}

std::optional<std::intptr_t> PlatformServices::window_message(const std::uint32_t message,
                                                              const std::uintptr_t word,
                                                              const std::intptr_t long_value) {
    return impl_->window_message(message, static_cast<WPARAM>(word),
                                 static_cast<LPARAM>(long_value));
}

} // namespace strata::desktop
