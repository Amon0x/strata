// WebP: the RIFF container, VP8L lossless images, ALPH alpha planes and ANIM/ANMF animation.
// Lossy VP8 key frames decode in vp8_decode.cpp. Behaviour follows the WebP container and lossless
// bitstream specifications (RFC 9649) and, where they leave room, libwebp's decoder and its
// WebPAnimDecoder, so frames match what libwebp-based viewers show.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    throw std::invalid_argument("WebP decode: " + std::string(reason));
}

[[nodiscard]] std::uint32_t le16(const std::uint8_t* const bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0U]) | static_cast<std::uint32_t>(bytes[1U]) << 8U;
}

[[nodiscard]] std::uint32_t le24(const std::uint8_t* const bytes) noexcept {
    return le16(bytes) | static_cast<std::uint32_t>(bytes[2U]) << 16U;
}

[[nodiscard]] std::uint32_t le32(const std::uint8_t* const bytes) noexcept {
    return le24(bytes) | static_cast<std::uint32_t>(bytes[3U]) << 24U;
}

[[nodiscard]] std::uint32_t div_round_up(const std::uint32_t value, const std::uint32_t bits) {
    return (value + (std::uint32_t{1U} << bits) - 1U) >> bits;
}

// --- VP8L ---------------------------------------------------------------------------------------

/** LSB-first bit reader over a VP8L stream. Reading past the end is an error, never zeros. */
class BitReader final {
  public:
    explicit BitReader(const std::span<const std::uint8_t> bytes) noexcept
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
            fail("the lossless bitstream is truncated");
        const auto value =
            static_cast<std::uint32_t>(buffer_ & ((std::uint64_t{1U} << count) - 1U));
        buffer_ >>= count;
        count_ -= count;
        return value;
    }

    /** The next `count` bits, zero-padded past the end; callers check available() before use. */
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

  private:
    const std::uint8_t* next_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    std::uint64_t buffer_ = 0U;
    std::uint32_t count_ = 0U;
};

/**
 * A canonical prefix code, packed like deflate's. Codes up to root_bits long resolve in one lookup
 * of a table no wider than the longest code; longer ones walk the canonical counts bit by bit. A
 * code with a single symbol takes no bits at all, as the format allows.
 */
class PrefixCode final {
  public:
    static constexpr std::uint32_t root_bits = 8U;
    static constexpr std::uint32_t maximum_length = 15U;

    /** Builds the code; returns the bytes its tables hold, which the decoder budgets. */
    std::size_t build(const std::span<const std::uint8_t> lengths) {
        counts_.fill(0U);
        std::size_t used = 0U;
        std::uint32_t only = 0U;
        for (std::size_t symbol = 0U; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] == 0U)
                continue;
            ++counts_[lengths[symbol]];
            ++used;
            only = static_cast<std::uint32_t>(symbol);
        }
        symbols_.clear();
        fast_.clear();
        if (used == 0U)
            fail("a prefix code has no symbols");
        if (used == 1U) {
            single_ = true;
            symbol_ = only;
            return 0U;
        }
        single_ = false;
        // As libwebp decides: apart from a single symbol, every code must be complete.
        std::int64_t left = 1;
        std::uint32_t longest = 0U;
        for (std::uint32_t length = 1U; length <= maximum_length; ++length) {
            left = left * 2 - counts_[length];
            if (left < 0)
                fail("a prefix code is over-subscribed");
            if (counts_[length] != 0U)
                longest = length;
        }
        if (left != 0)
            fail("a prefix code is incomplete");
        std::array<std::uint32_t, maximum_length + 2U> offsets{};
        for (std::uint32_t length = 1U; length <= maximum_length; ++length)
            offsets[length + 1U] = offsets[length] + counts_[length];
        symbols_.resize(used);
        for (std::size_t symbol = 0U; symbol < lengths.size(); ++symbol) {
            if (lengths[symbol] != 0U)
                symbols_[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
        }
        width_ = std::min(longest, root_bits);
        const std::size_t entries = std::size_t{1U} << width_;
        fast_.assign(entries, 0U);
        std::uint32_t code = 0U;
        std::size_t index = 0U;
        for (std::uint32_t length = 1U; length <= width_; ++length) {
            for (std::uint32_t entry = 0U; entry < counts_[length]; ++entry, ++code, ++index) {
                std::uint32_t reversed = 0U;
                for (std::uint32_t bit = 0U; bit < length; ++bit)
                    reversed |= ((code >> bit) & 1U) << (length - 1U - bit);
                const std::uint32_t value = length << 16U | symbols_[index];
                for (std::size_t fill = reversed; fill < entries; fill += std::size_t{1U} << length)
                    fast_[fill] = value;
            }
            code <<= 1U;
        }
        return fast_.size() * sizeof(std::uint32_t) + symbols_.size() * sizeof(std::uint16_t);
    }

    [[nodiscard]] std::uint32_t decode(BitReader& input) const {
        if (single_)
            return symbol_;
        const std::uint32_t entry = fast_[input.peek(width_)];
        if (entry != 0U) {
            const std::uint32_t length = entry >> 16U;
            if (length > input.available())
                fail("the lossless bitstream is truncated");
            input.drop(length);
            return entry & 0xFFFFU;
        }
        // Longer than the table: canonical decode, one bit at a time.
        std::int32_t code = 0;
        std::int32_t first = 0;
        std::int32_t index = 0;
        for (std::uint32_t length = 1U; length <= maximum_length; ++length) {
            code |= static_cast<std::int32_t>(input.take(1U));
            const auto count = static_cast<std::int32_t>(counts_[length]);
            if (code - count < first)
                return symbols_[static_cast<std::size_t>(index + (code - first))];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        fail("the lossless bitstream contains an undefined prefix code");
    }

  private:
    std::array<std::uint16_t, maximum_length + 1U> counts_{};
    std::vector<std::uint16_t> symbols_;
    std::vector<std::uint32_t> fast_;
    std::uint32_t width_ = 1U;
    std::uint32_t symbol_ = 0U;
    bool single_ = false;
};

