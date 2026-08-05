#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "effect_pass.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include "blur_pass.hpp"
#include "blur_shaders.hpp"
#include "clip_mask.hpp"

namespace strata::d3d11 {
namespace {

using Microsoft::WRL::ComPtr;

void require_hresult(const HRESULT result, const char* const operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed");
}

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(const std::string_view source,
                                              const char* const entry, const char* const target) {
    ComPtr<ID3DBlob> bytecode;
    ComPtr<ID3DBlob> errors;
    const HRESULT result =
        D3DCompile(source.data(), source.size(), "strata.d3d11.generated-effect.hlsl", nullptr,
                   nullptr, entry, target, D3DCOMPILE_ENABLE_STRICTNESS, 0U, &bytecode, &errors);
    if (FAILED(result)) {
        const std::string detail =
            errors != nullptr ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                            errors->GetBufferSize())
                              : std::string("unknown compiler error");
        throw std::runtime_error("D3D11 effect shader compilation failed: " + detail);
    }
    return bytecode;
}

constexpr std::string_view effect_prelude = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float2 effectPadding;
};

Texture2D EffectSource : register(t0);
Texture2D EffectBackdrop : register(t1);
SamplerState EffectSampler : register(s0);

struct PixelInput {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

struct EffectInput {
    float2 uv;
    float2 localUv;
    float2 pixel;
    float2 logicalPixel;
};

float effectFloat(uint slot) {
    uint vectorIndex = min(slot / 4, 3);
    uint component = slot % 4;
    return effectParameters[vectorIndex][component];
}
float2 effectFloat2(uint slot) {
    return float2(effectFloat(slot), effectFloat(slot + 1));
}
float4 effectFloat4(uint slot) {
    return float4(
        effectFloat(slot), effectFloat(slot + 1),
        effectFloat(slot + 2), effectFloat(slot + 3)
    );
}
float4 effectColor(uint slot) { return effectFloat4(slot); }
float effectTime() { return effectTimeValue; }
float effectOpacity() { return effectOpacityValue; }
float4 effectUnpremultiply(float4 color) {
    color.rgb = color.a > 0.00001 ? color.rgb / color.a : 0.0;
    return color;
}
float4 sampleEffectSource(float2 uv) {
    return effectUnpremultiply(
        EffectSource.Sample(EffectSampler, clamp(uv, 0.0, 1.0))
    );
}
float4 sampleEffectBackdrop(float2 uv) {
    return effectUnpremultiply(
        EffectBackdrop.Sample(EffectSampler, clamp(uv, 0.0, 1.0))
    );
}

float effectCornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom
        ? (right ? effectRadii.z : effectRadii.w)
        : (right ? effectRadii.y : effectRadii.x);
}

float effectDistance(float2 pixel) {
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = pixel - (effectBounds.xy + halfSize);
    float radius = min(effectCornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
}

float effectMask(float2 pixel) {
    return 1.0 - smoothstep(-1.0, 1.0, effectDistance(pixel));
}
)hlsl";

constexpr std::string_view effect_entry = R"hlsl(
float4 main(PixelInput pixelInput) : SV_TARGET {
    EffectInput input;
    input.uv = pixelInput.uv;
    input.pixel = pixelInput.position.xy;
    input.logicalPixel = input.pixel * effectLogicalSize / max(effectTargetSize, 1.0);
    input.localUv = (input.pixel - effectBounds.xy) / max(effectBounds.zw, 1.0);
    float4 color = effect(input);
    color.rgb *= color.a;
    return color;
}
)hlsl";

constexpr std::string_view composite_pixel = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float2 effectPadding;
};
Texture2D EffectSource : register(t0);
SamplerState EffectSampler : register(s0);
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float cornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom ? (right ? effectRadii.z : effectRadii.w)
                  : (right ? effectRadii.y : effectRadii.x);
}
float mask(float2 pixel) {
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = pixel - (effectBounds.xy + halfSize);
    float radius = min(cornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    float distance = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    return 1.0 - smoothstep(-1.0, 1.0, distance);
}
float4 main(PixelInput input) : SV_TARGET {
    float4 color = EffectSource.Sample(EffectSampler, clamp(input.uv, 0.0, 1.0));
    color *= mask(input.position.xy) * effectOpacityValue;
    float2 logicalPixel =
        input.position.xy * effectLogicalSize / max(effectTargetSize, 1.0);
    return strataApplyRoundedClips(color, logicalPixel);
}
)hlsl";

