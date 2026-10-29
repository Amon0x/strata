#include "resource/image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "resource/image_codecs.hpp"

namespace strata::resource {
namespace {

[[nodiscard]] std::uint32_t big_endian_u32(const std::span<const std::uint8_t> bytes,
                                           const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) << 24U |
           static_cast<std::uint32_t>(bytes[offset + 1U]) << 16U |
           static_cast<std::uint32_t>(bytes[offset + 2U]) << 8U |
           static_cast<std::uint32_t>(bytes[offset + 3U]);
}

} // namespace

void validate_image_dimensions(const ImageDimensions dimensions) {
    constexpr std::uint32_t maximum_dimension = 32'768U;
    constexpr std::uint64_t maximum_pixels = 268'435'456U;
    if (dimensions.width == 0U || dimensions.height == 0U ||
        dimensions.width > maximum_dimension || dimensions.height > maximum_dimension ||
        static_cast<std::uint64_t>(dimensions.width) * dimensions.height > maximum_pixels) {
        throw std::invalid_argument("image dimensions exceed the portable texture limits");
    }
}

ImageDimensions inspect_png(const std::vector<std::uint8_t>& bytes) {
    return codec::inspect_png(bytes);
}

namespace codec {

ImageDimensions inspect_png(const std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 8U> signature{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
    };
    constexpr std::size_t ihdr_envelope_size = 33U;
    if (bytes.size() < ihdr_envelope_size ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        big_endian_u32(bytes, 8U) != 13U || bytes[12U] != 'I' || bytes[13U] != 'H' ||
        bytes[14U] != 'D' || bytes[15U] != 'R') {
        throw std::invalid_argument("texture resource is not a PNG with a leading IHDR chunk");
    }
    const ImageDimensions dimensions{big_endian_u32(bytes, 16U), big_endian_u32(bytes, 20U)};
    validate_image_dimensions(dimensions);
    // Refused here, at Surface creation, rather than when a frame first samples the image.
    if (static_cast<std::uint64_t>(dimensions.width) * dimensions.height >
        maximum_png_decode_pixels) {
        throw std::invalid_argument("PNG has more pixels than the decoder accepts");
    }
    return dimensions;
}

void validate_canvas(const ImageDimensions dimensions, const char* const format) {
    try {
        validate_image_dimensions(dimensions);
    } catch (const std::invalid_argument& error) {
        throw std::invalid_argument(std::string(format) + " decode: " + error.what());
    }
    if (static_cast<std::uint64_t>(dimensions.width) * dimensions.height >
        maximum_image_decode_pixels) {
        throw std::invalid_argument(std::string(format) +
                                    " decode: the image has more pixels than the decoder accepts");
    }
}

FrameSink::FrameSink(const ImageDimensions canvas, const std::uint32_t frame_limit,
                     const char* const format)
    : frame_limit_(frame_limit), format_(format) {
    validate_canvas(canvas, format);
    image_.width = canvas.width;
    image_.height = canvas.height;
}

void FrameSink::reserve_next() const {
    const std::uint64_t pixels = static_cast<std::uint64_t>(image_.width) * image_.height;
    if (image_.frames.size() >= maximum_image_frames ||
        (image_.frames.size() + 1U) * pixels > maximum_animation_decode_pixels) {
        throw std::invalid_argument(std::string(format_) +
                                    " decode: the animation has more frames than the decoder "
                                    "accepts");
    }
}

void FrameSink::push(std::vector<std::uint8_t> rgba, const std::uint32_t delay_ms) {
    reserve_next();
    if (rgba.size() != static_cast<std::size_t>(image_.width) * image_.height * 4U)
        throw std::logic_error("a decoded frame does not fill its canvas");
    image_.frames.push_back(ImageFrame{std::move(rgba), delay_ms});
}

DecodedImage FrameSink::finish(const std::uint32_t loop_count) {
    if (image_.frames.empty())
        throw std::invalid_argument(std::string(format_) + " decode: the image has no frames");
    image_.loop_count = image_.frames.size() == 1U ? 1U : loop_count;
    return std::move(image_);
}

void blend_over(std::uint8_t* const destination, const std::uint8_t* const source) noexcept {
    const std::uint32_t source_alpha = source[3U];
    if (source_alpha == 255U) {
        std::copy_n(source, 4U, destination);
        return;
    }
    if (source_alpha == 0U)
        return;
    // The destination's weight approximates its alpha * (255 - source alpha) / 255 as libwebp
    // does, and the colours are averaged by both weights in 24-bit fixed point.
    const std::uint32_t destination_factor = (destination[3U] * (256U - source_alpha)) >> 8U;
    const std::uint32_t alpha = source_alpha + destination_factor;
    const std::uint32_t scale = (std::uint32_t{1U} << 24U) / alpha;
    for (std::size_t channel = 0U; channel < 3U; ++channel) {
        const std::uint32_t unscaled =
            source[channel] * source_alpha + destination[channel] * destination_factor;
        destination[channel] = static_cast<std::uint8_t>((unscaled * scale) >> 24U);
    }
    destination[3U] = static_cast<std::uint8_t>(alpha);
}

void clear_rect(std::vector<std::uint8_t>& canvas, const std::uint32_t canvas_width,
                const std::uint32_t x, const std::uint32_t y, const std::uint32_t width,
                const std::uint32_t height) noexcept {
    for (std::uint32_t row = 0U; row < height; ++row) {
        const std::size_t start = ((static_cast<std::size_t>(y) + row) * canvas_width + x) * 4U;
        std::fill_n(canvas.begin() + static_cast<std::ptrdiff_t>(start),
                    static_cast<std::size_t>(width) * 4U, std::uint8_t{0U});
    }
}

} // namespace codec

