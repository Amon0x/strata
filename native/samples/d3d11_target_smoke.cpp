#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <strata/d3d11.hpp>
#include <strata/host.hpp>
#include <strata/render_packet.hpp>
#include <strata/win32.hpp>

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

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void require_adapter(
    const strata_adapter_result result,
    const std::string_view operation
) {
    if (result.status == STRATA_STATUS_OK) return;
    const std::string detail = result.message.size == 0U
        ? std::string{}
        : std::string(result.message.data, result.message.size);
    throw std::runtime_error(
        std::string(operation) + " failed" +
        (detail.empty() ? std::string{} : ": " + detail)
    );
}

std::int64_t adapter_clock(void* const user_data) noexcept {
    return *static_cast<std::int64_t*>(user_data);
}

void install_host_state(ID3D11DeviceContext* const context, ID3D11RenderTargetView* const target) {
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);
    constexpr D3D11_VIEWPORT viewport{1.0F, 2.0F, 3.0F, 4.0F, 0.25F, 0.75F};
    context->RSSetViewports(1U, &viewport);
    context->OMSetRenderTargets(1U, &target, nullptr);
}

void check_host_state(ID3D11DeviceContext* const context, ID3D11RenderTargetView* const target) {
    D3D11_PRIMITIVE_TOPOLOGY topology{};
    context->IAGetPrimitiveTopology(&topology);
    check(topology == D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
          "external D3D11 rendering changed the host primitive topology");

    UINT viewport_count = 1U;
    D3D11_VIEWPORT viewport{};
    context->RSGetViewports(&viewport_count, &viewport);
    check(viewport_count == 1U && viewport.TopLeftX == 1.0F && viewport.TopLeftY == 2.0F &&
              viewport.Width == 3.0F && viewport.Height == 4.0F && viewport.MinDepth == 0.25F &&
              viewport.MaxDepth == 0.75F,
          "external D3D11 rendering changed the host viewport");

    ComPtr<ID3D11RenderTargetView> restored_target;
    context->OMGetRenderTargets(1U, &restored_target, nullptr);
    check(restored_target.Get() == target,
          "external D3D11 rendering changed the host render target");
}

[[nodiscard]] std::array<std::uint8_t, 4U> read_pixel(ID3D11DeviceContext* const context,
                                                      ID3D11Texture2D* const target,
                                                      ID3D11Texture2D* const staging) {
    context->OMSetRenderTargets(0U, nullptr, nullptr);
    context->CopyResource(staging, target);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    require_hresult(context->Map(staging, 0U, D3D11_MAP_READ, 0U, &mapped),
                    "D3D11 target readback");
    const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData);
    const std::array result{bytes[0U], bytes[1U], bytes[2U], bytes[3U]};
    context->Unmap(staging, 0U);
    return result;
}

} // namespace

