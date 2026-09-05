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
#include "gpu/clip.hpp"

namespace strata::d3d11 {

using gpu::rounded_clip_hlsl;
using gpu::RoundedClipMode;
using gpu::RoundedClipConstants;

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