constexpr std::string_view cached_composite_pixel = R"hlsl(
cbuffer EffectData : register(b0) {
    float2 effectLogicalSize;
    float2 effectTargetSize;
    float4 effectBounds;
    float4 effectRadii;
    float4 effectParameters[4];
    float effectOpacityValue;
    float effectTimeValue;
    float2 effectCacheOrigin;
};
Texture2D EffectSource : register(t0);
SamplerState EffectSampler : register(s0);
struct PixelInput { float4 position : SV_POSITION; float2 uv : TEXCOORD0; };
float cornerRadius(float2 centered) {
    bool right = centered.x >= 0.0;
    bool bottom = centered.y >= 0.0;
    return bottom ? (right ? effectRadii.z : effectRadii.w)
                  : (right ? effectRadii.y : effectRadii.x);
}
float mask(float2 pixel) {
    float2 halfSize = effectBounds.zw * 0.5;
    float2 centered = pixel - (effectBounds.xy + halfSize);
    float radius = min(cornerRadius(centered), min(halfSize.x, halfSize.y));
    float2 q = abs(centered) - halfSize + radius;
    float distance = min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - radius;
    return 1.0 - smoothstep(-1.0, 1.0, distance);
}
float4 main(PixelInput input) : SV_TARGET {
    uint width;
    uint height;
    EffectSource.GetDimensions(width, height);
    float2 uv = (input.position.xy - effectCacheOrigin) /
        max(float2(width, height), 1.0);
    float4 color = EffectSource.Sample(EffectSampler, clamp(uv, 0.0, 1.0));
    color *= mask(input.position.xy) * effectOpacityValue;
    float2 logicalPixel =
        input.position.xy * effectLogicalSize / max(effectTargetSize, 1.0);
    return strataApplyRoundedClips(color, logicalPixel);
}
)hlsl";

struct EffectConstants final {
    float logical_size[2]{};
    float target_size[2]{};
    float bounds[4]{};
    float radii[4]{};
    float parameters[16]{};
    float opacity = 1.0F;
    float time = 0.0F;
    float padding[2]{};
};
static_assert(sizeof(EffectConstants) % 16U == 0U);

[[nodiscard]] D3D11_RECT effect_scissor(const host::EffectBatch& effect, const std::uint32_t width,
                                        const std::uint32_t height, const double logical_width,
                                        const double logical_height) noexcept {
    const double scale_x = logical_width > 0.0 ? width / logical_width : 1.0;
    const double scale_y = logical_height > 0.0 ? height / logical_height : 1.0;
    LONG left = static_cast<LONG>(
        std::clamp(std::floor(effect.x * scale_x), 0.0, static_cast<double>(width)));
    LONG top = static_cast<LONG>(
        std::clamp(std::floor(effect.y * scale_y), 0.0, static_cast<double>(height)));
    LONG right = static_cast<LONG>(std::clamp(std::ceil((effect.x + effect.width) * scale_x), 0.0,
                                              static_cast<double>(width)));
    LONG bottom = static_cast<LONG>(std::clamp(std::ceil((effect.y + effect.height) * scale_y), 0.0,
                                               static_cast<double>(height)));
    const std::uint64_t clip_right = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(effect.scissor.x) + effect.scissor.width, width);
    const std::uint64_t clip_bottom = std::min<std::uint64_t>(
        static_cast<std::uint64_t>(effect.scissor.y) + effect.scissor.height, height);
    left = std::max(left, static_cast<LONG>(std::min(effect.scissor.x, width)));
    top = std::max(top, static_cast<LONG>(std::min(effect.scissor.y, height)));
    right = std::min(right, static_cast<LONG>(clip_right));
    bottom = std::min(bottom, static_cast<LONG>(clip_bottom));
    return D3D11_RECT{left, top, right, bottom};
}

} // namespace

