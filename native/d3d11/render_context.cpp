#include "render_context.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
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
#include <strata/render_packet.hpp>
#include "shaders.hpp"
#include "texture_store.hpp"

namespace strata::d3d11 {
namespace {

using host::BlurBatch;
using host::DrawBatch;
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
    struct GeometryBuffers final {
        ComPtr<ID3D11Buffer> vertices;
        ComPtr<ID3D11Buffer> indices;
        std::uint32_t vertex_capacity = 0U;
        std::uint32_t index_capacity = 0U;
        std::optional<std::uint64_t> epoch;
    };

    Impl(ID3D11Device* const device, ID3D11DeviceContext* const context)
        : device(device), context(context) {
        if (device == nullptr || context == nullptr) {
            throw std::invalid_argument("D3D11 render context requires a device and context");
        }
        create_pipeline();
        textures = std::make_unique<TextureStore>(device, context);
        blur = std::make_unique<BlurPass>(device, context);
    }

    void create_pipeline() {
        const ComPtr<ID3DBlob> vertex_bytecode = compile_shader(shaders::vertex, "main", "vs_5_0");
        const ComPtr<ID3DBlob> pixel_bytecode =
            compile_shader(std::string(shaders::pixel_common) + std::string(shaders::builtin_entry),
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
        constant_desc.ByteWidth = 16U;
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
        context->OMSetRenderTargets(0U, nullptr, nullptr);
        ID3D11ShaderResourceView* const no_resource = nullptr;
        context->PSSetShaderResources(0U, 1U, &no_resource);
        target_texture = next_texture;
        target = next_target;
        width = next_width;
        height = next_height;
        logical_width = next_logical_width;
        logical_height = next_logical_height;
        blur->resize(width, height);
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
    }

    void ensure_buffer(ComPtr<ID3D11Buffer>& buffer, std::uint32_t& capacity,
                       const std::size_t required, const UINT bind) {
        if (required == 0U || required <= capacity)
            return;
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = next_capacity(required);
        descriptor.Usage = D3D11_USAGE_DYNAMIC;
        descriptor.BindFlags = bind;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ComPtr<ID3D11Buffer> replacement;
        require_hresult(device->CreateBuffer(&descriptor, nullptr, &replacement),
                        "D3D11 GPU buffer creation");
        buffer = std::move(replacement);
        capacity = descriptor.ByteWidth;
    }

    void upload(ID3D11Buffer* const buffer, const void* const bytes, const std::size_t size) const {
        if (size == 0U)
            return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(context->Map(buffer, 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                        "D3D11 GPU buffer mapping");
        std::memcpy(mapped.pData, bytes, size);
        context->Unmap(buffer, 0U);
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
        const std::string source = std::string(shaders::pixel_common) +
                                   std::string(shaders::material_prelude) +
                                   std::string(hlsl_source) + std::string(shaders::material_entry);
        const ComPtr<ID3DBlob> bytecode = compile_shader(source, "main", "ps_5_0");
        ComPtr<ID3D11PixelShader> shader;
        require_hresult(device->CreatePixelShader(bytecode->GetBufferPointer(),
                                                  bytecode->GetBufferSize(), nullptr, &shader),
                        "D3D11 material pixel shader creation");
        material_shaders.insert_or_assign(std::string(id), std::move(shader));
    }

    void draw(const DrawBatch& batch) {
        if (batch.index_count == 0U)
            return;
        const auto declared = material_shaders.find(batch.material);
        context->PSSetShader(declared != material_shaders.end() ? declared->second.Get()
                                                                : pixel_shader.Get(),
                             nullptr, 0U);
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
        context->OMSetBlendState(blend_state(batch.blend_mode), nullptr, 0xffffffffU);
        textures->bind(batch);
        context->DrawIndexed(batch.index_count, batch.first_index,
                             static_cast<INT>(batch.base_vertex));
    }

    void bind_draw_pipeline(const GeometryBuffers& buffers) const {
        ID3D11RenderTargetView* const target_view = target.Get();
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
        ID3D11Buffer* const vertex = buffers.vertices.Get();
        context->IASetVertexBuffers(0U, 1U, &vertex, &stride, &offset);
        context->IASetIndexBuffer(buffers.indices.Get(), DXGI_FORMAT_R32_UINT, 0U);
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

    void begin_frame(const std::array<float, 4U> clear_color, const float frame_seconds) {
        if (target == nullptr)
            throw std::logic_error("D3D11 render target is not configured");
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(
            context->Map(viewport_buffer.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
            "D3D11 viewport constant mapping");
        const std::array<float, 4U> frame_values{
            static_cast<float>(logical_width),
            static_cast<float>(logical_height),
            frame_seconds,
            0.0F,
        };
        std::memcpy(mapped.pData, frame_values.data(), sizeof(frame_values));
        context->Unmap(viewport_buffer.Get(), 0U);
        context->ClearRenderTargetView(target.Get(), clear_color.data());
    }

    [[nodiscard]] RenderLayerTelemetry render_layer(const std::string_view id,
                                                    const RenderPacket& packet) {
        consume_resources(packet);
        if (target == nullptr)
            throw std::logic_error("D3D11 render target is not configured");
        RenderLayerTelemetry telemetry;
        GeometryBuffers& retained = geometry[std::string(id)];
        if (!retained.epoch.has_value() || *retained.epoch != packet.geometry_epoch) {
            ensure_buffer(retained.vertices, retained.vertex_capacity, packet.vertices.size(),
                          D3D11_BIND_VERTEX_BUFFER);
            const std::size_t index_bytes = packet.indices.size() * sizeof(std::uint32_t);
            ensure_buffer(retained.indices, retained.index_capacity, index_bytes,
                          D3D11_BIND_INDEX_BUFFER);
            upload(retained.vertices.Get(), packet.vertices.data(), packet.vertices.size());
            upload(retained.indices.Get(), packet.indices.data(), index_bytes);
            retained.epoch = packet.geometry_epoch;
        }
        bind_draw_pipeline(retained);
        for (const SubmissionBatch& batch : packet.batches) {
            if (const auto* value = std::get_if<DrawBatch>(&batch); value != nullptr) {
                draw(*value);
            } else {
                const BlurPassTelemetry measured =
                    blur->execute(std::get<BlurBatch>(batch), target_texture.Get(), target.Get(),
                                  width, height, logical_width, logical_height);
                telemetry.blur_passes += measured.passes;
                telemetry.blur_target_width =
                    std::max(telemetry.blur_target_width, measured.target_width);
                telemetry.blur_target_height =
                    std::max(telemetry.blur_target_height, measured.target_height);
                telemetry.blur_nanos += measured.nanos;
                bind_draw_pipeline(retained);
            }
        }
        return telemetry;
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
    std::map<std::string, GeometryBuffers, std::less<>> geometry;
    std::map<std::string, ComPtr<ID3D11BlendState>, std::less<>> blends;
    std::map<std::string, ComPtr<ID3D11PixelShader>, std::less<>> material_shaders;
    std::unique_ptr<TextureStore> textures;
    std::unique_ptr<BlurPass> blur;
};

RenderContext::RenderContext(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : impl_(std::make_unique<Impl>(device, context)) {}

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

void RenderContext::consume_resources(const host::RenderPacket& packet) {
    impl_->consume_resources(packet);
}

void RenderContext::begin_frame(const std::array<float, 4U> clear_color,
                                const float frame_seconds) {
    impl_->begin_frame(clear_color, frame_seconds);
}

RenderLayerTelemetry RenderContext::render_layer(const std::string_view id,
                                                 const host::RenderPacket& packet) {
    return impl_->render_layer(id, packet);
}

} // namespace strata::d3d11
