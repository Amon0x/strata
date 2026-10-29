#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace strata::resource {

enum class TextureSampling : std::uint32_t { nearest = 0U, linear = 1U };
/** How a texture's bytes are stored: an encoded file the core decodes, or decoded RGBA8 rows. */
enum class ImageEncoding : std::uint32_t { png = 0U, rgba8 = 1U, jpeg = 2U, gif = 3U, webp = 4U };

struct ImageDimensions final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] friend bool operator==(const ImageDimensions&, const ImageDimensions&) = default;
};

/** Immutable image metadata consumed by geometry planning independently of encoded upload bytes. */
struct TextureResourceDescriptor final {
    /** Author-facing id referenced by render commands. */
    std::string logical_id;
    /** Surface-scoped id used by host resource and draw records. */
    std::string host_id;
    TextureSampling sampling = TextureSampling::linear;
    ImageEncoding encoding = ImageEncoding::png;
    ImageDimensions dimensions;

    [[nodiscard]] friend bool operator==(
        const TextureResourceDescriptor&,
        const TextureResourceDescriptor&
    ) = default;
};

/** Immutable, surface-owned encoded image transferred to a host render adapter. */
struct EncodedTextureResource final {
    /** Author-facing id referenced by render commands. */
    std::string logical_id;
    /** Surface-scoped id used by host resource and draw records. */
    std::string host_id;
    TextureSampling sampling = TextureSampling::linear;
    ImageEncoding encoding = ImageEncoding::png;
    ImageDimensions dimensions;
    std::vector<std::uint8_t> bytes;

    [[nodiscard]] TextureResourceDescriptor descriptor() const {
        if (logical_id.empty() || host_id.empty()) {
            throw std::invalid_argument(
                "encoded texture requires non-empty logical and surface host ids"
            );
        }
        return TextureResourceDescriptor{
            logical_id, host_id, sampling, encoding, dimensions,
        };
    }
};

/** Throws unless the dimensions fit the portable texture limits shared by every image source. */
void validate_image_dimensions(ImageDimensions dimensions);

/** Validates the bounded PNG envelope and reads its mandatory IHDR dimensions. */
[[nodiscard]] ImageDimensions inspect_png(const std::vector<std::uint8_t>& bytes);

/**
 * The most pixels decode_png_rgba8 accepts: 64 Mpx, a 256 MiB RGBA8 result (8192 x 8192, say).
 * Deflate shrinks uniform rows a thousandfold and a 1-bit row is a 32nd of its RGBA8 expansion, so
 * this, not the encoded size, is what bounds the memory a small PNG can demand.
 */
inline constexpr std::uint64_t maximum_png_decode_pixels = std::uint64_t{64U} * 1024U * 1024U;

/**
 * Decodes a PNG into tightly packed, top-down RGBA8 rows with straight (unpremultiplied) alpha:
 * the texel convention every packet image carries. Samples are taken as stored, with no gamma,
 * sRGB or ICC conversion; 16-bit samples keep their high byte; low bit depths expand by bit
 * replication; palette and tRNS transparency become alpha, and images without alpha are opaque.
 * Throws std::invalid_argument for a malformed stream, dimensions other than `expected`, or more
 * than maximum_png_decode_pixels. Inflating never produces more than the image's scanlines (its
 * rows plus their filter bytes), and back-references must stay within the zlib header's window.
 */
[[nodiscard]] std::vector<std::uint8_t> decode_png_rgba8(std::span<const std::uint8_t> bytes,
                                                         ImageDimensions expected);

/** The encoded raster formats the core decodes, told apart by their signatures, never by name. */
enum class ImageFormat : std::uint8_t { png, jpeg, gif, webp };

/** What inspect_image reads from a header without decoding pixels. */
struct ImageInfo final {
    ImageFormat format = ImageFormat::png;
    /** The canvas every frame fills; for a JPEG, as displayed after its EXIF orientation. */
    ImageDimensions dimensions;

    [[nodiscard]] friend bool operator==(const ImageInfo&, const ImageInfo&) = default;
};

/** One composited frame: a whole canvas of top-down, straight-alpha RGBA8 rows. */
struct ImageFrame final {
    std::vector<std::uint8_t> rgba;
    /** How long the frame shows, as the file stores it: no minimum is imposed here. */
    std::uint32_t delay_ms = 0U;
};

/**
 * A decoded image: one frame for a still, every frame of an animation. Each frame is already
 * composited onto the canvas (blending, disposal and offsets applied), so a player only swaps
 * whole frames.
 */
struct DecodedImage final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<ImageFrame> frames;
    /** How many times the animation plays; 0 plays it forever. A still image has 1. */
    std::uint32_t loop_count = 1U;
};

/**
 * The most pixels a canvas of any format may have: the PNG limit, and for the same reason.
 * Compression lets a few bytes declare an enormous canvas, so this bounds what decoding allocates.
 */
inline constexpr std::uint64_t maximum_image_decode_pixels = maximum_png_decode_pixels;

/**
 * The most frames, and the most pixels across all of them, a decoded animation may hold: 1024
 * frames and 128 Mpx (512 MiB of RGBA8, 320 x 240 for about 1700 frames). Frames are whole
 * canvases, so a GIF that repaints one pixel per frame still costs a canvas per frame.
 */
inline constexpr std::uint32_t maximum_image_frames = 1024U;
inline constexpr std::uint64_t maximum_animation_decode_pixels =
    std::uint64_t{128U} * 1024U * 1024U;

/** The format whose signature starts `bytes`, or nothing when no raster decoder claims them. */
[[nodiscard]] std::optional<ImageFormat>
sniff_image_format(std::span<const std::uint8_t> bytes) noexcept;

/** The texture encoding that carries bytes of `format`. */
[[nodiscard]] ImageEncoding image_encoding(ImageFormat format) noexcept;

/**
 * Validates a PNG, JPEG, GIF or WebP header and reads the canvas dimensions, refusing a canvas
 * beyond the portable texture limits or maximum_image_decode_pixels, as inspect_png does. Reads
 * headers only, so a stream may still fail to decode. Throws std::invalid_argument.
 */
[[nodiscard]] ImageInfo inspect_image(std::span<const std::uint8_t> bytes);

/** decode_image's default frame limit: every frame the stream holds. */
inline constexpr std::uint32_t all_image_frames = 0xFFFFFFFFU;

/**
 * Decodes a PNG (APNG included), JPEG, GIF or WebP into composited frames. Decoding stops after
 * `frame_limit` frames and reads nothing past them, so a caller that shows only the first frame
 * passes 1 and pays for one. An animation that would pass maximum_image_frames or
 * maximum_animation_decode_pixels within the limit is refused rather than cut short. Throws
 * std::invalid_argument for a malformed or unsupported stream.
 */
[[nodiscard]] DecodedImage decode_image(std::span<const std::uint8_t> bytes,
                                        std::uint32_t frame_limit = all_image_frames);

/**
 * Decodes the first frame of an encoded image as decode_png_rgba8 decodes a PNG: RGBA8 rows of
 * exactly `expected` dimensions (what inspect_image reported), or std::invalid_argument.
 */
[[nodiscard]] std::vector<std::uint8_t> decode_image_rgba8(std::span<const std::uint8_t> bytes,
                                                           ImageDimensions expected);

} // namespace strata::resource
