#include "image_codec.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>

namespace strata::headless {
namespace {

class UnsupportedImageCodec final : public ImageCodec {
  public:
    [[nodiscard]] DecodedImage decode_png(std::span<const std::uint8_t>, std::uint32_t,
                                          std::uint32_t) const override {
        throw std::runtime_error(
            "this headless-host build has no PNG decoder for app-declared textures");
    }
};

} // namespace

const ImageCodec& platform_image_codec() {
    static const UnsupportedImageCodec codec;
    return codec;
}

} // namespace strata::headless
