#include "png.hpp"
#include <cstdio>
#include <cstring>
#include <png.h>
#include <stdexcept>
#include <string>

#ifdef _MSC_VER
// run() below holds no object with a destructor; libpng's longjmp only returns into it.
#pragma warning(disable : 4611)
#endif

namespace strata::gpu {
namespace {

struct Decode {
    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<std::uint8_t>* pixels = nullptr;
    std::vector<png_bytep>* rows = nullptr;
    char failure[256]{};
};

void read(png_structp png, png_bytep data, png_size_t length) {
    auto& decode = *static_cast<Decode*>(png_get_io_ptr(png));
    if (length > decode.bytes.size() - decode.offset)
        png_error(png, "data is truncated");
    std::memcpy(data, decode.bytes.data() + decode.offset, length);
    decode.offset += length;
}

[[noreturn]] void fail(png_structp png, png_const_charp message) {
    auto& decode = *static_cast<Decode*>(png_get_error_ptr(png));
    std::snprintf(decode.failure, sizeof(decode.failure), "%s", message);
    png_longjmp(png, 1);
}

void warn(png_structp, png_const_charp) {}

/** Returns false when libpng reported an error, which is then in decode.failure. */
bool run(png_structp png, png_infop info, Decode& decode) {
    if (setjmp(png_jmpbuf(png)))
        return false;
    png_set_read_fn(png, &decode, read);
    png_read_info(png, info);
    if (png_get_image_width(png, info) != decode.width ||
        png_get_image_height(png, info) != decode.height)
        png_error(png, "dimensions disagree with resource declaration");
    // Samples are taken as stored and narrowed to their high byte: no gAMA, sRGB or iCCP
    // conversion, and 16-bit data is not assumed linear. That is what D3D11's WIC decode to
    // 32bppRGBA produces, so a PNG texture has the same texels on every backend.
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    if (!(png_get_color_type(png, info) & PNG_COLOR_MASK_ALPHA) &&
        !png_get_valid(png, info, PNG_INFO_tRNS))
        png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != static_cast<png_size_t>(decode.width) * 4U)
        png_error(png, "unsupported pixel layout");
    for (std::uint32_t y = 0; y < decode.height; ++y)
        (*decode.rows)[y] = decode.pixels->data() + static_cast<std::size_t>(y) * decode.width * 4U;
    png_read_image(png, decode.rows->data());
    png_read_end(png, nullptr);
    return true;
}

} // namespace

std::vector<std::uint8_t> decode_png(std::span<const std::uint8_t> bytes, std::uint32_t width,
                                     std::uint32_t height) {
    if (!width || !height)
        throw std::runtime_error("PNG dimensions must be positive");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    std::vector<png_bytep> rows(height);
    Decode decode{bytes, 0, width, height, &pixels, &rows};
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &decode, fail, warn);
    png_infop info = png ? png_create_info_struct(png) : nullptr;
    struct Guard {
        png_structp& png;
        png_infop& info;
        ~Guard() {
            png_destroy_read_struct(&png, &info, nullptr);
        }
    } guard{png, info};
    if (!png || !info)
        throw std::runtime_error("PNG decoder allocation failed");
    if (!run(png, info, decode))
        throw std::runtime_error("PNG decode: " + std::string(decode.failure));
    return pixels;
}
} // namespace strata::gpu
