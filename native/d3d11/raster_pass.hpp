#pragma once

#include <cstdint>
#include <memory>

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
 * coverage, and a colour job composites its layers before writing straight RGBA.
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
     * Fills the region at (`x`, `y`) of the atlas behind `target`, whose size the job was decoded
     * against: R8 coverage, or RGBA8 when `rgba`.
     */
    void run(const host::RasterJob& job, bool rgba, ID3D11UnorderedAccessView* target,
             std::uint32_t x, std::uint32_t y, std::uint32_t width, std::uint32_t height);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
