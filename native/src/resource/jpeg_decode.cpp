// A JPEG decoder for the files images are actually published as: baseline, extended sequential
// and progressive DCT with Huffman coding and 8-bit samples (ITU T.81). Arithmetic coding,
// lossless and hierarchical JPEG, 12-bit samples and DNL markers are refused.
//
// The output is meant to match libjpeg-turbo's default decode, which browsers, Pillow and most
// tools share: the islow integer IDCT, its "fancy" triangle-filter upsampling of 2x1, 1x2 and 2x2
// subsampled chroma, and its fixed-point YCbCr conversion. Each is reproduced in the same integer
// arithmetic, so the result is the same to the bit for sane input rather than merely close.

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
    throw std::invalid_argument("JPEG decode: " + std::string(reason));
}

/**
 * Zigzag position -> natural (row-major) position, with 16 spare entries so a corrupt run that
 * steps past position 63 lands on a harmless index instead of outside the table, as libjpeg does.
 */
constexpr std::array<std::uint8_t, 80U> natural_order{
    0U,  1U,  8U,  16U, 9U,  2U,  3U,  10U, 17U, 24U, 32U, 25U, 18U, 11U, 4U,  5U,
    12U, 19U, 26U, 33U, 40U, 48U, 41U, 34U, 27U, 20U, 13U, 6U,  7U,  14U, 21U, 28U,
    35U, 42U, 49U, 56U, 57U, 50U, 43U, 36U, 29U, 22U, 15U, 23U, 30U, 37U, 44U, 51U,
    58U, 59U, 52U, 45U, 38U, 31U, 39U, 46U, 53U, 60U, 61U, 54U, 47U, 55U, 62U, 63U,
    63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U, 63U,
};

[[nodiscard]] std::uint32_t big_endian16(const std::span<const std::uint8_t> bytes,
                                         const std::size_t offset) noexcept {
    return static_cast<std::uint32_t>(bytes[offset]) << 8U | bytes[offset + 1U];
}

[[nodiscard]] std::uint8_t clamp_sample(const std::int64_t value) noexcept {
    return static_cast<std::uint8_t>(std::clamp<std::int64_t>(value, 0, 255));
}

/**
 * Reads the entropy-coded segment after an SOS, MSB first, removing 0xFF00 stuffing. At a marker
 * it supplies zero bits, as libjpeg does, but counts them: a valid scan never consumes bits past
 * its last byte (encoders pad with ones), so consuming more than a few means the data is truncated
 * or corrupt, and decoding stops instead of inventing an image out of zeros.
 */
class EntropyReader final {
  public:
    EntropyReader(const std::span<const std::uint8_t> bytes, const std::size_t position) noexcept
        : bytes_(bytes), position_(position) {}

    [[nodiscard]] std::uint32_t peek16() {
        if (count_ < 16U)
            refill();
        return static_cast<std::uint32_t>(buffer_ >> 48U);
    }

    void drop(const std::uint32_t count) {
        buffer_ <<= count;
        count_ -= count;
        if (count_ < phantom_) {
            overrun_ += phantom_ - count_;
            phantom_ = count_;
            // 64 bits of slack covers any decoder that reads a whole final byte it did not need.
            if (overrun_ > 64U)
                fail("the image data is truncated");
        }
    }

    [[nodiscard]] std::uint32_t take(const std::uint32_t count) {
        if (count == 0U)
            return 0U;
        if (count_ < count)
            refill();
        const auto value = static_cast<std::uint32_t>(buffer_ >> (64U - count));
        drop(count);
        return value;
    }

    /** Reads `count` bits as a signed magnitude in JPEG's EXTEND convention. */
    [[nodiscard]] std::int32_t take_signed(const std::uint32_t count) {
        if (count == 0U)
            return 0;
        if (count > 16U)
            fail("a coefficient has an impossible magnitude category");
        const std::uint32_t value = take(count);
        if (value < (1U << (count - 1U)))
            return static_cast<std::int32_t>(value) - static_cast<std::int32_t>((1U << count) - 1U);
        return static_cast<std::int32_t>(value);
    }

    /** Consumes the RSTn marker that must end a restart interval, and resets the bit buffer. */
    void restart(const std::uint32_t expected) {
        buffer_ = 0U;
        count_ = 0U;
        phantom_ = 0U;
        overrun_ = 0U;
        skip_to_marker();
        if (position_ + 1U >= bytes_.size() || bytes_[position_ + 1U] != 0xD0U + expected)
            fail("a restart marker is missing or out of sequence");
        position_ += 2U;
        at_marker_ = false;
    }

    /** The offset of the marker that ends the scan; any bytes before it are skipped. */
    [[nodiscard]] std::size_t finish() noexcept {
        skip_to_marker();
        return position_;
    }

  private:
    void refill() {
        while (count_ <= 56U) {
            std::uint64_t byte = 0U;
            if (!at_marker_ && position_ < bytes_.size()) {
                byte = bytes_[position_];
                if (byte == 0xFFU) {
                    const std::uint8_t next =
                        position_ + 1U < bytes_.size() ? bytes_[position_ + 1U] : 0xD9U;
                    if (next == 0x00U) {
                        position_ += 2U;
                    } else {
                        at_marker_ = true;
                        byte = 0U;
                    }
                } else {
                    ++position_;
                }
            } else {
                at_marker_ = true;
            }
            if (at_marker_)
                phantom_ += 8U;
            buffer_ |= byte << (56U - count_);
            count_ += 8U;
        }
    }

    void skip_to_marker() noexcept {
        while (position_ + 1U < bytes_.size()) {
            if (bytes_[position_] == 0xFFU && bytes_[position_ + 1U] != 0x00U &&
                bytes_[position_ + 1U] != 0xFFU) {
                return;
            }
            ++position_;
        }
        position_ = bytes_.size();
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0U;
    std::uint64_t buffer_ = 0U;
    std::uint32_t count_ = 0U;
    /** Zero bits in the buffer that were supplied past a marker, at its least significant end. */
    std::uint32_t phantom_ = 0U;
    std::uint32_t overrun_ = 0U;
    bool at_marker_ = false;
};

/** A DHT table: 9-bit lookup for short codes, libjpeg's maxcode walk for the rest. */
class Huffman final {
  public:
    static constexpr std::uint32_t fast_bits = 9U;

