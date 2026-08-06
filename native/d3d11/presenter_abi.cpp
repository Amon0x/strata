#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <strata/d3d11.h>

#include <d3d11.h>

#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <strata/d3d11.hpp>
#include <strata/strata.hpp>

namespace {

thread_local std::string last_error;

class CallbackFailure final : public std::runtime_error {
  public:
    explicit CallbackFailure(const strata_status status)
        : std::runtime_error("D3D11 program source callback failed"), status(status) {}

    strata_status status = STRATA_STATUS_INTERNAL_ERROR;
};

[[nodiscard]] strata_adapter_result result(
    const strata_status status,
    std::string message = {}
) noexcept {
    last_error = std::move(message);
    return {
        status,
        0U,
        strata_string_view{last_error.data(), last_error.size()},
    };
}

[[nodiscard]] strata_adapter_result ok() noexcept {
    return result(STRATA_STATUS_OK);
}

[[nodiscard]] std::string_view view(const strata_string_view value) {
    if (value.size != 0U && value.data == nullptr) {
        throw std::invalid_argument("adapter string view has a null data pointer");
    }
    return value.size == 0U ? std::string_view{} : std::string_view(value.data, value.size);
}

template <typename Operation>
[[nodiscard]] strata_adapter_result guarded(Operation&& operation) noexcept {
    try {
        std::forward<Operation>(operation)();
        return ok();
    } catch (const CallbackFailure& error) {
        return result(error.status, error.what());
    } catch (const strata::AbiError& error) {
        return result(error.status(), error.what());
    } catch (const std::invalid_argument& error) {
        return result(STRATA_STATUS_INVALID_ARGUMENT, error.what());
    } catch (const std::bad_alloc& error) {
        return result(STRATA_STATUS_OUT_OF_MEMORY, error.what());
    } catch (const std::logic_error& error) {
        return result(STRATA_STATUS_INVARIANT_FAILURE, error.what());
    } catch (const std::exception& error) {
        return result(STRATA_STATUS_INTERNAL_ERROR, error.what());
    } catch (...) {
        return result(STRATA_STATUS_INTERNAL_ERROR, "unknown D3D11 adapter failure");
    }
}

[[nodiscard]] strata::d3d11::ContextStatePolicy context_state(
    const strata_d3d11_context_state_policy value
) {
    switch (value) {
    case STRATA_D3D11_CONTEXT_STATE_PRESERVE:
        return strata::d3d11::ContextStatePolicy::preserve;
    case STRATA_D3D11_CONTEXT_STATE_HOST_MANAGED:
        return strata::d3d11::ContextStatePolicy::host_managed;
    default:
        throw std::invalid_argument("unknown D3D11 context-state policy");
    }
}

[[nodiscard]] strata::d3d11::TargetLoadAction load_action(
    const strata_d3d11_target_load_action value
) {
    switch (value) {
    case STRATA_D3D11_TARGET_PRESERVE:
        return strata::d3d11::TargetLoadAction::preserve;
    case STRATA_D3D11_TARGET_CLEAR:
        return strata::d3d11::TargetLoadAction::clear;
    default:
        throw std::invalid_argument("unknown D3D11 target load action");
    }
}

[[nodiscard]] strata::d3d11::RenderTarget target(
    const strata_d3d11_render_target* const value
) {
    if (value == nullptr || value->struct_size < sizeof(strata_d3d11_render_target)) {
        throw std::invalid_argument("D3D11 render target is incomplete");
    }
    return {
        static_cast<ID3D11Texture2D*>(value->texture),
        static_cast<ID3D11RenderTargetView*>(value->render_target_view),
        value->framebuffer_width,
        value->framebuffer_height,
        value->logical_width,
        value->logical_height,
    };
}

[[nodiscard]] strata::d3d11::FrameOptions frame_options(
    const strata_d3d11_frame_options* const value
) {
    if (value == nullptr) return {};
    if (value->struct_size < sizeof(strata_d3d11_frame_options)) {
        throw std::invalid_argument("D3D11 frame options are incomplete");
    }
    strata::d3d11::FrameOptions options;
    options.load_action = load_action(value->load_action);
    for (std::size_t index = 0U; index < options.clear_color.size(); ++index) {
        options.clear_color[index] = value->clear_color[index];
    }
    return options;
}

[[nodiscard]] strata_d3d11_render_telemetry telemetry(
    const strata::d3d11::RenderLayerTelemetry& value
) noexcept {
    return {
        value.blur_passes,
        value.blur_target_width,
        value.blur_target_height,
        value.blur_nanos,
        value.effect_passes,
        value.effect_target_width,
        value.effect_target_height,
        value.effect_nanos,
    };
}

} // namespace

struct strata_d3d11_presenter final {
    std::unique_ptr<strata::d3d11::Presenter> value;
};