struct EffectPassRenderer::Impl final {
    struct Target final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> target;
        ComPtr<ID3D11ShaderResourceView> view;
    };
    struct Workspace final {
        Target capture;
        Target backdrop;
        Target ping;
        Target pong;
    };
    struct Pass final {
        std::uint32_t kind = 0U;
        double radius = 0.0;
        std::uint32_t downsample = 1U;
        std::uint32_t radius_parameter = UINT32_MAX;
        std::uint32_t downsample_parameter = UINT32_MAX;
        std::string shader_source;
    };
    struct ShaderProgram final {
        std::shared_future<ComPtr<ID3DBlob>> pending;
        ComPtr<ID3D11PixelShader> shader;
    };
    struct CacheKey final {
        std::string layer;
        std::uint32_t source_order = 0U;
        bool content = false;
        [[nodiscard]] friend bool operator<(const CacheKey& left, const CacheKey& right) noexcept {
            if (left.layer != right.layer)
                return left.layer < right.layer;
            if (left.source_order != right.source_order) {
                return left.source_order < right.source_order;
            }
            return left.content < right.content;
        }
    };
    struct CachedSample final {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
        LONG left = 0;
        LONG top = 0;
        double sampled_seconds = 0.0;
        std::uint64_t geometry_epoch = 0U;
        host::EffectBatch effect;
    };

    Impl(
        ID3D11Device* const source_device,
        ID3D11DeviceContext* const source_context,
        const bool asynchronous_shader_compilation
    )
        : device(source_device),
          context(source_context),
          blur(source_device, source_context),
          asynchronous_shader_compilation(asynchronous_shader_compilation) {
        if (device == nullptr || context == nullptr) {
            throw std::invalid_argument("D3D11 effects require a device and context");
        }
        const ComPtr<ID3DBlob> vertex_code = compile_shader(blur_shaders::vertex, "main", "vs_5_0");
        rounded_clips = std::make_unique<RoundedClipBuffer>(device, context);
        require_hresult(device->CreateVertexShader(vertex_code->GetBufferPointer(),
                                                   vertex_code->GetBufferSize(), nullptr, &vertex),
                        "D3D11 effect vertex shader creation");
        const ComPtr<ID3DBlob> composite_code = compile_shader(
            std::string(rounded_clip_hlsl) + std::string(composite_pixel),
            "main",
            "ps_5_0"
        );
        require_hresult(device->CreatePixelShader(composite_code->GetBufferPointer(),
                                                  composite_code->GetBufferSize(), nullptr,
                                                  &composite),
                        "D3D11 effect composite shader creation");
        const ComPtr<ID3DBlob> cached_composite_code =
            compile_shader(
                std::string(rounded_clip_hlsl) + std::string(cached_composite_pixel),
                "main",
                "ps_5_0"
            );
        require_hresult(device->CreatePixelShader(cached_composite_code->GetBufferPointer(),
                                                  cached_composite_code->GetBufferSize(), nullptr,
                                                  &cached_composite),
                        "D3D11 cached effect composite shader creation");
        D3D11_BUFFER_DESC constants_descriptor{};
        constants_descriptor.ByteWidth = sizeof(EffectConstants);
        constants_descriptor.Usage = D3D11_USAGE_DYNAMIC;
        constants_descriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constants_descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        require_hresult(device->CreateBuffer(&constants_descriptor, nullptr, &constants),
                        "D3D11 effect constant buffer creation");
        D3D11_SAMPLER_DESC sampler_descriptor{};
        sampler_descriptor.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler_descriptor.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_descriptor.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_descriptor.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler_descriptor.MaxLOD = D3D11_FLOAT32_MAX;
        require_hresult(device->CreateSamplerState(&sampler_descriptor, &sampler),
                        "D3D11 effect sampler creation");
        D3D11_RASTERIZER_DESC rasterizer_descriptor{};
        rasterizer_descriptor.FillMode = D3D11_FILL_SOLID;
        rasterizer_descriptor.CullMode = D3D11_CULL_NONE;
        rasterizer_descriptor.ScissorEnable = TRUE;
        rasterizer_descriptor.DepthClipEnable = TRUE;
        require_hresult(device->CreateRasterizerState(&rasterizer_descriptor, &rasterizer),
                        "D3D11 effect rasterizer creation");
        D3D11_BLEND_DESC replace_descriptor{};
        replace_descriptor.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        require_hresult(device->CreateBlendState(&replace_descriptor, &replace_blend),
                        "D3D11 effect replace blend creation");
        D3D11_BLEND_DESC composite_descriptor = replace_descriptor;
        auto& blend = composite_descriptor.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D11_BLEND_ONE;
        blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = D3D11_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D11_BLEND_ONE;
        blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        require_hresult(device->CreateBlendState(&composite_descriptor, &composite_blend),
                        "D3D11 effect composite blend creation");
    }

    void complete_shader_programs() {
        bool completed = false;
        for (auto current = shader_programs.begin();
             current != shader_programs.end();) {
            const bool referenced = std::ranges::any_of(
                programs,
                [&current](const auto& declaration) {
                    return std::ranges::any_of(
                        declaration.second,
                        [&current](const Pass& pass) {
                            return pass.shader_source == current->first;
                        }
                    );
                }
            );
            if (!referenced && current->second.pending.valid() &&
                current->second.pending.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
                ++current;
                continue;
            }
            if (!referenced) {
                current = shader_programs.erase(current);
                continue;
            }
            ShaderProgram& program = current->second;
            if (program.shader != nullptr || !program.pending.valid() ||
                program.pending.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
                ++current;
                continue;
            }
            const ComPtr<ID3DBlob> bytecode = program.pending.get();
            require_hresult(
                device->CreatePixelShader(
                    bytecode->GetBufferPointer(),
                    bytecode->GetBufferSize(),
                    nullptr,
                    &program.shader
                ),
                "D3D11 authored effect shader creation"
            );
            program.pending = {};
            completed = true;
            ++current;
        }
        // Samples produced while an authored pass was still compiling intentionally skipped that
        // pass. They are no longer valid once its shader becomes available, regardless of the
        // effect's authored refresh rate.
        if (completed) cached_samples.clear();
    }

    void resize(const std::uint32_t next_width, const std::uint32_t next_height,
                const DXGI_FORMAT next_format) {
        if (next_width == width && next_height == height && next_format == format)
            return;
        width = next_width;
        height = next_height;
        format = next_format;
        content_targets.clear();
        processing.reset();
        cached_samples.clear();
        layer_epochs.clear();
        blur.resize(width, height, format);
    }

    void begin_layer(
        const std::string_view layer_id,
        const std::uint64_t geometry_epoch,
        const bool dirty_all,
        const std::span<const host::GeometryDirtyRegion> dirty_regions
    ) {
        const std::string layer(layer_id);
        const auto found = layer_epochs.find(layer);
        if (found != layer_epochs.end() && found->second == geometry_epoch)
            return;
        layer_epochs.insert_or_assign(layer, geometry_epoch);
        const auto intersects = [](const host::EffectBatch& effect,
                                   const host::GeometryDirtyRegion& dirty) {
            const double effect_right = effect.x + effect.width;
            const double effect_bottom = effect.y + effect.height;
            const double dirty_right = dirty.x + dirty.width;
            const double dirty_bottom = dirty.y + dirty.height;
            return dirty.x <= effect_right && dirty_right >= effect.x &&
                dirty.y <= effect_bottom && dirty_bottom >= effect.y;
        };
        for (auto& [key, sample] : cached_samples) {
            if (key.layer != layer) continue;
            const bool dirty = dirty_all || std::ranges::any_of(
                dirty_regions,
                [&sample, &intersects](const host::GeometryDirtyRegion& region) {
                    return intersects(sample.effect, region);
                }
            );
            if (!dirty) sample.geometry_epoch = geometry_epoch;
        }
    }

    [[nodiscard]] Target target() const {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = width;
        descriptor.Height = height;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = format;
        descriptor.SampleDesc.Count = 1U;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        Target result;
        require_hresult(device->CreateTexture2D(&descriptor, nullptr, &result.texture),
                        "D3D11 effect target texture creation");
        require_hresult(
            device->CreateRenderTargetView(result.texture.Get(), nullptr, &result.target),
            "D3D11 effect target view creation");
        require_hresult(
            device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view),
            "D3D11 effect source view creation");
        return result;
    }

    [[nodiscard]] CachedSample cached_sample(const std::uint32_t sample_width,
                                             const std::uint32_t sample_height, const LONG left,
                                             const LONG top) const {
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = sample_width;
        descriptor.Height = sample_height;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = format;
        descriptor.SampleDesc.Count = 1U;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CachedSample result;
        result.width = sample_width;
        result.height = sample_height;
        result.left = left;
        result.top = top;
        require_hresult(device->CreateTexture2D(&descriptor, nullptr, &result.texture),
                        "D3D11 cached effect texture creation");
        require_hresult(
            device->CreateShaderResourceView(result.texture.Get(), nullptr, &result.view),
            "D3D11 cached effect view creation");
        return result;
    }

    [[nodiscard]] Workspace& processing_workspace(const std::size_t depth) {
        if (width == 0U || height == 0U) {
            throw std::logic_error("D3D11 effect workspace has no framebuffer size");
        }
        if (depth > host::maximum_content_effect_depth) {
            throw std::length_error("D3D11 content effect nesting exceeds the packet limit");
        }
        if (!processing.has_value()) {
            processing.emplace(Workspace{target(), target(), target(), target()});
        }
        return *processing;
    }

    [[nodiscard]] Target& content_target(const std::size_t depth) {
        if (width == 0U || height == 0U) {
            throw std::logic_error("D3D11 content target has no framebuffer size");
        }
        if (depth >= host::maximum_content_effect_depth) {
            throw std::length_error("D3D11 content effect nesting exceeds the packet limit");
        }
        while (content_targets.size() <= depth) {
            content_targets.push_back(target());
        }
        return content_targets[depth];
    }

    void upload_constants(const host::EffectBatch& effect, const double logical_width,
                          const double logical_height, const double frame_seconds,
                          const float cache_origin_x = 0.0F,
                          const float cache_origin_y = 0.0F) const {
        const double scale_x = logical_width > 0.0 ? width / logical_width : 1.0;
        const double scale_y = logical_height > 0.0 ? height / logical_height : 1.0;
        EffectConstants value;
        value.logical_size[0] = static_cast<float>(logical_width);
        value.logical_size[1] = static_cast<float>(logical_height);
        value.target_size[0] = static_cast<float>(width);
        value.target_size[1] = static_cast<float>(height);
        value.bounds[0] = static_cast<float>(effect.x * scale_x);
        value.bounds[1] = static_cast<float>(effect.y * scale_y);
        value.bounds[2] = static_cast<float>(effect.width * scale_x);
        value.bounds[3] = static_cast<float>(effect.height * scale_y);
        const double radius_scale = std::sqrt(std::abs(scale_x * scale_y));
        for (std::size_t index = 0U; index < effect.radii.size(); ++index) {
            value.radii[index] = static_cast<float>(effect.radii[index] * radius_scale);
        }
        for (std::uint32_t index = 0U; index < effect.parameter_count; ++index) {
            value.parameters[index] = static_cast<float>(effect.parameters[index]);
        }
        value.opacity = static_cast<float>(effect.opacity);
        value.time = static_cast<float>(frame_seconds);
        value.padding[0] = cache_origin_x;
        value.padding[1] = cache_origin_y;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(context->Map(constants.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                        "D3D11 effect constant mapping");
        std::memcpy(mapped.pData, &value, sizeof(value));
        context->Unmap(constants.Get(), 0U);
    }

    void draw_cached(const CachedSample& sample, ID3D11RenderTargetView* const destination,
                     const host::EffectBatch& effect, const double logical_width,
                     const double logical_height, const double frame_seconds) const {
        upload_constants(effect, logical_width, logical_height, frame_seconds,
                         static_cast<float>(sample.left), static_cast<float>(sample.top));
        ID3D11ShaderResourceView* resources[]{sample.view.Get(), nullptr};
        ID3D11Buffer* constant_buffers[]{constants.Get()};
        ID3D11SamplerState* samplers[]{sampler.Get()};
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F,
        };
        const D3D11_RECT clip =
            effect_scissor(effect, width, height, logical_width, logical_height);
        context->OMSetRenderTargets(1U, &destination, nullptr);
        context->OMSetBlendState(composite_blend.Get(), nullptr, UINT32_MAX);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1U, &viewport);
        context->RSSetScissorRects(1U, &clip);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0U);
        context->PSSetShader(cached_composite.Get(), nullptr, 0U);
        rounded_clips->bind(
            effect.rounded_clips,
            RoundedClipMode::premultiplied_alpha
        );
        context->PSSetConstantBuffers(0U, 1U, constant_buffers);
        context->PSSetSamplers(0U, 1U, samplers);
        context->PSSetShaderResources(0U, 2U, resources);
        context->Draw(3U, 0U);
        ID3D11ShaderResourceView* cleared[]{nullptr, nullptr};
        context->PSSetShaderResources(0U, 2U, cleared);
    }

    void draw_pass(ID3D11PixelShader* const shader, ID3D11ShaderResourceView* const source,
                   ID3D11ShaderResourceView* const backdrop,
                   ID3D11RenderTargetView* const destination, const host::EffectBatch& effect,
                   const double logical_width, const double logical_height,
                   const double frame_seconds, ID3D11BlendState* const blend) const {
        upload_constants(effect, logical_width, logical_height, frame_seconds);
        ID3D11ShaderResourceView* resources[]{source, backdrop != nullptr ? backdrop : source};
        ID3D11Buffer* constant_buffers[]{constants.Get()};
        ID3D11SamplerState* samplers[]{sampler.Get()};
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F,
        };
        const D3D11_RECT clip =
            effect_scissor(effect, width, height, logical_width, logical_height);
        context->OMSetRenderTargets(1U, &destination, nullptr);
        context->OMSetBlendState(blend, nullptr, UINT32_MAX);
        context->RSSetState(rasterizer.Get());
        context->RSSetViewports(1U, &viewport);
        context->RSSetScissorRects(1U, &clip);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0U);
        context->PSSetShader(shader, nullptr, 0U);
        rounded_clips->bind(
            effect.rounded_clips,
            RoundedClipMode::premultiplied_alpha
        );
        context->PSSetConstantBuffers(0U, 1U, constant_buffers);
        context->PSSetSamplers(0U, 1U, samplers);
        context->PSSetShaderResources(0U, 2U, resources);
        context->Draw(3U, 0U);
        ID3D11ShaderResourceView* cleared[]{nullptr, nullptr};
        context->PSSetShaderResources(0U, 2U, cleared);
    }

    [[nodiscard]] double parameter(const host::EffectBatch& effect, const std::uint32_t slot,
                                   const double fallback) const noexcept {
        return slot < effect.parameter_count ? effect.parameters[slot] : fallback;
    }

    [[nodiscard]] EffectPassTelemetry
    apply(const std::string_view layer_id, const bool content, const std::size_t depth,
          const host::EffectBatch& effect, ID3D11Texture2D* const source_texture,
          ID3D11RenderTargetView* const source_target, ID3D11Texture2D* const backdrop_texture,
          ID3D11RenderTargetView* const destination, const double logical_width,
          const double logical_height, const double frame_seconds) {
        const auto started = std::chrono::steady_clock::now();
        complete_shader_programs();
        const D3D11_RECT clip =
            effect_scissor(effect, width, height, logical_width, logical_height);
        const std::uint32_t sample_width =
            static_cast<std::uint32_t>(std::max<LONG>(0, clip.right - clip.left));
        const std::uint32_t sample_height =
            static_cast<std::uint32_t>(std::max<LONG>(0, clip.bottom - clip.top));
        if (sample_width == 0U || sample_height == 0U)
            return {};
        const bool rate_limited = effect.refresh_rate > 0.0;
        const CacheKey cache_key{
            std::string(layer_id),
            effect.source_order,
            content,
        };
        auto cached = cached_samples.find(cache_key);
        if (!rate_limited && cached != cached_samples.end()) {
            cached_samples.erase(cached);
            cached = cached_samples.end();
        }
        const bool dimensions_match =
            cached != cached_samples.end() && cached->second.width == sample_width &&
            cached->second.height == sample_height && cached->second.left == clip.left &&
            cached->second.top == clip.top;
        const bool signature_matches = dimensions_match && cached->second.effect == effect;
        const auto layer_epoch = layer_epochs.find(std::string(layer_id));
        const bool geometry_matches =
            cached != cached_samples.end() && layer_epoch != layer_epochs.end() &&
            cached->second.geometry_epoch == layer_epoch->second;
        const double interval = rate_limited ? 1.0 / effect.refresh_rate : 0.0;
        const bool refresh_due =
            !rate_limited || !signature_matches || !geometry_matches ||
            frame_seconds < cached->second.sampled_seconds ||
            frame_seconds - cached->second.sampled_seconds + 1.0e-9 >= interval;
        if (!refresh_due) {
            draw_cached(cached->second, destination, effect, logical_width, logical_height,
                        frame_seconds);
            return EffectPassTelemetry{
                1U,
                static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                               std::chrono::steady_clock::now() - started)
                                               .count()),
                width,
                height,
            };
        }
        Workspace& work = processing_workspace(depth);
        context->OMSetRenderTargets(0U, nullptr, nullptr);
        ID3D11ShaderResourceView* cleared[]{nullptr, nullptr};
        context->PSSetShaderResources(0U, 2U, cleared);
        Target* current = &work.capture;
        if (source_texture != current->texture.Get()) {
            context->CopyResource(current->texture.Get(), source_texture);
        }
        context->CopyResource(work.backdrop.texture.Get(), backdrop_texture);
        ID3D11ShaderResourceView* const backdrop = work.backdrop.view.Get();
        EffectPassTelemetry telemetry;
        const auto program = programs.find(effect.effect);
        if (program != programs.end()) {
            for (const Pass& pass : program->second) {
                if (pass.kind == 0U) {
                    const double radius =
                        std::max(0.0, parameter(effect, pass.radius_parameter, pass.radius));
                    const std::uint32_t downsample = static_cast<std::uint32_t>(
                        std::clamp(std::llround(parameter(effect, pass.downsample_parameter,
                                                          static_cast<double>(pass.downsample))),
                                   1LL, 8LL));
                    const host::BlurBatch batch{
                        effect.source_order, effect.scissor, effect.x, effect.y,
                        effect.width,        effect.height,  radius,   downsample,
                    };
                    const BlurPassTelemetry measured =
                        blur.execute(batch, current->texture.Get(), current->target.Get(), width,
                                     height, logical_width, logical_height);
                    telemetry.passes += measured.passes;
                } else if (pass.kind == 1U) {
                    const auto shader = shader_programs.find(pass.shader_source);
                    if (shader == shader_programs.end() ||
                        shader->second.shader == nullptr) {
                        continue;
                    }
                    Target* next = current == &work.ping ? &work.pong : &work.ping;
                    const float clear[4]{};
                    context->ClearRenderTargetView(next->target.Get(), clear);
                    draw_pass(shader->second.shader.Get(), current->view.Get(), backdrop,
                              next->target.Get(),
                              effect, logical_width, logical_height, frame_seconds,
                              replace_blend.Get());
                    current = next;
                    ++telemetry.passes;
                }
            }
        }
        if (rate_limited) {
            context->OMSetRenderTargets(0U, nullptr, nullptr);
            ID3D11ShaderResourceView* unbound[]{nullptr, nullptr};
            context->PSSetShaderResources(0U, 2U, unbound);
            if (!dimensions_match) {
                CachedSample sample =
                    cached_sample(sample_width, sample_height, clip.left, clip.top);
                cached = cached_samples.insert_or_assign(cache_key, std::move(sample)).first;
            }
            const D3D11_BOX source_box{
                static_cast<UINT>(clip.left),  static_cast<UINT>(clip.top),    0U,
                static_cast<UINT>(clip.right), static_cast<UINT>(clip.bottom), 1U,
            };
            context->CopySubresourceRegion(cached->second.texture.Get(), 0U, 0U, 0U, 0U,
                                           current->texture.Get(), 0U, &source_box);
            cached->second.sampled_seconds = frame_seconds;
            cached->second.geometry_epoch =
                layer_epoch != layer_epochs.end() ? layer_epoch->second : 0U;
            cached->second.effect = effect;
            draw_cached(cached->second, destination, effect, logical_width, logical_height,
                        frame_seconds);
        } else {
            draw_pass(composite.Get(), current->view.Get(), backdrop, destination, effect,
                      logical_width, logical_height, frame_seconds, composite_blend.Get());
        }
        ++telemetry.passes;
        telemetry.target_width = width;
        telemetry.target_height = height;
        telemetry.nanos =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - started)
                                           .count());
        static_cast<void>(source_target);
        return telemetry;
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    BlurPass blur;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> composite;
    ComPtr<ID3D11PixelShader> cached_composite;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> replace_blend;
    ComPtr<ID3D11BlendState> composite_blend;
    std::unique_ptr<RoundedClipBuffer> rounded_clips;
    std::vector<Target> content_targets;
    std::optional<Workspace> processing;
    std::map<std::string, std::vector<Pass>, std::less<>> programs;
    std::map<std::string, ShaderProgram, std::less<>> shader_programs;
    std::map<CacheKey, CachedSample> cached_samples;
    std::map<std::string, std::uint64_t, std::less<>> layer_epochs;
    bool asynchronous_shader_compilation = false;
};