    void build(const std::span<const std::uint8_t, 16U> counts,
               const std::span<const std::uint8_t> values) {
        values_.fill(0U);
        std::copy(values.begin(), values.end(), values_.begin());
        fast_.fill(0U);
        std::int32_t code = 0;
        std::size_t index = 0U;
        for (std::uint32_t length = 1U; length <= 16U; ++length) {
            const std::uint32_t count = counts[length - 1U];
            offsets_[length] = static_cast<std::int32_t>(index) - code;
            for (std::uint32_t entry = 0U; entry < count; ++entry, ++code, ++index) {
                // As libjpeg requires: every length leaves a code unused, so the all-ones code
                // that would collide with marker padding never occurs.
                if (code + 1 >= (std::int32_t{1} << length))
                    fail("a Huffman table is over-subscribed");
                if (length <= fast_bits) {
                    const std::uint32_t shift = fast_bits - length;
                    const auto first = static_cast<std::uint32_t>(code) << shift;
                    for (std::uint32_t fill = 0U; fill < (1U << shift); ++fill)
                        fast_[first + fill] =
                            static_cast<std::uint16_t>(length << 8U | values_[index]);
                }
            }
            maxcode_[length] = count == 0U ? -1 : code - 1;
            code <<= 1;
        }
        defined_ = true;
    }

    [[nodiscard]] bool defined() const noexcept {
        return defined_;
    }

    [[nodiscard]] std::uint32_t decode(EntropyReader& input) const {
        const std::uint32_t bits = input.peek16();
        const std::uint16_t entry = fast_[bits >> (16U - fast_bits)];
        if (entry != 0U) {
            input.drop(entry >> 8U);
            return entry & 0xFFU;
        }
        for (std::uint32_t length = fast_bits + 1U; length <= 16U; ++length) {
            const auto code = static_cast<std::int32_t>(bits >> (16U - length));
            if (code <= maxcode_[length]) {
                input.drop(length);
                return values_[static_cast<std::size_t>(offsets_[length] + code)];
            }
        }
        fail("the image data contains an undefined Huffman code");
    }

