// libFuzzer entry point for the stateful render-packet decoder custom backends consume. Built only
// with -DSTRATA_BUILD_FUZZERS=ON (MSVC or Clang), which also instruments the decoder for coverage.
// Run, for example:
//   strata_render_packet_fuzzer -max_len=1048576 -timeout=10 -rss_limit_mb=2048 corpus/
//
// An input is an ordered packet stream: each packet is a little-endian u32 byte count followed by
// that many bytes, and whatever remains is the last packet. One decoder consumes them in order, so
// retained epochs, geometry patches, presentation groups and texture uploads are exercised across
// frames. Seed the corpus from headless packet recordings (docs/headless-testing.md) by rewriting
// each recorded frame into this framing.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include <strata/render_packet.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    strata::host::RenderPacketDecoder decoder;
    std::span<const std::uint8_t> input(data, size);
    while (!input.empty()) {
        std::span<const std::uint8_t> packet = input;
        if (input.size() >= 4U) {
            const std::size_t length = static_cast<std::size_t>(input[0U]) |
                                       static_cast<std::size_t>(input[1U]) << 8U |
                                       static_cast<std::size_t>(input[2U]) << 16U |
                                       static_cast<std::size_t>(input[3U]) << 24U;
            input = input.subspan(4U);
            packet = input.subspan(0U, length < input.size() ? length : input.size());
        }
        input = input.subspan(packet.size());
        try {
            static_cast<void>(decoder.decode(packet));
        } catch (const std::exception&) {
            // A host discards a stream whose packet it cannot decode; crashes, hangs and unbounded
            // allocation are the findings.
            decoder.reset();
        }
    }
    return 0;
}