/** The five codes of one prefix-code group: green/length/cache, red, blue, alpha, distance. */
using CodeGroup = std::array<PrefixCode, 5U>;

constexpr std::uint32_t literal_alphabet = 256U;
constexpr std::uint32_t length_codes = 24U;
constexpr std::uint32_t distance_alphabet = 40U;
constexpr std::uint32_t maximum_cache_bits = 11U;

/**
 * The tables of every prefix-code group an image keeps. Groups are indexed by 16 bits, so a small
 * stream can name 65536 of them; encoders use a few hundred, and this caps what they may cost.
 */
constexpr std::size_t maximum_code_table_bytes = std::size_t{64U} * 1024U * 1024U;

/** The 120 short distance codes: (dx, dy) neighbours, nearest first (RFC 9649 section 4.2.2). */
constexpr std::array<std::array<std::int8_t, 2U>, 120U> distance_neighbours{{
    {0, 1},  {1, 0},  {1, 1},  {-1, 1}, {0, 2},  {2, 0},  {1, 2},  {-1, 2}, {2, 1},  {-2, 1},
    {2, 2},  {-2, 2}, {0, 3},  {3, 0},  {1, 3},  {-1, 3}, {3, 1},  {-3, 1}, {2, 3},  {-2, 3},
    {3, 2},  {-3, 2}, {0, 4},  {4, 0},  {1, 4},  {-1, 4}, {4, 1},  {-4, 1}, {3, 3},  {-3, 3},
    {2, 4},  {-2, 4}, {4, 2},  {-4, 2}, {0, 5},  {3, 4},  {-3, 4}, {4, 3},  {-4, 3}, {5, 0},
    {1, 5},  {-1, 5}, {5, 1},  {-5, 1}, {2, 5},  {-2, 5}, {5, 2},  {-5, 2}, {4, 4},  {-4, 4},
    {3, 5},  {-3, 5}, {5, 3},  {-5, 3}, {0, 6},  {6, 0},  {1, 6},  {-1, 6}, {6, 1},  {-6, 1},
    {2, 6},  {-2, 6}, {6, 2},  {-6, 2}, {4, 5},  {-4, 5}, {5, 4},  {-5, 4}, {3, 6},  {-3, 6},
    {6, 3},  {-6, 3}, {0, 7},  {7, 0},  {1, 7},  {-1, 7}, {5, 5},  {-5, 5}, {7, 1},  {-7, 1},
    {4, 6},  {-4, 6}, {6, 4},  {-6, 4}, {2, 7},  {-2, 7}, {7, 2},  {-7, 2}, {3, 7},  {-3, 7},
    {7, 3},  {-7, 3}, {5, 6},  {-5, 6}, {6, 5},  {-6, 5}, {8, 0},  {4, 7},  {-4, 7}, {7, 4},
    {-7, 4}, {8, 1},  {8, 2},  {6, 6},  {-6, 6}, {8, 3},  {5, 7},  {-5, 7}, {7, 5},  {-7, 5},
    {8, 4},  {6, 7},  {-6, 7}, {7, 6},  {-7, 6}, {8, 5},  {7, 7},  {-7, 7}, {8, 6},  {8, 7},
}};

/** Adds two ARGB pixels channel by channel, modulo 256. */
[[nodiscard]] constexpr std::uint32_t add_pixels(const std::uint32_t a,
                                                 const std::uint32_t b) noexcept {
    return (((a & 0xFF00FF00U) + (b & 0xFF00FF00U)) & 0xFF00FF00U) |
           (((a & 0x00FF00FFU) + (b & 0x00FF00FFU)) & 0x00FF00FFU);
}

[[nodiscard]] constexpr std::uint32_t average(const std::uint32_t a,
                                              const std::uint32_t b) noexcept {
    return (((a ^ b) & 0xFEFEFEFEU) >> 1U) + (a & b);
}

[[nodiscard]] constexpr std::int32_t channel(const std::uint32_t pixel,
                                             const std::uint32_t shift) noexcept {
    return static_cast<std::int32_t>((pixel >> shift) & 0xFFU);
}

[[nodiscard]] std::uint32_t select(const std::uint32_t left, const std::uint32_t top,
                                   const std::uint32_t top_left) noexcept {
    // Picks whichever of left and top is nearer, in Manhattan distance, to left + top - top_left.
    std::int32_t to_left = 0;
    std::int32_t to_top = 0;
    for (std::uint32_t shift = 0U; shift < 32U; shift += 8U) {
        to_left += std::abs(channel(top, shift) - channel(top_left, shift));
        to_top += std::abs(channel(left, shift) - channel(top_left, shift));
    }
    return to_left < to_top ? left : top;
}

[[nodiscard]] std::uint32_t clamp_channels(const std::array<std::int32_t, 4U>& values) noexcept {
    std::uint32_t result = 0U;
    for (std::size_t index = 0U; index < 4U; ++index)
        result |= static_cast<std::uint32_t>(std::clamp(values[index], 0, 255)) << (8U * index);
    return result;
}