  private:
    std::array<std::uint8_t, 256U> values_{};
    std::array<std::int32_t, 17U> maxcode_{};
    std::array<std::int32_t, 17U> offsets_{};
    std::array<std::uint16_t, std::size_t{1U} << fast_bits> fast_{};
    bool defined_ = false;
};

struct Component final {
    std::uint32_t id = 0U;
    std::uint32_t h = 1U;
    std::uint32_t v = 1U;
    std::uint32_t quant_table = 0U;
    /** The component's own size: ceil(image size * factor / maximum factor). */
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    /** The block grid padded to whole MCUs, which interleaved scans cover. */
    std::uint32_t blocks_x = 0U;
    std::uint32_t blocks_y = 0U;
    /** Natural-order quantizers, latched at the component's first scan as libjpeg does. */
    std::array<std::uint16_t, 64U> quant{};
    bool scanned = false;
    /** Progressive only: 64 coefficients per block, grown a block row at a time. */
    std::vector<std::int16_t> coefficients;
    /** Decoded samples, stride blocks_x * 8, grown a block row at a time. */
    std::vector<std::uint8_t> samples;
    std::uint32_t block_rows = 0U;
    std::int32_t dc_prediction = 0;
    std::uint32_t dc_table = 0U;
    std::uint32_t ac_table = 0U;
};

enum class ColorModel : std::uint8_t { gray, ycbcr, rgb, cmyk, ycck };

/** libjpeg's jidctint islow constants: FIX(x) at CONST_BITS = 13. */
constexpr std::int64_t fix_0_298631336 = 2446;
constexpr std::int64_t fix_0_390180644 = 3196;
constexpr std::int64_t fix_0_541196100 = 4433;
constexpr std::int64_t fix_0_765366865 = 6270;
constexpr std::int64_t fix_0_899976223 = 7373;
constexpr std::int64_t fix_1_175875602 = 9633;
constexpr std::int64_t fix_1_501321110 = 12299;
constexpr std::int64_t fix_1_847759065 = 15137;
constexpr std::int64_t fix_1_961570560 = 16069;
constexpr std::int64_t fix_2_053119869 = 16819;
constexpr std::int64_t fix_2_562915447 = 20995;
constexpr std::int64_t fix_3_072711026 = 25172;

[[nodiscard]] constexpr std::int64_t descale(const std::int64_t value, const int bits) noexcept {
    return (value + (std::int64_t{1} << (bits - 1))) >> bits;
}

/**
 * libjpeg's accurate integer IDCT (jidctint.c), dequantizing as it reads. Arithmetic is 64-bit so
 * corrupt coefficients cannot overflow; for any coefficients a real encoder writes the result is
 * the 32-bit one, sample for sample.
 */
void inverse_dct(const std::int16_t* const coefficients, const std::uint16_t* const quant,
                 std::uint8_t* const output, const std::size_t stride) noexcept {
    constexpr int const_bits = 13;
    constexpr int pass1_bits = 2;
    std::array<std::int64_t, 64U> work{};
    for (std::size_t column = 0U; column < 8U; ++column) {
        const auto in = [&](const std::size_t row) {
            return static_cast<std::int64_t>(coefficients[row * 8U + column]) *
                   quant[row * 8U + column];
        };
        std::int64_t z2 = in(2U);
        std::int64_t z3 = in(6U);
        std::int64_t z1 = (z2 + z3) * fix_0_541196100;
        std::int64_t tmp2 = z1 + z3 * -fix_1_847759065;
        std::int64_t tmp3 = z1 + z2 * fix_0_765366865;
        z2 = in(0U);
        z3 = in(4U);
        std::int64_t tmp0 = (z2 + z3) * (std::int64_t{1} << const_bits);
        std::int64_t tmp1 = (z2 - z3) * (std::int64_t{1} << const_bits);
        const std::int64_t tmp10 = tmp0 + tmp3;
        const std::int64_t tmp13 = tmp0 - tmp3;
        const std::int64_t tmp11 = tmp1 + tmp2;
        const std::int64_t tmp12 = tmp1 - tmp2;
        tmp0 = in(7U);
        tmp1 = in(5U);
        tmp2 = in(3U);
        tmp3 = in(1U);
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        std::int64_t z4 = tmp1 + tmp3;
        const std::int64_t z5 = (z3 + z4) * fix_1_175875602;
        tmp0 *= fix_0_298631336;
        tmp1 *= fix_2_053119869;
        tmp2 *= fix_3_072711026;
        tmp3 *= fix_1_501321110;
        z1 *= -fix_0_899976223;
        z2 *= -fix_2_562915447;
        z3 *= -fix_1_961570560;
        z4 *= -fix_0_390180644;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        constexpr int shift = const_bits - pass1_bits;
        work[0U * 8U + column] = descale(tmp10 + tmp3, shift);
        work[7U * 8U + column] = descale(tmp10 - tmp3, shift);
        work[1U * 8U + column] = descale(tmp11 + tmp2, shift);
        work[6U * 8U + column] = descale(tmp11 - tmp2, shift);
        work[2U * 8U + column] = descale(tmp12 + tmp1, shift);
        work[5U * 8U + column] = descale(tmp12 - tmp1, shift);
        work[3U * 8U + column] = descale(tmp13 + tmp0, shift);
        work[4U * 8U + column] = descale(tmp13 - tmp0, shift);
    }
    for (std::size_t row = 0U; row < 8U; ++row) {
        const std::int64_t* const in = work.data() + row * 8U;
        std::int64_t z2 = in[2U];
        std::int64_t z3 = in[6U];
        std::int64_t z1 = (z2 + z3) * fix_0_541196100;
        std::int64_t tmp2 = z1 + z3 * -fix_1_847759065;
        std::int64_t tmp3 = z1 + z2 * fix_0_765366865;
        std::int64_t tmp0 = (in[0U] + in[4U]) * (std::int64_t{1} << const_bits);
        std::int64_t tmp1 = (in[0U] - in[4U]) * (std::int64_t{1} << const_bits);
        const std::int64_t tmp10 = tmp0 + tmp3;
        const std::int64_t tmp13 = tmp0 - tmp3;
        const std::int64_t tmp11 = tmp1 + tmp2;
        const std::int64_t tmp12 = tmp1 - tmp2;
        tmp0 = in[7U];
        tmp1 = in[5U];
        tmp2 = in[3U];
        tmp3 = in[1U];
        z1 = tmp0 + tmp3;
        z2 = tmp1 + tmp2;
        z3 = tmp0 + tmp2;
        std::int64_t z4 = tmp1 + tmp3;
        const std::int64_t z5 = (z3 + z4) * fix_1_175875602;
        tmp0 *= fix_0_298631336;
        tmp1 *= fix_2_053119869;
        tmp2 *= fix_3_072711026;
        tmp3 *= fix_1_501321110;
        z1 *= -fix_0_899976223;
        z2 *= -fix_2_562915447;
        z3 *= -fix_1_961570560;
        z4 *= -fix_0_390180644;
        z3 += z5;
        z4 += z5;
        tmp0 += z1 + z3;
        tmp1 += z2 + z4;
        tmp2 += z2 + z3;
        tmp3 += z1 + z4;
        constexpr int shift = const_bits + pass1_bits + 3;
        std::uint8_t* const out = output + row * stride;
        out[0U] = clamp_sample(descale(tmp10 + tmp3, shift) + 128);
        out[7U] = clamp_sample(descale(tmp10 - tmp3, shift) + 128);
        out[1U] = clamp_sample(descale(tmp11 + tmp2, shift) + 128);
        out[6U] = clamp_sample(descale(tmp11 - tmp2, shift) + 128);
        out[2U] = clamp_sample(descale(tmp12 + tmp1, shift) + 128);
        out[5U] = clamp_sample(descale(tmp12 - tmp1, shift) + 128);
        out[3U] = clamp_sample(descale(tmp13 + tmp0, shift) + 128);
        out[4U] = clamp_sample(descale(tmp13 - tmp0, shift) + 128);
    }
}

/** libjpeg's YCbCr -> RGB tables (jdcolor.c), 16-bit fixed point. */
struct ColorTables final {
    ColorTables() {
        constexpr std::int32_t one_half = std::int32_t{1} << 15U;
        for (std::int32_t index = 0; index < 256; ++index) {
            const std::int32_t x = index - 128;
            const auto i = static_cast<std::size_t>(index);
            cr_r[i] = (91881 * x + one_half) >> 16;
            cb_b[i] = (116130 * x + one_half) >> 16;
            cr_g[i] = -46802 * x;
            cb_g[i] = -22554 * x + one_half;
        }
    }

    void convert(const std::uint8_t y, const std::uint8_t cb, const std::uint8_t cr,
                 std::uint8_t* const out) const noexcept {
        out[0U] = clamp_sample(y + cr_r[cr]);
        out[1U] = clamp_sample(y + ((cb_g[cb] + cr_g[cr]) >> 16));
        out[2U] = clamp_sample(y + cb_b[cb]);
    }

