#include "resource/image.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "resource/image_codecs.hpp"

namespace strata::resource {
namespace {

[[noreturn]] void fail(const std::string_view reason) {
    throw std::invalid_argument("PNG decode: " + std::string(reason));
}

[[nodiscard]] std::uint32_t big_endian(const std::uint8_t* const bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0U]) << 24U |
           static_cast<std::uint32_t>(bytes[1U]) << 16U |
           static_cast<std::uint32_t>(bytes[2U]) << 8U | static_cast<std::uint32_t>(bytes[3U]);
}

[[nodiscard]] constexpr std::array<std::uint32_t, 256U> crc_table() noexcept {
    std::array<std::uint32_t, 256U> table{};
    for (std::uint32_t index = 0U; index < table.size(); ++index) {
        std::uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ (0xEDB88320U & (0U - (value & 1U)));
        table[index] = value;
    }
    return table;
}

[[nodiscard]] std::uint32_t crc32(const std::span<const std::uint8_t> bytes) noexcept {
    static constexpr std::array<std::uint32_t, 256U> table = crc_table();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes)
        crc = table[(crc ^ byte) & 0xFFU] ^ (crc >> 8U);
    return ~crc;
}

/** LSB-first bit reader over a zlib stream (RFC 1950/1951). */
class Bits final {
  public:
    explicit Bits(const std::span<const std::uint8_t> bytes) noexcept
        : next_(bytes.data()), end_(bytes.data() + bytes.size()) {}

    void refill() noexcept {
        while (count_ <= 56U && next_ != end_) {
            buffer_ |= static_cast<std::uint64_t>(*next_++) << count_;
            count_ += 8U;
        }
    }

    [[nodiscard]] std::uint32_t take(const std::uint32_t count) {
        if (count == 0U)
            return 0U;
        if (count_ < count)
            refill();
        if (count_ < count)
            fail("compressed image data is truncated");
        const auto value = static_cast<std::uint32_t>(buffer_ & ((std::uint64_t{1U} << count) - 1U));
        buffer_ >>= count;
        count_ -= count;
        return value;
    }

    [[nodiscard]] std::uint32_t peek(const std::uint32_t count) noexcept {
        if (count_ < count)
            refill();
        return static_cast<std::uint32_t>(buffer_ & ((std::uint64_t{1U} << count) - 1U));
    }

    [[nodiscard]] std::uint32_t available() const noexcept {
        return count_;
    }

    void drop(const std::uint32_t count) noexcept {
        buffer_ >>= count;
        count_ -= count;
    }

    void align() noexcept {
        drop(count_ % 8U);
    }

  private:
    const std::uint8_t* next_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    std::uint64_t buffer_ = 0U;
    std::uint32_t count_ = 0U;
};

/**
 * A canonical Huffman code. Codes no longer than the lookup width (the longest code, at most
 * fast_bits) resolve in one table lookup; longer codes walk the canonical counts one bit at a time.
 * A small code builds a small table, so a stream of tiny dynamic blocks costs little per block.
 */
class Huffman final {
  public:
    static constexpr std::uint32_t fast_bits = 10U;
    static constexpr std::size_t maximum_symbols = 288U;

    /** Whether a code may leave bit patterns unassigned, as zlib lets literal and distance codes. */
    enum class Completeness : std::uint8_t { required, single_code_allowed };

