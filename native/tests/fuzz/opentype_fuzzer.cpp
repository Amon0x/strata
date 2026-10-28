// libFuzzer entry point for the OpenType reader. Built only with -DSTRATA_BUILD_FUZZERS=ON
// (MSVC or Clang), which also instruments the core library for coverage. Run, for example:
//   strata_opentype_fuzzer -max_len=1048576 -timeout=10 -rss_limit_mb=2048 corpus/
// seeded with the bundled fonts under src/main/resources/assets/strata/fonts.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include "font/opentype.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    try {
        const strata::font::OpenTypeFont font =
            strata::font::OpenTypeFont::parse(strata::resource::ResourceBytes(data, data + size));
        // Shaping walks the parsed GSUB/GPOS structures, not just the reader.
        static_cast<void>(font.shape_utf8(std::string_view("ffi AV To\xCC\x81"), 16.0));
    } catch (const std::exception&) {
        // Rejecting a malformed font is the expected outcome; hangs and crashes are the findings.
    }
    return 0;
}
