#include "render_context.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "blur_pass.hpp"
#include "clip_mask.hpp"
#include "effect_pass.hpp"
#include "shaders.hpp"
#include "texture_store.hpp"
#include <strata/render_packet.hpp>

namespace strata::d3d11 {
namespace {

using host::BlurBatch;
using host::DrawBatch;
using host::EffectBatch;
using host::EffectBatchKind;
using host::RenderPacket;
using host::ResourceOperation;
using host::SubmissionBatch;
using Microsoft::WRL::ComPtr;

[[noreturn]] void fail_hresult(const std::string_view operation, const HRESULT result) {
    throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                             std::to_string(result));
}

void require_hresult(const HRESULT result, const std::string_view operation) {
    if (FAILED(result))
        fail_hresult(operation, result);
}

[[nodiscard]] ComPtr<ID3DBlob> compile_shader(const std::string_view source,
                                              const char* const entry, const char* const target) {
    ComPtr<ID3DBlob> result;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT status =
        D3DCompile(source.data(), source.size(), "strata.d3d11.generated.hlsl", nullptr, nullptr,
                   entry, target, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U,
                   &result, &diagnostics);
    if (FAILED(status)) {
        const std::string detail =
            diagnostics == nullptr
                ? std::string{}
                : std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                              diagnostics->GetBufferSize());
        throw std::runtime_error("D3D11 shader compilation failed: " + detail);
    }
    return result;
}

[[nodiscard]] std::uint32_t next_capacity(const std::size_t required) {
    if (required > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("D3D11 GPU buffer exceeds the byte-width limit");
    }
    std::uint64_t capacity = 4'096U;
    while (capacity < required)
        capacity *= 2U;
    if (capacity > std::numeric_limits<std::uint32_t>::max())
        capacity = required;
    return static_cast<std::uint32_t>(capacity);
}

} // namespace

struct RenderContext::Impl final {
    struct EffectPassDeclaration final {
        std::uint32_t kind = 0U;
        double radius = 0.0;
        std::uint32_t downsample = 1U;
        std::uint32_t radius_parameter = 0U;
        std::uint32_t downsample_parameter = 0U;
        std::string source;

        [[nodiscard]] friend bool operator==(
            const EffectPassDeclaration&,
            const EffectPassDeclaration&
        ) = default;
    };

    struct GeometryBuffers final {
        static constexpr std::size_t ring_size = 3U;
        std::array<ComPtr<ID3D11Buffer>, ring_size> vertices;
        std::array<ComPtr<ID3D11Buffer>, ring_size> indices;
        std::uint32_t vertex_capacity = 0U;
        std::uint32_t index_capacity = 0U;
        std::size_t active = 0U;
        std::optional<std::uint64_t> epoch;
    };

    struct MaterialProgram final {
        std::shared_future<ComPtr<ID3DBlob>> pending;
        ComPtr<ID3D11PixelShader> shader;
    };

    Impl(
        ID3D11Device* const device,
        ID3D11DeviceContext* const context,
        const bool asynchronous_shader_compilation
    )
        : device(device),
          context(context),
          asynchronous_shader_compilation(asynchronous_shader_compilation) {
        if (device == nullptr || context == nullptr) {
            throw std::invalid_argument("D3D11 render context requires a device and context");
        }
        create_pipeline();
        rounded_clips = std::make_unique<RoundedClipBuffer>(device, context);
        textures = std::make_unique<TextureStore>(device, context);
        blur = std::make_unique<BlurPass>(device, context);
        effects = std::make_unique<EffectPassRenderer>(
            device,
            context,
            asynchronous_shader_compilation
        );
    }