    std::array<std::int32_t, 256U> cr_r{};
    std::array<std::int32_t, 256U> cb_b{};
    std::array<std::int32_t, 256U> cr_g{};
    std::array<std::int32_t, 256U> cb_g{};
};

/** Reads the EXIF orientation (1-8) from an APP1 payload; 1 when absent or malformed. */
[[nodiscard]] std::uint32_t exif_orientation(const std::span<const std::uint8_t> payload) noexcept {
    constexpr std::string_view exif_header("Exif\0\0", 6U);
    if (payload.size() < exif_header.size() + 8U ||
        !std::equal(
            exif_header.begin(), exif_header.end(), payload.begin(),
            [](const char a, const std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; })) {
        return 1U;
    }
    const std::span<const std::uint8_t> tiff = payload.subspan(exif_header.size());
    const bool little = tiff[0U] == 'I' && tiff[1U] == 'I';
    if (!little && !(tiff[0U] == 'M' && tiff[1U] == 'M'))
        return 1U;
    const auto u16 = [&](const std::size_t at) -> std::uint32_t {
        const std::uint32_t first = tiff[at];
        const std::uint32_t second = tiff[at + 1U];
        return little ? first | second << 8U : first << 8U | second;
    };
    const auto u32 = [&](const std::size_t at) -> std::uint32_t {
        return little ? u16(at) | u16(at + 2U) << 16U : u16(at) << 16U | u16(at + 2U);
    };
    if (u16(2U) != 42U)
        return 1U;
    const std::size_t directory = u32(4U);
    if (directory > tiff.size() - 2U)
        return 1U;
    const std::size_t entries = u16(directory);
    for (std::size_t entry = 0U; entry < entries; ++entry) {
        const std::size_t at = directory + 2U + entry * 12U;
        if (at > tiff.size() - 12U)
            return 1U;
        // Tag 0x0112, one SHORT stored in the first half of the value field.
        if (u16(at) == 0x0112U && u16(at + 2U) == 3U && u32(at + 4U) == 1U) {
            const std::uint32_t orientation = u16(at + 8U);
            return orientation >= 1U && orientation <= 8U ? orientation : 1U;
        }
    }
    return 1U;
}

class Decoder final {
  public:
    explicit Decoder(const std::span<const std::uint8_t> bytes) noexcept : bytes_(bytes) {}

    /** Reads markers up to the first scan: frame header, tables and orientation. */
    [[nodiscard]] ImageDimensions inspect() {
        parse(true);
        return oriented_dimensions();
    }

    [[nodiscard]] DecodedImage decode() {
        parse(false);
        for (const Component& component : components_) {
            if (!component.scanned)
                fail("a component has no image data");
        }
        if (progressive_) {
            for (Component& component : components_)
                transform_coefficients(component);
        }
        std::vector<std::uint8_t> rgba = convert();
        rgba = orient(std::move(rgba));
        const ImageDimensions dimensions = oriented_dimensions();
        FrameSink sink(dimensions, 1U, "JPEG");
        sink.push(std::move(rgba), 0U);
        return sink.finish(1U);
    }

  private:
    [[nodiscard]] ImageDimensions oriented_dimensions() const noexcept {
        return orientation_ >= 5U ? ImageDimensions{height_, width_}
                                  : ImageDimensions{width_, height_};
    }

    /** The payload of the length-prefixed segment at position_, which then moves past it. */
    [[nodiscard]] std::span<const std::uint8_t> segment() {
        if (bytes_.size() - position_ < 2U)
            fail("a marker segment is truncated");
        const std::uint32_t length = big_endian16(bytes_, position_);
        if (length < 2U || length > bytes_.size() - position_)
            fail("a marker segment is truncated");
        const std::span<const std::uint8_t> payload = bytes_.subspan(position_ + 2U, length - 2U);
        position_ += length;
        return payload;
    }

    void parse(const bool header_only) {
        if (bytes_.size() < 4U || bytes_[0U] != 0xFFU || bytes_[1U] != 0xD8U)
            fail("bytes are not a JPEG");
        position_ = 2U;
        bool scanned = false;
        for (;;) {
            if (position_ >= bytes_.size()) {
                // A file cut off after its last complete scan still decodes, as libjpeg's
                // does; one with no scan at all does not.
                if (!scanned)
                    fail("the image has no data");
                return;
            }
            if (bytes_[position_] != 0xFFU)
                fail("a marker is expected");
            // Any number of 0xFF fill bytes may precede a marker.
            while (position_ < bytes_.size() && bytes_[position_] == 0xFFU)
                ++position_;
            if (position_ >= bytes_.size())
                fail("a marker is truncated");
            const std::uint8_t marker = bytes_[position_++];
            switch (marker) {
            case 0xC0U:
            case 0xC1U:
            case 0xC2U:
                frame(segment(), marker == 0xC2U);
                break;
            case 0xC3U:
            case 0xC5U:
            case 0xC6U:
            case 0xC7U:
                fail("lossless and hierarchical JPEG are not supported");
            case 0xC9U:
            case 0xCAU:
            case 0xCBU:
            case 0xCDU:
            case 0xCEU:
            case 0xCFU:
                fail("arithmetic-coded JPEG is not supported");
            case 0xC4U:
                huffman_tables(segment());
                break;
            case 0xDBU:
                quantization_tables(segment());
                break;
            case 0xDDU: {
                const std::span<const std::uint8_t> payload = segment();
                if (payload.size() != 2U)
                    fail("DRI is malformed");
                restart_interval_ = big_endian16(payload, 0U);
                break;
            }
            case 0xDAU:
                if (components_.empty())
                    fail("a scan precedes the frame header");
                if (header_only)
                    return;
                scan(segment());
                scanned = true;
                break;
            case 0xD9U:
                if (!scanned) {
                    if (header_only && !components_.empty())
                        return;
                    fail("the image has no data");
                }
                return;
            case 0xDCU:
                fail("DNL markers are not supported");
            case 0xE1U: {
                const std::span<const std::uint8_t> payload = segment();
                // The first EXIF block before the image data decides, in inspect and decode alike.
                if (!scanned && !seen_exif_ && payload.size() >= 6U && payload[0U] == 'E' &&
                    payload[1U] == 'x' && payload[2U] == 'i' && payload[3U] == 'f') {
                    orientation_ = exif_orientation(payload);
                    seen_exif_ = true;
                }
                break;
            }
            case 0xEEU: {
                const std::span<const std::uint8_t> payload = segment();
                if (payload.size() >= 12U && payload[0U] == 'A' && payload[1U] == 'd' &&
                    payload[2U] == 'o' && payload[3U] == 'b' && payload[4U] == 'e') {
                    adobe_transform_ = payload[11U];
                }
                break;
            }
            case 0xE0U: {
                const std::span<const std::uint8_t> payload = segment();
                if (payload.size() >= 5U && payload[0U] == 'J' && payload[1U] == 'F' &&
                    payload[2U] == 'I' && payload[3U] == 'F' && payload[4U] == 0U) {
                    jfif_ = true;
                }
                break;
            }
            default:
                if (marker == 0xD8U || (marker >= 0xD0U && marker <= 0xD7U) || marker == 0x01U)
                    fail("a marker appears out of place");
                // APPn, COM and the rest carry nothing the decoder needs.
                static_cast<void>(segment());
                break;
            }
        }
    }

