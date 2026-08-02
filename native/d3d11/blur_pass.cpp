#include "blur_pass.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "blur_shaders.hpp"
#include <strata/render_packet.hpp>

namespace strata::d3d11 {

using host::BlurBatch;

namespace {

using Microsoft::WRL::ComPtr;

void require_hresult(const HRESULT result, const std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                                 std::to_string(result));
    }
}

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(const std::string_view source,
                                              const char* const target) {
    ComPtr<ID3DBlob> result;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT status = D3DCompile(
        source.data(), source.size(), "strata.d3d11.blur.hlsl", nullptr, nullptr, "main", target,
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &result, &diagnostics);
    if (FAILED(status)) {
        const std::string detail =
            diagnostics == nullptr
                ? std::string{}
                : std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                              diagnostics->GetBufferSize());
        throw std::runtime_error("D3D11 blur shader compilation failed: " + detail);
    }
    return result;
}

struct Target final {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
};

struct PixelRect final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] std::uint32_t right() const noexcept {
        return x + width;
    }
    [[nodiscard]] std::uint32_t bottom() const noexcept {
        return y + height;
    }
};

struct UvRect final {
    float u = 0.0F;
    float v = 0.0F;
    float width = 1.0F;
    float height = 1.0F;
};

struct TargetKey final {
    std::uint32_t source_width = 0U;
    std::uint32_t source_height = 0U;
    std::uint32_t target_width = 0U;
    std::uint32_t target_height = 0U;
    std::uint32_t downsample = 1U;

    [[nodiscard]] friend auto operator<=>(const TargetKey&, const TargetKey&) = default;
};

struct TargetSet final {
    Target source;
    Target read;
    Target write;
    std::uint64_t last_used = 0U;
    std::uint64_t pixels = 0U;
};

struct BlurPlan final {
    PixelRect effect;
    PixelRect source;
    PixelRect scaled_effect;
    TargetKey targets;
    float radius = 0.5F;
};

[[nodiscard]] std::uint32_t clipped_edge(const long double value, const std::uint32_t maximum,
                                         const bool outward_trailing) noexcept {
    if (value <= 0.0L)
        return 0U;
    if (value >= static_cast<long double>(maximum))
        return maximum;
    if (!std::isfinite(value))
        return 0U;
    const long double rounded = outward_trailing ? std::ceil(value) : std::floor(value);
    return static_cast<std::uint32_t>(rounded);
}