    void build(const std::span<const std::uint8_t> lengths, const Completeness completeness) {
        counts_.fill(0U);
        for (const std::uint8_t length : lengths)
            ++counts_[length];
        counts_[0U] = 0U;
        std::uint32_t longest = 0U;
        int left = 1;
        for (std::uint32_t length = 1U; length < counts_.size(); ++length) {
            if (counts_[length] != 0U)
                longest = length;
            left = left * 2 - counts_[length];
            if (left < 0)
                fail("a Huffman code is over-subscribed");
        }
        // As zlib decides: a code that leaves patterns unassigned is corrupt, except an empty code
        // and, for literal and distance codes, a single one-bit code.
        if (left > 0 && longest != 0U &&
            (completeness == Completeness::required || longest != 1U)) {
            fail("a Huffman code is incomplete");
        }
        std::array<std::uint16_t, 16U> offsets{};
        for (std::size_t length = 1U; length + 1U < offsets.size(); ++length)
            offsets[length + 1U] = static_cast<std::uint16_t>(offsets[length] + counts_[length]);
        for (std::size_t symbol = 0U; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] != 0U)
                symbols_[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
        }
        width_ = std::clamp(longest, 1U, fast_bits);
        const std::size_t entries = std::size_t{1U} << width_;
        std::fill_n(fast_.begin(), entries, std::uint16_t{0U});
        std::uint32_t code = 0U;
        std::size_t index = 0U;
        for (std::uint32_t length = 1U; length <= width_; ++length) {
            for (std::uint32_t entry = 0U; entry < counts_[length]; ++entry, ++code, ++index) {
                std::uint32_t reversed = 0U;
                for (std::uint32_t bit = 0U; bit < length; ++bit)
                    reversed |= ((code >> bit) & 1U) << (length - 1U - bit);
                const auto value = static_cast<std::uint16_t>(length << 12U | symbols_[index]);
                for (std::size_t fill = reversed; fill < entries; fill += std::size_t{1U} << length)
                    fast_[fill] = value;
            }
            code <<= 1U;
        }
    }

    [[nodiscard]] std::uint32_t decode(Bits& input) const {
        if (input.available() < width_)
            input.refill();
        if (input.available() >= width_) {
            const std::uint16_t entry = fast_[input.peek(width_)];
            if (entry != 0U) {
                input.drop(entry >> 12U);
                return entry & 0x0FFFU;
            }
        }
        // Canonical decode, one bit at a time (as zlib's puff does).
        std::int32_t code = 0;
        std::int32_t first = 0;
        std::int32_t index = 0;
        for (std::size_t length = 1U; length < counts_.size(); ++length) {
            code |= static_cast<std::int32_t>(input.take(1U));
            const std::int32_t count = counts_[length];
            if (code - count < first)
                return symbols_[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        fail("compressed image data contains an undefined Huffman code");
    }

  private:
    std::array<std::uint16_t, 16U> counts_{};
    std::array<std::uint16_t, maximum_symbols> symbols_{};
    std::array<std::uint16_t, std::size_t{1U} << fast_bits> fast_{};
    std::uint32_t width_ = 1U;
};

constexpr std::array<std::uint16_t, 29U> length_base{
    3U,  4U,  5U,  6U,  7U,  8U,  9U,  10U, 11U,  13U,  15U,  17U,  19U,  23U, 27U,
    31U, 35U, 43U, 51U, 59U, 67U, 83U, 99U, 115U, 131U, 163U, 195U, 227U, 258U,
};
constexpr std::array<std::uint8_t, 29U> length_extra{
    0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 1U, 1U, 1U, 2U, 2U, 2U,
    2U, 3U, 3U, 3U, 3U, 4U, 4U, 4U, 4U, 5U, 5U, 5U, 5U, 0U,
};
constexpr std::array<std::uint16_t, 30U> distance_base{
    1U,    2U,    3U,    4U,    5U,    7U,    9U,    13U,    17U,    25U,
    33U,   49U,   65U,   97U,   129U,  193U,  257U,  385U,   513U,   769U,
    1025U, 1537U, 2049U, 3073U, 4097U, 6145U, 8193U, 12289U, 16385U, 24577U,
};
constexpr std::array<std::uint8_t, 30U> distance_extra{
    0U, 0U, 0U, 0U, 1U, 1U, 2U, 2U,  3U,  3U,  4U,  4U,  5U,  5U,  6U,
    6U, 7U, 7U, 8U, 8U, 9U, 9U, 10U, 10U, 11U, 11U, 12U, 12U, 13U, 13U,
};

/** The fixed codes of RFC 1951 3.2.6, built once rather than for every fixed block. */
struct FixedCodes final {
    FixedCodes() {
        std::array<std::uint8_t, 288U> lengths{};
        std::fill(lengths.begin(), lengths.begin() + 144, std::uint8_t{8U});
        std::fill(lengths.begin() + 144, lengths.begin() + 256, std::uint8_t{9U});
        std::fill(lengths.begin() + 256, lengths.begin() + 280, std::uint8_t{7U});
        std::fill(lengths.begin() + 280, lengths.end(), std::uint8_t{8U});
        literals.build(lengths, Huffman::Completeness::required);
        // Distance codes 30 and 31 complete the code but name no distance; decoding rejects them.
        std::array<std::uint8_t, 32U> distance_lengths{};
        distance_lengths.fill(5U);
        distances.build(distance_lengths, Huffman::Completeness::required);
    }

    Huffman literals;
    Huffman distances;
};

/**
 * Decodes one block's symbols. `limit` is the scanline size, which no stream may inflate past, and
 * `window` the zlib header's window, which no back-reference may reach beyond.
 */
void inflate_codes(Bits& input, const Huffman& literals, const Huffman& distances,
                   std::vector<std::uint8_t>& output, const std::size_t limit,
                   const std::size_t window) {
    for (;;) {
        const std::uint32_t symbol = literals.decode(input);
        if (symbol < 256U) {
            if (output.size() == limit)
                fail("image data inflates beyond its scanlines");
            output.push_back(static_cast<std::uint8_t>(symbol));
            continue;
        }
        if (symbol == 256U)
            return;
        const std::uint32_t length_symbol = symbol - 257U;
        if (length_symbol >= length_base.size())
            fail("compressed image data contains an invalid length");
        const std::size_t length =
            length_base[length_symbol] + input.take(length_extra[length_symbol]);
        const std::uint32_t distance_symbol = distances.decode(input);
        if (distance_symbol >= distance_base.size())
            fail("compressed image data contains an invalid distance");
        const std::size_t distance =
            distance_base[distance_symbol] + input.take(distance_extra[distance_symbol]);
        if (distance > output.size())
            fail("compressed image data refers before its start");
        if (distance > window)
            fail("compressed image data refers beyond its window");
        if (length > limit - output.size())
            fail("image data inflates beyond its scanlines");
        const std::size_t from = output.size() - distance;
        for (std::size_t offset = 0U; offset < length; ++offset)
            output.push_back(output[from + offset]);
    }
}

/** Inflates one zlib stream (RFC 1950) whose result must be exactly `expected` bytes. */
[[nodiscard]] std::vector<std::uint8_t> inflate(const std::span<const std::uint8_t> stream,
                                                const std::size_t expected) {
    if (stream.size() < 6U)
        fail("compressed image data is truncated");
    const std::uint32_t method = stream[0U];
    const std::uint32_t flags = stream[1U];
    if ((method & 0x0FU) != 8U || (method >> 4U) > 7U || (method << 8U | flags) % 31U != 0U ||
        (flags & 0x20U) != 0U) {
        fail("image data is not a zlib deflate stream");
    }
    // CINFO is the base-two logarithm of the window minus eight; 7 is the 32 KiB deflate maximum.
    const std::size_t window = std::size_t{1U} << ((method >> 4U) + 8U);
    Bits input(stream.subspan(2U));
    std::vector<std::uint8_t> output;
    // Reserve only what the data present can inflate to: a header that declares a large image
    // must not allocate it before its data does. Deflate's densest code expands 2 bits into 258
    // bytes.
    constexpr std::size_t maximum_expansion = 258U * 4U;
    output.reserve(stream.size() < expected / maximum_expansion
                       ? stream.size() * maximum_expansion
                       : expected);
    static const FixedCodes fixed;
    Huffman literals;
    Huffman distances;
    bool final_block = false;
    while (!final_block) {
        final_block = input.take(1U) != 0U;
        const std::uint32_t type = input.take(2U);
        if (type == 0U) {
            input.align();
            const std::uint32_t length = input.take(16U);
            const std::uint32_t complement = input.take(16U);
            if ((length ^ 0xFFFFU) != complement)
                fail("a stored block's length is corrupt");
            if (length > expected - output.size())
                fail("image data inflates beyond its scanlines");
            for (std::uint32_t index = 0U; index < length; ++index)
                output.push_back(static_cast<std::uint8_t>(input.take(8U)));
        } else if (type == 1U) {
            inflate_codes(input, fixed.literals, fixed.distances, output, expected, window);
        } else if (type == 2U) {
            const std::uint32_t literal_count = input.take(5U) + 257U;
            const std::uint32_t distance_count = input.take(5U) + 1U;
            const std::uint32_t code_count = input.take(4U) + 4U;
            if (literal_count > 286U || distance_count > 30U)
                fail("a dynamic block declares too many codes");
            constexpr std::array<std::uint8_t, 19U> order{
                16U, 17U, 18U, 0U, 8U, 7U, 9U, 6U, 10U, 5U, 11U, 4U, 12U, 3U, 13U, 2U, 14U, 1U, 15U,
            };
            std::array<std::uint8_t, 19U> code_lengths{};
            for (std::uint32_t index = 0U; index < code_count; ++index)
                code_lengths[order[index]] = static_cast<std::uint8_t>(input.take(3U));
            Huffman lengths_code;
            lengths_code.build(code_lengths, Huffman::Completeness::required);
            std::array<std::uint8_t, 316U> lengths{};
            std::uint32_t index = 0U;
            while (index < literal_count + distance_count) {
                const std::uint32_t symbol = lengths_code.decode(input);
                if (symbol < 16U) {
                    lengths[index++] = static_cast<std::uint8_t>(symbol);
                    continue;
                }
                std::uint8_t value = 0U;
                std::uint32_t repeat = 0U;
                if (symbol == 16U) {
                    if (index == 0U)
                        fail("a dynamic block repeats a length before the first");
                    value = lengths[index - 1U];
                    repeat = 3U + input.take(2U);
                } else if (symbol == 17U) {
                    repeat = 3U + input.take(3U);
                } else {
                    repeat = 11U + input.take(7U);
                }
                if (repeat > literal_count + distance_count - index)
                    fail("a dynamic block's code lengths overrun their table");
                std::fill_n(lengths.begin() + index, repeat, value);
                index += repeat;
            }
            if (lengths[256U] == 0U)
                fail("a dynamic block has no end-of-block code");
            literals.build(std::span<const std::uint8_t>(lengths.data(), literal_count),
                           Huffman::Completeness::single_code_allowed);
            distances.build(
                std::span<const std::uint8_t>(lengths.data() + literal_count, distance_count),
                Huffman::Completeness::single_code_allowed);
            inflate_codes(input, literals, distances, output, expected, window);
        } else {
            fail("compressed image data uses a reserved block type");
        }
    }
    if (output.size() != expected)
        fail("image data inflates to fewer bytes than its scanlines");
    input.align();
    std::uint32_t checksum = 0U;
    for (int byte = 0; byte < 4; ++byte)
        checksum = checksum << 8U | input.take(8U);
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (std::size_t offset = 0U; offset < output.size();) {
        // 5552 bytes is the longest run before the sums can overflow 32 bits.
        const std::size_t end = std::min(output.size(), offset + 5552U);
        for (; offset < end; ++offset) {
            a += output[offset];
            b += a;
        }
        a %= 65521U;
        b %= 65521U;
    }
    if ((b << 16U | a) != checksum)
        fail("image data fails its zlib checksum");
    return output;
}

[[nodiscard]] std::uint8_t paeth(const std::uint8_t left, const std::uint8_t above,
                                 const std::uint8_t corner) noexcept {
    const int estimate = static_cast<int>(left) + above - corner;
    const int to_left = std::abs(estimate - left);
    const int to_above = std::abs(estimate - above);
    const int to_corner = std::abs(estimate - corner);
    if (to_left <= to_above && to_left <= to_corner)
        return left;
    return to_above <= to_corner ? above : corner;
}

struct Header final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t depth = 0U;
    std::uint32_t color = 0U;
    bool interlaced = false;

    [[nodiscard]] std::uint32_t channels() const noexcept {
        switch (color) {
        case 2U: return 3U;
        case 4U: return 2U;
        case 6U: return 4U;
        default: return 1U;
        }
    }
    [[nodiscard]] std::size_t row_bytes(const std::uint32_t pixels) const noexcept {
        return (static_cast<std::size_t>(pixels) * channels() * depth + 7U) / 8U;
    }
};

