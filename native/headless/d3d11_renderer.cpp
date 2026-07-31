#include "d3d11_renderer.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include "d3d11/render_context.hpp"
#include "host/render_packet.hpp"

namespace strata::headless {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void fail_hresult(const std::string_view operation, const HRESULT result) {
    throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                             std::to_string(result));
}

void require_hresult(const HRESULT result, const std::string_view operation) {
    if (FAILED(result))
        fail_hresult(operation, result);
}

class ComApartment final {
  public:
    ComApartment() {
        const HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(status))
            owns_ = true;
        else if (status != RPC_E_CHANGED_MODE) {
            fail_hresult("headless COM initialization", status);
        }
    }
    ~ComApartment() {
        if (owns_)
            CoUninitialize();
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

  private:
    bool owns_ = false;
};

} // namespace

struct D3D11Renderer::Impl final {
    Impl() {
        const std::array feature_levels{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        require_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0U,
                                          feature_levels.data(),
                                          static_cast<UINT>(feature_levels.size()),
                                          D3D11_SDK_VERSION, &device, &selected, &context),
                        "headless D3D11/WARP device creation");
        renderer = std::make_unique<d3d11::RenderContext>(device.Get(), context.Get());
    }

    ~Impl() {
        renderer.reset();
        if (context != nullptr)
            context->ClearState();
    }

    void resize(const std::uint32_t next_width, const std::uint32_t next_height,
                const double next_logical_width, const double next_logical_height) {
        if (next_width == 0U || next_height == 0U || next_logical_width <= 0.0 ||
            next_logical_height <= 0.0) {
            throw std::invalid_argument("headless D3D11 viewport must be positive");
        }
        if (next_width != width || next_height != height || target == nullptr) {
            context->OMSetRenderTargets(0U, nullptr, nullptr);
            target_view.Reset();
            target.Reset();
            staging.Reset();

            D3D11_TEXTURE2D_DESC target_descriptor{};
            target_descriptor.Width = next_width;
            target_descriptor.Height = next_height;
            target_descriptor.MipLevels = 1U;
            target_descriptor.ArraySize = 1U;
            target_descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            target_descriptor.SampleDesc.Count = 1U;
            target_descriptor.Usage = D3D11_USAGE_DEFAULT;
            target_descriptor.BindFlags = D3D11_BIND_RENDER_TARGET;
            require_hresult(device->CreateTexture2D(&target_descriptor, nullptr, &target),
                            "headless D3D11 render texture creation");
            require_hresult(device->CreateRenderTargetView(target.Get(), nullptr, &target_view),
                            "headless D3D11 render-target view creation");

            D3D11_TEXTURE2D_DESC staging_descriptor = target_descriptor;
            staging_descriptor.Usage = D3D11_USAGE_STAGING;
            staging_descriptor.BindFlags = 0U;
            staging_descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            require_hresult(device->CreateTexture2D(&staging_descriptor, nullptr, &staging),
                            "headless D3D11 staging texture creation");
            const std::uint64_t byte_count =
                static_cast<std::uint64_t>(next_width) * next_height * 4U;
            if (byte_count > std::numeric_limits<std::size_t>::max()) {
                throw std::length_error("headless D3D11 framebuffer exceeds address space");
            }
            pixels.resize(static_cast<std::size_t>(byte_count));
            width = next_width;
            height = next_height;
        }
        renderer->set_target(target.Get(), target_view.Get(), width, height, next_logical_width,
                             next_logical_height);
    }

    void declare_material(const std::string_view id, const std::string_view source) {
        renderer->declare_material(id, source);
        declared_materials.emplace(id);
    }

    void render(const host::RenderPacket& packet, const std::int64_t time_nanoseconds) {
        if (target == nullptr || staging == nullptr) {
            throw std::logic_error("headless D3D11 renderer must be sized before rendering");
        }
        for (const host::SubmissionBatch& batch : packet.batches) {
            const auto* draw = std::get_if<host::DrawBatch>(&batch);
            if (draw == nullptr || draw->material == "strata:unified_ui" ||
                declared_materials.contains(draw->material) ||
                std::ranges::find(material_fallbacks, draw->material) != material_fallbacks.end()) {
                continue;
            }
            material_fallbacks.push_back(draw->material);
        }
        std::array<float, 4U> clear_float{};
        for (std::size_t index = 0U; index < clear.size(); ++index) {
            clear_float[index] = static_cast<float>(clear[index]) / 255.0F;
        }
        renderer->begin_frame(
            clear_float,
            static_cast<float>(static_cast<double>(time_nanoseconds) / 1'000'000'000.0));
        static_cast<void>(renderer->render_layer("headless", packet));
        context->OMSetRenderTargets(0U, nullptr, nullptr);
        context->CopyResource(staging.Get(), target.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped),
                        "headless D3D11 readback mapping");
        const std::size_t row_bytes = static_cast<std::size_t>(width) * 4U;
        for (std::uint32_t row = 0U; row < height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * row_bytes,
                        static_cast<const std::uint8_t*>(mapped.pData) +
                            static_cast<std::size_t>(row) * mapped.RowPitch,
                        row_bytes);
        }
        context->Unmap(staging.Get(), 0U);
    }

    ComApartment apartment;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11RenderTargetView> target_view;
    ComPtr<ID3D11Texture2D> staging;
    std::unique_ptr<d3d11::RenderContext> renderer;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::array<std::uint8_t, 4U> clear{9U, 11U, 15U, 255U};
    std::vector<std::uint8_t> pixels;
    std::set<std::string, std::less<>> declared_materials;
    std::vector<std::string> material_fallbacks;
};

D3D11Renderer::D3D11Renderer() : impl_(std::make_unique<Impl>()) {}
D3D11Renderer::~D3D11Renderer() = default;

void D3D11Renderer::resize(const std::uint32_t framebuffer_width,
                           const std::uint32_t framebuffer_height, const double logical_width,
                           const double logical_height) {
    impl_->resize(framebuffer_width, framebuffer_height, logical_width, logical_height);
}

void D3D11Renderer::set_clear_color(const std::array<std::uint8_t, 4U> rgba) noexcept {
    impl_->clear = rgba;
}

void D3D11Renderer::declare_material(const std::string_view id, const std::string_view source) {
    impl_->declare_material(id, source);
}

void D3D11Renderer::render(const host::RenderPacket& packet, const std::int64_t time_nanoseconds) {
    impl_->render(packet, time_nanoseconds);
}

void D3D11Renderer::consume_resources(const host::RenderPacket& packet) {
    impl_->renderer->consume_resources(packet);
}

std::string_view D3D11Renderer::backend() const noexcept {
    return "d3d11";
}
std::uint32_t D3D11Renderer::width() const noexcept {
    return impl_->width;
}
std::uint32_t D3D11Renderer::height() const noexcept {
    return impl_->height;
}
std::span<const std::uint8_t> D3D11Renderer::pixels() const noexcept {
    return impl_->pixels;
}
const std::vector<std::string>& D3D11Renderer::material_fallbacks() const noexcept {
    return impl_->material_fallbacks;
}

} // namespace strata::headless
