#include "raster_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "gpu/raster_shaders.hpp"
#include <strata/raster_job.hpp>

namespace strata::d3d11 {
namespace {

using Microsoft::WRL::ComPtr;
namespace shaders = gpu::raster_shaders;

void require_hresult(const HRESULT result, const std::string_view operation) {
    if (FAILED(result)) {
        throw std::runtime_error(std::string(operation) + " failed with HRESULT " +
                                 std::to_string(result));
    }
}

using shaders::entries;
using Bytecode = std::array<ComPtr<ID3DBlob>, entries.size()>;

[[nodiscard]] ComPtr<ID3DBlob> compile(const char* const entry, const bool rgba) {
    const D3D_SHADER_MACRO rgba_macros[]{{"STRATA_RASTER_RGBA", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT status =
        D3DCompile(shaders::source.data(), shaders::source.size(), "strata.d3d11.raster.hlsl",
                   rgba ? rgba_macros : nullptr, nullptr, entry, "cs_5_0",
                   D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0U, &code,
                   &diagnostics);
    if (FAILED(status)) {
        const std::string detail =
            diagnostics == nullptr
                ? std::string{}
                : std::string(static_cast<const char*>(diagnostics->GetBufferPointer()),
                              diagnostics->GetBufferSize());
        throw std::runtime_error("D3D11 raster shader compilation failed: " + detail);
    }
    return code;
}

/** The kernels' bytecode, compiled once per process; it needs no device. */
[[nodiscard]] std::shared_future<Bytecode>& bytecode() {
    static std::once_flag started;
    static std::shared_future<Bytecode> result;
    std::call_once(started, [] {
        result = std::async(std::launch::async, [] {
                     Bytecode code;
                     for (std::size_t index = 0U; index < entries.size(); ++index)
                         code[index] = compile(entries[index], index + 1U == entries.size());
                     return code;
                 }).share();
    });
    return result;
}

[[nodiscard]] ComPtr<ID3D11ComputeShader> create(ID3D11Device* const device,
                                                 const ComPtr<ID3DBlob>& code) {
    ComPtr<ID3D11ComputeShader> shader;
    require_hresult(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(),
                                                nullptr, &shader),
                    "raster compute shader creation");
    return shader;
}

[[nodiscard]] std::uint32_t groups(const std::uint32_t threads) noexcept {
    return (threads + shaders::group_size - 1U) / shaders::group_size;
}

/** A structured buffer the CPU rewrites per job, grown to fit. */
struct UploadBuffer final {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11ShaderResourceView> view;
    std::size_t capacity = 0U;

    void write(ID3D11Device* const device, ID3D11DeviceContext* const context,
               const void* const data, const std::size_t count, const std::size_t stride) {
        if (count > capacity || buffer == nullptr) {
            capacity = std::max<std::size_t>({count, capacity * 2U, 1024U});
            D3D11_BUFFER_DESC descriptor{};
            descriptor.ByteWidth = static_cast<UINT>(capacity * stride);
            descriptor.Usage = D3D11_USAGE_DYNAMIC;
            descriptor.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            descriptor.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            descriptor.StructureByteStride = static_cast<UINT>(stride);
            buffer.Reset();
            view.Reset();
            require_hresult(device->CreateBuffer(&descriptor, nullptr, &buffer),
                            "raster upload buffer creation");
            require_hresult(device->CreateShaderResourceView(buffer.Get(), nullptr, &view),
                            "raster upload view creation");
        }
        if (count == 0U)
            return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(context->Map(buffer.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                        "raster upload map");
        std::memcpy(mapped.pData, data, count * stride);
        context->Unmap(buffer.Get(), 0U);
    }
};

/** A zeroed GPU-only buffer of 32-bit integers the kernels accumulate into, grown to fit. */
struct WorkBuffer final {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> view;
    std::size_t capacity = 0U;

    void reserve(ID3D11Device* const device, ID3D11DeviceContext* const context,
                 const std::size_t count) {
        if (count <= capacity && buffer != nullptr)
            return;
        capacity = std::max<std::size_t>({count, capacity * 2U, 4096U});
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = static_cast<UINT>(capacity * 4U);
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        descriptor.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        descriptor.StructureByteStride = 4U;
        buffer.Reset();
        view.Reset();
        require_hresult(device->CreateBuffer(&descriptor, nullptr, &buffer),
                        "raster work buffer creation");
        require_hresult(device->CreateUnorderedAccessView(buffer.Get(), nullptr, &view),
                        "raster work view creation");
        // The kernels leave every cell and sum they use at zero; a new buffer starts there.
        constexpr std::array<UINT, 4U> zero{};
        context->ClearUnorderedAccessViewUint(view.Get(), zero.data());
    }
};

} // namespace

struct RasterPass::Impl final {
    Impl(ID3D11Device* const device, ID3D11DeviceContext* const context)
        : device(device), context(context) {
        UINT support = 0U;
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 query{DXGI_FORMAT_R8_UNORM, 0U};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &query,
                                                  sizeof(query)))) {
            support = query.OutFormatSupport2;
        }
        if ((support & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0U)
            throw std::runtime_error("D3D11 device cannot store R8 coverage from compute shaders");
        const Bytecode& code = bytecode().get();
        accumulate = create(device, code[0]);
        stroke = create(device, code[1]);
        resolve_coverage = create(device, code[2]);
        resolve_composite = create(device, code[3]);
        finish = create(device, code[4]);
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = sizeof(shaders::Constants);
        descriptor.Usage = D3D11_USAGE_DYNAMIC;
        descriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        require_hresult(device->CreateBuffer(&descriptor, nullptr, &constants),
                        "raster constant buffer creation");
    }