    void create_pipeline() {
        const ComPtr<ID3DBlob> vertex_bytecode = compile_shader(shaders::vertex, "main", "vs_5_0");
        const ComPtr<ID3DBlob> pixel_bytecode =
            compile_shader(std::string(shaders::pixel_common) +
                               std::string(rounded_clip_hlsl) +
                               std::string(shaders::builtin_entry),
                           "main", "ps_5_0");
        require_hresult(device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
                                                   vertex_bytecode->GetBufferSize(), nullptr,
                                                   &vertex_shader),
                        "D3D11 vertex shader creation");
        require_hresult(device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
                                                  pixel_bytecode->GetBufferSize(), nullptr,
                                                  &pixel_shader),
                        "D3D11 pixel shader creation");
        const std::array layout{
            D3D11_INPUT_ELEMENT_DESC{"POSITION", 0U, DXGI_FORMAT_R32G32B32_FLOAT, 0U, 0U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
            D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 0U, DXGI_FORMAT_R32G32_FLOAT, 0U, 12U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
            D3D11_INPUT_ELEMENT_DESC{"COLOR", 0U, DXGI_FORMAT_R8G8B8A8_UNORM, 0U, 20U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
            D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 1U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 24U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
            D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 2U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 40U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
            D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 3U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 56U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
            D3D11_INPUT_ELEMENT_DESC{"TEXCOORD", 4U, DXGI_FORMAT_R32G32B32A32_FLOAT, 0U, 72U,
                                     D3D11_INPUT_PER_VERTEX_DATA, 0U},
        };
        require_hresult(device->CreateInputLayout(layout.data(), static_cast<UINT>(layout.size()),
                                                  vertex_bytecode->GetBufferPointer(),
                                                  vertex_bytecode->GetBufferSize(), &input_layout),
                        "D3D11 vertex input layout creation");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = 32U;
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        require_hresult(device->CreateBuffer(&constant_desc, nullptr, &viewport_buffer),
                        "D3D11 viewport constant buffer creation");

        D3D11_RASTERIZER_DESC rasterizer_desc{};
        rasterizer_desc.FillMode = D3D11_FILL_SOLID;
        rasterizer_desc.CullMode = D3D11_CULL_NONE;
        rasterizer_desc.ScissorEnable = TRUE;
        rasterizer_desc.DepthClipEnable = TRUE;
        require_hresult(device->CreateRasterizerState(&rasterizer_desc, &rasterizer),
                        "D3D11 rasterizer creation");
    }

    void set_target(ID3D11Texture2D* const next_texture, ID3D11RenderTargetView* const next_target,
                    const std::uint32_t next_width, const std::uint32_t next_height,
                    const double next_logical_width, const double next_logical_height) {
        if (next_texture == nullptr || next_target == nullptr || next_width == 0U ||
            next_height == 0U || !std::isfinite(next_logical_width) ||
            !std::isfinite(next_logical_height) || next_logical_width <= 0.0 ||
            next_logical_height <= 0.0) {
            throw std::invalid_argument("D3D11 render target dimensions must be positive");
        }
        const bool target_changed = target_texture.Get() != next_texture;
        context->OMSetRenderTargets(0U, nullptr, nullptr);
        ID3D11ShaderResourceView* const no_resource = nullptr;
        context->PSSetShaderResources(0U, 1U, &no_resource);
        target_texture = next_texture;
        target = next_target;
        width = next_width;
        height = next_height;
        logical_width = next_logical_width;
        logical_height = next_logical_height;
        D3D11_TEXTURE2D_DESC target_description{};
        next_texture->GetDesc(&target_description);
        if (target_description.Width != width || target_description.Height != height) {
            throw std::invalid_argument("D3D11 render target dimensions do not match its texture");
        }
        D3D11_RENDER_TARGET_VIEW_DESC target_view_description{};
        next_target->GetDesc(&target_view_description);
        const DXGI_FORMAT target_format = target_view_description.Format != DXGI_FORMAT_UNKNOWN
                                              ? target_view_description.Format
                                              : target_description.Format;
        blur->resize(width, height, target_format);
        effects->resize(width, height, target_format);
        if (target_changed)
            effects->invalidate_cache();
    }

    void release_target() {
        context->ClearState();
        context->Flush();
        target.Reset();
        target_texture.Reset();
        width = 0U;
        height = 0U;
        logical_width = 0.0;
        logical_height = 0.0;
        blur->resize(0U, 0U, DXGI_FORMAT_UNKNOWN);
        effects->resize(0U, 0U, DXGI_FORMAT_UNKNOWN);
    }

    [[nodiscard]] ComPtr<ID3D11Buffer> create_geometry_buffer(
        const std::uint32_t capacity,
        const UINT bind
    ) const {
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = capacity;
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = bind;
        ComPtr<ID3D11Buffer> result;
        require_hresult(device->CreateBuffer(&descriptor, nullptr, &result),
                        "D3D11 GPU buffer creation");
        return result;
    }

    void ensure_geometry_buffers(
        GeometryBuffers& buffers,
        const std::size_t required_vertices,
        const std::size_t required_indices
    ) {
        const std::uint32_t vertex_capacity =
            required_vertices > buffers.vertex_capacity ||
                    buffers.vertex_capacity == 0U
                ? next_capacity(required_vertices)
                : buffers.vertex_capacity;
        const std::uint32_t index_capacity =
            required_indices > buffers.index_capacity ||
                    buffers.index_capacity == 0U
                ? next_capacity(required_indices)
                : buffers.index_capacity;
        if (vertex_capacity == buffers.vertex_capacity &&
            index_capacity == buffers.index_capacity &&
            buffers.vertices.front() != nullptr &&
            buffers.indices.front() != nullptr) {
            return;
        }
        for (std::size_t index = 0U; index < GeometryBuffers::ring_size; ++index) {
            buffers.vertices[index] = create_geometry_buffer(
                vertex_capacity,
                D3D11_BIND_VERTEX_BUFFER
            );
            buffers.indices[index] = create_geometry_buffer(
                index_capacity,
                D3D11_BIND_INDEX_BUFFER
            );
        }
        buffers.vertex_capacity = vertex_capacity;
        buffers.index_capacity = index_capacity;
        buffers.active = 0U;
        buffers.epoch.reset();
    }

    void upload_geometry(
        ID3D11Buffer* const buffer,
        const void* const bytes,
        const std::size_t size
    ) const {
        if (size == 0U) return;
        if (size > std::numeric_limits<UINT>::max()) {
            throw std::length_error("D3D11 geometry upload exceeds buffer range");
        }
        const D3D11_BOX range{
            0U,
            0U,
            0U,
            static_cast<UINT>(size),
            1U,
            1U,
        };
        context->UpdateSubresource(buffer, 0U, &range, bytes, 0U, 0U);
    }

    void patch_geometry(
        ID3D11Buffer* const buffer,
        const std::span<const host::GeometryPatch> patches
    ) const {
        for (const host::GeometryPatch& patch : patches) {
            if (patch.bytes.empty()) continue;
            if (patch.bytes.size() >
                std::numeric_limits<UINT>::max() - patch.offset) {
                throw std::length_error("D3D11 geometry patch exceeds buffer range");
            }
            const D3D11_BOX range{
                patch.offset,
                0U,
                0U,
                patch.offset + static_cast<UINT>(patch.bytes.size()),
                1U,
                1U,
            };
            context->UpdateSubresource(
                buffer,
                0U,
                &range,
                patch.bytes.data(),
                0U,
                0U
            );
        }
    }

    [[nodiscard]] ID3D11BlendState* blend_state(const std::string_view id) {
        const auto existing = blends.find(std::string(id));
        if (existing != blends.end())
            return existing->second.Get();
        D3D11_BLEND_DESC descriptor{};
        descriptor.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        auto& target_blend = descriptor.RenderTarget[0];
        if (id != "opaque") {
            target_blend.BlendEnable = TRUE;
            if (id == "straight_alpha") {
                target_blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
                target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            } else if (id == "premultiplied_alpha") {
                target_blend.SrcBlend = D3D11_BLEND_ONE;
                target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            } else if (id == "additive") {
                target_blend.SrcBlend = D3D11_BLEND_SRC_ALPHA;
                target_blend.DestBlend = D3D11_BLEND_ONE;
            } else if (id == "multiply") {
                target_blend.SrcBlend = D3D11_BLEND_DEST_COLOR;
                target_blend.DestBlend = D3D11_BLEND_ZERO;
            } else if (id == "rounded_multiply") {
                target_blend.SrcBlend = D3D11_BLEND_DEST_COLOR;
                target_blend.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
            } else {
                throw std::invalid_argument("D3D11 renderer received an unknown blend mode");
            }
            target_blend.BlendOp = D3D11_BLEND_OP_ADD;
            target_blend.SrcBlendAlpha = D3D11_BLEND_ONE;
            target_blend.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
            target_blend.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        ComPtr<ID3D11BlendState> result;
        require_hresult(device->CreateBlendState(&descriptor, &result),
                        "D3D11 blend state creation");
        ID3D11BlendState* const raw = result.Get();
        blends.emplace(std::string(id), std::move(result));
        return raw;
    }

    void declare_material(const std::string_view id, const std::string_view hlsl_source) {
        const auto declared = material_sources.find(id);
        if (declared != material_sources.end() && declared->second == hlsl_source) return;
        const std::string source = std::string(shaders::pixel_common) +
                                   std::string(rounded_clip_hlsl) +
                                   std::string(shaders::material_prelude) +
                                   std::string(hlsl_source) + std::string(shaders::material_entry);
        auto [program, inserted] = material_programs.try_emplace(source);
        if (inserted) {
            if (asynchronous_shader_compilation) {
                program->second.pending = std::async(
                    std::launch::async,
                    [source] {
                        return compile_shader(source, "main", "ps_5_0");
                    }
                ).share();
            } else {
                const ComPtr<ID3DBlob> bytecode =
                    compile_shader(source, "main", "ps_5_0");
                require_hresult(
                    device->CreatePixelShader(
                        bytecode->GetBufferPointer(),
                        bytecode->GetBufferSize(),
                        nullptr,
                        &program->second.shader
                    ),
                    "D3D11 material pixel shader creation"
                );
            }
        }
        material_sources.insert_or_assign(std::string(id), std::string(hlsl_source));
        material_program_ids.insert_or_assign(std::string(id), source);
    }

    void complete_material_programs() {
        bool completed = false;
        for (auto current = material_programs.begin();
             current != material_programs.end();) {
            const bool referenced = std::ranges::any_of(
                material_program_ids,
                [&current](const auto& entry) {
                    return entry.second == current->first;
                }
            );
            if (!referenced && current->second.pending.valid() &&
                current->second.pending.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready) {
                ++current;
                continue;
            }
            if (!referenced) {
                current = material_programs.erase(current);
                continue;
            }
            MaterialProgram& program = current->second;
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
                "D3D11 material pixel shader creation"
            );
            program.pending = {};
            completed = true;
            ++current;
        }
        // Backdrop/content samples captured while a material still used the fallback shader no
        // longer represent the layer after that authored material becomes available.
        if (completed) effects->invalidate_cache();
    }

    void declare_effect_pass(const std::string_view effect_id, const std::uint32_t index,
                             const std::uint32_t kind, const double radius,
                             const std::uint32_t downsample, const std::uint32_t radius_parameter,
                             const std::uint32_t downsample_parameter,
                             const std::string_view hlsl_source) {
        const EffectPassDeclaration declaration{
            kind,
            radius,
            downsample,
            radius_parameter,
            downsample_parameter,
            std::string(hlsl_source),
        };
        const std::pair<std::string, std::uint32_t> key{
            std::string(effect_id),
            index,
        };
        const auto declared = effect_pass_declarations.find(key);
        if (declared != effect_pass_declarations.end() &&
            declared->second == declaration) {
            return;
        }
        effects->declare_pass(effect_id, index, kind, radius, downsample, radius_parameter,
                              downsample_parameter, hlsl_source);
        effect_pass_declarations.insert_or_assign(key, declaration);
    }

    void draw(const DrawBatch& batch) {
        if (batch.index_count == 0U)
            return;
        ID3D11PixelShader* selected_shader = pixel_shader.Get();
        if (const auto id = material_program_ids.find(batch.material);
            id != material_program_ids.end()) {
            if (const auto program = material_programs.find(id->second);
                program != material_programs.end() &&
                program->second.shader != nullptr) {
                selected_shader = program->second.shader.Get();
            }
        }
        context->PSSetShader(selected_shader, nullptr, 0U);
        const std::uint64_t right =
            static_cast<std::uint64_t>(batch.scissor.x) + batch.scissor.width;
        const std::uint64_t bottom =
            static_cast<std::uint64_t>(batch.scissor.y) + batch.scissor.height;
        const D3D11_RECT scissor{
            static_cast<LONG>(std::min<std::uint64_t>(batch.scissor.x, width)),
            static_cast<LONG>(std::min<std::uint64_t>(batch.scissor.y, height)),
            static_cast<LONG>(std::min<std::uint64_t>(right, width)),
            static_cast<LONG>(std::min<std::uint64_t>(bottom, height)),
        };
        context->RSSetScissorRects(1U, &scissor);
        const bool clipped = !batch.rounded_clips.empty();
        const std::string_view resolved_blend =
            clipped && batch.blend_mode == "opaque"
                ? std::string_view("straight_alpha")
                : clipped && batch.blend_mode == "multiply"
                    ? std::string_view("rounded_multiply")
                    : std::string_view(batch.blend_mode);
        context->OMSetBlendState(blend_state(resolved_blend), nullptr, 0xffffffffU);
        const RoundedClipMode clip_mode =
            batch.blend_mode == "premultiplied_alpha"
                ? RoundedClipMode::premultiplied_alpha
                : clipped && batch.blend_mode == "multiply"
                    ? RoundedClipMode::multiply
                    : !clipped || batch.blend_mode == "multiply"
                        ? RoundedClipMode::hard
                        : RoundedClipMode::straight_alpha;
        rounded_clips->bind(batch.rounded_clips, clip_mode);
        textures->bind(batch);
        context->DrawIndexed(batch.index_count, batch.first_index,
                             static_cast<INT>(batch.base_vertex));
    }

    void bind_draw_pipeline(const GeometryBuffers& buffers,
                            ID3D11RenderTargetView* const target_view) const {
        if (target_view == nullptr) {
            throw std::invalid_argument("D3D11 draw pipeline requires a render target");
        }
        context->OMSetRenderTargets(1U, &target_view, nullptr);
        const D3D11_VIEWPORT viewport{
            0.0F, 0.0F, static_cast<float>(width), static_cast<float>(height), 0.0F, 1.0F,
        };
        context->RSSetViewports(1U, &viewport);
        context->RSSetState(rasterizer.Get());
        context->IASetInputLayout(input_layout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        constexpr UINT stride = 88U;
        constexpr UINT offset = 0U;
        ID3D11Buffer* const vertex = buffers.vertices[buffers.active].Get();
        context->IASetVertexBuffers(0U, 1U, &vertex, &stride, &offset);
        context->IASetIndexBuffer(
            buffers.indices[buffers.active].Get(),
            DXGI_FORMAT_R32_UINT,
            0U
        );
        context->VSSetShader(vertex_shader.Get(), nullptr, 0U);
        ID3D11Buffer* const constants = viewport_buffer.Get();
        context->VSSetConstantBuffers(0U, 1U, &constants);
        context->PSSetConstantBuffers(0U, 1U, &constants);
        context->PSSetShader(pixel_shader.Get(), nullptr, 0U);
    }

    void consume_resources(const RenderPacket& packet) {
        for (const ResourceOperation& operation : packet.resources)
            textures->apply(operation);
    }

    void begin_frame(const std::array<float, 4U> clear_color, const double frame_seconds) {
        if (target == nullptr)
            throw std::logic_error("D3D11 render target is not configured");
        complete_material_programs();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(
            context->Map(viewport_buffer.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
            "D3D11 viewport constant mapping");
        const std::array<float, 8U> frame_values{
            static_cast<float>(logical_width),
            static_cast<float>(logical_height),
            static_cast<float>(width),
            static_cast<float>(height),
            static_cast<float>(frame_seconds),
            0.0F,
            0.0F,
            0.0F,
        };
        std::memcpy(mapped.pData, frame_values.data(), sizeof(frame_values));
        context->Unmap(viewport_buffer.Get(), 0U);
        context->ClearRenderTargetView(target.Get(), clear_color.data());
        current_frame_seconds = frame_seconds;
    }

    [[nodiscard]] RenderLayerTelemetry render_layer(const std::string_view id,
                                                    const RenderPacket& packet) {
        consume_resources(packet);
        if (target == nullptr)
            throw std::logic_error("D3D11 render target is not configured");
        RenderLayerTelemetry telemetry;
        GeometryBuffers& retained = geometry[std::string(id)];
        if (!retained.epoch.has_value() || *retained.epoch != packet.geometry_epoch) {
            const std::size_t index_bytes = packet.indices.size() * sizeof(std::uint32_t);
            ensure_geometry_buffers(retained, packet.vertices.size(), index_bytes);
            const bool incremental =
                retained.epoch.has_value() && !packet.full_geometry_payload;
            if (incremental &&
                (!packet.vertex_patches.empty() || !packet.index_patches.empty())) {
                const std::size_t next =
                    (retained.active + 1U) % GeometryBuffers::ring_size;
                context->CopyResource(
                    retained.vertices[next].Get(),
                    retained.vertices[retained.active].Get()
                );
                context->CopyResource(
                    retained.indices[next].Get(),
                    retained.indices[retained.active].Get()
                );
                patch_geometry(retained.vertices[next].Get(), packet.vertex_patches);
                patch_geometry(retained.indices[next].Get(), packet.index_patches);
                retained.active = next;
            } else if (!incremental) {
                const std::size_t next = retained.epoch.has_value()
                    ? (retained.active + 1U) % GeometryBuffers::ring_size
                    : retained.active;
                upload_geometry(
                    retained.vertices[next].Get(),
                    packet.vertices.data(),
                    packet.vertices.size()
                );
                upload_geometry(
                    retained.indices[next].Get(),
                    packet.indices.data(),
                    index_bytes
                );
                retained.active = next;
            }
            retained.epoch = packet.geometry_epoch;
        }
        effects->begin_layer(
            id,
            packet.geometry_epoch,
            packet.geometry_dirty_all,
            packet.geometry_dirty_regions
        );
        bind_draw_pipeline(retained, target.Get());
        struct ContentLayer final {
            EffectBatch effect;
            ID3D11Texture2D* parent_texture = nullptr;
            ID3D11RenderTargetView* parent_target = nullptr;
            std::size_t depth = 0U;
        };
        std::vector<ContentLayer> content_layers;
        ID3D11Texture2D* active_texture = target_texture.Get();
        ID3D11RenderTargetView* active_target = target.Get();
        const auto add_effect_telemetry = [&telemetry](const EffectPassTelemetry& measured) {
            telemetry.effect_passes += measured.passes;
            telemetry.effect_target_width =
                std::max(telemetry.effect_target_width, measured.target_width);
            telemetry.effect_target_height =
                std::max(telemetry.effect_target_height, measured.target_height);
            telemetry.effect_nanos += measured.nanos;
            // The public ABI's blur telemetry is the aggregate filtered-region budget.
            // Authored effect programs participate in that same budget so existing hosts
            // observe their GPU cost without a parallel instrumentation channel.
            telemetry.blur_passes += measured.passes;
            telemetry.blur_target_width =
                std::max(telemetry.blur_target_width, measured.target_width);
            telemetry.blur_target_height =
                std::max(telemetry.blur_target_height, measured.target_height);
            telemetry.blur_nanos += measured.nanos;
        };
        for (const SubmissionBatch& batch : packet.batches) {
            if (const auto* draw_batch = std::get_if<DrawBatch>(&batch); draw_batch != nullptr) {
                draw(*draw_batch);
            } else if (const auto* blur_batch = std::get_if<BlurBatch>(&batch);
                       blur_batch != nullptr) {
                const BlurPassTelemetry measured =
                    blur->execute(*blur_batch, active_texture, active_target, width, height,
                                  logical_width, logical_height);
                telemetry.blur_passes += measured.passes;
                telemetry.blur_target_width =
                    std::max(telemetry.blur_target_width, measured.target_width);
                telemetry.blur_target_height =
                    std::max(telemetry.blur_target_height, measured.target_height);
                telemetry.blur_nanos += measured.nanos;
                bind_draw_pipeline(retained, active_target);
            } else if (const auto* backdrop_effect = std::get_if<EffectBatch>(&batch);
                       backdrop_effect != nullptr &&
                       backdrop_effect->kind == EffectBatchKind::backdrop) {
                add_effect_telemetry(effects->apply_backdrop(
                    id, content_layers.size(), *backdrop_effect, active_texture, active_target,
                    logical_width, logical_height, current_frame_seconds));
                bind_draw_pipeline(retained, active_target);
            } else if (const auto* content_effect = std::get_if<EffectBatch>(&batch);
                       content_effect != nullptr &&
                       content_effect->kind == EffectBatchKind::content_begin) {
                const std::size_t depth = content_layers.size();
                content_layers.push_back(ContentLayer{
                    *content_effect,
                    active_texture,
                    active_target,
                    depth,
                });
                active_target = effects->begin_content(depth);
                active_texture = effects->content_texture(depth);
                bind_draw_pipeline(retained, active_target);
            } else {
                if (!std::holds_alternative<host::ContentEffectEndBatch>(batch) ||
                    content_layers.empty()) {
                    throw std::logic_error("D3D11 content effect stack is invalid");
                }
                ContentLayer layer = std::move(content_layers.back());
                content_layers.pop_back();
                add_effect_telemetry(effects->finish_content(
                    id, layer.depth, layer.effect, layer.parent_texture, layer.parent_target,
                    logical_width, logical_height, current_frame_seconds));
                active_texture = layer.parent_texture;
                active_target = layer.parent_target;
                bind_draw_pipeline(retained, active_target);
            }
        }
        if (!content_layers.empty()) {
            throw std::logic_error("D3D11 content effect stack is unbalanced");
        }
        return telemetry;
    }

    void release_layer(const std::string_view id) noexcept {
        const auto found = geometry.find(id);
        if (found != geometry.end())
            geometry.erase(found);
        effects->release_layer(id);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target_texture;
    ComPtr<ID3D11RenderTargetView> target;
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> pixel_shader;
    ComPtr<ID3D11InputLayout> input_layout;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11Buffer> viewport_buffer;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    double logical_width = 0.0;
    double logical_height = 0.0;
    double current_frame_seconds = 0.0;
    std::map<std::string, GeometryBuffers, std::less<>> geometry;
    std::map<std::string, ComPtr<ID3D11BlendState>, std::less<>> blends;
    std::map<std::string, std::string, std::less<>> material_sources;
    std::map<std::string, std::string, std::less<>> material_program_ids;
    std::map<std::string, MaterialProgram, std::less<>> material_programs;
    std::map<
        std::pair<std::string, std::uint32_t>,
        EffectPassDeclaration
    > effect_pass_declarations;
    std::unique_ptr<TextureStore> textures;
    std::unique_ptr<RoundedClipBuffer> rounded_clips;
    std::unique_ptr<BlurPass> blur;
    std::unique_ptr<EffectPassRenderer> effects;
    bool asynchronous_shader_compilation = false;
};

RenderContext::RenderContext(
    ID3D11Device* const device,
    ID3D11DeviceContext* const context,
    const bool asynchronous_shader_compilation
)
    : impl_(std::make_unique<Impl>(
          device,
          context,
          asynchronous_shader_compilation
      )) {}

RenderContext::~RenderContext() = default;

void RenderContext::set_target(ID3D11Texture2D* const texture, ID3D11RenderTargetView* const target,
                               const std::uint32_t framebuffer_width,
                               const std::uint32_t framebuffer_height, const double logical_width,
                               const double logical_height) {
    impl_->set_target(texture, target, framebuffer_width, framebuffer_height, logical_width,
                      logical_height);
}

void RenderContext::release_target() {
    impl_->release_target();
}

void RenderContext::declare_material(const std::string_view id,
                                     const std::string_view hlsl_source) {
    impl_->declare_material(id, hlsl_source);
}

void RenderContext::declare_effect_pass(const std::string_view effect_id, const std::uint32_t index,
                                        const std::uint32_t kind, const double radius,
                                        const std::uint32_t downsample,
                                        const std::uint32_t radius_parameter,
                                        const std::uint32_t downsample_parameter,
                                        const std::string_view hlsl_source) {
    impl_->declare_effect_pass(effect_id, index, kind, radius, downsample, radius_parameter,
                               downsample_parameter, hlsl_source);
}

void RenderContext::consume_resources(const host::RenderPacket& packet) {
    impl_->consume_resources(packet);
}

void RenderContext::begin_frame(const std::array<float, 4U> clear_color,
                                const double frame_seconds) {
    impl_->begin_frame(clear_color, frame_seconds);
}

RenderLayerTelemetry RenderContext::render_layer(const std::string_view id,
                                                 const host::RenderPacket& packet) {
    return impl_->render_layer(id, packet);
}

void RenderContext::release_layer(const std::string_view id) noexcept {
    impl_->release_layer(id);
}

} // namespace strata::d3d11