[[nodiscard]] std::uint32_t clamp_add_subtract_full(const std::uint32_t a, const std::uint32_t b,
                                                    const std::uint32_t c) noexcept {
    std::array<std::int32_t, 4U> values{};
    for (std::uint32_t index = 0U; index < 4U; ++index)
        values[index] = channel(a, 8U * index) + channel(b, 8U * index) - channel(c, 8U * index);
    return clamp_channels(values);
}

[[nodiscard]] std::uint32_t clamp_add_subtract_half(const std::uint32_t a,
                                                    const std::uint32_t b) noexcept {
    std::array<std::int32_t, 4U> values{};
    for (std::uint32_t index = 0U; index < 4U; ++index) {
        const std::int32_t first = channel(a, 8U * index);
        values[index] = first + (first - channel(b, 8U * index)) / 2;
    }
    return clamp_channels(values);
}

[[nodiscard]] std::uint32_t predict(const std::uint32_t mode, const std::uint32_t left,
                                    const std::uint32_t top, const std::uint32_t top_left,
                                    const std::uint32_t top_right) noexcept {
    switch (mode) {
    case 1U:
        return left;
    case 2U:
        return top;
    case 3U:
        return top_right;
    case 4U:
        return top_left;
    case 5U:
        return average(average(left, top_right), top);
    case 6U:
        return average(left, top_left);
    case 7U:
        return average(left, top);
    case 8U:
        return average(top_left, top);
    case 9U:
        return average(top, top_right);
    case 10U:
        return average(average(left, top_left), average(top, top_right));
    case 11U:
        return select(left, top, top_left);
    case 12U:
        return clamp_add_subtract_full(left, top, top_left);
    case 13U:
        return clamp_add_subtract_half(average(left, top), top_left);
    default:
        return 0xFF000000U; // Mode 0, and 14 and 15, which libwebp also predicts as black.
    }
}

enum class TransformKind : std::uint32_t {
    predictor = 0U,
    cross_color = 1U,
    subtract_green = 2U,
    color_indexing = 3U,
};

struct Transform final {
    TransformKind kind = TransformKind::predictor;
    /** Block size bits, or for colour indexing the pixels-per-byte bits. */
    std::uint32_t bits = 0U;
    /** The image width when the transform was read: what its inverse produces. */
    std::uint32_t width = 0U;
    /** The predictor or multiplier blocks, or the palette. */
    std::vector<std::uint32_t> data;
};

/** Decodes VP8L image streams (RFC 9649) into ARGB pixels. */
class LosslessDecoder final {
  public:
    explicit LosslessDecoder(const std::span<const std::uint8_t> bytes) noexcept : input_(bytes) {}

    /** The main image: transforms, then the entropy-coded pixels they reconstruct from. */
    [[nodiscard]] std::vector<std::uint32_t> image(const std::uint32_t width,
                                                   const std::uint32_t height) {
        std::vector<Transform> transforms;
        std::uint32_t seen = 0U;
        std::uint32_t coded_width = width;
        while (input_.take(1U) != 0U) {
            const std::uint32_t kind = input_.take(2U);
            if ((seen & (1U << kind)) != 0U)
                fail("a lossless transform is repeated");
            seen |= 1U << kind;
            Transform transform;
            transform.kind = static_cast<TransformKind>(kind);
            transform.width = coded_width;
            if (transform.kind == TransformKind::predictor ||
                transform.kind == TransformKind::cross_color) {
                transform.bits = input_.take(3U) + 2U;
                transform.data = entropy_coded(div_round_up(coded_width, transform.bits),
                                               div_round_up(height, transform.bits), false);
            } else if (transform.kind == TransformKind::color_indexing) {
                const std::uint32_t colors = input_.take(8U) + 1U;
                transform.bits = colors > 16U ? 0U : colors > 4U ? 1U : colors > 2U ? 2U : 3U;
                std::vector<std::uint32_t> palette = entropy_coded(colors, 1U, false);
                for (std::size_t index = 1U; index < palette.size(); ++index)
                    palette[index] = add_pixels(palette[index], palette[index - 1U]);
                // Indices past the palette select transparent black, as libwebp pads it.
                palette.resize(std::size_t{1U} << (8U >> transform.bits), 0U);
                transform.data = std::move(palette);
                coded_width = div_round_up(coded_width, transform.bits);
            }
            transforms.push_back(std::move(transform));
        }
        std::vector<std::uint32_t> pixels = entropy_coded(coded_width, height, true);
        for (auto transform = transforms.rbegin(); transform != transforms.rend(); ++transform)
            invert(*transform, pixels, height);
        return pixels;
    }

