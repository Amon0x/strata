// libFuzzer entry point for the core image decoders behind decode_image: PNG and APNG, JPEG, GIF
// and WebP (VP8L, VP8, ALPH and animation). Built only with -DSTRATA_BUILD_FUZZERS=ON. Run, for
// example:
//   strata_image_fuzzer -max_len=65536 -timeout=10 -rss_limit_mb=2048 corpus/
// seeded with native/tests/fixtures/images. The PNG fuzzer reaches further into PNG itself, since
// it repairs chunk CRCs; this one spends its time on the formats without checksums.

#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>

#include "resource/image.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* const data, const std::size_t size) {
    const std::span<const std::uint8_t> bytes(data, size);
    try {
        static_cast<void>(strata::resource::inspect_image(bytes));
    } catch (const std::exception&) {
    }
    try {
        // A low frame limit keeps each run fast; a long animation's frames are alike anyway.
        static_cast<void>(strata::resource::decode_image(bytes, 8U));
    } catch (const std::exception&) {
        // Rejecting a malformed image is the expected outcome; crashes, hangs and unbounded
        // allocation are the findings.
    }
    return 0;
}
