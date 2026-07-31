#include "texture_store.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "host/render_packet.hpp"

namespace strata::d3d11 {

using host::DrawBatch;
using host::ResourceOperation;

namespace {

using Microsoft::WRL::ComPtr;

void require_hresult(const HRESULT result, const std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                                 std::to_string(result));
    }
}

struct Texture final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11SamplerState> sampler;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

[[nodiscard]] DXGI_FORMAT atlas_format(const std::uint32_t format) {
    if (format == 0U)
        return DXGI_FORMAT_R8_UNORM;
    if (format == 1U)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    throw std::invalid_argument("D3D11 renderer received an unknown atlas format");
}

} // namespace

struct TextureStore::Impl final {
    Impl(ID3D11Device* const device, ID3D11DeviceContext* const context)
        : device(device), context(context) {
        if (device == nullptr || context == nullptr) {
            throw std::invalid_argument("D3D11 texture store requires a device and context");
        }
        constexpr std::array<std::uint8_t, 4U> pixel{255U, 255U, 255U, 255U};
        white = create_texture(1U, 1U, DXGI_FORMAT_R8G8B8A8_UNORM, 0U, pixel.data(),
                               static_cast<std::uint32_t>(pixel.size()));
    }

    [[nodiscard]] ComPtr<ID3D11SamplerState> create_sampler(const std::uint32_t sampling) const {
        D3D11_SAMPLER_DESC descriptor{};
        descriptor.Filter =
            sampling == 0U ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        descriptor.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        descriptor.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        descriptor.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        descriptor.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> result;
        require_hresult(device->CreateSamplerState(&descriptor, &result),
                        "texture sampler creation");
        return result;
    }

    [[nodiscard]] Texture create_texture(const std::uint32_t width, const std::uint32_t height,
                                         const DXGI_FORMAT format, const std::uint32_t sampling,
                                         const void* const pixels = nullptr,
                                         const std::uint32_t row_pitch = 0U) const {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = format;
        descriptor.SampleDesc.Count = 1U;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA initial{pixels, row_pitch, 0U};
        Texture result;
        require_hresult(device->CreateTexture2D(&descriptor, pixels == nullptr ? nullptr : &initial,
                                                &result.texture),
                        "texture creation");
        require_hresult(
            device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view),
            "texture view creation");
        result.sampler = create_sampler(sampling);
        result.format = format;
        result.width = width;
        result.height = height;
        return result;
    }

    [[nodiscard]] std::vector<std::uint8_t> decode_png(const ResourceOperation& operation) const {
        ComPtr<IWICImagingFactory> factory;
        require_hresult(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                         IID_PPV_ARGS(&factory)),
                        "WIC imaging factory creation");
        ComPtr<IWICStream> stream;
        require_hresult(factory->CreateStream(&stream), "WIC stream creation");
        if (operation.bytes.size() > std::numeric_limits<DWORD>::max()) {
            throw std::length_error("encoded D3D11 texture exceeds WIC's byte limit");
        }
        require_hresult(stream->InitializeFromMemory(const_cast<BYTE*>(operation.bytes.data()),
                                                     static_cast<DWORD>(operation.bytes.size())),
                        "WIC stream initialization");
        ComPtr<IWICBitmapDecoder> decoder;
        require_hresult(factory->CreateDecoderFromStream(stream.Get(), nullptr,
                                                         WICDecodeMetadataCacheOnLoad, &decoder),
                        "PNG decoder creation");
        ComPtr<IWICBitmapFrameDecode> frame;
        require_hresult(decoder->GetFrame(0U, &frame), "PNG frame decode");
        UINT decoded_width = 0U;
        UINT decoded_height = 0U;
        require_hresult(frame->GetSize(&decoded_width, &decoded_height), "PNG size read");
        if (decoded_width != operation.width || decoded_height != operation.height) {
            throw std::invalid_argument("decoded PNG dimensions differ from the native descriptor");
        }
        ComPtr<IWICFormatConverter> converter;
        require_hresult(factory->CreateFormatConverter(&converter),
                        "WIC format converter creation");
        require_hresult(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                              WICBitmapDitherTypeNone, nullptr, 0.0,
                                              WICBitmapPaletteTypeCustom),
                        "PNG RGBA conversion");
        const std::uint64_t row_pitch = static_cast<std::uint64_t>(decoded_width) * 4U;
        const std::uint64_t byte_count = row_pitch * decoded_height;
        if (row_pitch > std::numeric_limits<UINT>::max() ||
            byte_count > std::numeric_limits<UINT>::max()) {
            throw std::length_error("decoded D3D11 texture exceeds WIC's output limit");
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(byte_count));
        require_hresult(converter->CopyPixels(nullptr, static_cast<UINT>(row_pitch),
                                              static_cast<UINT>(byte_count), pixels.data()),
                        "PNG pixel copy");
        return pixels;
    }

    void apply(const ResourceOperation& operation) {
        if (operation.kind == 2U) {
            textures.erase(operation.texture);
            return;
        }
        if (operation.kind == 0U) {
            textures.insert_or_assign(operation.texture,
                                      create_texture(operation.width, operation.height,
                                                     atlas_format(operation.format),
                                                     operation.format == 0U ? 0U : 1U));
            return;
        }
        if (operation.kind == 3U) {
            const std::vector<std::uint8_t> pixels = decode_png(operation);
            textures.insert_or_assign(operation.texture,
                                      create_texture(operation.width, operation.height,
                                                     DXGI_FORMAT_R8G8B8A8_UNORM, operation.sampling,
                                                     pixels.data(), operation.width * 4U));
            return;
        }
        Texture& destination = textures.at(operation.texture);
        if (destination.format != atlas_format(operation.format)) {
            throw std::invalid_argument("atlas upload format differs from its live texture");
        }
        const std::uint64_t right = static_cast<std::uint64_t>(operation.x) + operation.width;
        const std::uint64_t bottom = static_cast<std::uint64_t>(operation.y) + operation.height;
        if (right > destination.width || bottom > destination.height) {
            throw std::invalid_argument("atlas upload exceeds its live texture");
        }
        const std::uint32_t bytes_per_pixel = operation.format == 0U ? 1U : 4U;
        const D3D11_BOX box{
            operation.x, operation.y, 0U, static_cast<UINT>(right), static_cast<UINT>(bottom), 1U,
        };
        context->UpdateSubresource(destination.texture.Get(), 0U, &box, operation.bytes.data(),
                                   operation.width * bytes_per_pixel, 0U);
    }

    void bind(const DrawBatch& batch) const {
        const Texture* selected = &white;
        if (batch.texture.has_value()) {
            const auto found = textures.find(*batch.texture);
            if (found != textures.end())
                selected = &found->second;
        }
        ID3D11ShaderResourceView* const view = selected->view.Get();
        ID3D11SamplerState* const sampler = selected->sampler.Get();
        context->PSSetShaderResources(0U, 1U, &view);
        context->PSSetSamplers(0U, 1U, &sampler);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    std::map<std::string, Texture, std::less<>> textures;
    Texture white;
};

TextureStore::TextureStore(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : impl_(std::make_unique<Impl>(device, context)) {}
TextureStore::~TextureStore() = default;

void TextureStore::apply(const ResourceOperation& operation) {
    impl_->apply(operation);
}
void TextureStore::bind(const DrawBatch& batch) const {
    impl_->bind(batch);
}

} // namespace strata::d3d11
