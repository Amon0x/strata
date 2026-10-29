// A GIF87a/GIF89a decoder that composites every frame onto the logical screen as browsers do:
// the canvas starts transparent, frames are clipped to it, a transparent index leaves the canvas
// beneath visible, and disposal 2 clears the frame's rectangle to transparent rather than to the
// background colour (which browsers have long ignored). Disposal 3 restores the canvas as it was
// before the frame.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource/image_codecs.hpp"

namespace strata::resource::codec {
namespace {

[[noreturn]] void fail(const std::string_view reason) {
    throw std::invalid_argument("GIF decode: " + std::string(reason));
}

using Palette = std::vector<std::array<std::uint8_t, 3U>>;

/** Reads the GIF byte stream with bounds checks; every read past the end is malformed input. */
class Cursor final {
  public:
    explicit Cursor(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    [[nodiscard]] std::uint8_t u8() {
        if (position_ >= bytes_.size())
            fail("the file is truncated");
        return bytes_[position_++];
    }

    [[nodiscard]] std::uint32_t u16() {
        const std::uint32_t low = u8();
        return low | static_cast<std::uint32_t>(u8()) << 8U;
    }

    [[nodiscard]] std::span<const std::uint8_t> take(const std::size_t count) {
        if (count > bytes_.size() - position_)
            fail("the file is truncated");
        const std::span<const std::uint8_t> result = bytes_.subspan(position_, count);
        position_ += count;
        return result;
    }

    [[nodiscard]] bool at_end() const noexcept {
        return position_ >= bytes_.size();
    }

    [[nodiscard]] Palette palette(const std::uint32_t size_bits) {
        const std::span<const std::uint8_t> data = take(std::size_t{3U} << (size_bits + 1U));
        Palette result(data.size() / 3U);
        for (std::size_t entry = 0U; entry < result.size(); ++entry)
            result[entry] = {data[entry * 3U], data[entry * 3U + 1U], data[entry * 3U + 2U]};
        return result;
    }

    /** Concatenates a chain of data sub-blocks, up to and including its empty terminator. */
    void sub_blocks(std::vector<std::uint8_t>* const into) {
        for (;;) {
            const std::uint8_t size = u8();
            if (size == 0U)
                return;
            const std::span<const std::uint8_t> data = take(size);
            if (into != nullptr)
                into->insert(into->end(), data.begin(), data.end());
        }
    }

  private:
    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0U;
};

struct Screen final {
    ImageDimensions dimensions;
    Palette global;
};

[[nodiscard]] Screen screen(Cursor& input) {
    const std::span<const std::uint8_t> signature = input.take(6U);
    const std::string_view text(reinterpret_cast<const char*>(signature.data()), signature.size());
    if (text != "GIF87a" && text != "GIF89a")
        fail("bytes are not a GIF");
    Screen result;
    result.dimensions.width = input.u16();
    result.dimensions.height = input.u16();
    const std::uint8_t packed = input.u8();
    static_cast<void>(input.u8()); // background colour index, which browsers ignore
    static_cast<void>(input.u8()); // pixel aspect ratio
    validate_canvas(result.dimensions, "GIF");
    if ((packed & 0x80U) != 0U)
        result.global = input.palette(packed & 0x07U);
    return result;
}

/**
 * Decodes one image's LZW stream, handing each pixel index to `emit` in stream order until the
 * image's `pixels` are complete. The code table is fixed-size, so a hostile stream costs time
 * proportional to its own length and memory independent of the frame's declared size. A stream
 * that ends early leaves the remaining pixels undrawn, as browsers do.
 */
template <typename Emit>
void decompress(const std::span<const std::uint8_t> data, const std::uint32_t minimum_code_size,
                const std::uint64_t pixels, Emit&& emit) {
    if (minimum_code_size < 1U || minimum_code_size > 8U)
        fail("an image has an invalid LZW code size");
    constexpr std::uint32_t table_size = 4096U;
    std::array<std::uint16_t, table_size> prefix{};
    std::array<std::uint8_t, table_size> suffix{};
    std::array<std::uint8_t, table_size> first{};
    std::array<std::uint16_t, table_size> length{};
    std::array<std::uint8_t, table_size> string{};
    const std::uint32_t clear = 1U << minimum_code_size;
    const std::uint32_t end = clear + 1U;
    for (std::uint32_t code = 0U; code < clear; ++code) {
        suffix[code] = first[code] = static_cast<std::uint8_t>(code);
        length[code] = 1U;
    }
    std::uint32_t code_size = minimum_code_size + 1U;
    std::uint32_t next = end + 1U;
    std::optional<std::uint32_t> previous;
    std::uint64_t emitted = 0U;
    std::uint32_t bits = 0U;
    std::uint32_t count = 0U;
    std::size_t position = 0U;
    while (emitted < pixels) {
        while (count < code_size && position < data.size()) {
            bits |= static_cast<std::uint32_t>(data[position++]) << count;
            count += 8U;
        }
        if (count < code_size)
            return;
        const std::uint32_t code = bits & ((1U << code_size) - 1U);
        bits >>= code_size;
        count -= code_size;
        if (code == clear) {
            code_size = minimum_code_size + 1U;
            next = end + 1U;
            previous.reset();
            continue;
        }
        if (code == end)
            return;
        std::uint32_t entry = code;
        if (!previous.has_value()) {
            if (code >= clear)
                fail("an image's LZW stream starts with an undefined code");
        } else if (code > next || (code == next && next >= table_size)) {
            fail("an image's LZW stream uses an undefined code");
        }
        if (previous.has_value() && next < table_size) {
            // The new entry is the previous string plus the first byte of this one; when this
            // code is that very entry (the KwKwK case), its first byte is the previous string's.
            prefix[next] = static_cast<std::uint16_t>(*previous);
            suffix[next] = code == next ? first[*previous] : first[code];
            first[next] = first[*previous];
            length[next] = static_cast<std::uint16_t>(length[*previous] + 1U);
            ++next;
            if (next == (1U << code_size) && code_size < 12U)
                ++code_size;
        }
        // Walk the string back to front, then emit it in order.
        const std::uint32_t string_length = length[entry];
        for (std::uint32_t index = string_length; index > 0U; --index) {
            string[index - 1U] = suffix[entry];
            entry = prefix[entry];
        }
        for (std::uint32_t index = 0U; index < string_length && emitted < pixels; ++index) {
            emit(string[index]);
            ++emitted;
        }
        previous = code;
    }
}

struct Control final {
    std::uint32_t delay_ms = 0U;
    std::uint32_t disposal = 0U;
    std::optional<std::uint8_t> transparent;
};

} // namespace

ImageDimensions inspect_gif(const std::span<const std::uint8_t> bytes) {
    Cursor input(bytes);
    return screen(input).dimensions;
}

DecodedImage decode_gif(const std::span<const std::uint8_t> bytes,
                        const std::uint32_t frame_limit) {
    Cursor input(bytes);
    const Screen header = screen(input);
    const std::uint32_t canvas_width = header.dimensions.width;
    const std::uint32_t canvas_height = header.dimensions.height;
    FrameSink sink(header.dimensions, frame_limit, "GIF");
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(canvas_width) * canvas_height * 4U);
    std::vector<std::uint8_t> saved;
    std::vector<std::uint8_t> data;
    // No NETSCAPE block means the animation plays once.
    std::uint32_t loop_count = 1U;
    Control control;
    while (sink.wants_more()) {
        if (input.at_end()) {
            // Many GIFs omit the trailer; whatever frames came complete still count.
            break;
        }
        const std::uint8_t introducer = input.u8();
        if (introducer == 0x3BU)
            break;
        if (introducer == 0x21U) {
            const std::uint8_t label = input.u8();
            if (label == 0xF9U) {
                const std::span<const std::uint8_t> block = input.take(1U);
                if (block[0U] < 4U)
                    fail("a graphic control extension is malformed");
                const std::span<const std::uint8_t> body = input.take(block[0U]);
                control.disposal = (body[0U] >> 2U) & 0x07U;
                control.delay_ms = (static_cast<std::uint32_t>(body[1U]) |
                                    static_cast<std::uint32_t>(body[2U]) << 8U) *
                                   10U;
                if ((body[0U] & 0x01U) != 0U)
                    control.transparent = body[3U];
                else
                    control.transparent.reset();
                input.sub_blocks(nullptr);
            } else if (label == 0xFFU) {
                const std::uint8_t size = input.u8();
                const std::span<const std::uint8_t> identifier = input.take(size);
                const std::string_view name(reinterpret_cast<const char*>(identifier.data()),
                                            identifier.size());
                data.clear();
                input.sub_blocks(&data);
                // NETSCAPE2.0 (or its ANIMEXTS1.0 twin) sub-block 1 holds the repetition count:
                // 0 repeats forever, n plays n more times after the first, as browsers count it.
                if ((name == "NETSCAPE2.0" || name == "ANIMEXTS1.0") && data.size() >= 3U &&
                    data[0U] == 1U) {
                    const std::uint32_t repeats = static_cast<std::uint32_t>(data[1U]) |
                                                  static_cast<std::uint32_t>(data[2U]) << 8U;
                    loop_count = repeats == 0U ? 0U : repeats + 1U;
                }
            } else {
                // Comments, plain text and unknown extensions.
                input.sub_blocks(nullptr);
            }
            continue;
        }
        if (introducer != 0x2CU)
            fail("an unknown block is present");
        const std::uint32_t left = input.u16();
        const std::uint32_t top = input.u16();
        const std::uint32_t width = input.u16();
        const std::uint32_t height = input.u16();
        const std::uint8_t packed = input.u8();
        Palette local;
        if ((packed & 0x80U) != 0U)
            local = input.palette(packed & 0x07U);
        const bool interlaced = (packed & 0x40U) != 0U;
        const Palette& palette = local.empty() ? header.global : local;
        if (palette.empty())
            fail("an image has no colour table");
        const std::uint32_t minimum_code_size = input.u8();
        data.clear();
        input.sub_blocks(&data);
        sink.reserve_next();

        if (control.disposal == 3U)
            saved = canvas;
        // Rows of an interlaced image arrive in four passes: every 8th row from 0, every 8th
        // from 4, every 4th from 2, then every 2nd from 1.
        constexpr std::array<std::array<std::uint32_t, 2U>, 4U> passes{{
            {0U, 8U},
            {4U, 8U},
            {2U, 4U},
            {1U, 2U},
        }};
        std::uint32_t pass = 0U;
        std::uint32_t row = 0U;
        std::uint32_t column = 0U;
        const auto advance_row = [&] {
            if (!interlaced) {
                ++row;
                return;
            }
            row += passes[pass][1U];
            while (row >= height && pass + 1U < passes.size()) {
                ++pass;
                row = passes[pass][0U];
            }
        };
        if (width != 0U && height != 0U) {
            decompress(
                data, minimum_code_size, static_cast<std::uint64_t>(width) * height,
                [&](const std::uint8_t index) {
                    const std::uint32_t x = left + column;
                    const std::uint32_t y = top + row;
                    if (x < canvas_width && y < canvas_height &&
                        (!control.transparent.has_value() || index != *control.transparent)) {
                        std::uint8_t* const out =
                            canvas.data() + (static_cast<std::size_t>(y) * canvas_width + x) * 4U;
                        // An index past the table draws black, as Pillow pads its palette.
                        if (index < palette.size())
                            std::copy_n(palette[index].begin(), 3U, out);
                        else
                            std::fill_n(out, 3U, std::uint8_t{0U});
                        out[3U] = 255U;
                    }
                    if (++column == width) {
                        column = 0U;
                        advance_row();
                    }
                });
        }
        sink.push(canvas, control.delay_ms);

        if (control.disposal == 2U && left < canvas_width && top < canvas_height) {
            clear_rect(canvas, canvas_width, left, top, std::min(width, canvas_width - left),
                       std::min(height, canvas_height - top));
        } else if (control.disposal == 3U) {
            canvas = saved;
        }
        // A graphic control extension governs only the image that follows it.
        control = Control{};
    }
    return sink.finish(loop_count);
}

} // namespace strata::resource::codec