EffectPassRenderer::EffectPassRenderer(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    const bool asynchronous_shader_compilation
)
    : impl_(std::make_unique<Impl>(
          device,
          context,
          asynchronous_shader_compilation
      )) {}

EffectPassRenderer::~EffectPassRenderer() = default;

void EffectPassRenderer::resize(const std::uint32_t width, const std::uint32_t height,
                                const DXGI_FORMAT format) {
    if ((width == 0U) != (height == 0U)) {
        throw std::invalid_argument(
            "D3D11 effect target dimensions must both be empty or positive");
    }
    if ((width == 0U || height == 0U) && format != DXGI_FORMAT_UNKNOWN) {
        throw std::invalid_argument("empty D3D11 effect targets require an unknown format");
    }
    if (width != 0U && height != 0U && format == DXGI_FORMAT_UNKNOWN) {
        throw std::invalid_argument("D3D11 effect targets require a concrete texture format");
    }
    impl_->resize(width, height, format);
}

void EffectPassRenderer::invalidate_cache() noexcept {
    impl_->cached_samples.clear();
    impl_->layer_epochs.clear();
}

void EffectPassRenderer::begin_layer(
    const std::string_view layer_id,
    const std::uint64_t geometry_epoch,
    const bool dirty_all,
    const std::span<const host::GeometryDirtyRegion> dirty_regions
) {
    impl_->begin_layer(layer_id, geometry_epoch, dirty_all, dirty_regions);
}