int main() {
    try {
        constexpr std::uint32_t width = 8U;
        constexpr std::uint32_t height = 8U;
        constexpr std::array feature_levels{D3D_FEATURE_LEVEL_11_0};
        D3D_FEATURE_LEVEL selected_feature_level{};
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        require_hresult(
            D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0U, feature_levels.data(),
                              static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &device,
                              &selected_feature_level, &context),
            "D3D11 WARP device creation");

        D3D11_TEXTURE2D_DESC target_description{};
        target_description.Width = width;
        target_description.Height = height;
        target_description.MipLevels = 1U;
        target_description.ArraySize = 1U;
        target_description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        target_description.SampleDesc.Count = 1U;
        target_description.Usage = D3D11_USAGE_DEFAULT;
        target_description.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target_texture;
        require_hresult(device->CreateTexture2D(&target_description, nullptr, &target_texture),
                        "D3D11 target texture creation");
        ComPtr<ID3D11RenderTargetView> target_view;
        require_hresult(device->CreateRenderTargetView(target_texture.Get(), nullptr, &target_view),
                        "D3D11 target-view creation");

        D3D11_TEXTURE2D_DESC staging_description = target_description;
        staging_description.Usage = D3D11_USAGE_STAGING;
        staging_description.BindFlags = 0U;
        staging_description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        require_hresult(device->CreateTexture2D(&staging_description, nullptr, &staging),
                        "D3D11 staging texture creation");

        strata::d3d11::Renderer renderer(device.Get(), context.Get());
        const strata::d3d11::RenderTarget target{
            target_texture.Get(),       target_view.Get(),           width, height,
            static_cast<double>(width), static_cast<double>(height),
        };
        strata::host::RenderPacket packet;
        packet.geometry_epoch = 1U;

        constexpr std::array red{1.0F, 0.0F, 0.0F, 1.0F};
        context->ClearRenderTargetView(target_view.Get(), red.data());
        install_host_state(context.Get(), target_view.Get());
        static_cast<void>(renderer.render("installed.target", packet, target));
        check_host_state(context.Get(), target_view.Get());
        check(read_pixel(context.Get(), target_texture.Get(), staging.Get()) ==
                  std::array<std::uint8_t, 4U>{255U, 0U, 0U, 255U},
              "external D3D11 rendering did not preserve existing target contents");

        install_host_state(context.Get(), target_view.Get());
        strata::d3d11::FrameOptions clear_frame;
        clear_frame.load_action = strata::d3d11::TargetLoadAction::clear;
        clear_frame.clear_color = {0.0F, 0.0F, 1.0F, 1.0F};
        clear_frame.time_seconds = 0.25;
        static_cast<void>(renderer.render("installed.target", packet, target, clear_frame));
        check_host_state(context.Get(), target_view.Get());
        check(read_pixel(context.Get(), target_texture.Get(), staging.Get()) ==
                  std::array<std::uint8_t, 4U>{0U, 0U, 255U, 255U},
              "external D3D11 rendering ignored an explicit clear load action");

        install_host_state(context.Get(), target_view.Get());
        strata::d3d11::RenderTarget invalid_target = target;
        invalid_target.framebuffer_width = 0U;
        bool invalid_rejected = false;
        try {
            static_cast<void>(renderer.render("installed.target", packet, invalid_target));
        } catch (const std::invalid_argument&) {
            invalid_rejected = true;
        }
        check(invalid_rejected, "external D3D11 renderer accepted an invalid target");
        check_host_state(context.Get(), target_view.Get());

        renderer.release_layer("installed.target");
        renderer.release_target();

        std::int64_t now = 1'000'000;
        strata::RuntimeOptions runtime_options;
        runtime_options.clock = [&now] { return now; };
        strata::Runtime runtime(std::move(runtime_options));
        runtime.configure_application(strata::ApplicationOptions{
            .id = "installed.d3d11.presenter",
        });
        constexpr std::string_view source = R"(
style Root {
  width: { weight: 1 };
  height: { weight: 1 };
  background: #00FF00FF;
  border: null;
}
overlay Main { root Panel(key: "installed.presenter", style: Root) }
)";
        check(
            runtime.activate(strata::SourceActivation{
                .generation = 1U,
                .entry_source_id = "installed/d3d11/presenter.strata",
                .entry_text = std::string(source),
            }).activated(),
            "external D3D11 presenter fixture did not activate"
        );
        strata::SurfaceOptions surface_options;
        surface_options.id = "installed.d3d11.presenter";
        surface_options.root_role = strata::SurfaceRootRole::overlay;
        surface_options.root_name = "Main";
        surface_options.environment.framebuffer_width = width;
        surface_options.environment.framebuffer_height = height;
        surface_options.environment.logical_width = width;
        surface_options.environment.logical_height = height;
        strata::Surface surface = runtime.create_surface(surface_options);
        surface_options.id = "installed.d3d11.presenter.second";
        strata::Surface second_surface = runtime.create_surface(surface_options);
        surface_options.id = "installed.d3d11.presenter.c";
        strata::Surface c_surface = runtime.create_surface(surface_options);

        strata::win32::InputAdapterOptions input_options;
        input_options.clock = [&now] { return now; };
        input_options.manage_pointer_capture = false;
        input_options.focus_on_pointer_press = false;
        input_options.consume_system_keys = true;
        strata::win32::InputAdapter input(std::move(input_options));
        check(
            input.handle(surface, nullptr, WM_MOUSEMOVE, 0U, MAKELPARAM(4, 5)).has_value() &&
                input.handle(surface, nullptr, WM_KEYDOWN, 'A', 0).has_value() &&
                input.handle(surface, nullptr, WM_CHAR, 0xD83DU, 0).has_value() &&
                input.handle(surface, nullptr, WM_CHAR, 0xDE00U, 0).has_value() &&
                input.handle(surface, nullptr, WM_UNICHAR, UNICODE_NOCHAR, 0) ==
                    std::optional<std::intptr_t>(1) &&
                !input.handle(surface, nullptr, WM_APP, 0U, 0).has_value(),
            "external Win32 input adapter did not classify messages"
        );

        strata::d3d11::Presenter presenter(runtime, device.Get(), context.Get());
        presenter.synchronize_programs();
        check(
            !presenter.reload_program_source("missing.program", ""),
            "external D3D11 presenter matched an undeclared program resource"
        );
        install_host_state(context.Get(), target_view.Get());
        const strata::d3d11::PresentedFrame presented = presenter.present(
            "installed.presenter",
            surface,
            target,
            now
        );
        check_host_state(context.Get(), target_view.Get());
        check(
            presented.surface.frame_index == 1U &&
                presented.surface.processed_input_event_count == 3U &&
                presented.packet_bytes >= 12U &&
                presenter.attached("installed.presenter"),
            "external D3D11 presenter did not frame, decode, and render its Surface"
        );
        presenter.attach("installed.presenter.second", second_surface);
        bool layer_reuse_rejected = false;
        try {
            static_cast<void>(presenter.present(
                "installed.presenter",
                second_surface,
                target,
                now
            ));
        } catch (const std::invalid_argument&) {
            layer_reuse_rejected = true;
        }
        check(
            layer_reuse_rejected,
            "external D3D11 presenter allowed one layer id to switch Surfaces"
        );
        static_cast<void>(presenter.present(
            "installed.presenter.second",
            second_surface,
            target,
            now
        ));
        presenter.detach("installed.presenter");
        presenter.detach("installed.presenter.second");
        check(
            !presenter.attached("installed.presenter") &&
                !presenter.attached("installed.presenter.second"),
            "external D3D11 presenter retained a detached Surface layer"
        );
        surface.close();
        second_surface.close();

        strata_win32_input_options c_input_options{};
        c_input_options.struct_size = sizeof(c_input_options);
        c_input_options.clock = strata_clock{
            sizeof(strata_clock),
            &now,
            &adapter_clock,
        };
        c_input_options.coordinate_scale = 1.0;
        strata_win32_input_adapter* c_input = nullptr;
        require_adapter(
            strata_win32_input_adapter_create(&c_input_options, &c_input),
            "C Win32 input adapter creation"
        );
        strata_win32_message_result c_message{};
        require_adapter(
            strata_win32_input_adapter_handle(
                c_input,
                c_surface.native_handle(),
                nullptr,
                WM_MOUSEMOVE,
                0U,
                MAKELPARAM(2, 3),
                &c_message
            ),
            "C Win32 input translation"
        );
        check(c_message.handled != 0U, "C Win32 input adapter ignored pointer input");

        strata_d3d11_presenter* c_presenter = nullptr;
        require_adapter(
            strata_d3d11_presenter_create(
                runtime.native_handle(),
                device.Get(),
                context.Get(),
                nullptr,
                &c_presenter
            ),
            "C D3D11 presenter creation"
        );
        require_adapter(
            strata_d3d11_presenter_synchronize_programs(c_presenter),
            "C D3D11 program synchronization"
        );
        const strata_d3d11_render_target c_target{
            sizeof(strata_d3d11_render_target),
            target_texture.Get(),
            target_view.Get(),
            width,
            height,
            static_cast<double>(width),
            static_cast<double>(height),
        };
        strata_d3d11_presented_frame c_frame{};
        c_frame.struct_size = sizeof(c_frame);
        require_adapter(
            strata_d3d11_presenter_present(
                c_presenter,
                strata::view("installed.presenter.c"),
                c_surface.native_handle(),
                &c_target,
                now,
                nullptr,
                &c_frame
            ),
            "C D3D11 Surface presentation"
        );
        check(
            c_frame.surface.frame_index == 1U &&
                c_frame.surface.processed_input_event_count == 1U &&
                c_frame.packet_bytes >= 12U,
            "C adapter boundary did not frame and present its Surface"
        );
        require_adapter(
            strata_d3d11_presenter_detach(
                c_presenter,
                strata::view("installed.presenter.c")
            ),
            "C D3D11 presenter detach"
        );
        c_surface.close();
        require_adapter(
            strata_d3d11_presenter_release_target(c_presenter),
            "C D3D11 target release"
        );
        strata_d3d11_presenter_destroy(c_presenter);
        strata_win32_input_adapter_destroy(c_input);
        presenter.release_target();
        runtime.close();
        context->OMSetRenderTargets(0U, nullptr, nullptr);
        std::cout
            << "strata_d3d11_target_smoke: target, presenter, and Win32 input adapter OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_d3d11_target_smoke: " << error.what() << '\n';
        return 1;
    }
}