  private:
    /** One entropy-coded image: colour cache, prefix codes (with meta codes at the top level). */
    [[nodiscard]] std::vector<std::uint32_t>
    entropy_coded(const std::uint32_t width, const std::uint32_t height, const bool top_level) {
        std::uint32_t cache_bits = 0U;
        if (input_.take(1U) != 0U) {
            cache_bits = input_.take(4U);
            if (cache_bits < 1U || cache_bits > maximum_cache_bits)
                fail("a colour cache has an invalid size");
        }
        std::uint32_t meta_bits = 0U;
        std::uint32_t meta_width = 0U;
        std::vector<std::uint32_t> meta;
        std::uint32_t group_count = 1U;
        if (top_level && input_.take(1U) != 0U) {
            meta_bits = input_.take(3U) + 2U;
            meta_width = div_round_up(width, meta_bits);
            meta = entropy_coded(meta_width, div_round_up(height, meta_bits), false);
            for (std::uint32_t& entry : meta) {
                entry = (entry >> 8U) & 0xFFFFU;
                group_count = std::max(group_count, entry + 1U);
            }
        }
        // Groups no block uses must still be read past, but are not kept: the stream can name
        // 65536 groups while the image uses a handful.
        std::vector<std::int32_t> slots(group_count, -1);
        std::int32_t kept = 0;
        if (meta.empty()) {
            slots[0U] = kept++;
        } else {
            for (std::uint32_t& entry : meta) {
                if (slots[entry] < 0)
                    slots[entry] = kept++;
                entry = static_cast<std::uint32_t>(slots[entry]);
            }
        }
        const std::uint32_t cache_size = cache_bits == 0U ? 0U : 1U << cache_bits;
        const std::array<std::uint32_t, 5U> alphabets{
            literal_alphabet + length_codes + cache_size,
            literal_alphabet,
            literal_alphabet,
            literal_alphabet,
            distance_alphabet,
        };
        std::vector<CodeGroup> groups(static_cast<std::size_t>(kept));
        CodeGroup discarded;
        for (std::uint32_t group = 0U; group < group_count; ++group) {
            CodeGroup& codes =
                slots[group] < 0 ? discarded : groups[static_cast<std::size_t>(slots[group])];
            for (std::size_t code = 0U; code < codes.size(); ++code) {
                const std::size_t bytes = read_code(alphabets[code], codes[code]);
                if (slots[group] >= 0) {
                    table_bytes_ += bytes;
                    if (table_bytes_ > maximum_code_table_bytes)
                        fail("the prefix codes exceed the decoder's table budget");
                }
            }
        }
        return decode_pixels(width, height, groups, meta, meta_bits, meta_width, cache_bits);
    }

    /** Reads one prefix code's lengths, in the simple or the normal form, and builds it. */
    std::size_t read_code(const std::uint32_t alphabet, PrefixCode& code) {
        lengths_.assign(alphabet, 0U);
        if (input_.take(1U) != 0U) {
            // Simple: one or two symbols of length 1. A symbol past the alphabet names nothing.
            const std::uint32_t count = input_.take(1U) + 1U;
            const std::uint32_t first = input_.take(input_.take(1U) == 0U ? 1U : 8U);
            if (first < alphabet)
                lengths_[first] = 1U;
            if (count == 2U) {
                const std::uint32_t second = input_.take(8U);
                if (second < alphabet)
                    lengths_[second] = 1U;
            }
            return code.build(lengths_);
        }
        constexpr std::array<std::uint8_t, 19U> order{
            17U, 18U, 0U, 1U, 2U, 3U, 4U, 5U, 16U, 6U, 7U, 8U, 9U, 10U, 11U, 12U, 13U, 14U, 15U,
        };
        std::array<std::uint8_t, 19U> length_lengths{};
        const std::uint32_t length_count = input_.take(4U) + 4U;
        for (std::uint32_t index = 0U; index < length_count; ++index)
            length_lengths[order[index]] = static_cast<std::uint8_t>(input_.take(3U));
        PrefixCode length_code;
        static_cast<void>(length_code.build(length_lengths));
        std::uint32_t remaining = alphabet;
        if (input_.take(1U) != 0U) {
            const std::uint32_t bits = 2U + 2U * input_.take(3U);
            remaining = 2U + input_.take(bits);
            if (remaining > alphabet)
                fail("a prefix code reads more lengths than its alphabet has");
        }
        std::uint32_t symbol = 0U;
        std::uint8_t previous = 8U;
        while (symbol < alphabet && remaining-- > 0U) {
            const std::uint32_t length = length_code.decode(input_);
            if (length < 16U) {
                lengths_[symbol++] = static_cast<std::uint8_t>(length);
                if (length != 0U)
                    previous = static_cast<std::uint8_t>(length);
                continue;
            }
            constexpr std::array<std::uint32_t, 3U> extra_bits{2U, 3U, 7U};
            constexpr std::array<std::uint32_t, 3U> repeat_base{3U, 3U, 11U};
            const std::uint32_t slot = length - 16U;
            const std::uint32_t repeat = input_.take(extra_bits[slot]) + repeat_base[slot];
            if (repeat > alphabet - symbol)
                fail("a prefix code's lengths overrun its alphabet");
            std::fill_n(lengths_.begin() + symbol, repeat,
                        length == 16U ? previous : std::uint8_t{0U});
            symbol += repeat;
        }
        return code.build(lengths_);
    }

    /** A length or distance from its prefix symbol and extra bits. */
    [[nodiscard]] std::uint32_t copy_value(const std::uint32_t symbol) {
        if (symbol < 4U)
            return symbol + 1U;
        const std::uint32_t extra = (symbol - 2U) >> 1U;
        const std::uint32_t offset = (2U + (symbol & 1U)) << extra;
        return offset + input_.take(extra) + 1U;
    }

