// libFuzzer entry point for the core PNG decoder and its inflate. Built only with
// -DSTRATA_BUILD_FUZZERS=ON (MSVC or Clang), which also instruments the core library for
// coverage. Run, for example:
//   strata_png_fuzzer -max_len=262144 -timeout=10 -rss_limit_mb=2048 corpus/
// seeded with the bundled PNGs under src/main/resources/assets/strata, each prefixed with a 0 byte.
//
// The first input byte selects a mode. An even byte decodes the rest as a PNG after rewriting every
// chunk CRC, so mutations reach the chunk parser and inflate instead of stopping at the CRC. An odd
// byte builds a PNG from a header and the rest as filtered scanlines in stored deflate blocks with
// a correct checksum, so mutations reach the scanline filters, Adam7 placement and sample
// expansion.

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string_view>
#include <vector>

#include "resource/image.hpp"

namespace {

using strata::resource::ImageDimensions;

[[nodiscard]] std::uint32_t big_endian(const std::uint8_t* const bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0U]) << 24U |
           static_cast<std::uint32_t>(bytes[1U]) << 16U |
           static_cast<std::uint32_t>(bytes[2U]) << 8U | static_cast<std::uint32_t>(bytes[3U]);
}

void put(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

[[nodiscard]] std::uint32_t crc32(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t value = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ (0xEDB88320U & (0U - (value & 1U)));
    }
    return ~value;
}

void chunk(std::vector<std::uint8_t>& out, const std::string_view type,
           const std::span<const std::uint8_t> data) {
    put(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type.begin(), type.end());
    out.insert(out.end(), data.begin(), data.end());
    put(out, crc32(std::span(out).subspan(start)));
}

void decode(const std::span<const std::uint8_t> png) {
    if (png.size() < 24U)
        return;
    const ImageDimensions dimensions{big_endian(png.data() + 16U), big_endian(png.data() + 20U)};
    try {
        static_cast<void>(strata::resource::decode_png_rgba8(png, dimensions));
    } catch (const std::exception&) {
        // Rejecting a malformed image is the expected outcome; crashes, hangs and unbounded
        // allocation are the findings.
    }
}

/** Mode 0: the input is a PNG whose chunk CRCs are rewritten to match their contents. */
void decode_with_valid_crcs(const std::span<const std::uint8_t> input) {
    std::vector<std::uint8_t> png(input.begin(), input.end());
    std::size_t offset = 8U;
    while (offset + 12U <= png.size()) {
        const std::uint32_t length = big_endian(png.data() + offset);
        if (length > png.size() - offset - 12U)
            break;
        const std::uint32_t crc = crc32(std::span(png).subspan(offset + 4U, length + 4U));
        for (std::size_t byte = 0U; byte < 4U; ++byte)
            png[offset + 8U + length + byte] = static_cast<std::uint8_t>(crc >> (24U - 8U * byte));
        offset += 12U + length;
    }
    decode(png);
}