void EffectPassRenderer::release_layer(const std::string_view layer_id) noexcept {
    const auto epoch = impl_->layer_epochs.find(layer_id);
    if (epoch != impl_->layer_epochs.end())
        impl_->layer_epochs.erase(epoch);
    std::erase_if(impl_->cached_samples,
                  [layer_id](const auto& entry) { return entry.first.layer == layer_id; });
}

void EffectPassRenderer::declare_pass(const std::string_view effect_id, const std::uint32_t index,
                                      const std::uint32_t kind, const double radius,
                                      const std::uint32_t downsample,
                                      const std::uint32_t radius_parameter,
                                      const std::uint32_t downsample_parameter,
                                      const std::string_view hlsl_source) {
    if (effect_id.empty())
        throw std::invalid_argument("effect id must not be empty");
    Impl::Pass pass;
    pass.kind = kind;
    pass.radius = radius;
    pass.downsample = std::clamp(downsample, 1U, 8U);
    pass.radius_parameter = radius_parameter;
    pass.downsample_parameter = downsample_parameter;
    if (kind == 1U && !hlsl_source.empty()) {
        pass.shader_source = std::string(effect_prelude) +
            std::string(rounded_clip_hlsl) + std::string(hlsl_source) +
            std::string(effect_entry);
        auto [program, inserted] =
            impl_->shader_programs.try_emplace(pass.shader_source);
        if (inserted) {
            if (impl_->asynchronous_shader_compilation) {
                const std::string source = pass.shader_source;
                program->second.pending = std::async(
                    std::launch::async,
                    [source] {
                        return compile_shader(source, "main", "ps_5_0");
                    }
                ).share();
            } else {
                const ComPtr<ID3DBlob> bytecode =
                    compile_shader(pass.shader_source, "main", "ps_5_0");
                require_hresult(
                    impl_->device->CreatePixelShader(
                        bytecode->GetBufferPointer(),
                        bytecode->GetBufferSize(),
                        nullptr,
                        &program->second.shader
                    ),
                    "D3D11 authored effect shader creation"
                );
            }
        }
    }
    std::vector<Impl::Pass>& program = impl_->programs[std::string(effect_id)];
    if (program.size() <= index)
        program.resize(static_cast<std::size_t>(index) + 1U);
    program[index] = std::move(pass);
    impl_->cached_samples.clear();
}

