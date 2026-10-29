#pragma once

// The per-format decoders behind inspect_image and decode_image. Each lives in its own file and
// reports malformed input as std::invalid_argument prefixed with its format's name.

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "resource/image.hpp"

namespace strata::resource::codec {

/**
 * Refuses a canvas beyond the portable texture limits or maximum_image_decode_pixels. Every
 * decoder calls this before it allocates anything sized by the canvas.
 */
void validate_canvas(ImageDimensions dimensions, const char* format);

/**
 * Collects an animation's composited frames and enforces the animation budgets. A decoder asks
 * wants_more() before decoding each frame and stops when it is false, so a caller after the first
 * frame never pays for the rest.
 */
class FrameSink final {
  public:
    /** `consumer`, when given, takes each frame as it comes (stream_image): nothing is kept. */
    FrameSink(ImageDimensions canvas, std::uint32_t frame_limit, const char* format,
              const ImageFrameConsumer* consumer = nullptr);

    [[nodiscard]] bool wants_more() const noexcept {
        return consumer_ != nullptr ? !stopped_ : image_.frames.size() < frame_limit_;
    }
    /** Throws when one more frame would pass maximum_image_frames or the pixel budget. */
    void reserve_next() const;
    void push(std::vector<std::uint8_t> rgba, std::uint32_t delay_ms);
    [[nodiscard]] DecodedImage finish(std::uint32_t loop_count);

  private:
    DecodedImage image_;
    std::uint32_t frame_limit_ = 0U;
    const char* format_ = "";
    const ImageFrameConsumer* consumer_ = nullptr;
    bool stopped_ = false;
    std::size_t consumed_ = 0U;
};

/**
 * Source-over of one straight-alpha RGBA8 pixel onto another, in libwebp's integer arithmetic
 * (BlendPixelNonPremult): a transparent source leaves the destination untouched and an opaque one
 * replaces it, exactly, as every animated format's "over" blend requires.
 */
void blend_over(std::uint8_t* destination, const std::uint8_t* source) noexcept;

/** Clears a rectangle of an RGBA8 canvas to transparent black. */
void clear_rect(std::vector<std::uint8_t>& canvas, std::uint32_t canvas_width, std::uint32_t x,
                std::uint32_t y, std::uint32_t width, std::uint32_t height) noexcept;

// PNG and APNG (png_decode.cpp).
[[nodiscard]] ImageDimensions inspect_png(std::span<const std::uint8_t> bytes);
[[nodiscard]] DecodedImage decode_png(std::span<const std::uint8_t> bytes,
                                      std::uint32_t frame_limit,
                                      const ImageFrameConsumer* consumer = nullptr);

// JPEG (jpeg_decode.cpp): one frame, EXIF orientation applied.
[[nodiscard]] ImageDimensions inspect_jpeg(std::span<const std::uint8_t> bytes);
[[nodiscard]] DecodedImage decode_jpeg(std::span<const std::uint8_t> bytes);

// GIF (gif_decode.cpp).
[[nodiscard]] ImageDimensions inspect_gif(std::span<const std::uint8_t> bytes);
[[nodiscard]] DecodedImage decode_gif(std::span<const std::uint8_t> bytes,
                                      std::uint32_t frame_limit,
                                      const ImageFrameConsumer* consumer = nullptr);

// WebP container, VP8L and ALPH (webp_decode.cpp).
[[nodiscard]] ImageDimensions inspect_webp(std::span<const std::uint8_t> bytes);
[[nodiscard]] DecodedImage decode_webp(std::span<const std::uint8_t> bytes,
                                       std::uint32_t frame_limit,
                                       const ImageFrameConsumer* consumer = nullptr);

// VP8 lossy key frames (vp8_decode.cpp), the payload of a WebP 'VP8 ' chunk.
/** Reads a VP8 key frame's dimensions from its 10-byte header. */
[[nodiscard]] ImageDimensions inspect_vp8(std::span<const std::uint8_t> frame);
/**
 * Decodes a VP8 key frame into opaque RGBA8 rows of exactly `expected` dimensions, converting its
 * 4:2:0 YUV with libwebp's fancy upsampler and integer BT.601 coefficients, so the result matches
 * libwebp's RGBA output.
 */
[[nodiscard]] std::vector<std::uint8_t> decode_vp8_rgba8(std::span<const std::uint8_t> frame,
                                                         ImageDimensions expected);

} // namespace strata::resource::codec
