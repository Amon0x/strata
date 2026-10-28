#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "resource/image.hpp"
#include "tests/png_fixture.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#endif

namespace {

using strata::resource::decode_png_rgba8;
using strata::resource::ImageDimensions;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void u32(std::vector<std::uint8_t>& out, const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8)
        out.push_back(static_cast<std::uint8_t>(value >> shift));
}

std::uint32_t crc(const std::span<const std::uint8_t> bytes) {
    std::uint32_t value = 0xFFFFFFFFU;
    for (const std::uint8_t byte : bytes) {
        value ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1U) ^ (0xEDB88320U & (0U - (value & 1U)));
    }
    return ~value;
}

struct Chunk final {
    std::string type;
    std::vector<std::uint8_t> data;
    bool damage_crc = false;
};

/** A PNG whose image data is the given filtered scanlines in one stored deflate block. */
std::vector<std::uint8_t> png(const std::uint32_t width, const std::uint32_t height,
                              const std::uint8_t depth, const std::uint8_t color,
                              const bool interlaced, const std::vector<std::uint8_t>& scanlines,
                              const std::vector<Chunk>& before_data = {},
                              const bool damage_header = false, const bool damage_adler = false) {
    std::vector<std::uint8_t> result{0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    const auto chunk = [&result](const Chunk& value) {
        u32(result, static_cast<std::uint32_t>(value.data.size()));
        std::vector<std::uint8_t> body(value.type.begin(), value.type.end());
        body.insert(body.end(), value.data.begin(), value.data.end());
        result.insert(result.end(), body.begin(), body.end());
        u32(result, crc(body) ^ (value.damage_crc ? 1U : 0U));
    };
    std::vector<std::uint8_t> header;
    u32(header, width);
    u32(header, height);
    header.insert(header.end(), {depth, color, 0U, 0U, static_cast<std::uint8_t>(interlaced)});
    chunk(Chunk{"IHDR", header, damage_header});
    for (const Chunk& extra : before_data)
        chunk(extra);
    std::vector<std::uint8_t> zlib{0x78, 0x01, 0x01};
    const auto length = static_cast<std::uint16_t>(scanlines.size());
    zlib.insert(zlib.end(),
                {static_cast<std::uint8_t>(length), static_cast<std::uint8_t>(length >> 8U),
                 static_cast<std::uint8_t>(~length),
                 static_cast<std::uint8_t>(static_cast<std::uint16_t>(~length) >> 8U)});
    zlib.insert(zlib.end(), scanlines.begin(), scanlines.end());
    std::uint32_t a = 1U;
    std::uint32_t b = 0U;
    for (const std::uint8_t byte : scanlines) {
        a = (a + byte) % 65521U;
        b = (b + a) % 65521U;
    }
    u32(zlib, (b << 16U | a) ^ (damage_adler ? 1U : 0U));
    chunk(Chunk{"IDAT", zlib});
    chunk(Chunk{"IEND", {}});
    return result;
}

bool rejects(const std::vector<std::uint8_t>& bytes, const ImageDimensions dimensions) {
    try {
        static_cast<void>(decode_png_rgba8(bytes, dimensions));
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

void test_sixteen_bit_samples_keep_their_high_byte() {
    // As every backend sampled them before images were decoded in the core: gAMA is ignored.
    const std::array<std::uint16_t, 4U> sample{0x3480U, 0x80FFU, 0xFF7FU, 0xFFFFU};
    for (const std::optional<std::uint32_t> gamma : {std::optional<std::uint32_t>{},
                                                     std::optional<std::uint32_t>{100000U}}) {
        const std::vector<std::uint8_t> pixels =
            decode_png_rgba8(strata::test::rgba16_png(1U, 1U, sample, gamma), {1U, 1U});
        check(pixels == std::vector<std::uint8_t>({0x34U, 0x80U, 0xFFU, 0xFFU}),
              "a 16-bit PNG did not decode to the high byte of each sample");
    }
}

void test_palette_and_low_depth_gray() {
    // 2-bit palette, four pixels 0..3; tRNS covers only the first two entries.
    const std::vector<std::uint8_t> palette_png = png(
        4U, 1U, 2U, 3U, false, {0U, 0b00011011U},
        {Chunk{"PLTE", {10U, 20U, 30U, 40U, 50U, 60U, 70U, 80U, 90U, 100U, 110U, 120U}},
         Chunk{"tRNS", {0U, 128U}}});
    check(decode_png_rgba8(palette_png, {4U, 1U}) ==
              std::vector<std::uint8_t>({10U, 20U, 30U, 0U, 40U, 50U, 60U, 128U, 70U, 80U, 90U,
                                         255U, 100U, 110U, 120U, 255U}),
          "a palette image did not take its colours and tRNS alpha from the palette");
    const std::vector<std::uint8_t> out_of_range =
        png(4U, 1U, 2U, 3U, false, {0U, 0b00011011U}, {Chunk{"PLTE", {1U, 2U, 3U}}});
    check(rejects(out_of_range, {4U, 1U}), "a pixel past the palette was accepted");

    // 1-bit and 4-bit gray expand by bit replication; a tRNS gray value is transparent.
    check(decode_png_rgba8(png(2U, 1U, 1U, 0U, false, {0U, 0b01000000U}), {2U, 1U}) ==
              std::vector<std::uint8_t>({0U, 0U, 0U, 255U, 255U, 255U, 255U, 255U}),
          "1-bit gray did not expand to black and white");
    check(decode_png_rgba8(png(2U, 1U, 4U, 0U, false, {0U, 0x5AU}, {Chunk{"tRNS", {0U, 0x0AU}}}),
                           {2U, 1U}) ==
              std::vector<std::uint8_t>({0x55U, 0x55U, 0x55U, 255U, 0xAAU, 0xAAU, 0xAAU, 0U}),
          "4-bit gray did not replicate its bits or honour its tRNS value");
}

void test_filters_and_interlacing() {
    // One 8-bit RGB row per filter type, each reconstructing the same pixels 10,20,30 / 40,50,60.
    const std::vector<std::uint8_t> expected_row{10U, 20U, 30U, 255U, 40U, 50U, 60U, 255U};
    const std::vector<std::uint8_t> filtered{
        0U, 10U, 20U, 30U, 40U, 50U, 60U,   // none
        1U, 10U, 20U, 30U, 30U, 30U, 30U,   // sub
        2U, 0U,  0U,  0U,  0U,  0U,  0U,    // up
        3U, 5U,  10U, 15U, 15U, 15U, 15U,   // average: (left + up) / 2
        4U, 0U,  0U,  0U,  0U,  0U,  0U,    // paeth: up wins
    };
    std::vector<std::uint8_t> expected;
    for (int row = 0; row < 5; ++row)
        expected.insert(expected.end(), expected_row.begin(), expected_row.end());
    check(decode_png_rgba8(png(2U, 5U, 8U, 2U, false, filtered), {2U, 5U}) == expected,
          "a scanline filter did not reconstruct its row");

    // A 3x3 interlaced gray image: Adam7 passes 1, 4, 5, 6 and 7 carry pixels.
    // Pixel (x, y) has value 10 * (y * 3 + x).
    const std::vector<std::uint8_t> passes{
        0U, 0U,                // pass 1: (0,0)
        0U, 20U,               // pass 4: (2,0)
        0U, 60U, 80U,          // pass 5: (0,2) (2,2)
        0U, 10U, 0U, 70U,      // pass 6: (1,0) / (1,2)
        0U, 30U, 40U, 50U,     // pass 7: row 1
    };
    std::vector<std::uint8_t> gray;
    for (std::uint8_t value = 0U; value < 90U; value += 10U)
        gray.insert(gray.end(), {value, value, value, 255U});
    check(decode_png_rgba8(png(3U, 3U, 8U, 0U, true, passes), {3U, 3U}) == gray,
          "an interlaced image did not place its passes");
}

void test_malformed_streams_are_rejected() {
    const std::vector<std::uint8_t> gray{0U, 7U};
    check(!rejects(png(1U, 1U, 8U, 0U, false, gray), {1U, 1U}), "a valid 1x1 PNG was rejected");
    check(rejects(png(1U, 1U, 8U, 0U, false, gray), {2U, 1U}),
          "a PNG whose dimensions differ from its declaration was accepted");
    check(rejects(png(1U, 1U, 8U, 0U, false, gray, {}, true), {1U, 1U}),
          "a damaged IHDR was accepted");
    check(rejects(png(1U, 1U, 8U, 0U, false, gray, {}, false, true), {1U, 1U}),
          "image data failing its zlib checksum was accepted");
    check(rejects(png(1U, 1U, 8U, 0U, false, {0U}), {1U, 1U}),
          "image data shorter than its scanlines was accepted");
    check(rejects(png(1U, 1U, 8U, 0U, false, {5U, 7U}), {1U, 1U}),
          "an unknown scanline filter was accepted");
    check(rejects(png(1U, 1U, 8U, 0U, false, gray, {Chunk{"ZZZZ", {}}}), {1U, 1U}),
          "an unknown critical chunk was accepted");
    std::vector<std::uint8_t> truncated = png(1U, 1U, 8U, 0U, false, gray);
    truncated.resize(truncated.size() - 20U);
    check(rejects(truncated, {1U, 1U}), "a truncated PNG was accepted");
    // A damaged ancillary chunk is ignored, as libpng ignores it: the tRNS does not apply.
    check(decode_png_rgba8(png(1U, 1U, 8U, 0U, false, gray, {Chunk{"tRNS", {0U, 7U}, true}}),
                           {1U, 1U})[3U] == 255U,
          "a damaged ancillary chunk was applied");
}

#if defined(_WIN32)
using Microsoft::WRL::ComPtr;

void require(const HRESULT result, const std::string_view operation) {
    if (FAILED(result))
        throw std::runtime_error(std::string(operation) + " failed");
}

/** Pixels in a WIC format, `stride` bytes per row. */
struct Source final {
    WICPixelFormatGUID format{};
    std::uint32_t stride = 0U;
    std::vector<std::uint8_t> bytes;
};

/** Encodes `pixels` as a PNG in `format` through WIC's own encoder. */
std::vector<std::uint8_t> wic_encode(IWICImagingFactory& factory, const std::uint32_t width,
                                     const std::uint32_t height, const Source& pixels,
                                     WICPixelFormatGUID format, const bool interlaced) {
    ComPtr<IWICBitmap> source;
    require(factory.CreateBitmapFromMemory(width, height, pixels.format, pixels.stride,
                                           static_cast<UINT>(pixels.bytes.size()),
                                           const_cast<BYTE*>(pixels.bytes.data()), &source),
            "WIC source bitmap");
    ComPtr<IStream> stream;
    require(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "memory stream");
    ComPtr<IWICBitmapEncoder> encoder;
    require(factory.CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "PNG encoder");
    require(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "PNG encoder init");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> options;
    require(encoder->CreateNewFrame(&frame, &options), "PNG frame");
    PROPBAG2 option{};
    option.pstrName = const_cast<LPOLESTR>(L"InterlaceOption");
    VARIANT value;
    VariantInit(&value);
    value.vt = VT_BOOL;
    value.boolVal = interlaced ? VARIANT_TRUE : VARIANT_FALSE;
    require(options->Write(1U, &option, &value), "PNG interlace option");
    require(frame->Initialize(options.Get()), "PNG frame init");
    require(frame->SetSize(width, height), "PNG frame size");
    WICPixelFormatGUID chosen = format;
    require(frame->SetPixelFormat(&chosen), "PNG pixel format");
    ComPtr<IWICPalette> palette;
    ComPtr<IWICFormatConverter> converter;
    require(factory.CreateFormatConverter(&converter), "format converter");
    ComPtr<IWICComponentInfo> info;
    require(factory.CreateComponentInfo(chosen, &info), "pixel format info");
    ComPtr<IWICPixelFormatInfo> format_info;
    require(info.As(&format_info), "pixel format info");
    UINT bits = 0U;
    require(format_info->GetBitsPerPixel(&bits), "pixel format depth");
    const bool indexed = chosen == GUID_WICPixelFormat1bppIndexed ||
                         chosen == GUID_WICPixelFormat2bppIndexed ||
                         chosen == GUID_WICPixelFormat4bppIndexed ||
                         chosen == GUID_WICPixelFormat8bppIndexed;
    if (indexed) {
        require(factory.CreatePalette(&palette), "palette");
        require(palette->InitializeFromBitmap(source.Get(), 1U << bits, TRUE), "palette colours");
        require(frame->SetPalette(palette.Get()), "frame palette");
    }
    require(converter->Initialize(source.Get(), chosen, WICBitmapDitherTypeNone, palette.Get(),
                                  0.0, indexed ? WICBitmapPaletteTypeCustom
                                               : WICBitmapPaletteTypeMedianCut),
            "conversion to the encoded format");
    require(frame->WriteSource(converter.Get(), nullptr), "PNG pixels");
    require(frame->Commit(), "PNG frame commit");
    require(encoder->Commit(), "PNG commit");
    HGLOBAL memory = nullptr;
    require(GetHGlobalFromStream(stream.Get(), &memory), "encoded bytes");
    STATSTG stat{};
    require(stream->Stat(&stat, STATFLAG_NONAME), "encoded size");
    const auto* const bytes = static_cast<const std::uint8_t*>(GlobalLock(memory));
    std::vector<std::uint8_t> result(bytes, bytes + stat.cbSize.QuadPart);
    GlobalUnlock(memory);
    return result;
}

/** What D3D11 sampled before images were decoded in the core: WIC's 32bppRGBA conversion. */
std::vector<std::uint8_t> wic_decode(IWICImagingFactory& factory,
                                     const std::vector<std::uint8_t>& bytes) {
    ComPtr<IWICStream> stream;
    require(factory.CreateStream(&stream), "WIC stream");
    require(stream->InitializeFromMemory(const_cast<BYTE*>(bytes.data()),
                                         static_cast<DWORD>(bytes.size())),
            "WIC stream init");
    ComPtr<IWICBitmapDecoder> decoder;
    require(factory.CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                            &decoder),
            "PNG decoder");
    ComPtr<IWICBitmapFrameDecode> frame;
    require(decoder->GetFrame(0U, &frame), "PNG frame decode");
    UINT width = 0U;
    UINT height = 0U;
    require(frame->GetSize(&width, &height), "PNG size");
    ComPtr<IWICFormatConverter> converter;
    require(factory.CreateFormatConverter(&converter), "format converter");
    require(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
                                  WICBitmapDitherTypeNone, nullptr, 0.0,
                                  WICBitmapPaletteTypeCustom),
            "RGBA conversion");
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) * height * 4U);
    require(converter->CopyPixels(nullptr, width * 4U, static_cast<UINT>(pixels.size()),
                                  pixels.data()),
            "pixel copy");
    return pixels;
}