struct Pass final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t dx = 1U;
    std::uint32_t dy = 1U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
};

[[nodiscard]] std::vector<Pass> passes(const Header& header) {
    if (!header.interlaced)
        return {Pass{0U, 0U, 1U, 1U, header.width, header.height}};
    constexpr std::array<std::array<std::uint32_t, 4U>, 7U> adam7{{
        {0U, 0U, 8U, 8U},
        {4U, 0U, 8U, 8U},
        {0U, 4U, 4U, 8U},
        {2U, 0U, 4U, 4U},
        {0U, 2U, 2U, 4U},
        {1U, 0U, 2U, 2U},
        {0U, 1U, 1U, 2U},
    }};
    std::vector<Pass> result;
    for (const auto& [x, y, dx, dy] : adam7) {
        const std::uint32_t width = header.width > x ? (header.width - x + dx - 1U) / dx : 0U;
        const std::uint32_t height = header.height > y ? (header.height - y + dy - 1U) / dy : 0U;
        result.push_back(Pass{x, y, dx, dy, width, height});
    }
    return result;
}

/** One APNG frame: its fcTL and, unless it is the default image, its fdAT data. */
struct FrameControl final {
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t delay_ms = 0U;
    /** APNG_DISPOSE_OP_NONE, _BACKGROUND or _PREVIOUS. */
    std::uint8_t dispose = 0U;
    /** APNG_BLEND_OP_SOURCE or _OVER. */
    std::uint8_t blend = 0U;
    /** The IDAT image is this frame, rather than a fallback the animation skips. */
    bool default_image = false;
    std::vector<std::uint8_t> data;
};

