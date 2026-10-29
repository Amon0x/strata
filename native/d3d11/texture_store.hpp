#pragma once

#include <memory>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace strata::host {
struct DrawBatch;
struct ResourceOperation;
} // namespace strata::host

namespace strata::d3d11 {

/** D3D11 owner for packet-declared atlases and encoded image resources. */
class TextureStore final {
  public:
    TextureStore(ID3D11Device* device, ID3D11DeviceContext* context);
    ~TextureStore();

    TextureStore(const TextureStore&) = delete;
    TextureStore& operator=(const TextureStore&) = delete;

    void apply(const host::ResourceOperation& operation);
    /**
     * Runs the live art jobs (raster jobs with art layers) at `seconds`: they draw again every
     * frame, until a release, a new texture or a write over their region replaces them.
     */
    void run_live(double seconds);
    void bind(const host::DrawBatch& batch) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace strata::d3d11
