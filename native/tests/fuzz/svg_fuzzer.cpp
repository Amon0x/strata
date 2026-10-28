// libFuzzer entry point for the static SVG parser and its reference rasterizer. Built only with
// -DSTRATA_BUILD_FUZZERS=ON (MSVC or Clang), which also instruments the SVG library for coverage.
// Run, for example:
//   strata_svg_fuzzer -max_len=65536 -timeout=10 -rss_limit_mb=2048 -dict=svg.dict corpus/
// seeded with the bundled SVGs under src/main/resources/assets/strata/images.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <string_view>

#include <strata/svg.hpp>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    try {
        const strata::svg::Document document =
            strata::svg::parse(std::string_view(reinterpret_cast<const char*>(data), size));
        // A small raster walks every parsed command through transforms, flattening and coverage.
        strata::svg::RasterOptions options;
        options.width = 24U;
        options.height = 24U;
        options.samples_per_axis = 2U;
        static_cast<void>(strata::svg::rasterize(document, options));
    } catch (const std::exception&) {
        // Rejecting unsupported or malformed content is the expected outcome; crashes, hangs and
        // unbounded allocation are the findings.
    }
    return 0;
}
