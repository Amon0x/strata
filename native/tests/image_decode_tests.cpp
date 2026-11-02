// Checks the JPEG, GIF, APNG and WebP decoders against Pillow. The fixtures and Pillow's decode of
// each (frames stacked in one PNG) are produced by tests/fixtures/images/generate.py; manifest.txt
// says how to compare them.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "base/pending.hpp"
#include "compiler/source.hpp"
#include "resource/image.hpp"
#include "resource/image_stream.hpp"
#include "runtime/application.hpp"
#include "ui/render/submission.hpp"
#include "ui/render/surface_textures.hpp"
#include "ui/surface.hpp"
#include "ui/svg_image.hpp"
#include "ui/widget/presentation.hpp"

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

} // namespace

// Streaming hands over exactly the frames decode_image keeps, in order, and stops when asked.
void test_streamed_frames(const std::filesystem::path& root, const std::vector<Fixture>& fixtures) {
    for (const Fixture& fixture : fixtures) {
        const std::vector<std::uint8_t> bytes = read_file(root / fixture.name);
        const DecodedImage kept = decode_image(bytes);
        std::vector<strata::resource::ImageFrame> streamed;
        const std::uint32_t loops = strata::resource::stream_image(
            bytes, [&streamed](std::vector<std::uint8_t>& rgba, const std::uint32_t delay_ms) {
                streamed.push_back(strata::resource::ImageFrame{std::move(rgba), delay_ms});
                return true;
            });
        check(loops == kept.loop_count && streamed.size() == kept.frames.size(),
              fixture.name + ": streaming gave a different frame count or loop count");
        for (std::size_t index = 0U; index < streamed.size(); ++index) {
            check(streamed[index].rgba == kept.frames[index].rgba &&
                      streamed[index].delay_ms == kept.frames[index].delay_ms,
                  fixture.name + ": streamed frame " + std::to_string(index) + " differs");
        }
        if (kept.frames.size() > 1U) {
            std::size_t taken = 0U;
            static_cast<void>(strata::resource::stream_image(
                bytes,
                [&taken](std::vector<std::uint8_t>&, std::uint32_t) { return ++taken < 1U; }));
            check(taken == 1U, fixture.name + ": streaming went on after it was told to stop");
        }
        std::optional<strata::resource::ImageDecoder> initial(std::in_place, bytes);
        const auto first = initial->next();
        check(first.has_value() && first->rgba == kept.frames.front().rgba,
              fixture.name + ": resumable decoding lost its first frame");
        strata::resource::ImageDecoder resumed(std::move(*initial));
        initial.reset();
        for (std::size_t index = 1U; index < kept.frames.size(); ++index) {
            const auto frame = resumed.next();
            check(frame.has_value() && frame->rgba == kept.frames[index].rgba &&
                      frame->delay_ms == kept.frames[index].delay_ms,
                  fixture.name + ": transferred continuation restarted or lost compositing state");
        }
        check(!resumed.next().has_value() && resumed.loop_count() == kept.loop_count,
              fixture.name + ": resumable decoding lost its terminal frame/loop metadata");
        resumed.rewind();
        for (const auto& expected : kept.frames) {
            const auto frame = resumed.next();
            check(frame.has_value() && frame->rgba == expected.rgba &&
                      frame->delay_ms == expected.delay_ms,
                  fixture.name + ": rewind retained disposal/parser state from the preceding play");
        }
        check(!resumed.next().has_value(), fixture.name + ": rewind duplicated the final frame");
    }
}