struct Png final {
    Header header;
    std::vector<std::uint8_t> idat;
    std::vector<std::array<std::uint8_t, 4U>> palette;
    std::optional<std::array<std::uint16_t, 3U>> transparent;
    /** An acTL preceded the image data, so the fcTL frames, not IDAT alone, are the image. */
    bool animated = false;
    std::uint32_t declared_frames = 0U;
    std::uint32_t plays = 0U;
    std::vector<FrameControl> frames;
};

/**
 * Validates the chunk stream and gathers what decoding needs. APNG chunks are read only when
 * `animation` asks for them; otherwise they are ancillary chunks like any other and ignored.
 */
[[nodiscard]] Png parse(const std::span<const std::uint8_t> bytes,
                        const std::optional<ImageDimensions> expected, const bool animation) {
    constexpr std::array<std::uint8_t, 8U> signature{
        0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU,
    };
    if (bytes.size() < signature.size() ||
        !std::equal(signature.begin(), signature.end(), bytes.begin())) {
        fail("bytes are not a PNG");
    }
    Png png;
    Header& header = png.header;
    bool have_header = false;
    bool idat_finished = false;
    bool ended = false;
    std::vector<std::uint8_t>& compressed = png.idat;
    std::vector<std::uint8_t> palette_alpha;
    std::uint32_t sequence = 0U;
    bool seen_animation_control = false;
    std::size_t offset = signature.size();
    while (!ended) {
        if (bytes.size() - offset < 12U)
            fail("a chunk is truncated");
        const std::uint32_t length = big_endian(bytes.data() + offset);
        if (length > 0x7FFFFFFFU || length > bytes.size() - offset - 12U)
            fail("a chunk is truncated");
        const std::span<const std::uint8_t> typed = bytes.subspan(offset + 4U, length + 4U);
        const std::string_view type(reinterpret_cast<const char*>(typed.data()), 4U);
        const std::span<const std::uint8_t> data = typed.subspan(4U);
        const bool critical = (type[0U] & 0x20) == 0;
        offset += 12U + length;
        if (crc32(typed) != big_endian(typed.data() + typed.size())) {
            // As libpng does: a damaged critical chunk fails, a damaged ancillary one is ignored.
            if (critical)
                fail("a critical chunk fails its CRC");
            continue;
        }
        if (!have_header && type != "IHDR")
            fail("the first chunk is not IHDR");
        if (type == "IHDR") {
            if (have_header || length != 13U)
                fail("IHDR is repeated or malformed");
            header.width = big_endian(data.data());
            header.height = big_endian(data.data() + 4U);
            header.depth = data[8U];
            header.color = data[9U];
            header.interlaced = data[12U] == 1U;
            const bool depth_valid =
                (header.color == 0U && (header.depth == 1U || header.depth == 2U ||
                                        header.depth == 4U || header.depth == 8U ||
                                        header.depth == 16U)) ||
                (header.color == 3U && (header.depth == 1U || header.depth == 2U ||
                                        header.depth == 4U || header.depth == 8U)) ||
                ((header.color == 2U || header.color == 4U || header.color == 6U) &&
                 (header.depth == 8U || header.depth == 16U));
            if (!depth_valid || data[10U] != 0U || data[11U] != 0U || data[12U] > 1U)
                fail("IHDR describes an unsupported pixel format");
            if (expected.has_value() &&
                (header.width != expected->width || header.height != expected->height)) {
                fail("dimensions disagree with the resource declaration");
            }
            validate_image_dimensions(ImageDimensions{header.width, header.height});
            if (static_cast<std::uint64_t>(header.width) * header.height >
                maximum_png_decode_pixels) {
                fail("the image has more pixels than the decoder accepts");
            }
            have_header = true;
        } else if (type == "PLTE") {
            if (!compressed.empty() || length % 3U != 0U || length == 0U || length > 768U)
                fail("PLTE is misplaced or malformed");
            png.palette.clear();
            for (std::size_t entry = 0U; entry < length; entry += 3U)
                png.palette.push_back({data[entry], data[entry + 1U], data[entry + 2U], 255U});
        } else if (type == "tRNS") {
            if (!compressed.empty())
                fail("tRNS follows image data");
            if (header.color == 3U) {
                palette_alpha.assign(data.begin(), data.end());
            } else if (header.color == 0U && length == 2U) {
                const auto gray = static_cast<std::uint16_t>(data[0U] << 8U | data[1U]);
                png.transparent = std::array<std::uint16_t, 3U>{gray, gray, gray};
            } else if (header.color == 2U && length == 6U) {
                png.transparent = std::array<std::uint16_t, 3U>{
                    static_cast<std::uint16_t>(data[0U] << 8U | data[1U]),
                    static_cast<std::uint16_t>(data[2U] << 8U | data[3U]),
                    static_cast<std::uint16_t>(data[4U] << 8U | data[5U]),
                };
            }
        } else if (type == "IDAT") {
            if (idat_finished)
                fail("image data chunks are not consecutive");
            compressed.insert(compressed.end(), data.begin(), data.end());
        } else if (type == "IEND") {
            ended = true;
        } else if (animation && type == "acTL") {
            // An acTL after the image data makes no animation; the file is a still PNG.
            if (seen_animation_control)
                fail("acTL is repeated");
            seen_animation_control = true;
            if (compressed.empty()) {
                if (length != 8U)
                    fail("acTL is malformed");
                png.animated = true;
                png.declared_frames = big_endian(data.data());
                png.plays = big_endian(data.data() + 4U);
                if (png.declared_frames == 0U || png.declared_frames > 0x7FFFFFFFU)
                    fail("acTL declares an invalid frame count");
            }
        } else if (animation && png.animated && type == "fcTL") {
            if (length != 26U || big_endian(data.data()) != sequence++)
                fail("fcTL is malformed or out of sequence");
            FrameControl frame;
            frame.width = big_endian(data.data() + 4U);
            frame.height = big_endian(data.data() + 8U);
            frame.x = big_endian(data.data() + 12U);
            frame.y = big_endian(data.data() + 16U);
            const std::uint64_t numerator = static_cast<std::uint64_t>(data[20U]) << 8U | data[21U];
            std::uint64_t denominator = static_cast<std::uint64_t>(data[22U]) << 8U | data[23U];
            // A zero denominator means hundredths of a second.
            if (denominator == 0U)
                denominator = 100U;
            frame.delay_ms =
                static_cast<std::uint32_t>((numerator * 1000U + denominator / 2U) / denominator);
            frame.dispose = data[24U];
            frame.blend = data[25U];
            frame.default_image = compressed.empty();
            if (frame.width == 0U || frame.height == 0U ||
                static_cast<std::uint64_t>(frame.x) + frame.width > header.width ||
                static_cast<std::uint64_t>(frame.y) + frame.height > header.height ||
                frame.dispose > 2U || frame.blend > 1U) {
                fail("fcTL places a frame outside the image or uses an unknown operation");
            }
            if (frame.default_image &&
                (!png.frames.empty() || frame.x != 0U || frame.y != 0U ||
                 frame.width != header.width || frame.height != header.height)) {
                fail("the default image's fcTL must cover the whole image");
            }
            if (!frame.default_image && !png.frames.empty() && !png.frames.back().default_image &&
                png.frames.back().data.empty()) {
                fail("an APNG frame has no data");
            }
            png.frames.push_back(std::move(frame));
        } else if (animation && png.animated && type == "fdAT") {
            if (length < 4U || big_endian(data.data()) != sequence++)
                fail("fdAT is malformed or out of sequence");
            if (png.frames.empty() || png.frames.back().default_image)
                fail("fdAT has no frame to belong to");
            png.frames.back().data.insert(png.frames.back().data.end(), data.begin() + 4,
                                          data.end());
        } else if (critical) {
            fail("an unknown critical chunk is present");
        }
        if (type != "IDAT" && !compressed.empty())
            idat_finished = true;
    }
    if (compressed.empty())
        fail("the image has no data");
    if (header.color == 3U) {
        if (png.palette.empty())
            fail("an indexed image has no palette");
        for (std::size_t entry = 0U; entry < png.palette.size() && entry < palette_alpha.size();
             ++entry) {
            png.palette[entry][3U] = palette_alpha[entry];
        }
    }
    if (png.animated) {
        if (png.frames.size() != png.declared_frames)
            fail("acTL's frame count disagrees with the frames present");
        if (!png.frames.back().default_image && png.frames.back().data.empty())
            fail("an APNG frame has no data");
    }
    return png;
}

