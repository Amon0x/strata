#include "capture_renderer.hpp"

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "image_codec.hpp"
#include "software_renderer.hpp"
#if defined(_WIN32)
#include "d3d11_renderer.hpp"
#endif

namespace strata::headless {

std::unique_ptr<CaptureRenderer> create_capture_renderer(const std::string_view backend) {
    if (backend == "reference") {
        return std::make_unique<SoftwareRenderer>(platform_image_codec());
    }
    if (backend == "d3d11") {
#if defined(_WIN32)
        return std::make_unique<D3D11Renderer>();
#else
        throw std::invalid_argument("the d3d11 capture backend is available only on Windows");
#endif
    }
    throw std::invalid_argument("unknown headless render backend '" + std::string(backend) + "'");
}

} // namespace strata::headless
