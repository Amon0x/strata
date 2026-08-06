#include <strata/d3d11.hpp>

#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <strata/render_packet.hpp>
#include <strata/strata.hpp>

namespace strata::d3d11 {
namespace {

struct LoadedMaterial final {
    MaterialDeclaration declaration;
    std::string source;
};

struct LoadedEffectPass final {
    EffectPassDeclaration declaration;
    std::string source;
};

[[nodiscard]] std::string copied(const strata_string_view value) {
    return value.size == 0U ? std::string{} : std::string(value.data, value.size);
}

struct ByteCapture final {
    std::vector<std::uint8_t> value;
    bool failed = false;
};

void capture_bytes(
    void* const user_data,
    const strata_bytes_view bytes
) noexcept {
    auto& capture = *static_cast<ByteCapture*>(user_data);
    try {
        if (bytes.size == 0U) {
            capture.value.clear();
        } else {
            capture.value.assign(bytes.data, bytes.data + bytes.size);
        }
    } catch (...) {
        capture.failed = true;
    }
}

[[nodiscard]] std::vector<MaterialDeclaration> material_declarations(
    strata_runtime* const runtime
) {
    constexpr std::string_view backend = "hlsl";
    const strata_string_view backend_view{backend.data(), backend.size()};
    std::size_t count = 0U;
    require_ok(
        strata_runtime_read_material_declarations(
            runtime,
            backend_view,
            nullptr,
            0U,
            &count
        ),
        "D3D11 material declaration count"
    );
    std::vector<strata_material_declaration> native(count);
    if (count != 0U) {
        require_ok(
            strata_runtime_read_material_declarations(
                runtime,
                backend_view,
                native.data(),
                native.size(),
                &count
            ),
            "D3D11 material declaration read"
        );
    }
    std::vector<MaterialDeclaration> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        result.push_back(MaterialDeclaration{
            copied(native[index].id),
            copied(native[index].blend_mode),
            copied(native[index].fallback),
            copied(native[index].source),
        });
    }
    return result;
}

[[nodiscard]] std::vector<EffectPassDeclaration> effect_pass_declarations(
    strata_runtime* const runtime
) {
    constexpr std::string_view backend = "hlsl";
    const strata_string_view backend_view{backend.data(), backend.size()};
    std::size_t count = 0U;
    require_ok(
        strata_runtime_read_effect_pass_declarations(
            runtime,
            backend_view,
            nullptr,
            0U,
            &count
        ),
        "D3D11 effect declaration count"
    );
    std::vector<strata_effect_pass_declaration> native(count);
    if (count != 0U) {
        require_ok(
            strata_runtime_read_effect_pass_declarations(
                runtime,
                backend_view,
                native.data(),
                native.size(),
                &count
            ),
            "D3D11 effect declaration read"
        );
    }
    std::vector<EffectPassDeclaration> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
        result.push_back(EffectPassDeclaration{
            copied(native[index].effect_id),
            native[index].index,
            native[index].kind == STRATA_EFFECT_PASS_SHADER
                ? EffectPassKind::shader
                : native[index].kind == STRATA_EFFECT_PASS_SHADOW
                    ? EffectPassKind::shadow
                    : EffectPassKind::blur,
            native[index].radius,
            native[index].downsample,
            native[index].radius_parameter == STRATA_EFFECT_PARAMETER_NONE
                ? std::nullopt
                : std::optional(native[index].radius_parameter),
            native[index].downsample_parameter == STRATA_EFFECT_PARAMETER_NONE
                ? std::nullopt
                : std::optional(native[index].downsample_parameter),
            copied(native[index].source),
        });
    }
    return result;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> resource(
    strata_runtime* const runtime,
    const std::string_view id
) {
    ByteCapture capture;
    const strata_bytes_sink sink{
        sizeof(strata_bytes_sink),
        &capture,
        &capture_bytes,
    };
    const strata_result result = strata_runtime_read_resource(
        runtime,
        strata_string_view{id.data(), id.size()},
        &sink
    );
    if (result.status == STRATA_STATUS_NOT_FOUND) return std::nullopt;
    require_ok(result, "D3D11 program resource read");
    if (capture.failed) throw std::bad_alloc();
    return std::move(capture.value);
}

} // namespace

struct Presenter::Impl final {
    struct Layer final {
        std::unique_ptr<host::SurfacePacketStream> stream;
    };

    Impl(
        strata_runtime* const runtime,
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        PresenterOptions options
    ) : runtime(runtime),
        source_loader(std::move(options.load_program_source)),
        renderer(device, context, options.renderer) {
        if (runtime == nullptr) {
            throw std::invalid_argument("D3D11 presenter requires a live Runtime");
        }
    }

    [[nodiscard]] std::string load_source(const std::string_view resource_id) {
        if (resource_id.empty()) return {};
        if (source_loader) return source_loader(resource_id);
        const std::optional<std::vector<std::uint8_t>> bytes =
            resource(runtime, resource_id);
        if (!bytes.has_value()) {
            throw std::runtime_error(
                "D3D11 program resource is unavailable: " + std::string(resource_id)
            );
        }
        return std::string(
            reinterpret_cast<const char*>(bytes->data()),
            bytes->size()
        );
    }

