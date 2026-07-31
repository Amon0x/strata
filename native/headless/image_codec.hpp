#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace strata::headless {

struct DecodedImage final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<std::uint8_t> rgba;
};

/** Platform image-codec boundary used only for app-declared encoded textures. */
class ImageCodec {
  public:
    virtual ~ImageCodec() = default;

    [[nodiscard]] virtual DecodedImage decode_png(std::span<const std::uint8_t> bytes,
                                                  std::uint32_t expected_width,
                                                  std::uint32_t expected_height) const = 0;
};

/** Returns the platform codec shipped by this headless-host build. */
[[nodiscard]] const ImageCodec& platform_image_codec();

} // namespace strata::headless