    [[nodiscard]] std::vector<std::uint32_t>
    decode_pixels(const std::uint32_t width, const std::uint32_t height,
                  const std::vector<CodeGroup>& groups, const std::vector<std::uint32_t>& meta,
                  const std::uint32_t meta_bits, const std::uint32_t meta_width,
                  const std::uint32_t cache_bits) {
        const std::size_t total = static_cast<std::size_t>(width) * height;
        std::vector<std::uint32_t> result(total);
        std::vector<std::uint32_t> cache(cache_bits == 0U ? 0U : std::size_t{1U} << cache_bits);
        const auto remember = [&cache, cache_bits](const std::uint32_t argb) {
            if (cache_bits != 0U)
                cache[(0x1E35A7BDU * argb) >> (32U - cache_bits)] = argb;
        };
        std::size_t at = 0U;
        std::uint32_t x = 0U;
        std::uint32_t y = 0U;
        while (at < total) {
            const CodeGroup& codes =
                meta.empty() ? groups.front()
                             : groups[meta[static_cast<std::size_t>(y >> meta_bits) * meta_width +
                                           (x >> meta_bits)]];
            const std::uint32_t green = codes[0U].decode(input_);
            if (green < literal_alphabet) {
                const std::uint32_t red = codes[1U].decode(input_);
                const std::uint32_t blue = codes[2U].decode(input_);
                const std::uint32_t alpha = codes[3U].decode(input_);
                const std::uint32_t argb = alpha << 24U | red << 16U | green << 8U | blue;
                result[at++] = argb;
                remember(argb);
                if (++x == width) {
                    x = 0U;
                    ++y;
                }
            } else if (green < literal_alphabet + length_codes) {
                const std::uint32_t length = copy_value(green - literal_alphabet);
                const std::uint32_t code = copy_value(codes[4U].decode(input_));
                std::size_t distance = 0U;
                if (code > distance_neighbours.size()) {
                    distance = code - distance_neighbours.size();
                } else {
                    const auto& [dx, dy] = distance_neighbours[code - 1U];
                    const std::int64_t planar =
                        static_cast<std::int64_t>(dy) * width + static_cast<std::int64_t>(dx);
                    distance = planar < 1 ? 1U : static_cast<std::size_t>(planar);
                }
                if (distance > at)
                    fail("a lossless back-reference refers before the image");
                if (length > total - at)
                    fail("a lossless back-reference runs past the image");
                for (std::uint32_t index = 0U; index < length; ++index, ++at) {
                    result[at] = result[at - distance];
                    remember(result[at]);
                }
                x += length;
                y += x / width;
                x %= width;
            } else {
                const std::uint32_t argb = cache[green - literal_alphabet - length_codes];
                result[at++] = argb;
                remember(argb);
                if (++x == width) {
                    x = 0U;
                    ++y;
                }
            }
        }
        return result;
    }

    static void invert(const Transform& transform, std::vector<std::uint32_t>& pixels,
                       const std::uint32_t height) {
        const std::uint32_t width = transform.width;
        switch (transform.kind) {
        case TransformKind::predictor: {
            const std::uint32_t blocks = div_round_up(width, transform.bits);
            for (std::uint32_t y = 0U; y < height; ++y) {
                for (std::uint32_t x = 0U; x < width; ++x) {
                    const std::size_t at = static_cast<std::size_t>(y) * width + x;
                    std::uint32_t prediction = 0xFF000000U;
                    if (y == 0U) {
                        if (x != 0U)
                            prediction = pixels[at - 1U];
                    } else if (x == 0U) {
                        prediction = pixels[at - width];
                    } else {
                        const std::uint32_t mode =
                            (transform.data[static_cast<std::size_t>(y >> transform.bits) * blocks +
                                            (x >> transform.bits)] >>
                             8U) &
                            0x0FU;
                        // The rightmost column's top-right is the row's first pixel, which is
                        // what the next pixel in memory after the top neighbour is.
                        prediction = predict(mode, pixels[at - 1U], pixels[at - width],
                                             pixels[at - width - 1U], pixels[at - width + 1U]);
                    }
                    pixels[at] = add_pixels(pixels[at], prediction);
                }
            }
            break;
        }
        case TransformKind::cross_color: {
            const std::uint32_t blocks = div_round_up(width, transform.bits);
            const auto delta = [](const std::uint32_t multiplier, const std::uint32_t value) {
                return (static_cast<std::int32_t>(static_cast<std::int8_t>(multiplier)) *
                        static_cast<std::int32_t>(static_cast<std::int8_t>(value))) >>
                       5;
            };
            for (std::uint32_t y = 0U; y < height; ++y) {
                for (std::uint32_t x = 0U; x < width; ++x) {
                    const std::uint32_t element =
                        transform.data[static_cast<std::size_t>(y >> transform.bits) * blocks +
                                       (x >> transform.bits)];
                    const std::uint32_t green_to_red = element & 0xFFU;
                    const std::uint32_t green_to_blue = (element >> 8U) & 0xFFU;
                    const std::uint32_t red_to_blue = (element >> 16U) & 0xFFU;
                    std::uint32_t& argb = pixels[static_cast<std::size_t>(y) * width + x];
                    const std::uint32_t green = (argb >> 8U) & 0xFFU;
                    const auto red = static_cast<std::uint32_t>(
                                         static_cast<std::int32_t>((argb >> 16U) & 0xFFU) +
                                         delta(green_to_red, green)) &
                                     0xFFU;
                    const auto blue = static_cast<std::uint32_t>(
                                          static_cast<std::int32_t>(argb & 0xFFU) +
                                          delta(green_to_blue, green) + delta(red_to_blue, red)) &
                                      0xFFU;
                    argb = (argb & 0xFF00FF00U) | red << 16U | blue;
                }
            }
            break;
        }
        case TransformKind::subtract_green:
            for (std::uint32_t& argb : pixels) {
                const std::uint32_t green = (argb >> 8U) & 0xFFU;
                argb = add_pixels(argb, green << 16U | green);
            }
            break;
        case TransformKind::color_indexing: {
            const std::uint32_t packed_width = div_round_up(width, transform.bits);
            const std::uint32_t index_bits = 8U >> transform.bits;
            const std::uint32_t mask = (1U << index_bits) - 1U;
            const std::uint32_t per_byte = (1U << transform.bits) - 1U;
            std::vector<std::uint32_t> expanded(static_cast<std::size_t>(width) * height);
            for (std::uint32_t y = 0U; y < height; ++y) {
                const std::size_t row = static_cast<std::size_t>(y) * packed_width;
                for (std::uint32_t x = 0U; x < width; ++x) {
                    const std::uint32_t packed =
                        (pixels[row + (x >> transform.bits)] >> 8U) & 0xFFU;
                    const std::uint32_t index = (packed >> ((x & per_byte) * index_bits)) & mask;
                    expanded[static_cast<std::size_t>(y) * width + x] = transform.data[index];
                }
            }
            pixels = std::move(expanded);
            break;
        }
        }
    }