/**
 * Inflates and unfilters one image's scanlines into RGBA8: the whole image, or an APNG frame,
 * whose header is the image's with the frame's dimensions.
 */
[[nodiscard]] std::vector<std::uint8_t>
decode_pixels(const Png& png, const Header& header,
              const std::span<const std::uint8_t> compressed) {
    const auto& palette = png.palette;
    const auto& transparent = png.transparent;
    const std::vector<Pass> layout = passes(header);
    std::size_t scanline_bytes = 0U;
    for (const Pass& pass : layout) {
        if (pass.width != 0U && pass.height != 0U)
            scanline_bytes += (header.row_bytes(pass.width) + 1U) * pass.height;
    }
    const std::vector<std::uint8_t> filtered = inflate(compressed, scanline_bytes);

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(header.width) * header.height * 4U);
    const std::size_t filter_stride = std::max<std::size_t>(1U, header.channels() * header.depth / 8U);
    const std::uint32_t sample_mask = header.depth >= 16U ? 0xFFFFU : (1U << header.depth) - 1U;
    std::vector<std::uint8_t> previous;
    std::vector<std::uint8_t> row;
    std::size_t at = 0U;
    for (const Pass& pass : layout) {
        if (pass.width == 0U || pass.height == 0U)
            continue;
        const std::size_t stride = header.row_bytes(pass.width);
        previous.assign(stride, 0U);
        row.resize(stride);
        for (std::uint32_t y = 0U; y < pass.height; ++y) {
            const std::uint8_t filter = filtered[at++];
            std::copy_n(filtered.begin() + static_cast<std::ptrdiff_t>(at), stride, row.begin());
            at += stride;
            for (std::size_t x = 0U; x < stride; ++x) {
                const std::uint8_t left = x >= filter_stride ? row[x - filter_stride] : 0U;
                const std::uint8_t above = previous[x];
                const std::uint8_t corner = x >= filter_stride ? previous[x - filter_stride] : 0U;
                switch (filter) {
                case 0U: break;
                case 1U: row[x] = static_cast<std::uint8_t>(row[x] + left); break;
                case 2U: row[x] = static_cast<std::uint8_t>(row[x] + above); break;
                case 3U: row[x] = static_cast<std::uint8_t>(row[x] + ((left + above) >> 1U)); break;
                case 4U: row[x] = static_cast<std::uint8_t>(row[x] + paeth(left, above, corner)); break;
                default: fail("a scanline uses an unknown filter");
                }
            }
            // Samples are taken as stored: no gAMA, sRGB or iCCP conversion, 16-bit samples keep
            // their high byte, and low bit depths expand by bit replication. tRNS colours compare
            // at full depth. That is what libpng's expand/strip_16 and WIC's 32bppRGBA produce.
            const auto sample = [&](const std::uint32_t index) -> std::uint32_t {
                if (header.depth == 16U)
                    return static_cast<std::uint32_t>(row[index * 2U]) << 8U | row[index * 2U + 1U];
                if (header.depth == 8U)
                    return row[index];
                const std::size_t bit = static_cast<std::size_t>(index) * header.depth;
                return (row[bit / 8U] >> (8U - header.depth - bit % 8U)) & sample_mask;
            };
            const auto narrow = [&](const std::uint32_t value) -> std::uint8_t {
                if (header.depth == 16U)
                    return static_cast<std::uint8_t>(value >> 8U);
                return static_cast<std::uint8_t>(value * 255U / sample_mask);
            };
            const std::size_t y_pixel = pass.y + static_cast<std::size_t>(y) * pass.dy;
            for (std::uint32_t x = 0U; x < pass.width; ++x) {
                const std::size_t x_pixel = pass.x + static_cast<std::size_t>(x) * pass.dx;
                std::uint8_t* const out = pixels.data() + (y_pixel * header.width + x_pixel) * 4U;
                const std::uint32_t first = x * header.channels();
                switch (header.color) {
                case 0U: {
                    const std::uint32_t gray = sample(first);
                    out[0U] = out[1U] = out[2U] = narrow(gray);
                    out[3U] = transparent.has_value() && (*transparent)[0U] == gray ? 0U : 255U;
                    break;
                }
                case 2U: {
                    const std::uint32_t red = sample(first);
                    const std::uint32_t green = sample(first + 1U);
                    const std::uint32_t blue = sample(first + 2U);
                    out[0U] = narrow(red);
                    out[1U] = narrow(green);
                    out[2U] = narrow(blue);
                    out[3U] = transparent.has_value() && (*transparent)[0U] == red &&
                                      (*transparent)[1U] == green && (*transparent)[2U] == blue
                                  ? 0U
                                  : 255U;
                    break;
                }
                case 3U: {
                    const std::uint32_t index = sample(first);
                    if (index >= palette.size())
                        fail("a pixel indexes past the palette");
                    std::copy(palette[index].begin(), palette[index].end(), out);
                    break;
                }
                case 4U:
                    out[0U] = out[1U] = out[2U] = narrow(sample(first));
                    out[3U] = narrow(sample(first + 1U));
                    break;
                default:
                    out[0U] = narrow(sample(first));
                    out[1U] = narrow(sample(first + 1U));
                    out[2U] = narrow(sample(first + 2U));
                    out[3U] = narrow(sample(first + 3U));
                    break;
                }
            }
            previous.swap(row);
            row.resize(stride);
        }
    }
    return pixels;
}

} // namespace