[[nodiscard]] std::optional<BlurPlan> plan(const BlurBatch& batch,
                                           const std::uint32_t framebuffer_width,
                                           const std::uint32_t framebuffer_height,
                                           const double logical_width,
                                           const double logical_height) {
    if (batch.radius <= 0.0 || batch.width <= 0.0 || batch.height <= 0.0 ||
        framebuffer_width == 0U || framebuffer_height == 0U || !std::isfinite(logical_width) ||
        !std::isfinite(logical_height) || logical_width <= 0.0 || logical_height <= 0.0) {
        return std::nullopt;
    }
    const long double scale_x = static_cast<long double>(framebuffer_width) / logical_width;
    const long double scale_y = static_cast<long double>(framebuffer_height) / logical_height;
    std::uint32_t left =
        clipped_edge(static_cast<long double>(batch.x) * scale_x, framebuffer_width, false);
    std::uint32_t top =
        clipped_edge(static_cast<long double>(batch.y) * scale_y, framebuffer_height, false);
    std::uint32_t right = clipped_edge((static_cast<long double>(batch.x) + batch.width) * scale_x,
                                       framebuffer_width, true);
    std::uint32_t bottom = clipped_edge(
        (static_cast<long double>(batch.y) + batch.height) * scale_y, framebuffer_height, true);
    const std::uint64_t clip_right = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(batch.scissor.x) + batch.scissor.width, framebuffer_width);
    const std::uint64_t clip_bottom = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(batch.scissor.y) + batch.scissor.height, framebuffer_height);
    left = std::max(left, std::min(batch.scissor.x, framebuffer_width));
    top = std::max(top, std::min(batch.scissor.y, framebuffer_height));
    right = std::min(right, static_cast<std::uint32_t>(clip_right));
    bottom = std::min(bottom, static_cast<std::uint32_t>(clip_bottom));
    if (left >= right || top >= bottom)
        return std::nullopt;

    const PixelRect effect{left, top, right - left, bottom - top};
    const long double physical_radius =
        static_cast<long double>(batch.radius) * std::max(scale_x, scale_y);
    const std::uint32_t padding = static_cast<std::uint32_t>(
        std::clamp<long double>(std::ceil(physical_radius), 1.0L, 256.0L));
    const std::uint32_t source_left = effect.x > padding ? effect.x - padding : 0U;
    const std::uint32_t source_top = effect.y > padding ? effect.y - padding : 0U;
    const std::uint32_t source_right = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(effect.right()) + padding, framebuffer_width));
    const std::uint32_t source_bottom = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        static_cast<std::uint64_t>(effect.bottom()) + padding, framebuffer_height));
    const PixelRect source{
        source_left,
        source_top,
        source_right - source_left,
        source_bottom - source_top,
    };
    const std::uint32_t downsample = std::clamp(batch.downsample, 1U, 8U);
    const auto downsampled = [downsample](const std::uint32_t extent) noexcept {
        return extent / downsample + (extent % downsample == 0U ? 0U : 1U);
    };
    const std::uint32_t target_width = std::max(1U, downsampled(source.width));
    const std::uint32_t target_height = std::max(1U, downsampled(source.height));
    const PixelRect scaled_effect{
        (effect.x - source.x) / downsample,
        (effect.y - source.y) / downsample,
        std::max(1U, downsampled(effect.width)),
        std::max(1U, downsampled(effect.height)),
    };
    return BlurPlan{
        effect,
        source,
        scaled_effect,
        TargetKey{source.width, source.height, target_width, target_height, downsample},
        static_cast<float>(std::clamp(physical_radius / downsample, 0.5L, 32.0L)),
    };
}

[[nodiscard]] D3D11_RECT d3d_rect(const PixelRect rect) noexcept {
    return D3D11_RECT{
        static_cast<LONG>(rect.x),
        static_cast<LONG>(rect.y),
        static_cast<LONG>(rect.right()),
        static_cast<LONG>(rect.bottom()),
    };
}

[[nodiscard]] D3D11_VIEWPORT viewport(const PixelRect rect) noexcept {
    return D3D11_VIEWPORT{
        static_cast<float>(rect.x),
        static_cast<float>(rect.y),
        static_cast<float>(rect.width),
        static_cast<float>(rect.height),
        0.0F,
        1.0F,
    };
}

} // namespace

struct BlurPass::Impl final {
    Impl(ID3D11Device* const device, ID3D11DeviceContext* const context)
        : device(device), context(context) {
        if (device == nullptr || context == nullptr) {
            throw std::invalid_argument("D3D11 blur requires a device and context");
        }
        const ComPtr<ID3DBlob> vertex_code = compile_shader(blur_shaders::vertex, "vs_5_0");
        const ComPtr<ID3DBlob> pixel_code = compile_shader(blur_shaders::pixel, "ps_5_0");
        require_hresult(device->CreateVertexShader(vertex_code->GetBufferPointer(),
                                                   vertex_code->GetBufferSize(), nullptr,
                                                   &vertex_shader),
                        "blur vertex shader creation");
        require_hresult(device->CreatePixelShader(pixel_code->GetBufferPointer(),
                                                  pixel_code->GetBufferSize(), nullptr,
                                                  &pixel_shader),
                        "blur pixel shader creation");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = 48U;
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        require_hresult(device->CreateBuffer(&constant_desc, nullptr, &constants),
                        "blur constant buffer creation");

        D3D11_SAMPLER_DESC sampler_desc{};
        sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
        require_hresult(device->CreateSamplerState(&sampler_desc, &sampler),
                        "blur sampler creation");

        D3D11_RASTERIZER_DESC rasterizer_desc{};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.ScissorEnable = TRUE;
        rasterizer_desc.DepthClipEnable = TRUE;
        require_hresult(device->CreateRasterizerState(&rasterizer_desc, &rasterizer),
                        "blur rasterizer creation");
    }

