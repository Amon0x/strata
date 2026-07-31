#include "renderer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "host/render_packet.hpp"

namespace strata::desktop {
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

} // namespace

struct Renderer::Impl final {
    Impl(HWND window, const bool vsync) : window(window), vsync(vsync) {
        if (window == nullptr)
            throw std::invalid_argument("desktop renderer requires a window");
        const HRESULT com_status = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (SUCCEEDED(com_status))
            owns_com = true;
        else if (com_status != RPC_E_CHANGED_MODE)
            fail_hresult("COM initialization", com_status);
        create_device();
        renderer = std::make_unique<d3d11::RenderContext>(device.Get(), context.Get());
        RECT client{};
        if (!GetClientRect(window, &client))
            throw std::runtime_error("could not read window size");
        const std::uint32_t initial_width =
            static_cast<std::uint32_t>(std::max<LONG>(client.right - client.left, 1L));
        const std::uint32_t initial_height =
            static_cast<std::uint32_t>(std::max<LONG>(client.bottom - client.top, 1L));
        resize(initial_width, initial_height, initial_width, initial_height);
    }

    ~Impl() {
        renderer.reset();
        if (context != nullptr)
            context->ClearState();
        if (owns_com)
            CoUninitialize();
    }

    void create_device() {
        DXGI_SWAP_CHAIN_DESC swap_desc{};
        swap_desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        swap_desc.SampleDesc.Count = 1U;
        swap_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        swap_desc.BufferCount = 2U;
        swap_desc.OutputWindow = window;
        swap_desc.Windowed = TRUE;
        swap_desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const std::array feature_levels{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected{};
        require_hresult(D3D11CreateDeviceAndSwapChain(
                            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0U, feature_levels.data(),
                            static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &swap_desc,
                            &swap_chain, &device, &selected, &context),
                        "D3D11 device creation");
    }

    void resize(const std::uint32_t next_width, const std::uint32_t next_height,
                const double next_logical_width, const double next_logical_height) {
        if (next_width == 0U || next_height == 0U || next_logical_width <= 0.0 ||
            next_logical_height <= 0.0) {
            throw std::invalid_argument("desktop viewport must be positive");
        }
        if (next_width != width || next_height != height || target == nullptr) {
            context->OMSetRenderTargets(0U, nullptr, nullptr);
            target.Reset();
            back_buffer.Reset();
            require_hresult(
                swap_chain->ResizeBuffers(0U, next_width, next_height, DXGI_FORMAT_UNKNOWN, 0U),
                "swap-chain resize");
            require_hresult(swap_chain->GetBuffer(0U, IID_PPV_ARGS(&back_buffer)),
                            "swap-chain buffer acquisition");
            require_hresult(device->CreateRenderTargetView(back_buffer.Get(), nullptr, &target),
                            "render-target creation");
            width = next_width;
            height = next_height;
        }
        renderer->set_target(back_buffer.Get(), target.Get(), width, height, next_logical_width,
                             next_logical_height);
    }

    void begin_frame() {
        const float elapsed =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - started_at).count();
        constexpr std::array<float, 4U> clear{0.035F, 0.043F, 0.059F, 1.0F};
        renderer->begin_frame(clear, elapsed);
    }

    void end_frame() {
        require_hresult(swap_chain->Present(vsync ? 1U : 0U, 0U), "swap-chain presentation");
    }

    HWND window = nullptr;
    bool owns_com = false;
    bool vsync = true;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swap_chain;
    ComPtr<ID3D11Texture2D> back_buffer;
    ComPtr<ID3D11RenderTargetView> target;
    std::unique_ptr<d3d11::RenderContext> renderer;
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
};

Renderer::Renderer(HWND window, const bool vsync) : impl_(std::make_unique<Impl>(window, vsync)) {}
Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::declare_material(const std::string_view id, const std::string_view hlsl_source) {
    impl_->renderer->declare_material(id, hlsl_source);
}

void Renderer::resize(const std::uint32_t framebuffer_width, const std::uint32_t framebuffer_height,
                      const double logical_width, const double logical_height) {
    impl_->resize(framebuffer_width, framebuffer_height, logical_width, logical_height);
}

void Renderer::consume_resources(const host::RenderPacket& packet) {
    impl_->renderer->consume_resources(packet);
}

void Renderer::begin_frame() {
    impl_->begin_frame();
}

RenderLayerTelemetry Renderer::render_layer(const std::string_view id,
                                            const host::RenderPacket& packet) {
    return impl_->renderer->render_layer(id, packet);
}

void Renderer::end_frame() {
    impl_->end_frame();
}

void Renderer::render(const host::RenderPacket& packet) {
    begin_frame();
    static_cast<void>(render_layer("default", packet));
    end_frame();
}

} // namespace strata::desktop
