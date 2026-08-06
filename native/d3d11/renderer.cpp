#include <strata/d3d11.hpp>

#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <wrl/client.h>

#include "render_context.hpp"
#include <strata/render_packet.hpp>

namespace strata::d3d11 {
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

[[nodiscard]] bool same_object(IUnknown* const left, IUnknown* const right) {
    if (left == nullptr || right == nullptr)
        return false;
    ComPtr<IUnknown> left_identity;
    ComPtr<IUnknown> right_identity;
    require_hresult(left->QueryInterface(IID_PPV_ARGS(&left_identity)),
                    "D3D11 device identity query");
    require_hresult(right->QueryInterface(IID_PPV_ARGS(&right_identity)),
                    "D3D11 context-device identity query");
    return left_identity.Get() == right_identity.Get();
}

} // namespace

struct Renderer::Impl final {
    class StateScope final {
      public:
        explicit StateScope(Impl& owner) : owner_(owner) {
            if (owner_.active)
                throw std::logic_error("D3D11 renderer does not permit nested rendering");
            owner_.active = true;
            if (owner_.state_policy == ContextStatePolicy::preserve) {
                owner_.context1->SwapDeviceContextState(owner_.isolated_state.Get(),
                                                        &previous_state_);
            }
        }

        ~StateScope() {
            if (previous_state_ != nullptr) {
                ComPtr<ID3DDeviceContextState> rendered_state;
                owner_.context1->SwapDeviceContextState(previous_state_.Get(), &rendered_state);
            }
            owner_.active = false;
        }

        StateScope(const StateScope&) = delete;
        StateScope& operator=(const StateScope&) = delete;

      private:
        Impl& owner_;
        ComPtr<ID3DDeviceContextState> previous_state_;
    };

    Impl(ID3D11Device* const device, ID3D11DeviceContext* const context,
         const RendererOptions options)
        : device(device), context(context), state_policy(options.context_state),
          renderer(device, context, options.asynchronous_shader_compilation) {
        if (device == nullptr || context == nullptr)
            throw std::invalid_argument("D3D11 renderer requires a device and context");

        ComPtr<ID3D11Device> context_device;
        context->GetDevice(&context_device);
        if (!same_object(device, context_device.Get()))
            throw std::invalid_argument("D3D11 renderer device and context do not share ownership");

        if (state_policy == ContextStatePolicy::preserve) {
            require_hresult(device->QueryInterface(IID_PPV_ARGS(&device1)),
                            "D3D11.1 device query for context-state preservation");
            require_hresult(context->QueryInterface(IID_PPV_ARGS(&context1)),
                            "D3D11.1 context query for context-state preservation");
            const D3D_FEATURE_LEVEL feature_level = device->GetFeatureLevel();
            D3D_FEATURE_LEVEL selected_feature_level{};
            require_hresult(device1->CreateDeviceContextState(
                                0U, &feature_level, 1U, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                &selected_feature_level, &isolated_state),
                            "D3D11 isolated context-state creation");
        }
    }

    [[nodiscard]] RenderLayerTelemetry render(const std::string_view layer_id,
                                              const host::RenderPacket& packet,
                                              const RenderTarget& target,
                                              const FrameOptions options) {
        if (layer_id.empty())
            throw std::invalid_argument("D3D11 renderer layer id must not be empty");
        if (!std::isfinite(options.time_seconds))
            throw std::invalid_argument("D3D11 renderer frame time must be finite");
        if (target.texture == nullptr || target.view == nullptr)
            throw std::invalid_argument("D3D11 renderer target resources must not be null");

        ComPtr<ID3D11Device> target_device;
        target.texture->GetDevice(&target_device);
        if (!same_object(device.Get(), target_device.Get()))
            throw std::invalid_argument("D3D11 renderer target belongs to another device");
        ComPtr<ID3D11Resource> view_resource;
        target.view->GetResource(&view_resource);
        if (!same_object(target.texture, view_resource.Get()))
            throw std::invalid_argument(
                "D3D11 renderer texture and render-target view do not share ownership");
        D3D11_TEXTURE2D_DESC target_description{};
        target.texture->GetDesc(&target_description);
        if (target_description.SampleDesc.Count != 1U)
            throw std::invalid_argument("D3D11 renderer target must not be multisampled");

        StateScope state(*this);
        renderer.set_target(target.texture, target.view, target.framebuffer_width,
                            target.framebuffer_height, target.logical_width, target.logical_height);
        switch (options.load_action) {
        case TargetLoadAction::preserve:
            renderer.begin_frame(std::nullopt, options.time_seconds);
            break;
        case TargetLoadAction::clear:
            renderer.begin_frame(options.clear_color, options.time_seconds);
            break;
        default:
            throw std::invalid_argument("D3D11 renderer received an unknown target load action");
        }
        return renderer.render_layer(layer_id, packet);
    }

    void consume_resources(const host::RenderPacket& packet) {
        StateScope state(*this);
        renderer.consume_resources(packet);
    }

    void release_target() {
        StateScope state(*this);
        renderer.release_target();
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext1> context1;
    ComPtr<ID3DDeviceContextState> isolated_state;
    ContextStatePolicy state_policy = ContextStatePolicy::preserve;
    RenderContext renderer;
    bool active = false;
};

Renderer::Renderer(ID3D11Device* const device, ID3D11DeviceContext* const context,
                   const RendererOptions options)
    : impl_(std::make_unique<Impl>(device, context, options)) {}

Renderer::~Renderer() = default;
Renderer::Renderer(Renderer&&) noexcept = default;
Renderer& Renderer::operator=(Renderer&&) noexcept = default;

void Renderer::declare_material(const std::string_view id, const std::string_view hlsl_source) {
    impl_->renderer.declare_material(id, hlsl_source);
}

void Renderer::declare_effect_pass(const std::string_view effect_id, const std::uint32_t index,
                                   const std::uint32_t kind, const double radius,
                                   const std::uint32_t downsample,
                                   const std::uint32_t radius_parameter,
                                   const std::uint32_t downsample_parameter,
                                   const std::string_view hlsl_source) {
    impl_->renderer.declare_effect_pass(effect_id, index, kind, radius, downsample,
                                        radius_parameter, downsample_parameter, hlsl_source);
}

RenderLayerTelemetry Renderer::render(const std::string_view layer_id,
                                      const host::RenderPacket& packet, const RenderTarget& target,
                                      const FrameOptions options) {
    return impl_->render(layer_id, packet, target, options);
}

void Renderer::consume_resources(const host::RenderPacket& packet) {
    impl_->consume_resources(packet);
}

void Renderer::release_layer(const std::string_view layer_id) noexcept {
    impl_->renderer.release_layer(layer_id);
}

void Renderer::release_target() {
    impl_->release_target();
}

} // namespace strata::d3d11