    BitReader input_;
    std::vector<std::uint8_t> lengths_;
    std::size_t table_bytes_ = 0U;
};

struct LosslessHeader final {
    ImageDimensions dimensions;
    bool alpha = false;
};

/** Reads and checks the 5-byte VP8L header: signature, 14-bit sizes, alpha hint, version 0. */
[[nodiscard]] LosslessHeader lossless_header(const std::span<const std::uint8_t> chunk) {
    if (chunk.size() < 5U || chunk[0U] != 0x2FU)
        fail("a lossless image lacks the VP8L signature");
    const std::uint32_t bits = le32(chunk.data() + 1U);
    if ((bits >> 29U) != 0U)
        fail("a lossless image has an unknown version");
    return LosslessHeader{
        ImageDimensions{(bits & 0x3FFFU) + 1U, ((bits >> 14U) & 0x3FFFU) + 1U},
        ((bits >> 28U) & 1U) != 0U,
    };
}

[[nodiscard]] std::vector<std::uint8_t> decode_lossless(const std::span<const std::uint8_t> chunk,
                                                        const ImageDimensions expected) {
    const LosslessHeader header = lossless_header(chunk);
    if (header.dimensions != expected)
        fail("a lossless image's dimensions disagree with its frame");
    LosslessDecoder decoder(chunk.subspan(5U));
    const std::vector<std::uint32_t> argb = decoder.image(expected.width, expected.height);
    std::vector<std::uint8_t> rgba(argb.size() * 4U);
    for (std::size_t index = 0U; index < argb.size(); ++index) {
        rgba[index * 4U] = static_cast<std::uint8_t>(argb[index] >> 16U);
        rgba[index * 4U + 1U] = static_cast<std::uint8_t>(argb[index] >> 8U);
        rgba[index * 4U + 2U] = static_cast<std::uint8_t>(argb[index]);
        rgba[index * 4U + 3U] = static_cast<std::uint8_t>(argb[index] >> 24U);
    }
    return rgba;
}

// --- ALPH ---------------------------------------------------------------------------------------

/** Decodes an ALPH chunk into the alpha channel of `rgba`, a frame of `dimensions`. */
void apply_alpha(const std::span<const std::uint8_t> chunk, const ImageDimensions dimensions,
                 std::vector<std::uint8_t>& rgba) {
    if (chunk.empty())
        fail("an alpha chunk is empty");
    const std::uint32_t header = chunk[0U];
    const std::uint32_t method = header & 3U;
    const std::uint32_t filter = (header >> 2U) & 3U;
    // Bits 4-5 describe how the encoder quantized levels; decoding ignores them, as libwebp does
    // unless asked to dither.
    if (method > 1U || ((header >> 4U) & 3U) > 1U || (header >> 6U) != 0U)
        fail("an alpha chunk has an unknown header");
    const std::size_t width = dimensions.width;
    const std::size_t total = width * dimensions.height;
    const std::span<const std::uint8_t> data = chunk.subspan(1U);
    std::vector<std::uint8_t> alpha(total);
    if (method == 0U) {
        if (data.size() < total)
            fail("an uncompressed alpha plane is truncated");
        std::copy_n(data.begin(), total, alpha.begin());
    } else {
        // A headerless VP8L stream whose green channel is the alpha plane.
        LosslessDecoder decoder(data);
        const std::vector<std::uint32_t> argb = decoder.image(dimensions.width, dimensions.height);
        for (std::size_t index = 0U; index < total; ++index)
            alpha[index] = static_cast<std::uint8_t>(argb[index] >> 8U);
    }
    // Unfiltering: the first row predicts from the left (its first pixel from 0), and every later
    // row's first pixel from the one above it.
    if (filter != 0U) {
        for (std::size_t y = 0U; y < dimensions.height; ++y) {
            std::uint8_t* const row = alpha.data() + y * width;
            const std::uint8_t* const above = y == 0U ? nullptr : row - width;
            if (above == nullptr || filter == 1U) {
                std::uint8_t left = above == nullptr ? std::uint8_t{0U} : above[0U];
                for (std::size_t x = 0U; x < width; ++x) {
                    row[x] = static_cast<std::uint8_t>(row[x] + left);
                    left = row[x];
                }
            } else if (filter == 2U) {
                for (std::size_t x = 0U; x < width; ++x)
                    row[x] = static_cast<std::uint8_t>(row[x] + above[x]);
            } else {
                std::uint8_t left = above[0U];
                std::uint8_t top_left = above[0U];
                for (std::size_t x = 0U; x < width; ++x) {
                    const std::uint8_t top = above[x];
                    const int gradient = std::clamp(left + top - top_left, 0, 255);
                    left = static_cast<std::uint8_t>(row[x] + gradient);
                    top_left = top;
                    row[x] = left;
                }
            }
        }
    }
    for (std::size_t index = 0U; index < total; ++index)
        rgba[index * 4U + 3U] = alpha[index];
}

