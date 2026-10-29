#pragma once

#include <cstdint>
#include <memory>
#include <span>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11UnorderedAccessView;

namespace strata::host {
struct RasterJob;
} // namespace strata::host

namespace strata::d3d11 {

/**
 * Rasterizes raster jobs (strata/raster_job.hpp) with compute shaders straight into an atlas:
 * each segment's exact area accumulates into cells by atomics, each row resolves its cells to
 * coverage, and a colour job composites its layers before writing straight RGBA. An art layer's
 * program compiles on a background thread the first time it is seen; until it is ready (or if it
 * fails to compile) the layer draws nothing.
 */
class RasterPass final {
  public:
    RasterPass(ID3D11Device* device, ID3D11DeviceContext* context);
    ~RasterPass();

    RasterPass(const RasterPass&) = delete;
    RasterPass& operator=(const RasterPass&) = delete;

    /**
     * Starts compiling the kernels on a background thread, once per process, so the first frame
     * that rasterizes does not compile them. A pass made before it finishes waits for it.
     */
    static void prepare();

    /**
     * One job of a batch: the region at (`x`, `y`) of the atlas behind `target`, whose size the
     * job was decoded against: R8 coverage, or RGBA8 when `rgba`.
     */
    struct Target final {
        const host::RasterJob* job = nullptr;
        bool rgba = false;
        ID3D11UnorderedAccessView* target = nullptr;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        std::uint32_t width = 0U;
        std::uint32_t height = 0U;
    };

    /** Fills one job's region. */
    void run(const host::RasterJob& job, bool rgba, ID3D11UnorderedAccessView* target,
             std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height,
             double seconds = 0.0);

    /**
     * Fills every job's region at once: each kernel runs once for all of them (an art program
     * once for each of its layers), so a batch costs a few dispatches rather than a few a job.
     */
    void run(std::span<const Target> jobs, double seconds = 0.0);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