ID3D11RenderTargetView* EffectPassRenderer::begin_content(const std::size_t depth) {
    Impl::Target& target = impl_->content_target(depth);
    const float clear[4]{};
    impl_->context->ClearRenderTargetView(target.target.Get(), clear);
    return target.target.Get();
}

ID3D11Texture2D* EffectPassRenderer::content_texture(const std::size_t depth) {
    return impl_->content_target(depth).texture.Get();
}

EffectPassTelemetry EffectPassRenderer::apply_backdrop(
    const std::string_view layer_id, const std::size_t depth, const host::EffectBatch& effect,
    ID3D11Texture2D* const target_texture, ID3D11RenderTargetView* const target,
    const double logical_width, const double logical_height, const double frame_seconds) {
    return impl_->apply(layer_id, false, depth, effect, target_texture, target, target_texture,
                        target, logical_width, logical_height, frame_seconds);
}

EffectPassTelemetry EffectPassRenderer::finish_content(
    const std::string_view layer_id, const std::size_t depth, const host::EffectBatch& effect,
    ID3D11Texture2D* const backdrop_texture, ID3D11RenderTargetView* const destination,
    const double logical_width, const double logical_height, const double frame_seconds) {
    Impl::Target& content = impl_->content_target(depth);
    return impl_->apply(layer_id, true, depth, effect, content.texture.Get(), content.target.Get(),
                        backdrop_texture, destination, logical_width, logical_height,
                        frame_seconds);
}

} // namespace strata::d3d11
