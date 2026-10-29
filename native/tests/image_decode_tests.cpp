// Checks the JPEG, GIF, APNG and WebP decoders against Pillow. The fixtures and Pillow's decode of
// each (frames stacked in one PNG) are produced by tests/fixtures/images/generate.py; manifest.txt
// says how to compare them.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "resource/image.hpp"
#include "ui/render/submission.hpp"
#include "ui/render/surface_textures.hpp"

namespace {

using strata::resource::decode_image;
using strata::resource::DecodedImage;
using strata::resource::ImageDimensions;
using strata::resource::ImageFormat;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::vector<std::uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(stream.good(), "cannot open " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct Fixture final {
    std::string name;
    std::string expected;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::uint32_t loop_count = 0U;
    int tolerance = 0;
    std::vector<std::uint32_t> delays;
};

std::vector<Fixture> manifest(const std::filesystem::path& root) {
    std::ifstream stream(root / "manifest.txt");
    check(stream.good(), "cannot open the image fixture manifest");
    std::vector<Fixture> fixtures;
    std::string line;
    while (std::getline(stream, line)) {
        if (line.empty())
            continue;
        std::istringstream fields(line);
        Fixture fixture;
        std::string delays;
        fields >> fixture.name >> fixture.expected >> fixture.width >> fixture.height >>
            fixture.loop_count >> fixture.tolerance >> delays;
        check(!fields.fail(), "malformed manifest line: " + line);
        std::istringstream list(delays);
        for (std::string delay; std::getline(list, delay, ',');)
            fixture.delays.push_back(static_cast<std::uint32_t>(std::stoul(delay)));
        fixtures.push_back(std::move(fixture));
    }
    check(fixtures.size() >= 20U, "the image fixture manifest is incomplete");
    return fixtures;
}

ImageFormat format_of(const std::string_view name) {
    if (name.ends_with(".jpg"))
        return ImageFormat::jpeg;
    if (name.ends_with(".gif"))
        return ImageFormat::gif;
    if (name.ends_with(".webp"))
        return ImageFormat::webp;
    return ImageFormat::png;
}

/** The largest channel difference; pixels both sides leave fully transparent compare by alpha. */
int difference(const std::span<const std::uint8_t> actual,
               const std::span<const std::uint8_t> expected) {
    int largest = 0;
    for (std::size_t pixel = 0U; pixel + 3U < actual.size(); pixel += 4U) {
        const bool transparent = actual[pixel + 3U] == 0U && expected[pixel + 3U] == 0U;
        for (std::size_t channel = transparent ? 3U : 0U; channel < 4U; ++channel) {
            largest = std::max(largest,
                               std::abs(int{actual[pixel + channel]} - expected[pixel + channel]));
        }
    }
    return largest;
}

void test_fixture(const std::filesystem::path& root, const Fixture& fixture) {
    const std::string label = fixture.name + ": ";
    const std::vector<std::uint8_t> bytes = read_file(root / fixture.name);
    const ImageDimensions size{fixture.width, fixture.height};
    check(strata::resource::sniff_image_format(bytes) == format_of(fixture.name),
          label + "format was not sniffed");
    const strata::resource::ImageInfo info = strata::resource::inspect_image(bytes);
    check(info.format == format_of(fixture.name) && info.dimensions == size,
          label + "inspect_image reported the wrong format or dimensions");

    const DecodedImage image = decode_image(bytes);
    check(image.width == size.width && image.height == size.height,
          label + "decoded dimensions differ");
    check(image.frames.size() == fixture.delays.size(),
          label + "decoded " + std::to_string(image.frames.size()) + " frames, expected " +
              std::to_string(fixture.delays.size()));
    check(image.loop_count == fixture.loop_count,
          label + "loop count " + std::to_string(image.loop_count) + ", expected " +
              std::to_string(fixture.loop_count));

    const std::size_t frame_bytes = static_cast<std::size_t>(size.width) * size.height * 4U;
    const std::vector<std::uint8_t> stacked_png = read_file(root / fixture.expected);
    const std::vector<std::uint8_t> stacked = strata::resource::decode_png_rgba8(
        stacked_png,
        ImageDimensions{size.width, size.height * static_cast<std::uint32_t>(image.frames.size())});
    for (std::size_t index = 0U; index < image.frames.size(); ++index) {
        const std::string frame = label + "frame " + std::to_string(index) + ": ";
        check(image.frames[index].delay_ms == fixture.delays[index], frame + "delay differs");
        check(image.frames[index].rgba.size() == frame_bytes, frame + "has the wrong size");
        const int largest = difference(
            image.frames[index].rgba, std::span(stacked).subspan(index * frame_bytes, frame_bytes));
        check(largest <= fixture.tolerance,
              frame + "differs from Pillow by " + std::to_string(largest));
    }

    // Asking for one frame decodes one, and it is the first frame of the full decode.
    const DecodedImage first = decode_image(bytes, 1U);
    check(first.frames.size() == 1U && first.frames[0U].rgba == image.frames[0U].rgba,
          label + "a one-frame decode differs from the full decode's first frame");
    check(strata::resource::decode_image_rgba8(bytes, size) == image.frames[0U].rgba,
          label + "decode_image_rgba8 differs from the first frame");
    bool mismatch_rejected = false;
    try {
        static_cast<void>(strata::resource::decode_image_rgba8(
            bytes, ImageDimensions{size.width + 1U, size.height}));
    } catch (const std::invalid_argument&) {
        mismatch_rejected = true;
    }
    check(mismatch_rejected, label + "decode_image_rgba8 accepted the wrong dimensions");
}

/** Decoding damaged input either succeeds or throws std::invalid_argument; nothing else. */
void decode_or_reject(const std::span<const std::uint8_t> bytes) {
    try {
        static_cast<void>(strata::resource::inspect_image(bytes));
    } catch (const std::invalid_argument&) {
    }
    try {
        static_cast<void>(decode_image(bytes));
    } catch (const std::invalid_argument&) {
    }
}

void test_damaged_fixtures(const std::filesystem::path& root,
                           const std::vector<Fixture>& fixtures) {
    std::uint32_t state = 0x12345678U;
    for (const Fixture& fixture : fixtures) {
        const std::vector<std::uint8_t> bytes = read_file(root / fixture.name);
        for (std::size_t length = 0U; length < bytes.size(); ++length)
            decode_or_reject(std::span(bytes).first(length));
        std::vector<std::uint8_t> damaged;
        for (int round = 0; round < 300; ++round) {
            damaged = bytes;
            for (int flip = 0; flip < 1 + round % 4; ++flip) {
                state = state * 1664525U + 1013904223U;
                const std::size_t at = (state >> 8U) % damaged.size();
                damaged[at] = static_cast<std::uint8_t>(damaged[at] ^ (1U << ((state >> 4U) % 8U)));
            }
            decode_or_reject(damaged);
        }
    }
}

bool rejects(const std::vector<std::uint8_t>& bytes) {
    try {
        static_cast<void>(decode_image(bytes));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

bool inspect_rejects(const std::vector<std::uint8_t>& bytes) {
    try {
        static_cast<void>(strata::resource::inspect_image(bytes));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void u16le(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8U));
}

/** A GIF of `frames` 1x1 frames on a 1x1 canvas. */
std::vector<std::uint8_t> tiny_animated_gif(const std::uint32_t frames) {
    std::vector<std::uint8_t> gif{'G', 'I', 'F', '8', '9', 'a'};
    u16le(gif, 1U);
    u16le(gif, 1U);
    gif.insert(gif.end(), {0x80U, 0U, 0U, 0U, 0U, 0U, 255U, 255U, 255U});
    for (std::uint32_t frame = 0U; frame < frames; ++frame) {
        gif.insert(gif.end(), {0x21U, 0xF9U, 4U, 0U, 1U, 0U, 0U, 0U});
        gif.insert(gif.end(), {0x2CU, 0U, 0U, 0U, 0U, 1U, 0U, 1U, 0U, 0U});
        // Code size 2: clear (4), index frame % 2, end (5), packed LSB first in 3-bit codes.
        const std::uint32_t codes = 4U | (frame % 2U) << 3U | 5U << 6U;
        gif.insert(gif.end(), {2U, 2U, static_cast<std::uint8_t>(codes),
                               static_cast<std::uint8_t>(codes >> 8U), 0U});
    }
    gif.push_back(0x3BU);
    return gif;
}

void test_bounds() {
    // Headers that declare more pixels than the decoders accept are refused before decoding.
    std::vector<std::uint8_t> gif{'G', 'I', 'F', '8', '9', 'a'};
    u16le(gif, 9000U);
    u16le(gif, 9000U);
    gif.insert(gif.end(), {0U, 0U, 0U, 0x3BU});
    check(inspect_rejects(gif) && rejects(gif), "a GIF beyond the pixel limit was accepted");

    const std::vector<std::uint8_t> jpeg{
        0xFFU, 0xD8U, 0xFFU, 0xC0U, 0U,    11U, 8U,    0x23U, 0x28U,
        0x23U, 0x28U, 1U,    1U,    0x11U, 0U,  0xFFU, 0xD9U,
    };
    check(inspect_rejects(jpeg) && rejects(jpeg), "a JPEG beyond the pixel limit was accepted");

    std::vector<std::uint8_t> webp{'R', 'I', 'F', 'F', 22U, 0U, 0U, 0U, 'W', 'E', 'B', 'P',
                                   'V', 'P', '8', 'X', 10U, 0U, 0U, 0U, 0U,  0U,  0U,  0U};
    webp.insert(webp.end(), {0xFFU, 0x3FU, 0U, 0xFFU, 0x3FU, 0U});
    check(inspect_rejects(webp) && rejects(webp), "a WebP beyond the pixel limit was accepted");

    // A 4096 x 4096 JPEG whose scan has no data at all, with one-bit codes for "no change" and
    // "end of block": decoding must stop at the missing data, not invent 262,144 flat blocks.
    std::vector<std::uint8_t> starved{0xFFU, 0xD8U, 0xFFU, 0xC0U, 0U, 11U,   8U, 0x10U,
                                      0U,    0x10U, 0U,    1U,    1U, 0x11U, 0U};
    starved.insert(starved.end(), {0xFFU, 0xDBU, 0U, 67U, 0U});
    starved.insert(starved.end(), 64U, std::uint8_t{1U});
    for (const std::uint8_t table : {std::uint8_t{0x00U}, std::uint8_t{0x10U}}) {
        starved.insert(starved.end(), {0xFFU, 0xC4U, 0U, 20U, table, 1U});
        starved.insert(starved.end(), 16U, std::uint8_t{0U});
    }
    starved.insert(starved.end(), {0xFFU, 0xDAU, 0U, 8U, 1U, 1U, 0U, 0U, 63U, 0U, 0xFFU, 0xD9U});
    check(strata::resource::inspect_image(starved).dimensions == ImageDimensions{4096U, 4096U},
          "the starved JPEG's header was not read");
    check(rejects(starved), "a JPEG decoded far more blocks than its data holds");

    // Animations stop at the frame limit when asked for fewer frames, and are refused past it.
    const std::vector<std::uint8_t> many =
        tiny_animated_gif(strata::resource::maximum_image_frames + 1U);
    check(rejects(many), "an animation beyond the frame limit was accepted");
    const DecodedImage first = decode_image(many, 1U);
    check(first.frames.size() == 1U &&
              first.frames[0U].rgba == std::vector<std::uint8_t>({0U, 0U, 0U, 255U}),
          "a one-frame decode of a long animation failed");
    const DecodedImage within = decode_image(tiny_animated_gif(3U));
    check(within.frames.size() == 3U && within.frames[1U].rgba[0U] == 255U,
          "a short tiny animation decoded wrongly");

    check(!strata::resource::sniff_image_format(std::vector<std::uint8_t>{'<', 's', 'v', 'g'})
               .has_value(),
          "SVG text was sniffed as a raster image");
    check(inspect_rejects({'<', 's', 'v', 'g'}), "inspect_image accepted SVG text");
}

void test_apng_still_compatibility(const std::filesystem::path& root) {
    // decode_png_rgba8 keeps decoding an APNG's default image, as it always has.
    const std::vector<std::uint8_t> bytes = read_file(root / "apng_hidden_default.png");
    const std::vector<std::uint8_t> still = strata::resource::decode_png_rgba8(bytes, {22U, 17U});
    const DecodedImage animation = decode_image(bytes);
    check(still.size() == 22U * 17U * 4U && still != animation.frames[0U].rgba,
          "the APNG default image was not kept apart from the animation");
    check(still[3U] == 255U, "the APNG default image lost its opaque background");
}

void test_surface_textures_decode_every_format(const std::filesystem::path& root) {
    // A Surface's static images reach the host as RGBA8 decoded by the core, whatever their format.
    std::vector<strata::resource::EncodedTextureResource> textures;
    std::vector<std::vector<std::uint8_t>> expected;
    for (const std::string_view name : {"jpeg_progressive_420.jpg", "gif_handmade.gif",
                                        "apng_blend_dispose.png", "webp_lossy_alpha.webp"}) {
        std::vector<std::uint8_t> bytes = read_file(root / std::string(name));
        const strata::resource::ImageInfo info = strata::resource::inspect_image(bytes);
        expected.push_back(decode_image(bytes, 1U).frames.front().rgba);
        textures.push_back(strata::resource::EncodedTextureResource{
            std::string(name),
            "fixture/static/" + std::string(name),
            strata::resource::TextureSampling::linear,
            strata::resource::image_encoding(info.format),
            info.dimensions,
            std::move(bytes),
        });
    }
    strata::ui::SurfaceTextures surface_textures("fixture", textures);
    strata::ui::RenderSubmission submission;
    for (const strata::resource::EncodedTextureResource& texture : textures) {
        strata::ui::SubmissionBatch batch;
        batch.texture = texture.host_id;
        submission.batches.push_back(batch);
    }
    surface_textures.plan(submission, true);
    check(surface_textures.uploads().size() == textures.size(),
          "a Surface did not upload every encoded image");
    for (const strata::ui::TextureUpload& upload : surface_textures.uploads()) {
        const auto index = static_cast<std::size_t>(
            std::ranges::find(textures, upload.descriptor->logical_id,
                              &strata::resource::EncodedTextureResource::logical_id) -
            textures.begin());
        check(index < textures.size() && std::ranges::equal(upload.bytes, expected[index]),
              "a Surface uploaded other pixels than the image's first frame");
    }
}

} // namespace

int strata_test_image_decode(const int argument_count, const char* const* const arguments) {
    try {
        check(argument_count >= 2, "expected the image fixture directory");
        const std::filesystem::path root(arguments[1]);
        const std::vector<Fixture> fixtures = manifest(root);
        for (const Fixture& fixture : fixtures)
            test_fixture(root, fixture);
        test_damaged_fixtures(root, fixtures);
        test_bounds();
        test_apng_still_compatibility(root);
        test_surface_textures_decode_every_format(root);
    } catch (const std::exception& error) {
        std::cerr << "strata_image_decode_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "strata_image_decode_tests: OK\n";
    return 0;
}