    [[nodiscard]] Target create_target(const std::uint32_t target_width,
                                       const std::uint32_t target_height,
                                       const bool renderable) const {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = target_width;
        descriptor.Height = target_height;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = format;
        descriptor.SampleDesc.Count = 1U;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags =
            D3D11_BIND_SHADER_RESOURCE | (renderable ? D3D11_BIND_RENDER_TARGET : 0U);
        Target result;
        require_hresult(device->CreateTexture2D(&descriptor, nullptr, &result.texture),
                        "blur texture creation");
        require_hresult(
            device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view),
            "blur texture view creation");
        if (renderable) {
            require_hresult(
                device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.target),
                "blur target view creation");
        }
        return result;
    }

    void resize(const std::uint32_t next_width, const std::uint32_t next_height,
                const DXGI_FORMAT next_format) {
        if (next_width == width && next_height == height && next_format == format)
            return;
        context->OMSetRenderTargets(0U, nullptr, nullptr);
        ID3D11ShaderResourceView* const none = nullptr;
        context->PSSetShaderResources(0U, 1U, &none);
        targets.clear();
        retained_pixels = 0U;
        width = next_width;
        height = next_height;
        format = next_format;
    }

    [[nodiscard]] TargetSet& acquire(const TargetKey& key) {
        if (auto found = targets.find(key); found != targets.end()) {
            found->second.last_used = ++usage_clock;
            return found->second;
        }
        const std::uint64_t pixels =
            static_cast<std::uint64_t>(key.source_width) * key.source_height +
            static_cast<std::uint64_t>(key.target_width) * key.target_height * 2U;
        while (!targets.empty() && (targets.size() >= maximum_retained_sets ||
                                    retained_pixels + pixels > maximum_retained_pixels)) {
            const auto oldest = std::ranges::min_element(
                targets, {}, [](const auto& entry) { return entry.second.last_used; });
            retained_pixels -= oldest->second.pixels;
            targets.erase(oldest);
        }
        TargetSet created{
            create_target(key.source_width, key.source_height, false),
            create_target(key.target_width, key.target_height, true),
            create_target(key.target_width, key.target_height, true),
            ++usage_clock,
            pixels,
        };
        retained_pixels += pixels;
        return targets.emplace(key, std::move(created)).first->second;
    }

    void bind_constants(const std::uint32_t input_width, const std::uint32_t input_height,
                        const float direction_x, const float direction_y, const float radius,
                        const UvRect source_uv) const {
        const std::array<float, 12U> values{
            1.0F / static_cast<float>(input_width),
            1.0F / static_cast<float>(input_height),
            direction_x,
            direction_y,
            radius,
            0.0F,
            0.0F,
            0.0F,
            source_uv.u,
            source_uv.v,
            source_uv.width,
            source_uv.height,
        };
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(context->Map(constants.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                        "blur constant mapping");
        std::memcpy(mapped.pData, values.data(), sizeof(values));
        context->Unmap(constants.Get(), 0U);
        ID3D11Buffer* const buffer = constants.Get();
        context->PSSetConstantBuffers(0U, 1U, &buffer);
    }

    void pass(ID3D11RenderTargetView* const output, ID3D11ShaderResourceView* const input,
              const PixelRect output_rect, const PixelRect scissor, const std::uint32_t input_width,
              const std::uint32_t input_height, const float direction_x, const float direction_y,
              const float radius, const UvRect source_uv = {}) const {
        context->OMSetRenderTargets(1U, &output, nullptr);
        const D3D11_VIEWPORT output_viewport = viewport(output_rect);
        context->RSSetViewports(1U, &output_viewport);
        const D3D11_RECT output_scissor = d3d_rect(scissor);
        context->RSSetScissorRects(1U, &output_scissor);
        bind_constants(input_width, input_height, direction_x, direction_y, radius, source_uv);
        context->PSSetShaderResources(0U, 1U, &input);
        context->Draw(3U, 0U);
        ID3D11ShaderResourceView* const none = nullptr;
        context->PSSetShaderResources(0U, 1U, &none);
    }

    [[nodiscard]] BlurPassTelemetry
    execute(const BlurBatch& batch, ID3D11Texture2D* const back_buffer,
            ID3D11RenderTargetView* const output, const std::uint32_t framebuffer_width,
            const std::uint32_t framebuffer_height, const double logical_width,
            const double logical_height) {
        const std::optional<BlurPlan> blur =
            plan(batch, framebuffer_width, framebuffer_height, logical_width, logical_height);
        if (!blur.has_value())
            return {};
        if (back_buffer == nullptr || output == nullptr) {
            throw std::invalid_argument("D3D11 blur requires live source and destination targets");
        }
        if (framebuffer_width != width || framebuffer_height != height) {
            resize(framebuffer_width, framebuffer_height, format);
        }
        TargetSet& target = acquire(blur->targets);

        context->OMSetRenderTargets(0U, nullptr, nullptr);
        const D3D11_BOX source_box{
            blur->source.x, blur->source.y, 0U, blur->source.right(), blur->source.bottom(), 1U,
        };
        context->CopySubresourceRegion(target.source.texture.Get(), 0U, 0U, 0U, 0U, back_buffer, 0U,
                                       &source_box);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex_shader.Get(), nullptr, 0U);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0U);
        context->RSSetState(rasterizer.Get());
        context->OMSetBlendState(nullptr, nullptr, 0xffffffffU);
        ID3D11SamplerState* const sampler_value = sampler.Get();
        context->PSSetSamplers(0U, 1U, &sampler_value);

        const PixelRect whole_target{
            0U,
            0U,
            blur->targets.target_width,
            blur->targets.target_height,
        };
        pass(target.read.target.Get(), target.source.view.Get(), whole_target, whole_target,
             blur->targets.source_width, blur->targets.source_height, 0.0F, 0.0F, 0.0F);
        pass(target.write.target.Get(), target.read.view.Get(), whole_target, whole_target,
             blur->targets.target_width, blur->targets.target_height, 1.0F, 0.0F, blur->radius);
        pass(target.read.target.Get(), target.write.view.Get(), whole_target, whole_target,
             blur->targets.target_width, blur->targets.target_height, 0.0F, 1.0F, blur->radius);
        const UvRect effect_uv{
            static_cast<float>(blur->scaled_effect.x) / blur->targets.target_width,
            static_cast<float>(blur->scaled_effect.y) / blur->targets.target_height,
            static_cast<float>(blur->scaled_effect.width) / blur->targets.target_width,
            static_cast<float>(blur->scaled_effect.height) / blur->targets.target_height,
        };
        pass(output, target.read.view.Get(), blur->effect, blur->effect, blur->targets.target_width,
             blur->targets.target_height, 0.0F, 0.0F, 0.0F, effect_uv);
        return BlurPassTelemetry{
            4U,
            blur->targets.target_width,
            blur->targets.target_height,
            0U,
        };
    }

    static constexpr std::size_t maximum_retained_sets = 8U;
    static constexpr std::uint64_t maximum_retained_pixels = 16U * 1'024U * 1'024U;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    std::map<TargetKey, TargetSet> targets;
    std::uint64_t retained_pixels = 0U;
    std::uint64_t usage_clock = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
};

BlurPass::BlurPass(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : impl_(std::make_unique<Impl>(device, context)) {}
BlurPass::~BlurPass() = default;

void BlurPass::resize(const std::uint32_t width, const std::uint32_t height,
                      const DXGI_FORMAT format) {
    if ((width == 0U) != (height == 0U)) {
        throw std::invalid_argument("D3D11 blur target dimensions must both be empty or positive");
    }
    if ((width == 0U) != (format == DXGI_FORMAT_UNKNOWN)) {
        throw std::invalid_argument("D3D11 blur target format does not match its dimensions");
    }
    impl_->resize(width, height, format);
}

BlurPassTelemetry BlurPass::execute(const BlurBatch& batch, ID3D11Texture2D* const back_buffer,
                                    ID3D11RenderTargetView* const target,
                                    const std::uint32_t framebuffer_width,
                                    const std::uint32_t framebuffer_height,
                                    const double logical_width, const double logical_height) {
    const auto started = std::chrono::steady_clock::now();
    BlurPassTelemetry telemetry = impl_->execute(batch, back_buffer, target, framebuffer_width,
                                                 framebuffer_height, logical_width, logical_height);
    telemetry.nanos =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started)
                                       .count());
    return telemetry;
}

} // namespace strata::d3d11