    void synchronize_programs() {
        std::vector<LoadedMaterial> materials;
        for (MaterialDeclaration declaration : material_declarations(runtime)) {
            materials.push_back(LoadedMaterial{
                declaration,
                load_source(declaration.source),
            });
        }
        std::vector<LoadedEffectPass> effects;
        for (EffectPassDeclaration declaration : effect_pass_declarations(runtime)) {
            effects.push_back(LoadedEffectPass{
                declaration,
                load_source(declaration.source),
            });
        }
        for (const LoadedMaterial& material : materials) {
            renderer.declare_material(material.declaration.id, material.source);
        }
        for (const LoadedEffectPass& effect : effects) {
            renderer.declare_effect_pass(
                effect.declaration.effect_id,
                effect.declaration.index,
                static_cast<std::uint32_t>(effect.declaration.kind),
                effect.declaration.radius,
                effect.declaration.downsample,
                effect.declaration.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                effect.declaration.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                effect.source
            );
        }
        programs_synchronized = true;
    }

    [[nodiscard]] bool reload_program_source(
        const std::string_view resource_id,
        const std::string_view hlsl_source
    ) {
        if (resource_id.empty()) {
            throw std::invalid_argument("D3D11 program resource id must not be empty");
        }
        bool declared = false;
        for (const MaterialDeclaration& declaration : material_declarations(runtime)) {
            if (declaration.source != resource_id) continue;
            renderer.declare_material(declaration.id, hlsl_source);
            declared = true;
        }
        for (const EffectPassDeclaration& pass : effect_pass_declarations(runtime)) {
            if (pass.source != resource_id) continue;
            renderer.declare_effect_pass(
                pass.effect_id,
                pass.index,
                static_cast<std::uint32_t>(pass.kind),
                pass.radius,
                pass.downsample,
                pass.radius_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                pass.downsample_parameter.value_or(STRATA_EFFECT_PARAMETER_NONE),
                hlsl_source
            );
            declared = true;
        }
        return declared;
    }

    void attach(const std::string_view layer_id, strata_surface* const surface) {
        if (layer_id.empty()) {
            throw std::invalid_argument("D3D11 presenter layer id must not be empty");
        }
        if (surface == nullptr) {
            throw std::invalid_argument("D3D11 presenter requires a live Surface");
        }
        const auto found = layers.find(layer_id);
        if (found != layers.end()) {
            if (!found->second.stream->matches(surface)) {
                throw std::invalid_argument(
                    "D3D11 presenter layer id is attached to another Surface"
                );
            }
            return;
        }
        layers.emplace(
            std::string(layer_id),
            Layer{std::make_unique<host::SurfacePacketStream>(surface)}
        );
    }

    [[nodiscard]] PresentedFrame present(
        const std::string_view layer_id,
        strata_surface* const surface,
        const RenderTarget& target,
        const std::int64_t time_nanoseconds,
        FrameOptions options
    ) {
        if (!programs_synchronized) synchronize_programs();
        attach(layer_id, surface);
        const auto entry = layers.find(layer_id);

        const host::SurfacePacketFrame frame = entry->second.stream->frame(time_nanoseconds);
        options.time_seconds = static_cast<double>(time_nanoseconds) / 1'000'000'000.0;
        return PresentedFrame{
            frame.surface,
            frame.packet_bytes,
            renderer.render(layer_id, *frame.packet, target, options),
        };
    }

    void detach(const std::string_view layer_id) {
        const auto found = layers.find(layer_id);
        if (found == layers.end()) return;
        const host::RenderPacket& packet = found->second.stream->prepare_release();
        renderer.consume_resources(packet);
        found->second.stream->acknowledge_release();
        renderer.release_layer(layer_id);
        layers.erase(found);
    }

    void discard(const std::string_view layer_id) noexcept {
        const auto found = layers.find(layer_id);
        if (found == layers.end()) return;
        renderer.release_layer(layer_id);
        layers.erase(found);
    }

    strata_runtime* runtime = nullptr;
    ProgramSourceLoader source_loader;
    Renderer renderer;
    std::map<std::string, Layer, std::less<>> layers;
    bool programs_synchronized = false;
};

Presenter::Presenter(
    Runtime& runtime,
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    PresenterOptions options
) : Presenter(runtime.native_handle(), device, context, std::move(options)) {}

Presenter::Presenter(
    strata_runtime* const runtime,
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    PresenterOptions options
) : impl_(std::make_unique<Impl>(runtime, device, context, std::move(options))) {}

Presenter::~Presenter() = default;
Presenter::Presenter(Presenter&&) noexcept = default;
Presenter& Presenter::operator=(Presenter&&) noexcept = default;

void Presenter::synchronize_programs() {
    impl_->synchronize_programs();
}

bool Presenter::reload_program_source(
    const std::string_view resource_id,
    const std::string_view hlsl_source
) {
    return impl_->reload_program_source(resource_id, hlsl_source);
}

void Presenter::attach(const std::string_view layer_id, Surface& surface) {
    attach(layer_id, surface.native_handle());
}

void Presenter::attach(
    const std::string_view layer_id,
    strata_surface* const surface
) {
    impl_->attach(layer_id, surface);
}

PresentedFrame Presenter::present(
    const std::string_view layer_id,
    Surface& surface,
    const RenderTarget& target,
    const std::int64_t time_nanoseconds,
    const FrameOptions options
) {
    return present(
        layer_id,
        surface.native_handle(),
        target,
        time_nanoseconds,
        options
    );
}

PresentedFrame Presenter::present(
    const std::string_view layer_id,
    strata_surface* const surface,
    const RenderTarget& target,
    const std::int64_t time_nanoseconds,
    const FrameOptions options
) {
    return impl_->present(layer_id, surface, target, time_nanoseconds, options);
}

void Presenter::detach(const std::string_view layer_id) {
    impl_->detach(layer_id);
}

void Presenter::discard(const std::string_view layer_id) noexcept {
    impl_->discard(layer_id);
}

bool Presenter::attached(const std::string_view layer_id) const noexcept {
    return impl_->layers.contains(layer_id);
}

void Presenter::release_target() {
    impl_->renderer.release_target();
}

} // namespace strata::d3d11
