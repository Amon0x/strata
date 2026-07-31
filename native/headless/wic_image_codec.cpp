#include "image_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace strata::headless {
namespace {

using Microsoft::WRL::ComPtr;

void require_hresult(const HRESULT result, const char* const operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string("headless PNG ") + operation +
                                 " failed with HRESULT " + std::to_string(result));
    }
}

class WicImageCodec final : public ImageCodec {
  public:
    [[nodiscard]] DecodedImage decode_png(const std::span<const std::uint8_t> bytes,
                                          const std::uint32_t expected_width,
                                          const std::uint32_t expected_height) const override {
        if (bytes.empty() || bytes.size() > std::numeric_limits<DWORD>::max()) {
            throw std::invalid_argument("encoded PNG exceeds the WIC input domain");
        }
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool owns_com = SUCCEEDED(initialized);
        if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
            require_hresult(initialized, "COM initialization");
        }
        struct ComGuard final {
            bool owns = false;
            ~ComGuard() {
                if (owns)
                    CoUninitialize();
            }
        } guard{owns_com};

        ComPtr<IWICImagingFactory> factory;
        require_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory)),
                        "factory creation");
        ComPtr<IWICStream> stream;
        require_hresult(factory->CreateStream(&stream), "stream creation");
        require_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                                     static_cast<DWORD>(bytes.size())),
                        "stream initialization");
        ComPtr<IWICBitmapDecoder> decoder;
        require_hresult(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                         WICDecodeMetadataCacheOnLoad, &decoder),
                        "decoder creation");
        ComPtr<IWICBitmapFrameDecode> frame;
        require_hresult(decoder->GetFrame(0U, &frame), "frame decode");
        UINT width = 0U;
        UINT height = 0U;
        require_hresult(frame->GetSize(&width, &height), "dimension read");
        if (width != expected_width || height != expected_height) {
            throw std::invalid_argument(
                "decoded PNG dimensions differ from the render-packet descriptor");
        }
        ComPtr<IWICFormatConverter> converter;
        require_hresult(factory->CreateFormatConverter(&converter), "converter creation");
        require_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                              WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom),
                        "RGBA conversion");
        const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4U;
        const std::uint64_t byte_count = row_bytes * height;
        if (row_bytes > std::numeric_limits<UINT>::max() ||
            byte_count > std::numeric_limits<UINT>::max()) {
            throw std::length_error("decoded PNG exceeds the WIC output domain");
        }
        DecodedImage result;
        result.width = width;
        result.height = height;
        result.rgba.resize(static_cast<std::size_t>(byte_count));
        require_hresult(converter->CopyPixels(nullptr, static_cast<UINT>(row_bytes),
                                              static_cast<UINT>(byte_count), result.rgba.data()),
                        "pixel copy");
        return result;
    }
};

} // namespace

const ImageCodec& platform_image_codec() {
    static const WicImageCodec codec;
    return codec;
}

} // namespace strata::headless
