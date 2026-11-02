#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <variant>
#include <vector>

#include <strata/render_packet.hpp>

namespace strata::headless {

/** One frame of a Surface's render packet stream with what a backend needs to render it again. */
struct RecordedPacket final {
    std::int64_t time_nanoseconds = 0;
    std::uint32_t framebuffer_width = 0U;
    std::uint32_t framebuffer_height = 0U;
    double logical_width = 0.0;
    double logical_height = 0.0;
    /** Live recording retains the producer's handle; disk replay owns its bounded input bytes. */
    std::variant<EncodedRenderPacket, std::vector<std::uint8_t>> payload;
    [[nodiscard]] std::span<const std::uint8_t> bytes() const noexcept {
        if (const auto* encoded = std::get_if<EncodedRenderPacket>(&payload))
            return encoded->bytes();
        return std::get<std::vector<std::uint8_t>>(payload);
    }
};

/**
 * Writes an ordered packet stream: "STRATAPS", u32 version 1, u32 frame count, then per frame
 * i64 time, u32 framebuffer width/height, f64 logical width/height, u32 byte count and the packet.
 */
void write_packet_stream(const std::filesystem::path& path, std::span<const RecordedPacket> frames);
[[nodiscard]] std::vector<RecordedPacket> read_packet_stream(const std::filesystem::path& path);

} // namespace strata::headless
