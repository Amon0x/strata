#include "raster_pass.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include "gpu/raster_shaders.hpp"
#include <strata/d3d11.hpp>
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

/**
 * Art compiles without fxc's optimizer: it is compiled the first time a page draws it, and the
 * driver optimizes the bytecode itself, so the program runs as fast and appears several times
 * sooner. The raster kernels, compiled at startup where nothing waits, keep it.
 */
constexpr UINT art_optimization = D3DCOMPILE_SKIP_OPTIMIZATION;

/** set_art_cache_directory's directory, read by the threads that write compiled art. */
std::mutex art_cache_mutex;
std::filesystem::path art_cache_directory;

[[nodiscard]] std::filesystem::path art_cache_file(const std::string_view text,
                                                   const std::string_view entry) {
    const std::lock_guard lock(art_cache_mutex);
    if (art_cache_directory.empty())
        return {};
    std::uint64_t hash = 1469598103934665603ULL;
    for (const std::string_view part :
         {text, std::string_view("|"), entry, std::string_view("|d3d11-fxc-skip-optimization-2")}) {
        for (const char byte : part) {
            hash ^= static_cast<unsigned char>(byte);
            hash *= 1099511628211ULL;
        }
    }
    char name[40];
    std::snprintf(name, sizeof(name), "art-%016llx.dxbc", static_cast<unsigned long long>(hash));
    return art_cache_directory / name;
}

/** A compiled art kernel from the cache, if one is there. */
[[nodiscard]] ComPtr<ID3DBlob> cached_art(const std::filesystem::path& file) {
    if (file.empty())
        return nullptr;
    std::ifstream input(file, std::ios::binary | std::ios::ate);
    if (!input)
        return nullptr;
    const std::streamsize size = input.tellg();
    if (size <= 0)
        return nullptr;
    ComPtr<ID3DBlob> blob;
    if (FAILED(D3DCreateBlob(static_cast<SIZE_T>(size), &blob)))
        return nullptr;
    input.seekg(0);
    if (!input.read(static_cast<char*>(blob->GetBufferPointer()), size))
        return nullptr;
    return blob;
}

/** Keeps a compiled art kernel for later runs; a cache that cannot be written is skipped. */
void cache_art(const std::filesystem::path& file, ID3DBlob* const code) noexcept {
    if (file.empty() || code == nullptr)
        return;
    try {
        std::error_code error;
        std::filesystem::create_directories(file.parent_path(), error);
        const std::filesystem::path partial = file.string() + ".partial";
        {
            std::ofstream output(partial, std::ios::binary | std::ios::trunc);
            output.write(static_cast<const char*>(code->GetBufferPointer()),
                         static_cast<std::streamsize>(code->GetBufferSize()));
            if (!output)
                return;
        }
        std::filesystem::rename(partial, file, error);
    } catch (...) {
    }
}

