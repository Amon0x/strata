#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace strata::test {

/**
 * Encodes a 16-bit RGBA PNG (stored, uncompressed deflate) with an optional gAMA chunk, in units of
 * 1/100000. Backends decode PNG textures with different libraries; fixtures like this pin what
 * they must agree on.
 */
[[nodiscard]] inline std::vector<std::uint8_t>
rgba16_png(const std::uint32_t width, const std::uint32_t height,
           const std::span<const std::uint16_t> rgba, const std::optional<std::uint32_t> gamma) {
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto u32 = [](std::vector<std::uint8_t>& out, const std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto chunk = [&](const std::string_view type, const std::vector<std::uint8_t>& data) {
        u32(png, static_cast<std::uint32_t>(data.size()));
        std::vector<std::uint8_t> body(type.begin(), type.end());
        body.insert(body.end(), data.begin(), data.end());
        std::uint32_t crc = 0xFFFFFFFFU;
        for (const std::uint8_t byte : body) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
        png.insert(png.end(), body.begin(), body.end());
        u32(png, ~crc);
    };
    std::vector<std::uint8_t> header;
    u32(header, width);
    u32(header, height);
    header.insert(header.end(), {16U, 6U, 0U, 0U, 0U});
    chunk("IHDR", header);
    if (gamma.has_value()) {
        std::vector<std::uint8_t> value;
        u32(value, *gamma);
        chunk("gAMA", value);
    }
    std::vector<std::uint8_t> raw;
    for (std::uint32_t y = 0U; y < height; ++y) {
        raw.push_back(0U);
        for (std::uint32_t x = 0U; x < width * 4U; ++x) {
            const std::uint16_t sample = rgba[static_cast<std::size_t>(y) * width * 4U + x];
            raw.push_back(static_cast<std::uint8_t>(sample >> 8U));
            raw.push_back(static_cast<std::uint8_t>(sample));
        }
    }
    // zlib stream of one stored block; fixtures stay well under its 65535-byte limit.
    std::vector<std::uint8_t> zlib{0x78, 0x01, 0x01};
    const auto length = static_cast<std::uint16_t>(raw.size());
    zlib.insert(zlib.end(),
                {static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8U),
                 static_cast<std::uint8_t>(~length),
                 static_cast<std::uint8_t>(static_cast<std::uint16_t>(~length) >> 8U)});
    zlib.insert(zlib.end(), raw.begin(), raw.end());
    std::uint32_t a = 1U, b = 0U;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    u32(zlib, b << 16U | a);
    chunk("IDAT", zlib);
    chunk("IEND", {});
    return png;
}

} // namespace strata::test