// --- Container ----------------------------------------------------------------------------------

struct Chunk final {
    std::string_view tag;
    std::span<const std::uint8_t> data;
};

/** Walks RIFF chunks, each padded to an even size; a final chunk may omit its pad byte. */
class Chunks final {
  public:
    explicit Chunks(const std::span<const std::uint8_t> bytes) noexcept : rest_(bytes) {}

    [[nodiscard]] bool next(Chunk& chunk) {
        if (rest_.empty())
            return false;
        if (rest_.size() < 8U)
            fail("a chunk header is truncated");
        const std::uint32_t size = le32(rest_.data() + 4U);
        if (size > rest_.size() - 8U)
            fail("a chunk is truncated");
        chunk.tag = std::string_view(reinterpret_cast<const char*>(rest_.data()), 4U);
        chunk.data = rest_.subspan(8U, size);
        const std::size_t padded = std::min<std::size_t>(rest_.size(), 8U + size + (size & 1U));
        rest_ = rest_.subspan(padded);
        return true;
    }

  private:
    std::span<const std::uint8_t> rest_;
};

/** One frame's bitstream: a VP8L image, or a VP8 key frame with an optional ALPH plane. */
struct FrameData final {
    std::span<const std::uint8_t> image;
    std::span<const std::uint8_t> alpha;
    bool lossless = false;
    bool has_alpha_chunk = false;
};

[[nodiscard]] ImageDimensions frame_dimensions(const FrameData& frame) {
    return frame.lossless ? lossless_header(frame.image).dimensions : inspect_vp8(frame.image);
}

/** Decodes a frame of `dimensions` into straight RGBA8; `has_alpha` as WebPGetFeatures reports. */
[[nodiscard]] std::vector<std::uint8_t>
decode_frame(const FrameData& frame, const ImageDimensions dimensions, bool& has_alpha) {
    if (frame.lossless) {
        has_alpha = lossless_header(frame.image).alpha;
        return decode_lossless(frame.image, dimensions);
    }
    if (inspect_vp8(frame.image) != dimensions)
        fail("a lossy image's dimensions disagree with its frame");
    std::vector<std::uint8_t> rgba = decode_vp8_rgba8(frame.image, dimensions);
    has_alpha = frame.has_alpha_chunk;
    if (frame.has_alpha_chunk)
        apply_alpha(frame.alpha, dimensions, rgba);
    return rgba;
}

/** What the container holds, before any bitstream is decoded. */
struct Layout final {
    ImageDimensions canvas;
    bool animated = false;
    /** The still image, when not animated. */
    FrameData still;
    /** The chunks after VP8X, for an animation. */
    std::span<const std::uint8_t> chunks;
};

constexpr std::uint32_t animation_flag = 0x02U;

[[nodiscard]] Layout layout(const std::span<const std::uint8_t> bytes) {
    if (bytes.size() < 12U ||
        std::string_view(reinterpret_cast<const char*>(bytes.data()), 4U) != "RIFF" ||
        std::string_view(reinterpret_cast<const char*>(bytes.data() + 8U), 4U) != "WEBP") {
        fail("bytes are not a WebP RIFF container");
    }
    // Bytes after the RIFF payload are ignored, as libwebp ignores them.
    const std::uint32_t riff_size = le32(bytes.data() + 4U);
    if (riff_size < 12U)
        fail("the RIFF container is too small");
    if (riff_size > bytes.size() - 8U)
        fail("the RIFF container is truncated");
    Chunks chunks(bytes.subspan(12U, riff_size - 4U));
    Chunk chunk;
    if (!chunks.next(chunk))
        fail("the container holds no chunks");
    Layout result;
    if (chunk.tag == "VP8 " || chunk.tag == "VP8L") {
        result.still.image = chunk.data;
        result.still.lossless = chunk.tag == "VP8L";
        result.canvas = frame_dimensions(result.still);
        return result;
    }
    if (chunk.tag != "VP8X")
        fail("the container starts with neither an image nor VP8X");
    if (chunk.data.size() != 10U)
        fail("VP8X is malformed");
    result.canvas =
        ImageDimensions{le24(chunk.data.data() + 4U) + 1U, le24(chunk.data.data() + 7U) + 1U};
    result.animated = (chunk.data[0U] & animation_flag) != 0U;
    if (result.animated) {
        const std::span<const std::uint8_t> after = bytes.subspan(12U, riff_size - 4U);
        result.chunks = after.subspan(std::min<std::size_t>(after.size(), 18U));
        return result;
    }
    // A still extended image: metadata and unknown chunks are skipped; the last ALPH before the
    // image is its alpha, as libwebp takes it.
    while (chunks.next(chunk)) {
        if (chunk.tag == "ALPH") {
            result.still.alpha = chunk.data;
            result.still.has_alpha_chunk = true;
        } else if (chunk.tag == "VP8 " || chunk.tag == "VP8L") {
            result.still.image = chunk.data;
            result.still.lossless = chunk.tag == "VP8L";
            if (result.still.lossless)
                result.still.has_alpha_chunk = false; // VP8L carries its own alpha.
            if (frame_dimensions(result.still) != result.canvas)
                fail("the image's dimensions disagree with the VP8X canvas");
            return result;
        }
    }
    fail("the container holds no image");
}