    void dispatch(ID3D11ComputeShader* const shader, const shaders::Constants& values,
                  const std::uint32_t threads) {
        if (threads == 0U)
            return;
        D3D11_MAPPED_SUBRESOURCE mapped{};
        require_hresult(context->Map(constants.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                        "raster constants map");
        std::memcpy(mapped.pData, &values, sizeof(values));
        context->Unmap(constants.Get(), 0U);
        context->CSSetShader(shader, nullptr, 0U);
        context->Dispatch(groups(threads), 1U, 1U);
    }

    void run(const host::RasterJob& job, const bool rgba, ID3D11UnorderedAccessView* const target,
             const std::uint32_t x, const std::uint32_t y, const std::uint32_t width,
             const std::uint32_t height) {
        // Every gradient layer's colour table, end to end.
        lut_data.clear();
        std::vector<std::uint32_t> lut_offsets;
        lut_offsets.reserve(job.layers.size());
        for (const host::RasterLayer& layer : job.layers) {
            lut_offsets.push_back(static_cast<std::uint32_t>(lut_data.size()));
            lut_data.insert(lut_data.end(), layer.lut.begin(), layer.lut.end());
        }
        if (lut_data.empty())
            lut_data.push_back(0U);
        segments.write(device.Get(), context.Get(), job.segments.data(), job.segments.size(),
                       sizeof(host::RasterSegment));
        pieces.write(device.Get(), context.Get(), job.pieces.data(), job.pieces.size(),
                     sizeof(host::RasterStrokePiece));
        luts.write(device.Get(), context.Get(), lut_data.data(), lut_data.size(), 4U);
        const std::size_t pixels = static_cast<std::size_t>(job.width) * job.height;
        cells.reserve(device.Get(), context.Get(), pixels * 2U);
        if (rgba)
            sums.reserve(device.Get(), context.Get(), pixels * 5U);
        else
            sums.reserve(device.Get(), context.Get(), 1U);

        // The atlas may still be bound for sampling from the frame before.
        ID3D11ShaderResourceView* const no_view = nullptr;
        context->PSSetShaderResources(0U, 1U, &no_view);
        const std::array<ID3D11ShaderResourceView*, 3U> views{
            segments.view.Get(), luts.view.Get(), pieces.view.Get()};
        context->CSSetShaderResources(0U, static_cast<UINT>(views.size()), views.data());
        const std::array<ID3D11UnorderedAccessView*, 3U> outputs{cells.view.Get(),
                                                                 sums.view.Get(), target};
        context->CSSetUnorderedAccessViews(0U, static_cast<UINT>(outputs.size()), outputs.data(),
                                           nullptr);
        ID3D11Buffer* const constant_buffer = constants.Get();
        context->CSSetConstantBuffers(0U, 1U, &constant_buffer);

        for (std::size_t index = 0U; index < job.layers.size(); ++index) {
            const host::RasterLayer& layer = job.layers[index];
            shaders::Constants values;
            values.content_x = job.x;
            values.content_y = job.y;
            values.content_width = job.width;
            values.content_height = job.height;
            values.region_x = x;
            values.region_y = y;
            values.region_width = width;
            values.region_height = height;
            values.first_segment = layer.first_segment;
            values.segment_count = layer.segment_count;
            values.fill = static_cast<std::uint32_t>(layer.fill);
            values.paint = static_cast<std::uint32_t>(layer.paint);
            values.color = layer.color;
            values.extend = static_cast<std::uint32_t>(layer.extend);
            values.lut_offset = lut_offsets[index];
            std::copy(layer.parameters.begin(), layer.parameters.end(), values.parameters);
            values.first_piece = layer.first_piece;
            values.piece_count = layer.piece_count;
            values.join = static_cast<std::uint32_t>(layer.stroke.join);
            values.cap = static_cast<std::uint32_t>(layer.stroke.cap);
            values.half_width = layer.stroke.half_width;
            values.miter_limit = layer.stroke.miter_limit;
            values.tolerance = layer.stroke.tolerance;
            dispatch(accumulate.Get(), values, layer.segment_count);
            dispatch(stroke.Get(), values, layer.piece_count);
            if (rgba)
                dispatch(resolve_composite.Get(), values, job.height);
            else
                dispatch(resolve_coverage.Get(), values, height);
            last = values;
        }
        if (rgba)
            dispatch(finish.Get(), last, height);

        const std::array<ID3D11UnorderedAccessView*, 3U> no_outputs{};
        context->CSSetUnorderedAccessViews(0U, static_cast<UINT>(no_outputs.size()),
                                           no_outputs.data(), nullptr);
        const std::array<ID3D11ShaderResourceView*, 3U> no_views{};
        context->CSSetShaderResources(0U, static_cast<UINT>(no_views.size()), no_views.data());
        context->CSSetShader(nullptr, nullptr, 0U);
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11ComputeShader> accumulate;
    ComPtr<ID3D11ComputeShader> stroke;
    ComPtr<ID3D11ComputeShader> resolve_coverage;
    ComPtr<ID3D11ComputeShader> resolve_composite;
    ComPtr<ID3D11ComputeShader> finish;
    ComPtr<ID3D11Buffer> constants;
    UploadBuffer segments;
    UploadBuffer luts;
    UploadBuffer pieces;
    WorkBuffer cells;
    WorkBuffer sums;
    std::vector<std::uint32_t> lut_data;
    shaders::Constants last;
};

void RasterPass::prepare() {
    static_cast<void>(bytecode());
}

RasterPass::RasterPass(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : impl_(std::make_unique<Impl>(device, context)) {}
RasterPass::~RasterPass() = default;

void RasterPass::run(const host::RasterJob& job, const bool rgba,
                     ID3D11UnorderedAccessView* const target, const std::uint32_t x,
                     const std::uint32_t y, const std::uint32_t width,
                     const std::uint32_t height) {
    impl_->run(job, rgba, target, x, y, width, height);
}

} // namespace strata::d3d11
