#include <strata/vulkan.h>

#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <strata/strata.hpp>
#include <strata/vulkan.hpp>

namespace {

thread_local std::string last_error;

class CallbackFailure final : public std::runtime_error {
  public:
    explicit CallbackFailure(const strata_status status)
        : std::runtime_error("Vulkan program source callback failed"), status(status) {}

    strata_status status = STRATA_STATUS_INTERNAL_ERROR;
};

[[nodiscard]] strata_adapter_result result(const strata_status status,
                                           std::string message = {}) noexcept {
    last_error = std::move(message);
    return {
        status,
        0U,
        strata_string_view{last_error.data(), last_error.size()},
    };
}

[[nodiscard]] strata_adapter_result ok() noexcept { return result(STRATA_STATUS_OK); }

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
        return result(STRATA_STATUS_INTERNAL_ERROR, "unknown Vulkan adapter failure");
    }
}

[[nodiscard]] strata::vulkan::TargetLoadAction
load_action(const strata_vulkan_target_load_action value) {
    switch (value) {
    case STRATA_VULKAN_TARGET_PRESERVE:
        return strata::vulkan::TargetLoadAction::preserve;
    case STRATA_VULKAN_TARGET_CLEAR:
        return strata::vulkan::TargetLoadAction::clear;
    default:
        throw std::invalid_argument("unknown Vulkan target load action");
    }
}

[[nodiscard]] strata::vulkan::RenderTarget target(const strata_vulkan_render_target* const value) {
    if (value == nullptr || value->struct_size < sizeof(strata_vulkan_render_target)) {
        throw std::invalid_argument("Vulkan render target is incomplete");
    }
    return {
        value->image,
        value->view,
        value->format,
        value->framebuffer_width,
        value->framebuffer_height,
        value->logical_width,
        value->logical_height,
        value->initial_layout,
        value->final_layout,
    };
}

[[nodiscard]] strata::vulkan::FrameOptions
frame_options(const strata_vulkan_frame_options* const value) {
    if (value == nullptr)
        return {};
    if (value->struct_size < sizeof(strata_vulkan_frame_options)) {
        throw std::invalid_argument("Vulkan frame options are incomplete");
    }
    if (value->reserved != 0U)
        throw std::invalid_argument("Vulkan reserved fields must be zero");
    strata::vulkan::FrameOptions options;
    options.load_action = load_action(value->load_action);
    for (std::size_t index = 0U; index < options.clear_color.size(); ++index) {
        options.clear_color[index] = value->clear_color[index];
    }
    return options;
}

[[nodiscard]] strata_vulkan_render_telemetry
telemetry(const strata::vulkan::RenderLayerTelemetry& value) noexcept {
    return {
        value.blur_passes,
        value.effect_passes,
    };
}

} // namespace

struct strata_vulkan_presenter final {
    std::unique_ptr<strata::vulkan::Presenter> value;
};