std::optional<ImageFormat> sniff_image_format(const std::span<const std::uint8_t> bytes) noexcept {
    const auto starts = [bytes](const std::string_view prefix, const std::size_t at = 0U) {
        return bytes.size() >= at + prefix.size() &&
               std::equal(prefix.begin(), prefix.end(),
                          bytes.begin() + static_cast<std::ptrdiff_t>(at),
                          [](const char expected, const std::uint8_t actual) {
                              return static_cast<std::uint8_t>(expected) == actual;
                          });
    };
    if (starts("\x89PNG\r\n\x1A\n"))
        return ImageFormat::png;
    if (starts("\xFF\xD8\xFF"))
        return ImageFormat::jpeg;
    if (starts("GIF87a") || starts("GIF89a"))
        return ImageFormat::gif;
    if (starts("RIFF") && starts("WEBP", 8U))
        return ImageFormat::webp;
    return std::nullopt;
}

ImageEncoding image_encoding(const ImageFormat format) noexcept {
    switch (format) {
    case ImageFormat::jpeg:
        return ImageEncoding::jpeg;
    case ImageFormat::gif:
        return ImageEncoding::gif;
    case ImageFormat::webp:
        return ImageEncoding::webp;
    case ImageFormat::png:
        break;
    }
    return ImageEncoding::png;
}

ImageInfo inspect_image(const std::span<const std::uint8_t> bytes) {
    const std::optional<ImageFormat> format = sniff_image_format(bytes);
    if (!format.has_value())
        throw std::invalid_argument("image is not a PNG, JPEG, GIF or WebP");
    ImageDimensions dimensions;
    switch (*format) {
    case ImageFormat::png:
        dimensions = codec::inspect_png(bytes);
        break;
    case ImageFormat::jpeg:
        dimensions = codec::inspect_jpeg(bytes);
        break;
    case ImageFormat::gif:
        dimensions = codec::inspect_gif(bytes);
        break;
    case ImageFormat::webp:
        dimensions = codec::inspect_webp(bytes);
        break;
    }
    return ImageInfo{*format, dimensions};
}

DecodedImage decode_image(const std::span<const std::uint8_t> bytes,
                          const std::uint32_t frame_limit) {
    const std::optional<ImageFormat> format = sniff_image_format(bytes);
    if (!format.has_value())
        throw std::invalid_argument("image is not a PNG, JPEG, GIF or WebP");
    if (frame_limit == 0U)
        throw std::invalid_argument("image decode must ask for at least one frame");
    switch (*format) {
    case ImageFormat::png:
        return codec::decode_png(bytes, frame_limit);
    case ImageFormat::jpeg:
        return codec::decode_jpeg(bytes);
    case ImageFormat::gif:
        return codec::decode_gif(bytes, frame_limit);
    case ImageFormat::webp:
        break;
    }
    return codec::decode_webp(bytes, frame_limit);
}

std::vector<std::uint8_t> decode_image_rgba8(const std::span<const std::uint8_t> bytes,
                                             const ImageDimensions expected) {
    DecodedImage image = decode_image(bytes, 1U);
    if (image.width != expected.width || image.height != expected.height)
        throw std::invalid_argument("image dimensions disagree with the resource declaration");
    return std::move(image.frames.front().rgba);
}

} // namespace strata::resource
