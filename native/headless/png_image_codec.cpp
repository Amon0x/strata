#include "gpu/png.hpp"
#include "image_codec.hpp"
namespace strata::headless {
namespace {
class PngCodec final : public ImageCodec {
    DecodedImage decode_png(std::span<const std::uint8_t> bytes, std::uint32_t width,
                            std::uint32_t height) const override {
        return {width, height, gpu::decode_png(bytes, width, height)};
    }
};
} // namespace
const ImageCodec& platform_image_codec() {
    static const PngCodec codec;
    return codec;
}
} // namespace strata::headless