void test_resumable_animation_bounds() {
    using strata::resource::ImageDecoder;
    const std::uint32_t count = strata::resource::maximum_image_frames + 1U;
    ImageDecoder long_animation(tiny_animated_gif(count));
    for (std::uint32_t index = 0U; index < count; ++index) {
        const auto frame = long_animation.next();
        const auto value = static_cast<std::uint8_t>(index % 2U == 0U ? 0U : 255U);
        check(frame.has_value() &&
                  frame->rgba == std::vector<std::uint8_t>{value, value, value, 255U},
              "a resumable animation accumulated a collection budget or repeated a frame");
    }
    check(!long_animation.next().has_value(), "resumable animation did not reach its trailer");
    auto damaged = tiny_animated_gif(1U);
    damaged.back() = 0x2CU; // Another image descriptor, truncated before its dimensions.
    ImageDecoder partial(std::move(damaged));
    const auto first = partial.next();
    check(first.has_value() && first->rgba == std::vector<std::uint8_t>{0U, 0U, 0U, 255U},
          "a malformed later frame suppressed the valid prefix");
    bool rejected = false;
    try {
        static_cast<void>(partial.next());
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected && first->rgba == std::vector<std::uint8_t>{0U, 0U, 0U, 255U},
          "a malformed continuation was accepted or modified an earlier decoded frame");
}

