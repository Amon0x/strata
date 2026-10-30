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
#include <wrl/client.h>

#include "raster_pass.hpp"
#include <strata/render_packet.hpp>

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
    /** Shape atlases (formats 1 and 2), which raster jobs write from compute shaders. */
    ComPtr<ID3D11UnorderedAccessView> writer;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

[[nodiscard]] DXGI_FORMAT atlas_format(const std::uint32_t format) {
    if (format == 0U || format == 2U)
        return DXGI_FORMAT_R8_UNORM;
    if (host::texture_format_rgba(format))
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
        RasterPass::prepare();
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
                                         const std::uint32_t row_pitch = 0U,
                                         const bool writable = false) const {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = format;
        descriptor.SampleDesc.Count = 1U;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (writable)
            descriptor.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
        const D3D11_SUBRESOURCE_DATA initial{pixels, row_pitch, 0U};
        Texture result;
        require_hresult(device->CreateTexture2D(&descriptor, pixels == nullptr ? nullptr : &initial,
                                                &result.texture),
                        "texture creation");
        require_hresult(
            device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view),
            "texture view creation");
        if (writable) {
            require_hresult(
                device->CreateUnorderedAccessView(result.texture.Get(), nullptr, &result.writer),
                "texture writer creation");
        }
        result.sampler = create_sampler(sampling);
        result.format = format;
        result.width = width;
        result.height = height;
        return result;
    }

    /** Drops the live jobs a new write over (part of) a texture replaces. */
    void forget_live(const ResourceOperation& operation, const bool whole) {
        std::erase_if(live, [&](const LiveJob& job) {
            if (job.operation.texture != operation.texture)
                return false;
            if (whole)
                return true;
            const ResourceOperation& other = job.operation;
            return operation.x < other.x + other.width && other.x < operation.x + operation.width &&
                   operation.y < other.y + other.height && other.y < operation.y + operation.height;
        });
    }

    void run_live(const double seconds) {
        // Every live job of the frame in one batch.
        live_batch.clear();
        for (LiveJob& job : live) {
            if (job.seconds == seconds)
                continue;
            const auto found = textures.find(job.operation.texture);
            if (found == textures.end() || found->second.writer == nullptr)
                continue;
            live_batch.push_back(RasterPass::Target{
                &job.operation.raster, job.operation.format == 1U, found->second.writer.Get(),
                job.operation.x, job.operation.y, job.operation.width, job.operation.height});
            job.seconds = seconds;
        }
        if (live_batch.empty())
            return;
        if (raster == nullptr)
            raster = std::make_unique<RasterPass>(device.Get(), context.Get());
        raster->run(live_batch, seconds);
    }

    void apply(const ResourceOperation& operation) {
        if (operation.kind == 2U || operation.kind == 0U || operation.kind == host::resource_image)
            forget_live(operation, true);
        else
            forget_live(operation, false);
        if (operation.kind == 2U) {
            textures.erase(operation.texture);
            return;
        }
        if (operation.kind == 0U) {
            textures.insert_or_assign(operation.texture,
                                      create_texture(operation.width, operation.height,
                                                     atlas_format(operation.format),
                                                     host::texture_format_exact(operation.format)
                                                         ? 0U
                                                         : 1U,
                                                     nullptr, 0U, operation.format != 0U));
            // Glyph pages (0, and subpixel 3) sample texel-exact; shape masks (2) and MSDF (1)
            // bilinearly.
            return;
        }
        if (operation.kind == host::resource_image) {
            // The core decoded the image: straight-alpha RGBA8 rows, sampled as stored.
            textures.insert_or_assign(operation.texture,
                                      create_texture(operation.width, operation.height,
                                                     DXGI_FORMAT_R8G8B8A8_UNORM, operation.sampling,
                                                     operation.bytes.data(), operation.width * 4U));
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
        if (operation.kind == host::resource_raster) {
            if (destination.writer == nullptr)
                throw std::invalid_argument("raster job targets an atlas compute cannot write");
            // Art draws every frame, with the frame's time: it runs when the frame renders.
            if (host::raster_job_has_art(operation.raster)) {
                live.push_back(LiveJob{operation, std::numeric_limits<double>::quiet_NaN()});
                return;
            }
            if (raster == nullptr)
                raster = std::make_unique<RasterPass>(device.Get(), context.Get());
            raster->run(operation.raster, operation.format == 1U, destination.writer.Get(),
                        operation.x, operation.y, operation.width, operation.height);
            return;
        }
        const std::uint32_t bytes_per_pixel = host::texture_format_rgba(operation.format) ? 4U : 1U;
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
    /** Made on the first raster job, so a stream without shapes compiles no compute shaders. */
    std::unique_ptr<RasterPass> raster;
    /** Art jobs, run again each frame; `seconds` is the frame time each last ran at. */
    struct LiveJob final {
        ResourceOperation operation;
        double seconds = 0.0;
    };
    std::vector<LiveJob> live;
    std::vector<RasterPass::Target> live_batch;
};

TextureStore::TextureStore(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : impl_(std::make_unique<Impl>(device, context)) {}
TextureStore::~TextureStore() = default;

void TextureStore::apply(const ResourceOperation& operation) {
    impl_->apply(operation);
}
void TextureStore::run_live(const double seconds) {
    impl_->run_live(seconds);
}
void TextureStore::bind(const DrawBatch& batch) const {
    impl_->bind(batch);
}

} // namespace strata::d3d11