extern "C" {

strata_adapter_result strata_d3d11_presenter_create(
    strata_runtime* const runtime,
    void* const device,
    void* const immediate_context,
    const strata_d3d11_presenter_options* const options,
    strata_d3d11_presenter** const out_presenter
) {
    if (out_presenter != nullptr) *out_presenter = nullptr;
    return guarded([&] {
        if (runtime == nullptr || device == nullptr || immediate_context == nullptr ||
            out_presenter == nullptr) {
            throw std::invalid_argument("D3D11 presenter creation arguments are incomplete");
        }
        strata::d3d11::PresenterOptions native_options;
        if (options != nullptr) {
            if (options->struct_size < sizeof(strata_d3d11_presenter_options)) {
                throw std::invalid_argument("D3D11 presenter options are incomplete");
            }
            native_options.renderer.context_state = context_state(options->context_state);
            native_options.renderer.asynchronous_shader_compilation =
                options->asynchronous_shader_compilation != 0U;
            if (options->load_program_source != nullptr) {
                const auto callback = options->load_program_source;
                void* const user_data = options->program_source_user_data;
                native_options.load_program_source = [callback, user_data](
                    const std::string_view resource_id
                ) {
                    strata_bytes_view source{};
                    const strata_status status = callback(
                        user_data,
                        strata_string_view{resource_id.data(), resource_id.size()},
                        &source
                    );
                    if (status != STRATA_STATUS_OK) throw CallbackFailure(status);
                    if (source.size != 0U && source.data == nullptr) {
                        throw std::invalid_argument(
                            "D3D11 program source callback returned null bytes"
                        );
                    }
                    return source.size == 0U
                        ? std::string{}
                        : std::string(
                              reinterpret_cast<const char*>(source.data),
                              source.size
                          );
                };
            }
        }
        auto presenter = std::make_unique<strata_d3d11_presenter>();
        presenter->value = std::make_unique<strata::d3d11::Presenter>(
            runtime,
            static_cast<ID3D11Device*>(device),
            static_cast<ID3D11DeviceContext*>(immediate_context),
            std::move(native_options)
        );
        *out_presenter = presenter.release();
    });
}

void strata_d3d11_presenter_destroy(strata_d3d11_presenter* const presenter) {
    delete presenter;
}

strata_adapter_result strata_d3d11_presenter_synchronize_programs(
    strata_d3d11_presenter* const presenter
) {
    return guarded([&] {
        if (presenter == nullptr) throw std::invalid_argument("D3D11 presenter is null");
        presenter->value->synchronize_programs();
    });
}

strata_adapter_result strata_d3d11_presenter_reload_program_source(
    strata_d3d11_presenter* const presenter,
    const strata_string_view resource_id,
    const strata_bytes_view source,
    uint32_t* const out_matched
) {
    if (out_matched != nullptr) *out_matched = 0U;
    return guarded([&] {
        if (presenter == nullptr || out_matched == nullptr ||
            (source.size != 0U && source.data == nullptr)) {
            throw std::invalid_argument("D3D11 program reload arguments are incomplete");
        }
        const std::string_view id = view(resource_id);
        const std::string_view text = source.size == 0U
            ? std::string_view{}
            : std::string_view(
                  reinterpret_cast<const char*>(source.data),
                  source.size
              );
        *out_matched = presenter->value->reload_program_source(id, text) ? 1U : 0U;
    });
}

strata_adapter_result strata_d3d11_presenter_attach(
    strata_d3d11_presenter* const presenter,
    const strata_string_view layer_id,
    strata_surface* const surface
) {
    return guarded([&] {
        if (presenter == nullptr) throw std::invalid_argument("D3D11 presenter is null");
        presenter->value->attach(view(layer_id), surface);
    });
}

strata_adapter_result strata_d3d11_presenter_present(
    strata_d3d11_presenter* const presenter,
    const strata_string_view layer_id,
    strata_surface* const surface,
    const strata_d3d11_render_target* const render_target,
    const int64_t time_nanoseconds,
    const strata_d3d11_frame_options* const options,
    strata_d3d11_presented_frame* const out_frame
) {
    return guarded([&] {
        if (presenter == nullptr || out_frame == nullptr ||
            out_frame->struct_size < sizeof(strata_d3d11_presented_frame)) {
            throw std::invalid_argument("D3D11 presentation arguments are incomplete");
        }
        const std::size_t struct_size = out_frame->struct_size;
        const strata::d3d11::PresentedFrame presented = presenter->value->present(
            view(layer_id),
            surface,
            target(render_target),
            time_nanoseconds,
            frame_options(options)
        );
        *out_frame = strata_d3d11_presented_frame{
            struct_size,
            presented.surface,
            presented.packet_bytes,
            telemetry(presented.rendering),
        };
    });
}

strata_adapter_result strata_d3d11_presenter_detach(
    strata_d3d11_presenter* const presenter,
    const strata_string_view layer_id
) {
    return guarded([&] {
        if (presenter == nullptr) throw std::invalid_argument("D3D11 presenter is null");
        presenter->value->detach(view(layer_id));
    });
}

void strata_d3d11_presenter_discard(
    strata_d3d11_presenter* const presenter,
    const strata_string_view layer_id
) {
    if (presenter == nullptr || (layer_id.size != 0U && layer_id.data == nullptr)) return;
    presenter->value->discard(
        layer_id.size == 0U ? std::string_view{} : std::string_view(layer_id.data, layer_id.size)
    );
}

strata_adapter_result strata_d3d11_presenter_attached(
    const strata_d3d11_presenter* const presenter,
    const strata_string_view layer_id,
    uint32_t* const out_attached
) {
    if (out_attached != nullptr) *out_attached = 0U;
    return guarded([&] {
        if (presenter == nullptr || out_attached == nullptr) {
            throw std::invalid_argument("D3D11 attachment query arguments are incomplete");
        }
        *out_attached = presenter->value->attached(view(layer_id)) ? 1U : 0U;
    });
}

strata_adapter_result strata_d3d11_presenter_release_target(
    strata_d3d11_presenter* const presenter
) {
    return guarded([&] {
        if (presenter == nullptr) throw std::invalid_argument("D3D11 presenter is null");
        presenter->value->release_target();
    });
}

} // extern "C"
