#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <strata/render_packet.hpp>

namespace strata::d3d11 {

static_assert(host::maximum_rounded_clip_depth == 16U);

inline constexpr std::string_view rounded_clip_hlsl = R"hlsl(
#define STRATA_MAX_ROUNDED_CLIPS 16
cbuffer RoundedClipData : register(b1) {
    float4 roundedClipBounds[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipRadii[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipInverseX[STRATA_MAX_ROUNDED_CLIPS];
    float4 roundedClipInverseY[STRATA_MAX_ROUNDED_CLIPS];
    uint roundedClipCount;
    uint roundedClipMode;
    float2 roundedClipPadding;
};

float roundedClipBoxSdf(float2 p, float2 halfSize, float4 radii) {
    float2 quadrant = step(float2(0.0, 0.0), p);
    float radius = lerp(
        lerp(radii.x, radii.w, quadrant.y),
        lerp(radii.y, radii.z, quadrant.y),
        quadrant.x
    );
    radius = min(max(radius, 0.0), min(halfSize.x, halfSize.y));
    float2 q = abs(p) - halfSize + radius;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radius;
}

float strataRoundedClipCoverage(float2 logicalPixel) {
    float coverage = 1.0;
    [loop]
    for (uint index = 0; index < roundedClipCount; ++index) {
        float2 localPixel = float2(
            dot(roundedClipInverseX[index].xy, logicalPixel) +
                roundedClipInverseX[index].z,
            dot(roundedClipInverseY[index].xy, logicalPixel) +
                roundedClipInverseY[index].z
        );
        float2 halfSize = roundedClipBounds[index].zw * 0.5;
        float2 center = roundedClipBounds[index].xy + halfSize;
        float distance = roundedClipBoxSdf(
            localPixel - center,
            halfSize,
            roundedClipRadii[index]
        );
        float softness = max(fwidth(distance), 0.0001);
        coverage *= 1.0 - smoothstep(-softness, softness, distance);
    }
    return coverage;
}

float4 strataApplyRoundedClips(float4 color, float2 logicalPixel) {
    float coverage = strataRoundedClipCoverage(logicalPixel);
    if (roundedClipMode == 3) {
        // Multiply blending intentionally ignores source alpha for covered pixels, but alpha still
        // owns the primitive/texture silhouette and must reject transparent parts of its quad.
        clip(color.a - 0.000001);
        color.rgb *= coverage;
        color.a = coverage;
    } else if (roundedClipMode == 2) {
        clip(coverage - 0.5);
    } else if (roundedClipMode == 1) {
        color *= coverage;
    } else {
        color.a *= coverage;
    }
    return color;
}
)hlsl";

enum class RoundedClipMode : std::uint32_t {
    straight_alpha = 0U,
    premultiplied_alpha = 1U,
    hard = 2U,
    multiply = 3U,
};

struct RoundedClipConstants final {
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> bounds{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> radii{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> inverse_x{};
    std::array<std::array<float, 4U>, host::maximum_rounded_clip_depth> inverse_y{};
    std::uint32_t count = 0U;
    std::uint32_t mode = 0U;
    std::array<float, 2U> padding{};
};
static_assert(sizeof(RoundedClipConstants) % 16U == 0U);

class RoundedClipBuffer final {
  public:
    RoundedClipBuffer(ID3D11Device* const device, ID3D11DeviceContext* const context)
        : context_(context) {
        if (device == nullptr || context == nullptr) {
            throw std::invalid_argument("D3D11 rounded clip buffer requires a device and context");
        }
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = static_cast<UINT>(sizeof(RoundedClipConstants));
        descriptor.Usage = D3D11_USAGE_DYNAMIC;
        descriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        const HRESULT status = device->CreateBuffer(&descriptor, nullptr, &buffer_);
        if (FAILED(status)) {
            throw std::runtime_error(
                "D3D11 rounded clip buffer creation failed with HRESULT " +
                std::to_string(status)
            );
        }
    }

    void bind(const std::span<const host::RoundedClip> clips, const RoundedClipMode mode) const {
        if (clips.size() > host::maximum_rounded_clip_depth) {
            throw std::length_error("D3D11 rounded clip stack exceeds the packet limit");
        }
        RoundedClipConstants constants;
        constants.count = static_cast<std::uint32_t>(clips.size());
        constants.mode = static_cast<std::uint32_t>(mode);
        for (std::size_t index = 0U; index < clips.size(); ++index) {
            const host::RoundedClip& clip = clips[index];
            constants.bounds[index] = {
                static_cast<float>(clip.x),
                static_cast<float>(clip.y),
                static_cast<float>(clip.width),
                static_cast<float>(clip.height),
            };
            for (std::size_t corner = 0U; corner < 4U; ++corner) {
                constants.radii[index][corner] = static_cast<float>(clip.radii[corner]);
            }
            constants.inverse_x[index] = {
                static_cast<float>(clip.inverse_transform[0U]),
                static_cast<float>(clip.inverse_transform[1U]),
                static_cast<float>(clip.inverse_transform[2U]),
                0.0F,
            };
            constants.inverse_y[index] = {
                static_cast<float>(clip.inverse_transform[3U]),
                static_cast<float>(clip.inverse_transform[4U]),
                static_cast<float>(clip.inverse_transform[5U]),
                0.0F,
            };
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        const HRESULT status = context_->Map(
            buffer_.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped
        );
        if (FAILED(status)) {
            throw std::runtime_error(
                "D3D11 rounded clip buffer mapping failed with HRESULT " +
                std::to_string(status)
            );
        }
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(buffer_.Get(), 0U);
        ID3D11Buffer* const buffer = buffer_.Get();
        context_->PSSetConstantBuffers(1U, 1U, &buffer);
    }

  private:
    ID3D11DeviceContext* context_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D11Buffer> buffer_;
};

} // namespace strata::d3d11