/** Mode 1: a small image built from a header and exactly as many scanline bytes as it needs. */
void decode_scanlines(const std::span<const std::uint8_t> input) {
    if (input.size() < 4U)
        return;
    constexpr std::array<std::array<std::uint8_t, 2U>, 15U> formats{{
        {1U, 0U}, {2U, 0U}, {4U, 0U}, {8U, 0U}, {16U, 0U},
        {8U, 2U}, {16U, 2U},
        {1U, 3U}, {2U, 3U}, {4U, 3U}, {8U, 3U},
        {8U, 4U}, {16U, 4U},
        {8U, 6U}, {16U, 6U},
    }};
    const std::uint32_t width = 1U + input[0U] % 40U;
    const std::uint32_t height = 1U + input[1U] % 40U;
    const auto& [depth, color] = formats[input[2U] % formats.size()];
    const bool interlaced = (input[3U] & 1U) != 0U;
    const bool with_transparency = (input[3U] & 2U) != 0U;
    const std::span<const std::uint8_t> body = input.subspan(4U);

    const std::uint32_t channels = color == 2U ? 3U : color == 4U ? 2U : color == 6U ? 4U : 1U;
    constexpr std::array<std::array<std::uint32_t, 4U>, 7U> adam7{{
        {0U, 0U, 8U, 8U}, {4U, 0U, 8U, 8U}, {0U, 4U, 4U, 8U}, {2U, 0U, 4U, 4U},
        {0U, 2U, 2U, 4U}, {1U, 0U, 2U, 2U}, {0U, 1U, 1U, 2U},
    }};
    std::size_t scanline_bytes = 0U;
    for (std::size_t pass = 0U; pass < (interlaced ? 7U : 1U); ++pass) {
        const auto [x, y, dx, dy] = interlaced ? adam7[pass] : std::array<std::uint32_t, 4U>{
                                                                   0U, 0U, 1U, 1U};
        const std::uint32_t pass_width = width > x ? (width - x + dx - 1U) / dx : 0U;
        const std::uint32_t pass_height = height > y ? (height - y + dy - 1U) / dy : 0U;
        if (pass_width != 0U && pass_height != 0U)
            scanline_bytes += ((pass_width * channels * depth + 7U) / 8U + 1U) * pass_height;
    }
    std::vector<std::uint8_t> scanlines(scanline_bytes, 0U);
    for (std::size_t index = 0U; index < scanlines.size() && index < body.size(); ++index)
        scanlines[index] = body[index];
    // The bytes past the scanlines supply the palette and transparency, read back to front.
    std::size_t tail = body.size();
    const auto take = [&](const std::size_t count) {
        std::vector<std::uint8_t> result;
        while (result.size() < count && tail > scanline_bytes)
            result.push_back(body[--tail]);
        result.resize(count, 0x80U);
        return result;
    };

    std::vector<std::uint8_t> png{0x89U, 'P', 'N', 'G', 0x0DU, 0x0AU, 0x1AU, 0x0AU};
    std::vector<std::uint8_t> header;
    put(header, width);
    put(header, height);
    header.insert(header.end(), {depth, color, 0U, 0U, static_cast<std::uint8_t>(interlaced)});
    chunk(png, "IHDR", header);
    if (color == 3U) {
        const std::size_t entries = 1U + (tail > scanline_bytes ? body[--tail] : 0U);
        chunk(png, "PLTE", take(entries * 3U));
        if (with_transparency)
            chunk(png, "tRNS", take(entries));
    } else if (with_transparency && (color == 0U || color == 2U)) {
        chunk(png, "tRNS", take(color == 0U ? 2U : 6U));
    }
    // Stored blocks of at most 65535 bytes each, then the Adler-32 of the scanlines.
    std::vector<std::uint8_t> zlib{0x78U, 0x01U};
    std::size_t at = 0U;
    do {
        const std::size_t length = std::min<std::size_t>(65535U, scanlines.size() - at);
        const bool last = at + length == scanlines.size();
        zlib.push_back(last ? 1U : 0U);
        zlib.insert(zlib.end(), {static_cast<std::uint8_t>(length),
                                 static_cast<std::uint8_t>(length >> 8U),
                                 static_cast<std::uint8_t>(~length),
                                 static_cast<std::uint8_t>(~length >> 8U)});
        zlib.insert(zlib.end(), scanlines.begin() + static_cast<std::ptrdiff_t>(at),
                    scanlines.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
    } while (at < scanlines.size());
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : scanlines) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    put(zlib, b << 16U | a);
    chunk(png, "IDAT", zlib);
    chunk(png, "IEND", {});
    decode(png);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    if (size == 0U)
        return 0;
    const std::span<const std::uint8_t> input(data + 1U, size - 1U);
    if ((data[0U] & 1U) == 0U)
        decode_with_valid_crcs(input);
    else
        decode_scanlines(input);
    return 0;
}