[[nodiscard]] ComPtr<ID3DBlob> compile(const char* const entry, const bool rgba,
                                       const std::string_view text = shaders::source,
                                       const UINT optimization = D3DCOMPILE_OPTIMIZATION_LEVEL3) {
    const D3D_SHADER_MACRO rgba_macros[]{{"STRATA_RASTER_RGBA", "1"}, {nullptr, nullptr}};
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> diagnostics;
    const HRESULT status = D3DCompile(
        text.data(), text.size(), "strata.d3d11.raster.hlsl", rgba ? rgba_macros : nullptr, nullptr,
        entry, "cs_5_0", D3DCOMPILE_ENABLE_STRICTNESS | optimization, 0U, &code, &diagnostics);
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
                         code[index] =
                             compile(entries[index], std::string_view(entries[index]) == "finish");
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

/**
 * A zeroed GPU-only buffer of `stride`-byte elements (32-bit integers the kernels accumulate
 * into, or art points), grown to fit.
 */
struct WorkBuffer final {
    ComPtr<ID3D11Buffer> buffer;
    ComPtr<ID3D11UnorderedAccessView> view;
    std::size_t capacity = 0U;
    UINT stride = 4U;

    void reserve(ID3D11Device* const device, ID3D11DeviceContext* const context,
                 const std::size_t count) {
        if (count <= capacity && buffer != nullptr)
            return;
        capacity = std::max<std::size_t>({count, capacity * 2U, 4096U});
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = static_cast<UINT>(capacity * stride);
        descriptor.Usage = D3D11_USAGE_DEFAULT;
        descriptor.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        descriptor.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        descriptor.StructureByteStride = stride;
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

/**
 * Whether two art layers' programs make the same points: same program, inputs and canvas size
 * (points are canvas-relative, so a glow and its line, or one art's tiles, share them).
 */
[[nodiscard]] bool same_points(const host::RasterArt& a, const host::RasterArt& b) {
    return a.count == b.count && a.width == b.width && a.height == b.height &&
           a.parameters == b.parameters && a.source == b.source;
}

} // namespace

/** An art program's kernel, compiled in the background when a layer first needs it. */
struct ArtProgram final {
    std::shared_future<ComPtr<ID3DBlob>> code;
    ComPtr<ID3D11ComputeShader> shader;
    bool failed = false;
};

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
        art_stroke = create(device, code[5]);
        art_fill = create(device, code[6]);
        positions.stride = sizeof(float) * 4U;
        D3D11_BUFFER_DESC descriptor{};
        descriptor.ByteWidth = sizeof(shaders::Dispatch);
        descriptor.Usage = D3D11_USAGE_DYNAMIC;
        descriptor.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        require_hresult(device->CreateBuffer(&descriptor, nullptr, &constants),
                        "raster constant buffer creation");
    }

    /** An art program's kernel once it is ready, else null. */
    [[nodiscard]] ID3D11ComputeShader* art_kernel(const std::string& source) {
        ArtProgram& kernel = art_programs[std::hash<std::string>{}(source)];
        if (kernel.failed)
            return nullptr;
        if (kernel.shader != nullptr)
            return kernel.shader.Get();
        if (!kernel.code.valid()) {
            const char* const entry = shaders::art_entry;
            std::string text = shaders::art_source(source);
            const std::filesystem::path file = art_cache_file(text, entry);
            // Compiled on an earlier run: made now, so the page shows its art from its first frame.
            if (ComPtr<ID3DBlob> cached = cached_art(file); cached != nullptr) {
                ComPtr<ID3D11ComputeShader> shader;
                if (SUCCEEDED(device->CreateComputeShader(
                        cached->GetBufferPointer(), cached->GetBufferSize(), nullptr, &shader))) {
                    kernel.shader = std::move(shader);
                    return kernel.shader.Get();
                }
            }
            kernel.code = std::async(std::launch::async, [text = std::move(text), entry, file] {
                              ComPtr<ID3DBlob> code = compile(entry, false, text, art_optimization);
                              cache_art(file, code.Get());
                              return code;
                          }).share();
        }
        if (kernel.code.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return nullptr;
        try {
            kernel.shader = create(device.Get(), kernel.code.get());
        } catch (const std::exception& error) {
            // A program that does not compile draws nothing, rather than failing frames; the
            // author sees why once.
            std::fprintf(stderr, "strata: an art program failed to compile: %s\n", error.what());
            kernel.failed = true;
            return nullptr;
        }
        return kernel.shader.Get();
    }

    /** A dispatch, planned before the batch's items are written. */
    struct Planned final {
        ID3D11ComputeShader* shader = nullptr;
        /** The output it writes, or null for a kernel that writes none. */
        ID3D11UnorderedAccessView* target = nullptr;
        shaders::Dispatch values;
        std::uint32_t groups = 0U;
    };

    /**
     * Plans `shader` over `work`, (table entry, threads or groups) pairs, as few dispatches as the
     * group limit allows: each pair one or more items of the batch list.
     */
    void plan(ID3D11ComputeShader* const shader, ID3D11UnorderedAccessView* const target,
              const bool by_group,
              const std::vector<std::pair<std::uint32_t, std::uint32_t>>& work) {
        const std::uint32_t capacity = by_group ? 65535U : 65535U * shaders::group_size;
        Planned current;
        const auto close = [&] {
            if (current.values.total == 0U)
                return;
            current.groups =
                by_group ? current.values.total
                         : (current.values.total + shaders::group_size - 1U) / shaders::group_size;
            planned.push_back(current);
        };
        const auto open = [&] {
            current = Planned{shader, target, {}, 0U};
            current.values.first_item = static_cast<std::uint32_t>(items.size());
        };
        open();
        for (const auto& [entry, count] : work) {
            std::uint32_t done = 0U;
            while (done < count) {
                if (current.values.total == capacity) {
                    close();
                    open();
                }
                const std::uint32_t take = std::min(count - done, capacity - current.values.total);
                items.push_back(shaders::BatchItem{entry, current.values.total, done, 0U});
                ++current.values.item_count;
                current.values.total += take;
                done += take;
            }
        }
        close();
    }

    void run(const std::span<const RasterPass::Target> jobs, const double seconds) {
        // Every job's segments, pieces, colour tables and art values end to end, and a table
        // entry for each layer saying where its own begin.
        segment_data.clear();
        piece_data.clear();
        lut_data.clear();
        art_data.clear();
        entries.clear();
        entry_jobs.clear();
        entry_layers.clear();
        entry_shares.clear();
        art_sources.clear();
        std::size_t cell_total = 0U;
        std::size_t sum_total = 0U;
        std::size_t point_total = 0U;
        std::size_t most_layers = 0U;
        for (std::size_t index = 0U; index < jobs.size(); ++index) {
            const RasterPass::Target& work = jobs[index];
            const host::RasterJob& job = *work.job;
            const auto segment_base = static_cast<std::uint32_t>(segment_data.size());
            segment_data.insert(segment_data.end(), job.segments.begin(), job.segments.end());
            const auto piece_base = static_cast<std::uint32_t>(piece_data.size());
            piece_data.insert(piece_data.end(), job.pieces.begin(), job.pieces.end());
            const std::size_t pixels = static_cast<std::size_t>(job.width) * job.height;
            const auto sum_base = static_cast<std::uint32_t>(sum_total);
            if (work.rgba)
                sum_total += pixels * 5U;
            most_layers = std::max(most_layers, job.layers.size());
            for (std::size_t layer_index = 0U; layer_index < job.layers.size(); ++layer_index) {
                const host::RasterLayer& layer = job.layers[layer_index];
                shaders::Constants values;
                bool sharing = false;
                values.content_x = job.x;
                values.content_y = job.y;
                values.content_width = job.width;
                values.content_height = job.height;
                values.region_x = work.x;
                values.region_y = work.y;
                values.region_width = work.width;
                values.region_height = work.height;
                values.first_segment = segment_base + layer.first_segment;
                values.segment_count = layer.segment_count;
                values.fill = static_cast<std::uint32_t>(layer.fill);
                values.paint = static_cast<std::uint32_t>(layer.paint);
                values.color = layer.color;
                values.extend = static_cast<std::uint32_t>(layer.extend);
                values.lut_offset = static_cast<std::uint32_t>(lut_data.size());
                lut_data.insert(lut_data.end(), layer.lut.begin(), layer.lut.end());
                std::copy(layer.parameters.begin(), layer.parameters.end(), values.parameters);
                values.first_piece = piece_base + layer.first_piece;
                values.piece_count = layer.piece_count;
                values.join = static_cast<std::uint32_t>(layer.stroke.join);
                values.cap = static_cast<std::uint32_t>(layer.stroke.cap);
                values.half_width = layer.stroke.half_width;
                values.miter_limit = layer.stroke.miter_limit;
                values.tolerance = layer.stroke.tolerance;
                if (const std::optional<host::RasterArt>& art = layer.art; art.has_value()) {
                    values.art_base = static_cast<std::uint32_t>(art_data.size());
                    const std::array<float, 8U> head{static_cast<float>(seconds),
                                                     static_cast<float>(art->count),
                                                     art->closed ? 1.0F : 0.0F,
                                                     art->x,
                                                     art->y,
                                                     art->width,
                                                     art->height,
                                                     art->segments ? 1.0F : 0.0F};
                    art_data.insert(art_data.end(), head.begin(), head.end());
                    art_data.insert(art_data.end(), art->parameters.begin(), art->parameters.end());
                    // A glow and its line run one program on one canvas: their points are the
                    // same, so the second shares the first's rather than running it again.
                    const auto same = std::find_if(
                        art_sources.begin(), art_sources.end(),
                        [&](const auto& source) { return same_points(*source.first, *art); });
                    if (same != art_sources.end()) {
                        values.points_base = same->second;
                        sharing = true;
                    } else {
                        values.points_base = static_cast<std::uint32_t>(point_total);
                        point_total += art->count;
                        art_sources.emplace_back(&*art, values.points_base);
                    }
                }
                values.cell_base = static_cast<std::uint32_t>(cell_total);
                cell_total += pixels * 2U;
                values.sum_base = sum_base;
                entries.push_back(values);
                entry_jobs.push_back(index);
                entry_layers.push_back(layer_index);
                entry_shares.push_back(sharing);
            }
        }
        if (entries.empty())
            return;

        // The dispatches, in the order their work depends on: every layer's edges into its own
        // cells, then each round of layers resolved (a colour job composites its layers in
        // order), then colour jobs finished.
        planned.clear();
        items.clear();
        std::vector<std::pair<std::uint32_t, std::uint32_t>> work;
        const auto entry_count = static_cast<std::uint32_t>(entries.size());
        const auto gather = [&](const auto& threads) {
            work.clear();
            for (std::uint32_t entry = 0U; entry < entry_count; ++entry) {
                if (const std::uint32_t count = threads(entry); count > 0U)
                    work.emplace_back(entry, count);
            }
        };
        gather([&](const std::uint32_t entry) { return entries[entry].segment_count; });
        plan(accumulate.Get(), nullptr, false, work);
        gather([&](const std::uint32_t entry) {
            return entries[entry].piece_count * shaders::stroke_parts;
        });
        plan(stroke.Get(), nullptr, false, work);
        // An art program writes its layer's points; shared kernels outline every layer's.
        std::vector<bool> drawn(entries.size(), false);
        for (std::uint32_t entry = 0U; entry < entry_count; ++entry) {
            const std::optional<host::RasterArt>& art =
                jobs[entry_jobs[entry]].job->layers[entry_layers[entry]].art;
            if (!art.has_value() || art->count == 0U)
                continue;
            if (ID3D11ComputeShader* const kernel = art_kernel(art->source); kernel != nullptr) {
                if (!entry_shares[entry])
                    plan(kernel, nullptr, false, {{entry, art->count}});
                drawn[entry] = true;
            }
        }
        for (const bool stroked : {true, false}) {
            gather([&](const std::uint32_t entry) -> std::uint32_t {
                const std::optional<host::RasterArt>& art =
                    jobs[entry_jobs[entry]].job->layers[entry_layers[entry]].art;
                if (!drawn[entry] || art->stroked != stroked)
                    return 0U;
                if (!stroked)
                    return art->count;
                return (art->segments ? art->count / 2U
                        : art->closed ? art->count
                                      : art->count - 1U) *
                       shaders::stroke_parts;
            });
            plan(stroked ? art_stroke.Get() : art_fill.Get(), nullptr, false, work);
        }
        for (std::size_t round = 0U; round < most_layers; ++round) {
            // Coverage writes its target as it resolves: one dispatch a target.
            std::vector<ID3D11UnorderedAccessView*> targets;
            for (const RasterPass::Target& job : jobs) {
                if (!job.rgba && round < job.job->layers.size() &&
                    std::find(targets.begin(), targets.end(), job.target) == targets.end())
                    targets.push_back(job.target);
            }
            for (ID3D11UnorderedAccessView* const target : targets) {
                gather([&](const std::uint32_t entry) -> std::uint32_t {
                    const RasterPass::Target& job = jobs[entry_jobs[entry]];
                    return !job.rgba && entry_layers[entry] == round && job.target == target
                               ? job.height
                               : 0U;
                });
                plan(resolve_coverage.Get(), target, true, work);
            }
            gather([&](const std::uint32_t entry) -> std::uint32_t {
                const RasterPass::Target& job = jobs[entry_jobs[entry]];
                return job.rgba && entry_layers[entry] == round ? job.job->height : 0U;
            });
            plan(resolve_composite.Get(), nullptr, true, work);
        }
        std::vector<ID3D11UnorderedAccessView*> targets;
        for (const RasterPass::Target& job : jobs) {
            if (job.rgba && std::find(targets.begin(), targets.end(), job.target) == targets.end())
                targets.push_back(job.target);
        }
        for (ID3D11UnorderedAccessView* const target : targets) {
            // A job's last layer stands for it: they share its region and sums.
            gather([&](const std::uint32_t entry) -> std::uint32_t {
                const RasterPass::Target& job = jobs[entry_jobs[entry]];
                return job.rgba && job.target == target &&
                               entry_layers[entry] + 1U == job.job->layers.size()
                           ? job.width * job.height
                           : 0U;
            });
            plan(finish.Get(), target, false, work);
        }

        segments.write(device.Get(), context.Get(), segment_data.data(), segment_data.size(),
                       sizeof(host::RasterSegment));
        pieces.write(device.Get(), context.Get(), piece_data.data(), piece_data.size(),
                     sizeof(host::RasterStrokePiece));
        if (lut_data.empty())
            lut_data.push_back(0U);
        luts.write(device.Get(), context.Get(), lut_data.data(), lut_data.size(), 4U);
        static_assert(shaders::art_value_count == 24U);
        if (art_data.empty())
            art_data.resize(shaders::art_value_count, 0.0F);
        arts.write(device.Get(), context.Get(), art_data.data(), art_data.size(), 4U);
        table.write(device.Get(), context.Get(), entries.data(), entries.size(),
                    sizeof(shaders::Constants));
        batch.write(device.Get(), context.Get(), items.data(), items.size(),
                    sizeof(shaders::BatchItem));
        cells.reserve(device.Get(), context.Get(), std::max<std::size_t>(cell_total, 1U));
        sums.reserve(device.Get(), context.Get(), std::max<std::size_t>(sum_total, 1U));
        positions.reserve(device.Get(), context.Get(), std::max<std::size_t>(point_total, 1U));

        // The atlas may still be bound for sampling from the frame before.
        ID3D11ShaderResourceView* const no_view = nullptr;
        context->PSSetShaderResources(0U, 1U, &no_view);
        const std::array<ID3D11ShaderResourceView*, 6U> views{
            segments.view.Get(), luts.view.Get(),  pieces.view.Get(),
            arts.view.Get(),     table.view.Get(), batch.view.Get()};
        context->CSSetShaderResources(0U, static_cast<UINT>(views.size()), views.data());
        ID3D11UnorderedAccessView* bound = jobs.front().target;
        const std::array<ID3D11UnorderedAccessView*, 4U> outputs{cells.view.Get(), sums.view.Get(),
                                                                 bound, positions.view.Get()};
        context->CSSetUnorderedAccessViews(0U, static_cast<UINT>(outputs.size()), outputs.data(),
                                           nullptr);
        ID3D11Buffer* const constant_buffer = constants.Get();
        context->CSSetConstantBuffers(0U, 1U, &constant_buffer);
        for (const Planned& dispatch : planned) {
            if (dispatch.target != nullptr && dispatch.target != bound) {
                bound = dispatch.target;
                context->CSSetUnorderedAccessViews(2U, 1U, &bound, nullptr);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            require_hresult(context->Map(constants.Get(), 0U, D3D11_MAP_WRITE_DISCARD, 0U, &mapped),
                            "raster constants map");
            std::memcpy(mapped.pData, &dispatch.values, sizeof(dispatch.values));
            context->Unmap(constants.Get(), 0U);
            context->CSSetShader(dispatch.shader, nullptr, 0U);
            context->Dispatch(dispatch.groups, 1U, 1U);
        }

        const std::array<ID3D11UnorderedAccessView*, 4U> no_outputs{};
        context->CSSetUnorderedAccessViews(0U, static_cast<UINT>(no_outputs.size()),
                                           no_outputs.data(), nullptr);
        const std::array<ID3D11ShaderResourceView*, 6U> no_views{};
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
    ComPtr<ID3D11ComputeShader> art_stroke;
    ComPtr<ID3D11ComputeShader> art_fill;
    ComPtr<ID3D11Buffer> constants;
    UploadBuffer segments;
    UploadBuffer luts;
    UploadBuffer pieces;
    UploadBuffer arts;
    /** The batch's table, an entry a job layer, and its dispatches' items. */
    UploadBuffer table;
    UploadBuffer batch;
    std::vector<shaders::Constants> entries;
    std::vector<std::size_t> entry_jobs;
    std::vector<std::size_t> entry_layers;
    /** Whether an entry's art points are an earlier entry's, and each set of points written. */
    std::vector<bool> entry_shares;
    std::vector<std::pair<const host::RasterArt*, std::uint32_t>> art_sources;
    std::vector<shaders::BatchItem> items;
    std::vector<Planned> planned;
    std::vector<host::RasterSegment> segment_data;
    std::vector<host::RasterStrokePiece> piece_data;
    std::vector<float> art_data;
    std::unordered_map<std::size_t, ArtProgram> art_programs;
    WorkBuffer cells;
    WorkBuffer sums;
    /** Every art layer's points, its program's output. */
    WorkBuffer positions;
    std::vector<std::uint32_t> lut_data;
};

void set_art_cache_directory(std::filesystem::path directory) {
    const std::lock_guard lock(art_cache_mutex);
    art_cache_directory = std::move(directory);
}

void RasterPass::prepare() {
    static_cast<void>(bytecode());
}

RasterPass::RasterPass(ID3D11Device* const device, ID3D11DeviceContext* const context)
    : impl_(std::make_unique<Impl>(device, context)) {}
RasterPass::~RasterPass() = default;

void RasterPass::run(const host::RasterJob& job, const bool rgba,
                     ID3D11UnorderedAccessView* const target, const std::uint32_t x,
                     const std::uint32_t y, const std::uint32_t width,
                     const std::uint32_t height, const double seconds) {
    const Target one{&job, rgba, target, x, y, width, height};
    impl_->run(std::span<const Target>(&one, 1U), seconds);
}

void RasterPass::run(const std::span<const Target> jobs, const double seconds) {
    impl_->run(jobs, seconds);
}

} // namespace strata::d3d11