std::vector<std::uint8_t> decode_png_rgba8(const std::span<const std::uint8_t> bytes,
                                           const ImageDimensions expected) {
    const Png png = parse(bytes, expected, false);
    return decode_pixels(png, png.header, png.idat);
}

namespace codec {

DecodedImage decode_png(const std::span<const std::uint8_t> bytes,
                        const std::uint32_t frame_limit) {
    const Png png = parse(bytes, std::nullopt, true);
    const std::uint32_t width = png.header.width;
    FrameSink sink(ImageDimensions{width, png.header.height}, frame_limit, "PNG");
    if (!png.animated) {
        sink.push(decode_pixels(png, png.header, png.idat), 0U);
        return sink.finish(1U);
    }
    // APNG composites each frame onto a canvas that starts transparent black (APNG spec 4.3).
    std::vector<std::uint8_t> canvas(static_cast<std::size_t>(width) * png.header.height * 4U);
    std::vector<std::uint8_t> saved;
    for (std::size_t index = 0U; index < png.frames.size() && sink.wants_more(); ++index) {
        const FrameControl& frame = png.frames[index];
        sink.reserve_next();
        Header region = png.header;
        region.width = frame.width;
        region.height = frame.height;
        const std::vector<std::uint8_t> pixels =
            decode_pixels(png, region, frame.default_image ? png.idat : frame.data);
        // The first frame has nothing to revert to, so PREVIOUS acts as BACKGROUND.
        const std::uint32_t dispose = index == 0U && frame.dispose == 2U ? 1U : frame.dispose;
        if (dispose == 2U)
            saved = canvas;
        for (std::uint32_t y = 0U; y < frame.height; ++y) {
            const std::uint8_t* source =
                pixels.data() + static_cast<std::size_t>(y) * frame.width * 4U;
            std::uint8_t* target =
                canvas.data() + ((static_cast<std::size_t>(frame.y) + y) * width + frame.x) * 4U;
            if (frame.blend == 0U) {
                std::copy_n(source, static_cast<std::size_t>(frame.width) * 4U, target);
                continue;
            }
            for (std::uint32_t x = 0U; x < frame.width; ++x, source += 4U, target += 4U)
                blend_over(target, source);
        }
        sink.push(canvas, frame.delay_ms);
        if (dispose == 1U)
            clear_rect(canvas, width, frame.x, frame.y, frame.width, frame.height);
        else if (dispose == 2U)
            canvas = std::move(saved);
    }
    return sink.finish(png.plays);
}

} // namespace codec

} // namespace strata::resource