extern "C" {

strata_adapter_result
strata_vulkan_presenter_create(strata_runtime* const runtime,
                               const strata_vulkan_device* const device,
                               const strata_vulkan_presenter_options* const options,
                               strata_vulkan_presenter** const out_presenter) {
    if (out_presenter != nullptr)
        *out_presenter = nullptr;
    return guarded([&] {
        if (runtime == nullptr || device == nullptr ||
            device->struct_size < sizeof(strata_vulkan_device) || device->reserved != 0U ||
            out_presenter == nullptr) {
            throw std::invalid_argument("Vulkan presenter creation arguments are incomplete");
        }
        strata::vulkan::PresenterOptions native_options;
        if (options != nullptr) {
            if (options->struct_size < sizeof(strata_vulkan_presenter_options)) {
                throw std::invalid_argument("Vulkan presenter options are incomplete");
            }
            if (options->reserved != 0U)
                throw std::invalid_argument("Vulkan reserved fields must be zero");
            if (options->load_program_source != nullptr) {
                const auto callback = options->load_program_source;
                void* const user_data = options->program_source_user_data;
                native_options.load_program_source =
                    [callback, user_data](const std::string_view resource_id) {
                        strata_bytes_view source{};
                        const strata_status status = callback(
                            user_data, strata_string_view{resource_id.data(), resource_id.size()},
                            &source);
                        if (status != STRATA_STATUS_OK)
                            throw CallbackFailure(status);
                        if (source.size != 0U && source.data == nullptr) {
                            throw std::invalid_argument(
                                "Vulkan program source callback returned null bytes");
                        }
                        return source.size == 0U
                                   ? std::string{}
                                   : std::string(reinterpret_cast<const char*>(source.data),
                                                 source.size);
                    };
            }
        }
        auto presenter = std::make_unique<strata_vulkan_presenter>();
        presenter->value = std::make_unique<strata::vulkan::Presenter>(
            runtime,
            strata::vulkan::Device{device->physical_device, device->device, device->queue,
                                   device->queue_family},
            std::move(native_options));
        *out_presenter = presenter.release();
    });
}

void strata_vulkan_presenter_destroy(strata_vulkan_presenter* const presenter) { delete presenter; }

strata_adapter_result
strata_vulkan_presenter_synchronize_programs(strata_vulkan_presenter* const presenter) {
    return guarded([&] {
        if (presenter == nullptr)
            throw std::invalid_argument("Vulkan presenter is null");
        presenter->value->synchronize_programs();
    });
}

strata_adapter_result strata_vulkan_presenter_reload_program_source(
    strata_vulkan_presenter* const presenter, const strata_string_view resource_id,
    const strata_bytes_view source, uint32_t* const out_matched) {
    if (out_matched != nullptr)
        *out_matched = 0U;
    return guarded([&] {
        if (presenter == nullptr || out_matched == nullptr ||
            (source.size != 0U && source.data == nullptr)) {
            throw std::invalid_argument("Vulkan program reload arguments are incomplete");
        }
        const std::string_view id = view(resource_id);
        const std::string_view text =
            source.size == 0U
                ? std::string_view{}
                : std::string_view(reinterpret_cast<const char*>(source.data), source.size);
        *out_matched = presenter->value->reload_program_source(id, text) ? 1U : 0U;
    });
}

strata_adapter_result strata_vulkan_presenter_attach(strata_vulkan_presenter* const presenter,
                                                     const strata_string_view layer_id,
                                                     strata_surface* const surface) {
    return guarded([&] {
        if (presenter == nullptr)
            throw std::invalid_argument("Vulkan presenter is null");
        presenter->value->attach(view(layer_id), surface);
    });
}

strata_adapter_result strata_vulkan_presenter_present(
    strata_vulkan_presenter* const presenter, const strata_string_view layer_id,
    strata_surface* const surface, const strata_vulkan_render_target* const render_target,
    const int64_t time_nanoseconds, const strata_vulkan_frame_options* const options,
    strata_vulkan_presented_frame* const out_frame) {
    return guarded([&] {
        if (presenter == nullptr || out_frame == nullptr ||
            out_frame->struct_size < sizeof(strata_vulkan_presented_frame)) {
            throw std::invalid_argument("Vulkan presentation arguments are incomplete");
        }
        const std::size_t struct_size = out_frame->struct_size;
        const strata::vulkan::PresentedFrame presented =
            presenter->value->present(view(layer_id), surface, target(render_target),
                                      time_nanoseconds, frame_options(options));
        *out_frame = strata_vulkan_presented_frame{
            struct_size,
            presented.surface,
            presented.packet_bytes,
            telemetry(presented.rendering),
        };
    });
}

strata_adapter_result strata_vulkan_presenter_detach(strata_vulkan_presenter* const presenter,
                                                     const strata_string_view layer_id) {
    return guarded([&] {
        if (presenter == nullptr)
            throw std::invalid_argument("Vulkan presenter is null");
        presenter->value->detach(view(layer_id));
    });
}

void strata_vulkan_presenter_discard(strata_vulkan_presenter* const presenter,
                                     const strata_string_view layer_id) {
    if (presenter == nullptr || (layer_id.size != 0U && layer_id.data == nullptr))
        return;
    presenter->value->discard(layer_id.size == 0U ? std::string_view{}
                                                  : std::string_view(layer_id.data, layer_id.size));
}

strata_adapter_result
strata_vulkan_presenter_attached(const strata_vulkan_presenter* const presenter,
                                 const strata_string_view layer_id, uint32_t* const out_attached) {
    if (out_attached != nullptr)
        *out_attached = 0U;
    return guarded([&] {
        if (presenter == nullptr || out_attached == nullptr) {
            throw std::invalid_argument("Vulkan attachment query arguments are incomplete");
        }
        *out_attached = presenter->value->attached(view(layer_id)) ? 1U : 0U;
    });
}

strata_adapter_result
strata_vulkan_presenter_release_target(strata_vulkan_presenter* const presenter) {
    return guarded([&] {
        if (presenter == nullptr)
            throw std::invalid_argument("Vulkan presenter is null");
        presenter->value->release_target();
    });
}

} // extern "C"
