#include "png.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace strata::headless {
namespace {

using Bytes = std::vector<std::uint8_t>;

void big_u32(Bytes& output, const std::uint32_t value) {
    output.push_back(static_cast<std::uint8_t>(value >> 24U));
    output.push_back(static_cast<std::uint8_t>(value >> 16U));
    output.push_back(static_cast<std::uint8_t>(value >> 8U));
    output.push_back(static_cast<std::uint8_t>(value));
}

[[nodiscard]] std::uint32_t crc32(const std::span<const std::uint8_t> bytes) noexcept {
    std::uint32_t crc = 0xffffffffU;
    for (const std::uint8_t byte : bytes) {
        crc ^= byte;
        for (std::uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

[[nodiscard]] std::uint32_t adler32(const std::span<const std::uint8_t> bytes) noexcept {
    constexpr std::uint32_t modulus = 65'521U;
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : bytes) {
        a = (a + byte) % modulus;
        b = (b + a) % modulus;
    }
    return b << 16U | a;
}

void chunk(Bytes& output, const std::string_view type,
           const std::span<const std::uint8_t> payload) {
    if (type.size() != 4U || payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("PNG chunk exceeds its encoded domain");
    }
    big_u32(output, static_cast<std::uint32_t>(payload.size()));
    const std::size_t crc_start = output.size();
    output.insert(output.end(), type.begin(), type.end());
    output.insert(output.end(), payload.begin(), payload.end());
    big_u32(output, crc32(std::span(output).subspan(crc_start)));
}

[[nodiscard]] Bytes stored_zlib(const std::span<const std::uint8_t> input) {
    Bytes output;
    output.reserve(input.size() + input.size() / 65'535U * 5U + 8U);
    output.push_back(0x78U);
    output.push_back(0x01U);
    std::size_t offset = 0U;
    do {
        const std::size_t count = std::min<std::size_t>(65'535U, input.size() - offset);
        const bool final = offset + count == input.size();
        output.push_back(final ? 0x01U : 0x00U);
        const std::uint16_t length = static_cast<std::uint16_t>(count);
        const std::uint16_t complement = static_cast<std::uint16_t>(~length);
        output.push_back(static_cast<std::uint8_t>(length));
        output.push_back(static_cast<std::uint8_t>(length >> 8U));
        output.push_back(static_cast<std::uint8_t>(complement));
        output.push_back(static_cast<std::uint8_t>(complement >> 8U));
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(offset),
                      input.begin() + static_cast<std::ptrdiff_t>(offset + count));
        offset += count;
    } while (offset < input.size());
    big_u32(output, adler32(input));
    return output;
}

} // namespace

void write_png(const std::filesystem::path& path, const std::uint32_t width,
               const std::uint32_t height, const std::span<const std::uint8_t> rgba) {
    const std::uint64_t expected = static_cast<std::uint64_t>(width) * height * 4U;
    if (width == 0U || height == 0U || expected != rgba.size()) {
        throw std::invalid_argument("PNG output dimensions do not match its RGBA payload");
    }
    const std::uint64_t row_bytes = static_cast<std::uint64_t>(width) * 4U;
    const std::uint64_t filtered_size = (row_bytes + 1U) * height;
    if (filtered_size > std::numeric_limits<std::size_t>::max()) {
        throw std::length_error("PNG output exceeds host address space");
    }
    Bytes filtered;
    filtered.reserve(static_cast<std::size_t>(filtered_size));
    for (std::uint32_t y = 0U; y < height; ++y) {
        filtered.push_back(0U);
        const std::size_t begin = static_cast<std::size_t>(y) * static_cast<std::size_t>(row_bytes);
        filtered.insert(filtered.end(), rgba.begin() + static_cast<std::ptrdiff_t>(begin),
                        rgba.begin() + static_cast<std::ptrdiff_t>(begin + row_bytes));
    }

    Bytes output{0x89U, 0x50U, 0x4eU, 0x47U, 0x0dU, 0x0aU, 0x1aU, 0x0aU};
    Bytes header;
    big_u32(header, width);
    big_u32(header, height);
    header.insert(header.end(), {8U, 6U, 0U, 0U, 0U});
    chunk(output, "IHDR", header);
    const Bytes compressed = stored_zlib(filtered);
    chunk(output, "IDAT", compressed);
    chunk(output, "IEND", {});

    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(output.data()),
                 static_cast<std::streamsize>(output.size()));
    if (!stream)
        throw std::runtime_error("could not write headless PNG: " + path.string());
}

} // namespace strata::headless