    void frame(const std::span<const std::uint8_t> payload, const bool progressive) {
        if (!components_.empty())
            fail("the image has more than one frame header");
        if (payload.size() < 6U)
            fail("SOF is malformed");
        if (payload[0U] != 8U)
            fail("only 8-bit samples are supported");
        height_ = big_endian16(payload, 1U);
        width_ = big_endian16(payload, 3U);
        const std::uint32_t count = payload[5U];
        if (height_ == 0U)
            fail("DNL-defined heights are not supported");
        if ((count != 1U && count != 3U && count != 4U) || payload.size() != 6U + count * 3U)
            fail("SOF describes an unsupported component layout");
        validate_canvas(ImageDimensions{width_, height_}, "JPEG");
        progressive_ = progressive;
        for (std::uint32_t index = 0U; index < count; ++index) {
            const std::size_t at = 6U + index * 3U;
            Component component;
            component.id = payload[at];
            component.h = payload[at + 1U] >> 4U;
            component.v = payload[at + 1U] & 0x0FU;
            component.quant_table = payload[at + 2U];
            if (component.h < 1U || component.h > 4U || component.v < 1U || component.v > 4U ||
                component.quant_table > 3U) {
                fail("SOF has an invalid sampling factor or table");
            }
            for (const Component& other : components_) {
                if (other.id == component.id)
                    fail("SOF repeats a component id");
            }
            components_.push_back(std::move(component));
        }
        // A lone component is coded non-interleaved, so its own factors mean nothing.
        if (count == 1U)
            components_[0U].h = components_[0U].v = 1U;
        for (const Component& component : components_) {
            h_max_ = std::max(h_max_, component.h);
            v_max_ = std::max(v_max_, component.v);
        }
        mcus_x_ = (width_ + 8U * h_max_ - 1U) / (8U * h_max_);
        mcus_y_ = (height_ + 8U * v_max_ - 1U) / (8U * v_max_);
        for (Component& component : components_) {
            // Upsampling replicates or interpolates whole samples; fractional ratios are refused.
            if (h_max_ % component.h != 0U || v_max_ % component.v != 0U)
                fail("a sampling ratio is not an integer");
            component.width = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(width_) * component.h + h_max_ - 1U) / h_max_);
            component.height = static_cast<std::uint32_t>(
                (static_cast<std::uint64_t>(height_) * component.v + v_max_ - 1U) / v_max_);
            component.blocks_x = mcus_x_ * component.h;
            component.blocks_y = mcus_y_ * component.v;
        }
    }

    void huffman_tables(std::span<const std::uint8_t> payload) {
        while (!payload.empty()) {
            if (payload.size() < 17U)
                fail("DHT is malformed");
            const std::uint32_t kind = payload[0U] >> 4U;
            const std::uint32_t id = payload[0U] & 0x0FU;
            if (kind > 1U || id > 3U)
                fail("DHT names an invalid table");
            const std::span<const std::uint8_t, 16U> counts = payload.subspan<1U, 16U>();
            std::size_t total = 0U;
            for (const std::uint8_t count : counts)
                total += count;
            if (total > 256U || payload.size() < 17U + total)
                fail("DHT is malformed");
            (kind == 0U ? dc_tables_ : ac_tables_)[id].build(counts, payload.subspan(17U, total));
            payload = payload.subspan(17U + total);
        }
    }

    void quantization_tables(std::span<const std::uint8_t> payload) {
        while (!payload.empty()) {
            const std::uint32_t precision = payload[0U] >> 4U;
            const std::uint32_t id = payload[0U] & 0x0FU;
            const std::size_t size = 1U + 64U * (precision + 1U);
            if (precision > 1U || id > 3U || payload.size() < size)
                fail("DQT is malformed");
            for (std::size_t k = 0U; k < 64U; ++k) {
                quant_tables_[id][natural_order[k]] = static_cast<std::uint16_t>(
                    precision == 0U ? payload[1U + k] : big_endian16(payload, 1U + k * 2U));
            }
            quant_defined_[id] = true;
            payload = payload.subspan(size);
        }
    }

    /** Grows a component's storage to cover block row `row`, as the data reaches it. */
    void reach(Component& component, const std::uint32_t row) {
        if (row < component.block_rows)
            return;
        component.block_rows = row + 1U;
        const std::size_t blocks = static_cast<std::size_t>(component.blocks_x) * (row + 1U);
        if (progressive_)
            component.coefficients.resize(blocks * 64U);
        else
            component.samples.resize(blocks * 64U);
    }

    void scan(const std::span<const std::uint8_t> payload) {
        if (payload.empty())
            fail("SOS is malformed");
        const std::uint32_t count = payload[0U];
        if (count < 1U || count > 4U || payload.size() != 4U + count * 2U)
            fail("SOS is malformed");
        std::vector<Component*> members;
        for (std::uint32_t index = 0U; index < count; ++index) {
            const std::uint32_t id = payload[1U + index * 2U];
            const auto found = std::ranges::find(components_, id, &Component::id);
            if (found == components_.end() || std::ranges::find(members, &*found) != members.end())
                fail("SOS names an unknown or repeated component");
            found->dc_table = payload[2U + index * 2U] >> 4U;
            found->ac_table = payload[2U + index * 2U] & 0x0FU;
            if (found->dc_table > 3U || found->ac_table > 3U)
                fail("SOS names an invalid table");
            members.push_back(&*found);
        }
        const std::size_t tail = 1U + count * 2U;
        const std::uint32_t start = payload[tail];
        const std::uint32_t end = payload[tail + 1U];
        const std::uint32_t high = payload[tail + 2U] >> 4U;
        const std::uint32_t low = payload[tail + 2U] & 0x0FU;
        std::uint32_t blocks_per_mcu = 0U;
        for (const Component* const component : members)
            blocks_per_mcu += component->h * component->v;
        if (count > 1U && blocks_per_mcu > 10U)
            fail("an MCU has more than ten blocks");
        if (progressive_) {
            const bool dc = start == 0U;
            if ((dc && end != 0U) || (!dc && (end < start || end > 63U || count != 1U)) ||
                (high != 0U && low != high - 1U) || low > 13U) {
                fail("a progressive scan has invalid parameters");
            }
        } else if (start != 0U || end != 63U || high != 0U || low != 0U) {
            fail("a sequential scan has invalid parameters");
        }
        for (Component* const component : members) {
            const bool needs_dc = !progressive_ || (start == 0U && high == 0U);
            const bool needs_ac = !progressive_ || start != 0U;
            if ((needs_dc && !dc_tables_[component->dc_table].defined()) ||
                (needs_ac && !ac_tables_[component->ac_table].defined())) {
                fail("a scan uses an undefined Huffman table");
            }
            if (progressive_ && start != 0U && !component->scanned)
                fail("an AC scan precedes its component's DC scan");
            if (!component->scanned) {
                if (!quant_defined_[component->quant_table])
                    fail("a component uses an undefined quantization table");
                component->quant = quant_tables_[component->quant_table];
                component->scanned = true;
            }
            component->dc_prediction = 0;
        }

        EntropyReader input(bytes_, position_);
        eob_run_ = 0U;
        const bool interleaved = count > 1U;
        // A non-interleaved scan covers only the component's own blocks; each block is an MCU.
        const std::uint32_t columns = interleaved ? mcus_x_ : (members[0U]->width + 7U) / 8U;
        const std::uint32_t rows = interleaved ? mcus_y_ : (members[0U]->height + 7U) / 8U;
        const std::uint64_t total = static_cast<std::uint64_t>(columns) * rows;
        std::uint32_t restarts = 0U;
        for (std::uint64_t mcu = 0U; mcu < total; ++mcu) {
            if (restart_interval_ != 0U && mcu != 0U && mcu % restart_interval_ == 0U) {
                input.restart(restarts++ % 8U);
                eob_run_ = 0U;
                for (Component* const component : members)
                    component->dc_prediction = 0;
            }
            const auto mcu_x = static_cast<std::uint32_t>(mcu % columns);
            const auto mcu_y = static_cast<std::uint32_t>(mcu / columns);
            for (Component* const component : members) {
                const std::uint32_t h = interleaved ? component->h : 1U;
                const std::uint32_t v = interleaved ? component->v : 1U;
                reach(*component, mcu_y * v + v - 1U);
                for (std::uint32_t y = 0U; y < v; ++y) {
                    for (std::uint32_t x = 0U; x < h; ++x)
                        block(input, *component, mcu_x * h + x, mcu_y * v + y, start, end, high,
                              low);
                }
            }
        }
        position_ = input.finish();
    }

    void block(EntropyReader& input, Component& component, const std::uint32_t x,
               const std::uint32_t y, const std::uint32_t start, const std::uint32_t end,
               const std::uint32_t high, const std::uint32_t low) {
        const std::size_t index = static_cast<std::size_t>(y) * component.blocks_x + x;
        if (!progressive_) {
            std::array<std::int16_t, 64U> coefficients{};
            sequential_block(input, component, coefficients.data());
            const std::size_t stride = static_cast<std::size_t>(component.blocks_x) * 8U;
            inverse_dct(coefficients.data(), component.quant.data(),
                        component.samples.data() + static_cast<std::size_t>(y) * 8U * stride +
                            static_cast<std::size_t>(x) * 8U,
                        stride);
            return;
        }
        std::int16_t* const coefficients = component.coefficients.data() + index * 64U;
        if (start == 0U) {
            if (high == 0U) {
                const std::uint32_t category = dc_tables_[component.dc_table].decode(input);
                predict_dc(component, input.take_signed(category));
                coefficients[0U] = static_cast<std::int16_t>(
                    static_cast<std::uint32_t>(component.dc_prediction) << low);
            } else if (input.take(1U) != 0U) {
                coefficients[0U] = static_cast<std::int16_t>(coefficients[0U] | (1 << low));
            }
        } else if (high == 0U) {
            ac_first(input, ac_tables_[component.ac_table], coefficients, start, end, low);
        } else {
            ac_refine(input, ac_tables_[component.ac_table], coefficients, start, end, low);
        }
    }

    /** Adds a DC difference, wrapping as a 32-bit int would, so corrupt data cannot overflow. */
    static void predict_dc(Component& component, const std::int32_t difference) noexcept {
        component.dc_prediction =
            static_cast<std::int32_t>(static_cast<std::uint32_t>(component.dc_prediction) +
                                      static_cast<std::uint32_t>(difference));
    }

    void sequential_block(EntropyReader& input, Component& component,
                          std::int16_t* const coefficients) {
        const std::uint32_t category = dc_tables_[component.dc_table].decode(input);
        predict_dc(component, input.take_signed(category));
        coefficients[0U] = static_cast<std::int16_t>(component.dc_prediction);
        const Huffman& table = ac_tables_[component.ac_table];
        for (std::uint32_t k = 1U; k < 64U;) {
            const std::uint32_t symbol = table.decode(input);
            const std::uint32_t run = symbol >> 4U;
            const std::uint32_t size = symbol & 0x0FU;
            if (size == 0U) {
                if (run != 15U)
                    break;
                k += 16U;
                continue;
            }
            k += run;
            if (k > 63U)
                fail("a block has more than 64 coefficients");
            coefficients[natural_order[k]] = static_cast<std::int16_t>(input.take_signed(size));
            ++k;
        }
    }

    void ac_first(EntropyReader& input, const Huffman& table, std::int16_t* const coefficients,
                  const std::uint32_t start, const std::uint32_t end, const std::uint32_t low) {
        if (eob_run_ > 0U) {
            --eob_run_;
            return;
        }
        for (std::uint32_t k = start; k <= end; ++k) {
            const std::uint32_t symbol = table.decode(input);
            const std::uint32_t run = symbol >> 4U;
            const std::uint32_t size = symbol & 0x0FU;
            if (size == 0U) {
                if (run < 15U) {
                    eob_run_ = (1U << run) - 1U + input.take(run);
                    return;
                }
                k += 15U;
                continue;
            }
            k += run;
            if (k > end)
                fail("a block has more coefficients than its scan");
            coefficients[natural_order[k]] = static_cast<std::int16_t>(
                static_cast<std::uint32_t>(input.take_signed(size)) << low);
        }
    }

    /** Successive approximation of AC coefficients, as libjpeg's decode_mcu_AC_refine. */
    void ac_refine(EntropyReader& input, const Huffman& table, std::int16_t* const coefficients,
                   const std::uint32_t start, const std::uint32_t end, const std::uint32_t low) {
        const int plus = 1 << low;
        const int minus = -plus;
        const auto refine = [&](std::int16_t& coefficient) {
            if (input.take(1U) != 0U && (coefficient & plus) == 0) {
                coefficient =
                    static_cast<std::int16_t>(coefficient + (coefficient >= 0 ? plus : minus));
            }
        };
        std::uint32_t k = start;
        if (eob_run_ == 0U) {
            for (; k <= end; ++k) {
                const std::uint32_t symbol = table.decode(input);
                int run = static_cast<int>(symbol >> 4U);
                const std::uint32_t size = symbol & 0x0FU;
                int value = 0;
                if (size != 0U) {
                    if (size != 1U)
                        fail("a refinement scan has an invalid coefficient");
                    value = input.take(1U) != 0U ? plus : minus;
                } else if (run != 15) {
                    eob_run_ = (1U << static_cast<std::uint32_t>(run)) +
                               input.take(static_cast<std::uint32_t>(run));
                    break;
                }
                // Skip `run` zero coefficients, refining the nonzero ones passed on the way.
                for (; k <= end; ++k) {
                    std::int16_t& coefficient = coefficients[natural_order[k]];
                    if (coefficient != 0)
                        refine(coefficient);
                    else if (--run < 0)
                        break;
                }
                if (value != 0) {
                    if (k > end)
                        fail("a refinement scan places a coefficient outside its band");
                    coefficients[natural_order[k]] = static_cast<std::int16_t>(value);
                }
            }
        }
        if (eob_run_ > 0U) {
            for (; k <= end; ++k) {
                std::int16_t& coefficient = coefficients[natural_order[k]];
                if (coefficient != 0)
                    refine(coefficient);
            }
            --eob_run_;
        }
    }

    void transform_coefficients(Component& component) {
        reach(component, component.blocks_y - 1U);
        const std::size_t stride = static_cast<std::size_t>(component.blocks_x) * 8U;
        component.samples.resize(stride * component.blocks_y * 8U);
        for (std::size_t y = 0U; y < component.blocks_y; ++y) {
            for (std::size_t x = 0U; x < component.blocks_x; ++x) {
                inverse_dct(component.coefficients.data() + (y * component.blocks_x + x) * 64U,
                            component.quant.data(),
                            component.samples.data() + y * 8U * stride + x * 8U, stride);
            }
        }
        component.coefficients = {};
    }

    [[nodiscard]] ColorModel color_model() const {
        // libjpeg's default_decompress_parms.
        if (components_.size() == 1U)
            return ColorModel::gray;
        if (components_.size() == 3U) {
            if (jfif_)
                return ColorModel::ycbcr;
            if (adobe_transform_.has_value())
                return *adobe_transform_ == 0U ? ColorModel::rgb : ColorModel::ycbcr;
            if (components_[0U].id == 'R' && components_[1U].id == 'G' &&
                components_[2U].id == 'B') {
                return ColorModel::rgb;
            }
            return ColorModel::ycbcr;
        }
        if (adobe_transform_.has_value() && *adobe_transform_ != 0U)
            return ColorModel::ycck;
        return ColorModel::cmyk;
    }

    /**
     * One output row of a component at full resolution, upsampled as libjpeg-turbo does: the
     * triangle ("fancy") filters for 2x1, 1x2 and 2x2 ratios, and replication otherwise. Rows
     * outside the component repeat its edge rows, as libjpeg's context rows do; columns read the
     * IDCT's padding samples, which is also what libjpeg reads.
     */
    void upsample_row(const Component& component, const std::uint32_t y,
                      std::vector<std::uint8_t>& out) const {
        const std::uint32_t h_ratio = h_max_ / component.h;
        const std::uint32_t v_ratio = v_max_ / component.v;
        const std::size_t stride = static_cast<std::size_t>(component.blocks_x) * 8U;
        const auto row = [&](const std::int64_t index) {
            const auto clamped = static_cast<std::size_t>(
                std::clamp<std::int64_t>(index, 0, std::int64_t{component.height} - 1));
            return component.samples.data() + clamped * stride;
        };
        const std::uint32_t source_y = y / v_ratio;
        const std::uint32_t width = component.width;
        const bool fancy_h = h_ratio == 2U && width > 2U;
        if (v_ratio == 2U && (h_ratio == 1U || fancy_h)) {
            const bool upper = y % 2U == 0U;
            const std::uint8_t* const near_row = row(source_y);
            const std::uint8_t* const far_row =
                row(upper ? std::int64_t{source_y} - 1 : std::int64_t{source_y} + 1);
            if (h_ratio == 1U) {
                const int bias = upper ? 1 : 2;
                for (std::uint32_t x = 0U; x < width; ++x)
                    out[x] = static_cast<std::uint8_t>((near_row[x] * 3 + far_row[x] + bias) >> 2);
                return;
            }
            const auto sum = [&](const std::size_t x) { return near_row[x] * 3 + far_row[x]; };
            int this_sum = sum(0U);
            int next_sum = sum(1U);
            out[0U] = static_cast<std::uint8_t>((this_sum * 4 + 8) >> 4);
            out[1U] = static_cast<std::uint8_t>((this_sum * 3 + next_sum + 7) >> 4);
            int last_sum = this_sum;
            this_sum = next_sum;
            for (std::uint32_t x = 1U; x + 1U < width; ++x) {
                next_sum = sum(x + 1U);
                out[2U * x] = static_cast<std::uint8_t>((this_sum * 3 + last_sum + 8) >> 4);
                out[2U * x + 1U] = static_cast<std::uint8_t>((this_sum * 3 + next_sum + 7) >> 4);
                last_sum = this_sum;
                this_sum = next_sum;
            }
            out[2U * width - 2U] = static_cast<std::uint8_t>((this_sum * 3 + last_sum + 8) >> 4);
            out[2U * width - 1U] = static_cast<std::uint8_t>((this_sum * 4 + 7) >> 4);
            return;
        }
        const std::uint8_t* const in = row(source_y);
        if (v_ratio == 1U && fancy_h) {
            out[0U] = in[0U];
            out[1U] = static_cast<std::uint8_t>((in[0U] * 3 + in[1U] + 2) >> 2);
            for (std::uint32_t x = 1U; x + 1U < width; ++x) {
                const int value = in[x] * 3;
                out[2U * x] = static_cast<std::uint8_t>((value + in[x - 1U] + 1) >> 2);
                out[2U * x + 1U] = static_cast<std::uint8_t>((value + in[x + 1U] + 2) >> 2);
            }
            out[2U * width - 2U] =
                static_cast<std::uint8_t>((in[width - 1U] * 3 + in[width - 2U] + 1) >> 2);
            out[2U * width - 1U] = in[width - 1U];
            return;
        }
        for (std::uint32_t x = 0U; x < width_; ++x)
            out[x] = in[x / h_ratio];
    }

    [[nodiscard]] std::vector<std::uint8_t> convert() const {
        static const ColorTables tables;
        const ColorModel model = color_model();
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width_) * height_ * 4U);
        // Fancy upsampling writes whole pairs, so a row may run one sample past the image.
        std::array<std::vector<std::uint8_t>, 4U> rows;
        for (std::size_t index = 0U; index < components_.size(); ++index)
            rows[index].resize(static_cast<std::size_t>(width_) + 8U);
        for (std::uint32_t y = 0U; y < height_; ++y) {
            for (std::size_t index = 0U; index < components_.size(); ++index)
                upsample_row(components_[index], y, rows[index]);
            std::uint8_t* out = rgba.data() + static_cast<std::size_t>(y) * width_ * 4U;
            for (std::uint32_t x = 0U; x < width_; ++x, out += 4U) {
                out[3U] = 255U;
                switch (model) {
                case ColorModel::gray:
                    out[0U] = out[1U] = out[2U] = rows[0U][x];
                    break;
                case ColorModel::rgb:
                    out[0U] = rows[0U][x];
                    out[1U] = rows[1U][x];
                    out[2U] = rows[2U][x];
                    break;
                case ColorModel::ycbcr:
                    tables.convert(rows[0U][x], rows[1U][x], rows[2U][x], out);
                    break;
                case ColorModel::cmyk:
                case ColorModel::ycck: {
                    std::array<std::uint8_t, 3U> cmy{rows[0U][x], rows[1U][x], rows[2U][x]};
                    if (model == ColorModel::ycck) {
                        // libjpeg's ycck_cmyk_convert: YCC to RGB, then C, M, Y = 255 - R, G, B.
                        tables.convert(cmy[0U], cmy[1U], cmy[2U], cmy.data());
                        for (std::uint8_t& value : cmy)
                            value = static_cast<std::uint8_t>(255U - value);
                    }
                    // Adobe CMYK is stored inverted (255 is no ink), which Pillow and browsers
                    // assume of every CMYK JPEG; Pillow's cmyk2rgb then darkens by K.
                    const int ink_free = rows[3U][x];
                    for (std::size_t channel = 0U; channel < 3U; ++channel) {
                        const int ink = 255 - cmy[channel];
                        const int product = ink * ink_free + 128;
                        out[channel] = clamp_sample(ink_free - (((product >> 8) + product) >> 8));
                    }
                    break;
                }
                }
            }
        }
        return rgba;
    }

    /** Applies the EXIF orientation, so the pixels are as the photo is meant to be seen. */
    [[nodiscard]] std::vector<std::uint8_t> orient(std::vector<std::uint8_t> rgba) const {
        if (orientation_ == 1U)
            return rgba;
        const ImageDimensions out = oriented_dimensions();
        std::vector<std::uint8_t> result(rgba.size());
        for (std::uint32_t y = 0U; y < out.height; ++y) {
            for (std::uint32_t x = 0U; x < out.width; ++x) {
                std::uint32_t source_x = x;
                std::uint32_t source_y = y;
                switch (orientation_) {
                case 2U:
                    source_x = width_ - 1U - x;
                    break;
                case 3U:
                    source_x = width_ - 1U - x;
                    source_y = height_ - 1U - y;
                    break;
                case 4U:
                    source_y = height_ - 1U - y;
                    break;
                case 5U:
                    source_x = y;
                    source_y = x;
                    break;
                case 6U:
                    source_x = y;
                    source_y = height_ - 1U - x;
                    break;
                case 7U:
                    source_x = width_ - 1U - y;
                    source_y = height_ - 1U - x;
                    break;
                default:
                    source_x = width_ - 1U - y;
                    source_y = x;
                    break;
                }
                std::copy_n(rgba.data() +
                                (static_cast<std::size_t>(source_y) * width_ + source_x) * 4U,
                            4U, result.data() + (static_cast<std::size_t>(y) * out.width + x) * 4U);
            }
        }
        return result;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t position_ = 0U;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    bool progressive_ = false;
    std::vector<Component> components_;
    std::uint32_t h_max_ = 1U;
    std::uint32_t v_max_ = 1U;
    std::uint32_t mcus_x_ = 0U;
    std::uint32_t mcus_y_ = 0U;
    std::array<std::array<std::uint16_t, 64U>, 4U> quant_tables_{};
    std::array<bool, 4U> quant_defined_{};
    std::array<Huffman, 4U> dc_tables_{};
    std::array<Huffman, 4U> ac_tables_{};
    std::uint32_t restart_interval_ = 0U;
    std::uint32_t eob_run_ = 0U;
    std::uint32_t orientation_ = 1U;
    bool seen_exif_ = false;
    bool jfif_ = false;
    std::optional<std::uint32_t> adobe_transform_;
};

} // namespace

ImageDimensions inspect_jpeg(const std::span<const std::uint8_t> bytes) {
    Decoder decoder(bytes);
    return decoder.inspect();
}

DecodedImage decode_jpeg(const std::span<const std::uint8_t> bytes) {
    Decoder decoder(bytes);
    return decoder.decode();
}

} // namespace strata::resource::codec