/** Parses an ANMF payload's frame chunks: an optional ALPH, then one VP8 or VP8L. */
[[nodiscard]] FrameData animation_frame(const std::span<const std::uint8_t> payload) {
    FrameData frame;
    Chunks chunks(payload);
    Chunk chunk;
    while (chunks.next(chunk)) {
        if (chunk.tag == "ALPH") {
            if (!frame.has_alpha_chunk) {
                frame.alpha = chunk.data;
                frame.has_alpha_chunk = true;
            }
        } else if (chunk.tag == "VP8L") {
            if (frame.has_alpha_chunk)
                fail("a lossless frame has an ALPH chunk");
            frame.image = chunk.data;
            frame.lossless = true;
            return frame;
        } else if (chunk.tag == "VP8 ") {
            frame.image = chunk.data;
            return frame;
        }
    }
    fail("an animation frame holds no image");
}

struct Placement final {
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;

    [[nodiscard]] bool contains(const std::uint32_t px, const std::uint32_t py) const noexcept {
        return px >= x && px - x < width && py >= y && py - y < height;
    }
};

[[nodiscard]] DecodedImage decode_animation(const Layout& file, const std::uint32_t frame_limit) {
    FrameSink sink(file.canvas, frame_limit, "WebP");
    const std::size_t canvas_bytes =
        static_cast<std::size_t>(file.canvas.width) * file.canvas.height * 4U;
    // The canvas after the previous frame and its disposal: what the next frame draws over.
    std::vector<std::uint8_t> disposed(canvas_bytes, 0U);
    Placement previous;
    bool previous_disposes = false;
    bool previous_was_key = false;
    bool have_animation_header = false;
    bool first = true;
    std::uint32_t loop_count = 0U;
    Chunks chunks(file.chunks);
    Chunk chunk;
    while (sink.wants_more() && chunks.next(chunk)) {
        if (chunk.tag == "ANIM") {
            if (chunk.data.size() < 6U)
                fail("ANIM is malformed");
            // The background colour is only a hint; libwebp's animation decoder starts from, and
            // disposes to, transparent black, as browsers do.
            loop_count = le16(chunk.data.data() + 4U);
            have_animation_header = true;
            continue;
        }
        if (chunk.tag == "ALPH" || chunk.tag == "VP8 " || chunk.tag == "VP8L")
            fail("an animation has an image outside its frames");
        if (chunk.tag != "ANMF")
            continue;
        if (!have_animation_header)
            fail("an animation frame precedes ANIM");
        if (chunk.data.size() < 16U)
            fail("ANMF is malformed");
        const std::uint8_t* const header = chunk.data.data();
        const Placement placement{
            2U * le24(header),
            2U * le24(header + 3U),
            le24(header + 6U) + 1U,
            le24(header + 9U) + 1U,
        };
        const std::uint32_t duration = le24(header + 12U);
        const bool blend = (header[15U] & 2U) == 0U;
        const bool dispose = (header[15U] & 1U) != 0U;
        if (placement.x + placement.width > file.canvas.width ||
            placement.y + placement.height > file.canvas.height) {
            fail("an animation frame lies outside the canvas");
        }
        sink.reserve_next();
        const FrameData data = animation_frame(chunk.data.subspan(16U));
        const ImageDimensions size{placement.width, placement.height};
        if (frame_dimensions(data) != size)
            fail("an animation frame's image disagrees with its ANMF size");
        bool has_alpha = false;
        const std::vector<std::uint8_t> pixels = decode_frame(data, size, has_alpha);

        // libwebp's key frames start from a cleared canvas and draw without blending; every other
        // frame draws over the disposed canvas, blending only outside the rectangle the previous
        // frame disposed, where the canvas is already transparent.
        const bool full =
            placement.width == file.canvas.width && placement.height == file.canvas.height;
        const bool previous_full =
            previous.width == file.canvas.width && previous.height == file.canvas.height;
        const bool key = first || ((!has_alpha || !blend) && full) ||
                         (previous_disposes && (previous_full || previous_was_key));
        std::vector<std::uint8_t> canvas =
            key ? std::vector<std::uint8_t>(canvas_bytes, 0U) : disposed;
        for (std::uint32_t row = 0U; row < placement.height; ++row) {
            const std::uint32_t y = placement.y + row;
            for (std::uint32_t column = 0U; column < placement.width; ++column) {
                const std::uint32_t x = placement.x + column;
                const std::uint8_t* const source =
                    pixels.data() + (static_cast<std::size_t>(row) * placement.width + column) * 4U;
                std::uint8_t* const target =
                    canvas.data() + (static_cast<std::size_t>(y) * file.canvas.width + x) * 4U;
                if (!key && blend && !(previous_disposes && previous.contains(x, y)))
                    blend_over(target, source);
                else
                    std::copy_n(source, 4U, target);
            }
        }
        disposed = canvas;
        sink.push(std::move(canvas), duration);
        if (dispose) {
            clear_rect(disposed, file.canvas.width, placement.x, placement.y, placement.width,
                       placement.height);
        }
        previous = placement;
        previous_disposes = dispose;
        previous_was_key = key;
        first = false;
    }
    return sink.finish(loop_count);
}

} // namespace

ImageDimensions inspect_webp(const std::span<const std::uint8_t> bytes) {
    const Layout file = layout(bytes);
    validate_canvas(file.canvas, "WebP");
    return file.canvas;
}

DecodedImage decode_webp(const std::span<const std::uint8_t> bytes,
                         const std::uint32_t frame_limit) {
    const Layout file = layout(bytes);
    if (file.animated)
        return decode_animation(file, frame_limit);
    FrameSink sink(file.canvas, frame_limit, "WebP");
    bool has_alpha = false;
    sink.push(decode_frame(file.still, file.canvas, has_alpha), 0U);
    return sink.finish(1U);
}

} // namespace strata::resource::codec