/** Pool queue capacity and playback time are independent; neither can block a decoder worker. */
void test_image_stream_playback(const std::filesystem::path& root) {
    using namespace strata;
    for (const std::string_view name :
         {"gif_handmade.gif", "apng_blend_dispose.png", "webp_anim_handmade.webp",
          "webp_anim_lossless.webp"}) {
        const auto bytes = read_file(root / name);
        const auto expected = decode_image(bytes);
        base::TaskOwner tasks(base::TaskExecution::deterministic);
        resource::ImageStream stream(resource::prepare_image(bytes), tasks, std::string(name));
        std::int64_t now = 1'000'000'000;
        const auto initial_revision = stream.revision();
        stream.advance(now);
        check(stream.revision() == initial_revision &&
                  std::ranges::equal(stream.pixels(), expected.frames.front().rgba),
              "starting a stream decoded or published its first frame twice");
        const auto plays = expected.loop_count == 0U ? 2U : expected.loop_count;
        for (std::uint32_t play = 0U; play < plays; ++play) {
            for (const auto& frame : expected.frames) {
                stream.advance(now);
                check(std::ranges::equal(stream.pixels(), frame.rgba),
                      std::string(name) + ": owner-clock playback lost a frame or disposal state");
                const auto revision = stream.revision();
                const std::int64_t delay =
                    static_cast<std::int64_t>(frame.delay_ms < 20U ? 100U : frame.delay_ms) *
                    1'000'000;
                stream.advance(now + delay - 1);
                check(stream.revision() == revision,
                      "prefetch completion shortened a displayed frame's requested duration");
                now += delay;
            }
        }
        if (expected.loop_count != 0U) {
            stream.advance(now);
            const auto revision = stream.revision();
            stream.advance(now + 1'000'000'000);
            check(stream.finished() && stream.revision() == revision &&
                      std::ranges::equal(stream.pixels(), expected.frames.back().rgba),
                  "finite image playback did not freeze on its final canvas");
        }
    }

    base::TaskPool pool(1U);
    base::TaskOwner tasks(base::TaskExecution::asynchronous, {}, &pool);
    resource::ImageStream full(resource::prepare_image(tiny_animated_gif(2000U)), tasks, "full");
    full.advance(0);
    while (tasks.pending() != 0U) {
        check(tasks.wait_for_completion(std::chrono::seconds(5)), "image lookahead did not finish");
        static_cast<void>(tasks.drain());
    }
    const auto independent = base::Pending<DecodedImage>::start(
        tasks, base::TaskKind::image, base::TaskPriority::idle, "independent-image",
        [bytes = tiny_animated_gif(1U)](const base::TaskCancellation&) {
            return decode_image(bytes);
        });
    check(tasks.wait_for_completion(std::chrono::seconds(5)),
          "a full image lookahead queue parked the only pool worker");
    static_cast<void>(tasks.drain());
    check(independent.get() != nullptr &&
              independent.get()->frames.front().rgba == std::vector<std::uint8_t>{0U, 0U, 0U, 255U},
          "a full animation queue blocked unrelated image preparation");
    full.advance(250'000'000);
    check(std::ranges::equal(full.pixels(), std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "lookahead replayed the first frame instead of continuing from the prepared second frame");
    while (tasks.pending() != 0U) {
        check(tasks.wait_for_completion(std::chrono::seconds(5)), "image refill did not finish");
        static_cast<void>(tasks.drain());
    }

    std::promise<void> release;
    auto entered = std::make_shared<std::promise<void>>();
    auto started = entered->get_future();
    static_cast<void>(tasks.post(
        base::TaskKind::other, base::TaskPriority::frame_critical,
        [entered, gate = release.get_future().share()](const base::TaskCancellation&) {
            entered->set_value();
            gate.wait();
        }, [](const base::TaskOutcome&) {}));
    check(started.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
          "decoder starvation fixture did not occupy its worker");
    resource::ImageStream delayed(resource::prepare_image(tiny_animated_gif(8U)), tasks, "delayed");
    delayed.advance(0);
    delayed.advance(250'000'000);
    check(std::ranges::equal(delayed.pixels(),
                            std::array<std::uint8_t, 4U>{255U, 255U, 255U, 255U}),
          "a starved decoder did not hold its last valid canvas");
    {
        resource::ImageStream removed(
            resource::prepare_image(tiny_animated_gif(8U)), tasks, "removed");
        removed.advance(0);
    }
    release.set_value();
    while (tasks.pending() != 0U) {
        check(tasks.wait_for_completion(std::chrono::seconds(5)),
              "a removed stream or resumed decoder prevented settling");
        static_cast<void>(tasks.drain());
    }
    delayed.advance(275'000'000);
    check(std::ranges::equal(delayed.pixels(), std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "the owner frame callback did not adopt the expired canvas");
    delayed.advance(374'999'999);
    check(std::ranges::equal(delayed.pixels(), std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "late decoder adoption shortened the next frame to catch up");
    delayed.advance(375'000'000);
    check(std::ranges::equal(delayed.pixels(),
                            std::array<std::uint8_t, 4U>{255U, 255U, 255U, 255U}) &&
              tasks.statistics().kinds[static_cast<std::size_t>(base::TaskKind::image)].cancelled != 0U,
          "resumed playback lost its deadline or a retired stream remained eligible for adoption");

    std::size_t failures = 0U;
    base::TaskOwner inline_tasks(base::TaskExecution::deterministic, {}, nullptr,
        [&failures](const base::TaskKind kind, const std::exception_ptr& error) {
            check(kind == base::TaskKind::image && error != nullptr,
                  "image continuation failure lost its owner diagnostic");
            ++failures;
        });
    auto damaged = tiny_animated_gif(3U);
    damaged.back() = 0x2CU;
    resource::ImageStream partial(resource::prepare_image(std::move(damaged)), inline_tasks, "partial");
    partial.advance(0);
    partial.advance(100'000'000);
    check(std::ranges::equal(partial.pixels(),
                            std::array<std::uint8_t, 4U>{255U, 255U, 255U, 255U}),
          "a later decoding failure discarded an already prepared frame");
    partial.advance(200'000'000);
    partial.advance(300'000'000);
    check(failures == 1U && partial.finished() &&
              std::ranges::equal(partial.pixels(), std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "a failed slice lost its valid prefix or failed to freeze on the last good frame");
}

/** Pending bindings reserve their ids; only complete resources enter packet texture residency. */
void test_image_preparation() {
    using namespace strata;
    base::TaskPool pool(1U);
    std::size_t failures = 0U;
    base::TaskOwner tasks(base::TaskExecution::asynchronous, {}, &pool,
        [&failures](const base::TaskKind kind, const std::exception_ptr& error) {
            check(kind == base::TaskKind::image && error != nullptr,
                  "image preparation lost its diagnostic category");
            ++failures;
        });
    std::promise<void> release;
    static_cast<void>(tasks.post(
        base::TaskKind::other, base::TaskPriority::frame_critical,
        [gate = release.get_future().share()](const base::TaskCancellation&) { gate.wait(); },
        [](const base::TaskOutcome&) {}));
    resource::RuntimeImageStore runtime;
    for (const std::string id : {"photo", "vector", "broken"})
        runtime.publish(id, {1U, 1U}, resource::TextureSampling::linear, {255U, 0U, 0U, 255U});
    const std::string svg =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"100\">"
        "<path d=\"M0 0 L200 0 L200 100 Z\" fill=\"red\"/></svg>";
    std::vector<resource::ImageSource> sources;
    sources.push_back({"photo", "fixture/photo", "photo.gif",
                       resource::TextureSampling::linear, tiny_animated_gif(1U)});
    sources.push_back({"vector", "fixture/vector", "vector.svg",
                       resource::TextureSampling::linear, {svg.begin(), svg.end()}});
    sources.push_back({"broken", "fixture/broken", "broken.svg",
                       resource::TextureSampling::linear, {'<', 's', 'v', 'g', '>'}});
    resource::SvgImageRegistry registry(tasks, std::move(sources),
                                        &ui::validate_svg_image_geometry, &runtime, {});
    const auto pending = registry.lookup("photo");
    check(pending.status() == base::PendingStatus::pending && !pending.value.has_value(),
          "a pending static binding exposed same-id runtime pixels or partial dimensions");
    ui::SurfaceTextures textures("fixture", {});
    static_cast<void>(textures.sync(runtime, &registry));
    check(textures.descriptors().empty(),
          "pending declared bindings fell through to runtime texture descriptors");
    base::Readiness removed;
    {
        std::vector<resource::ImageSource> retired{
            {"retired", "fixture/retired", "retired.gif",
             resource::TextureSampling::linear, tiny_animated_gif(1U)}};
        resource::SvgImageRegistry closing(tasks, std::move(retired),
                                           &ui::validate_svg_image_geometry, &runtime, {});
        removed = closing.lookup("retired").readiness;
    }
    release.set_value();
    while (tasks.pending() != 0U) {
        check(tasks.wait_for_completion(std::chrono::seconds(5)), "image preparation did not settle");
        static_cast<void>(tasks.drain());
    }
    const auto photo = registry.lookup("photo");
    const auto vector = registry.lookup("vector");
    const auto broken = registry.lookup("broken");
    check(photo.value.has_value() && photo.value->dimensions == ImageDimensions{1U, 1U} &&
              photo.value->stream == nullptr && photo.status() == base::PendingStatus::ready &&
              vector.value.has_value() && vector.value->document != nullptr &&
              vector.value->dimensions == ImageDimensions{200U, 100U},
          "image adoption did not publish usable content and natural dimensions together");
    check(failures == 1U && !broken.value.has_value() &&
              broken.status() == base::PendingStatus::failed &&
              removed.status() == base::PendingStatus::failed,
          "a failed or retired image remained pending or adopted after cancellation");
    static_cast<void>(textures.sync(runtime, &registry));
    check(textures.descriptors().size() == 1U &&
              textures.descriptors().front().logical_id == "photo" &&
              textures.descriptors().front().encoding == resource::ImageEncoding::rgba8,
          "failed/vector bindings exposed host pixels or unprepared bytes reached residency");
    ui::RenderSubmission submission;
    ui::SubmissionBatch batch;
    batch.texture = "fixture/photo";
    submission.batches.push_back(std::move(batch));
    textures.plan(submission, true);
    check(textures.uploads().size() == 1U &&
              std::ranges::equal(textures.uploads().front().bytes,
                                std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "static adoption uploaded the shadowed runtime canvas instead of prepared pixels");
    textures.commit();

    const auto read = [&registry](const std::string_view id) {
        const auto image = registry.lookup(id);
        return base::ReadinessRead{image.readiness, image.status()};
    };
    auto observed = read("dynamic");
    const auto unrelated = read("unrelated");
    runtime.publish("dynamic", {2U, 1U}, resource::TextureSampling::linear,
                    std::vector<std::uint8_t>(8U, 255U));
    registry.sync_metadata();
    check(!observed.current() && unrelated.current() &&
              registry.lookup("dynamic").value->dimensions == ImageDimensions{2U, 1U},
          "runtime image appearance did not invalidate only its own metadata readers");
    observed = read("dynamic");
    runtime.publish("dynamic", {2U, 1U}, resource::TextureSampling::linear,
                    std::vector<std::uint8_t>(8U, 0U));
    registry.sync_metadata();
    check(observed.current(), "a pixel-only update invalidated natural-size consumers");
    runtime.publish("dynamic", {1U, 2U}, resource::TextureSampling::linear,
                    std::vector<std::uint8_t>(8U, 255U));
    registry.sync_metadata();
    check(!observed.current(), "dimension replacement retained a stale metadata observation");
    observed = read("dynamic");
    runtime.release("dynamic");
    registry.sync_metadata();
    check(!observed.current() && !registry.lookup("dynamic").value.has_value(),
          "runtime release retained its previous natural dimensions");
    observed = read("dynamic");
    runtime.publish("dynamic", {1U, 1U}, resource::TextureSampling::linear, {0U, 0U, 0U, 255U});
    registry.sync_metadata();
    check(!observed.current() && unrelated.current(),
          "a released image could not invalidate readers when published again");
}

/** Playback advances from the frame phase that owns the clock, never from paint. */
void test_image_paint_preserves_playback() {
    using namespace strata;
    base::TaskOwner tasks(base::TaskExecution::deterministic);
    auto images = std::make_shared<const resource::SvgImageRegistry>(
        tasks,
        std::vector<resource::ImageSource>{
            {"animation", "fixture/animation", "animation.gif",
             resource::TextureSampling::linear, tiny_animated_gif(8U)}},
        &ui::validate_svg_image_geometry, nullptr, nullptr);
    const auto ready = images->lookup("animation");
    check(ready.value.has_value() && ready.value->stream != nullptr,
          "image paint fixture did not prepare its animation");
    resource::ImageStream& stream = *ready.value->stream;
    runtime::ApplicationContext application("image-paint", runtime::ApplicationBundle::create());
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application.compile_and_activate(compiler::ModuleSource{"image-paint.strata", R"(
overlay Picture {
  root Image(key: "animation", image: "animation", layout: { width: 32, height: 32 })
}
)"}, no_imports, 0U).activated(), "image paint fixture did not activate");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 32;
    environment.framebuffer_height = 32;
    environment.logical_width = 32.0;
    environment.logical_height = 32.0;
    ui::Surface surface("image-paint", application, runtime::LayerRole::overlay, "Picture",
                        environment, {}, ui::WidgetRegistry{}, ui::BehaviorRegistry{}, nullptr,
                        ui::Theme{}, {}, images);
    static_cast<void>(surface.frame(0));
    const auto revision = stream.revision();
    check(std::ranges::equal(stream.pixels(), std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "initial image publication did not display its first canvas");
    surface.input().publish_frame_time(100'000'000);
    check(stream.revision() == revision &&
              std::ranges::equal(stream.pixels(), std::array<std::uint8_t, 4U>{0U, 0U, 0U, 255U}),
          "publishing a frame time alone advanced the image's playback");
    static_cast<void>(surface.frame(100'000'000));
    check(std::ranges::equal(stream.pixels(),
                            std::array<std::uint8_t, 4U>{255U, 255U, 255U, 255U}),
          "image playback did not start at its first visible frame");
}

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
        test_streamed_frames(root, fixtures);
        test_resumable_animation_bounds();
        test_image_stream_playback(root);
        test_image_preparation();
        test_image_paint_preserves_playback();
    } catch (const std::exception& error) {
        std::cerr << "strata_image_decode_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "strata_image_decode_tests: OK\n";
    return 0;
}