void test_decoding_matches_wic() {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                             IID_PPV_ARGS(&factory)),
            "WIC factory");
    // Odd dimensions put partial blocks in every Adam7 pass; gradients exercise every filter and
    // WIC's compressed (dynamic Huffman) streams.
    constexpr std::uint32_t width = 37U;
    constexpr std::uint32_t height = 23U;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4U);
    for (std::uint32_t y = 0U; y < height; ++y) {
        for (std::uint32_t x = 0U; x < width; ++x) {
            std::uint8_t* const pixel = rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4U;
            pixel[0U] = static_cast<std::uint8_t>(x * 7U);
            pixel[1U] = static_cast<std::uint8_t>(y * 11U);
            pixel[2U] = static_cast<std::uint8_t>((x * y) ^ 0x5AU);
            pixel[3U] = static_cast<std::uint8_t>((x + y) % 5U == 0U ? 0U : 64U + x * 5U);
        }
    }
    const Source rgba8{GUID_WICPixelFormat32bppRGBA, width * 4U, rgba};
    // 16-bit images are encoded from 16-bit samples, so narrowing them is actually exercised.
    const auto wide = [&](const WICPixelFormatGUID format, const std::uint32_t channels) {
        Source result{format, width * channels * 2U, {}};
        for (std::uint32_t y = 0U; y < height; ++y) {
            for (std::uint32_t x = 0U; x < width * channels; ++x) {
                const auto sample = static_cast<std::uint16_t>(x * 1777U + y * 40503U + 0x80U);
                result.bytes.push_back(static_cast<std::uint8_t>(sample));
                result.bytes.push_back(static_cast<std::uint8_t>(sample >> 8U));
            }
        }
        return result;
    };
    const Source rgba16 = wide(GUID_WICPixelFormat64bppRGBA, 4U);
    const Source rgb16 = wide(GUID_WICPixelFormat48bppRGB, 3U);
    const Source gray16 = wide(GUID_WICPixelFormat16bppGray, 1U);
    struct Case final {
        WICPixelFormatGUID format;
        const Source* source;
        std::string_view name;
    };
    const std::array<Case, 12U> cases{{
        {GUID_WICPixelFormat32bppRGBA, &rgba8, "RGBA8"},
        {GUID_WICPixelFormat64bppRGBA, &rgba16, "RGBA16"},
        {GUID_WICPixelFormat24bppBGR, &rgba8, "RGB8"},
        {GUID_WICPixelFormat48bppRGB, &rgb16, "RGB16"},
        {GUID_WICPixelFormat8bppGray, &rgba8, "gray8"},
        {GUID_WICPixelFormat16bppGray, &gray16, "gray16"},
        {GUID_WICPixelFormat4bppGray, &rgba8, "gray4"},
        {GUID_WICPixelFormat2bppGray, &rgba8, "gray2"},
        {GUID_WICPixelFormatBlackWhite, &rgba8, "gray1"},
        {GUID_WICPixelFormat8bppIndexed, &rgba8, "indexed8"},
        {GUID_WICPixelFormat4bppIndexed, &rgba8, "indexed4"},
        {GUID_WICPixelFormat1bppIndexed, &rgba8, "indexed1"},
    }};
    for (const Case& entry : cases) {
        for (const bool interlaced : {false, true}) {
            const std::vector<std::uint8_t> encoded =
                wic_encode(*factory.Get(), width, height, *entry.source, entry.format, interlaced);
            const std::vector<std::uint8_t> core = decode_png_rgba8(encoded, {width, height});
            const std::vector<std::uint8_t> wic = wic_decode(*factory.Get(), encoded);
            // WIC narrows 16-bit gray through a floating-point gamma round trip that lands one
            // step either side of the stored high byte; every other format matches it exactly.
            const int tolerance = entry.name == "gray16" ? 1 : 0;
            bool agrees = core.size() == wic.size();
            for (std::size_t index = 0U; agrees && index < core.size(); ++index)
                agrees = std::abs(int{core[index]} - int{wic[index]}) <= tolerance;
            check(agrees, "the core PNG decoder disagrees with WIC for " +
                              std::string(entry.name) + (interlaced ? " (interlaced)" : ""));
        }
    }
    factory.Reset();
    if (SUCCEEDED(initialized))
        CoUninitialize();
}
#endif

} // namespace

int strata_test_png_decode() {
    try {
        test_sixteen_bit_samples_keep_their_high_byte();
        test_palette_and_low_depth_gray();
        test_filters_and_interlacing();
        test_malformed_streams_are_rejected();
#if defined(_WIN32)
        test_decoding_matches_wic();
#endif
    } catch (const std::exception& error) {
        std::cerr << "strata_png_decode_tests: " << error.what() << '\n';
        return 1;
    }
    std::cout << "strata_png_decode_tests: OK\n";
    return 0;
}
