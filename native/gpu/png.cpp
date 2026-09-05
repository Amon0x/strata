#include "png.hpp"
#include <png.h>
#include <stdexcept>
#include <string>
namespace strata::gpu {
std::vector<std::uint8_t> decode_png(std::span<const std::uint8_t> bytes, std::uint32_t width,
                                     std::uint32_t height) {
    png_image image{};
    image.version = PNG_IMAGE_VERSION;
    struct Guard {
        png_image* image;
        ~Guard() { png_image_free(image); }
    } guard{&image};
    if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size()))
        throw std::runtime_error("PNG header: " + std::string(image.message));
    if (image.width != width || image.height != height)
        throw std::runtime_error("PNG dimensions disagree with resource declaration");
    image.format = PNG_FORMAT_RGBA;
    std::vector<std::uint8_t> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr))
        throw std::runtime_error("PNG decode: " + std::string(image.message));
    return pixels;
}
} // namespace strata::gpu
