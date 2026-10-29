// VP8 key frames (RFC 6386), the lossy payload of a WebP 'VP8 ' chunk. WebP stores exactly one
// key frame per chunk, so inter prediction, golden frames and probability persistence are absent.
//
// Every arithmetic step follows libwebp, which every browser and Pillow decode with: the integer
// transforms, the loop filters, the prediction edges and, past reconstruction, the "fancy" 4:2:0
// upsampler and the 14-bit YUV to RGB conversion. A decoded frame is therefore bit-identical to
// libwebp's RGBA output, not merely close to it.

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "resource/image_codecs.hpp"

namespace strata::resource::codec {
namespace {

[[noreturn]] void fail(const std::string_view reason) {
    throw std::invalid_argument("WebP decode: " + std::string(reason));
}

// Intra prediction modes in libwebp's numbering. The 16x16 and chroma modes reuse the first four,
// which is also how a 16x16 macroblock's mode becomes its neighbours' 4x4 context.
constexpr std::uint8_t mode_dc = 0U;
constexpr std::uint8_t mode_tm = 1U;
constexpr std::uint8_t mode_ve = 2U;
constexpr std::uint8_t mode_he = 3U;
constexpr std::uint8_t mode_rd = 4U;
constexpr std::uint8_t mode_vr = 5U;
constexpr std::uint8_t mode_ld = 6U;
constexpr std::uint8_t mode_vl = 7U;
constexpr std::uint8_t mode_hd = 8U;
constexpr std::uint8_t mode_hu = 9U;

// Token probabilities are indexed [plane type][band][context][node]. Plane types: 0 is the AC of a
// 16x16-predicted luma block, 1 the second-order (Y2) DC block, 2 chroma, 3 luma with its own DC.
constexpr std::size_t plane_types = 4U;
constexpr std::size_t bands = 8U;
constexpr std::size_t contexts = 3U;
constexpr std::size_t nodes = 11U;
using Probabilities = std::uint8_t[plane_types][bands][contexts][nodes];

// RFC 6386 section 13.5: the probabilities every key frame starts from.
constexpr Probabilities default_probabilities = {
    {{{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{253, 136, 254, 255, 228, 219, 128, 128, 128, 128, 128},
      {189, 129, 242, 255, 227, 213, 255, 219, 128, 128, 128},
      {106, 126, 227, 252, 214, 209, 255, 255, 128, 128, 128}},
     {{1, 98, 248, 255, 236, 226, 255, 255, 128, 128, 128},
      {181, 133, 238, 254, 221, 234, 255, 154, 128, 128, 128},
      {78, 134, 202, 247, 198, 180, 255, 219, 128, 128, 128}},
     {{1, 185, 249, 255, 243, 255, 128, 128, 128, 128, 128},
      {184, 150, 247, 255, 236, 224, 128, 128, 128, 128, 128},
      {77, 110, 216, 255, 236, 230, 128, 128, 128, 128, 128}},
     {{1, 101, 251, 255, 241, 255, 128, 128, 128, 128, 128},
      {170, 139, 241, 252, 236, 209, 255, 255, 128, 128, 128},
      {37, 116, 196, 243, 228, 255, 255, 255, 128, 128, 128}},
     {{1, 204, 254, 255, 245, 255, 128, 128, 128, 128, 128},
      {207, 160, 250, 255, 238, 128, 128, 128, 128, 128, 128},
      {102, 103, 231, 255, 211, 171, 128, 128, 128, 128, 128}},
     {{1, 152, 252, 255, 240, 255, 128, 128, 128, 128, 128},
      {177, 135, 243, 255, 234, 225, 128, 128, 128, 128, 128},
      {80, 129, 211, 255, 194, 224, 128, 128, 128, 128, 128}},
     {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {246, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {255, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}},
    {{{198, 35, 237, 223, 193, 187, 162, 160, 145, 155, 62},
      {131, 45, 198, 221, 172, 176, 220, 157, 252, 221, 1},
      {68, 47, 146, 208, 149, 167, 221, 162, 255, 223, 128}},
     {{1, 149, 241, 255, 221, 224, 255, 255, 128, 128, 128},
      {184, 141, 234, 253, 222, 220, 255, 199, 128, 128, 128},
      {81, 99, 181, 242, 176, 190, 249, 202, 255, 255, 128}},
     {{1, 129, 232, 253, 214, 197, 242, 196, 255, 255, 128},
      {99, 121, 210, 250, 201, 198, 255, 202, 128, 128, 128},
      {23, 91, 163, 242, 170, 187, 247, 210, 255, 255, 128}},
     {{1, 200, 246, 255, 234, 255, 128, 128, 128, 128, 128},
      {109, 178, 241, 255, 231, 245, 255, 255, 128, 128, 128},
      {44, 130, 201, 253, 205, 192, 255, 255, 128, 128, 128}},
     {{1, 132, 239, 251, 219, 209, 255, 165, 128, 128, 128},
      {94, 136, 225, 251, 218, 190, 255, 255, 128, 128, 128},
      {22, 100, 174, 245, 186, 161, 255, 199, 128, 128, 128}},
     {{1, 182, 249, 255, 232, 235, 128, 128, 128, 128, 128},
      {124, 143, 241, 255, 227, 234, 128, 128, 128, 128, 128},
      {35, 77, 181, 251, 193, 211, 255, 205, 128, 128, 128}},
     {{1, 157, 247, 255, 236, 231, 255, 255, 128, 128, 128},
      {121, 141, 235, 255, 225, 227, 255, 255, 128, 128, 128},
      {45, 99, 188, 251, 195, 217, 255, 224, 128, 128, 128}},
     {{1, 1, 251, 255, 213, 255, 128, 128, 128, 128, 128},
      {203, 1, 248, 255, 255, 128, 128, 128, 128, 128, 128},
      {137, 1, 177, 255, 224, 255, 128, 128, 128, 128, 128}}},
    {{{253, 9, 248, 251, 207, 208, 255, 192, 128, 128, 128},
      {175, 13, 224, 243, 193, 185, 249, 198, 255, 255, 128},
      {73, 17, 171, 221, 161, 179, 236, 167, 255, 234, 128}},
     {{1, 95, 247, 253, 212, 183, 255, 255, 128, 128, 128},
      {239, 90, 244, 250, 211, 209, 255, 255, 128, 128, 128},
      {155, 77, 195, 248, 188, 195, 255, 255, 128, 128, 128}},
     {{1, 24, 239, 251, 218, 219, 255, 205, 128, 128, 128},
      {201, 51, 219, 255, 196, 186, 128, 128, 128, 128, 128},
      {69, 46, 190, 239, 201, 218, 255, 228, 128, 128, 128}},
     {{1, 191, 251, 255, 255, 128, 128, 128, 128, 128, 128},
      {223, 165, 249, 255, 213, 255, 128, 128, 128, 128, 128},
      {141, 124, 248, 255, 255, 128, 128, 128, 128, 128, 128}},
     {{1, 16, 248, 255, 255, 128, 128, 128, 128, 128, 128},
      {190, 36, 230, 255, 236, 255, 128, 128, 128, 128, 128},
      {149, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{1, 226, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {247, 192, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {240, 128, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{1, 134, 252, 255, 255, 128, 128, 128, 128, 128, 128},
      {213, 62, 250, 255, 255, 128, 128, 128, 128, 128, 128},
      {55, 93, 255, 128, 128, 128, 128, 128, 128, 128, 128}},
     {{128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128},
      {128, 128, 128, 128, 128, 128, 128, 128, 128, 128, 128}}},
    {{{202, 24, 213, 235, 186, 191, 220, 160, 240, 175, 255},
      {126, 38, 182, 232, 169, 184, 228, 174, 255, 187, 128},
      {61, 46, 138, 219, 151, 178, 240, 170, 255, 216, 128}},
     {{1, 112, 230, 250, 199, 191, 247, 159, 255, 255, 128},
      {166, 109, 228, 252, 211, 215, 255, 174, 128, 128, 128},
      {39, 77, 162, 232, 172, 180, 245, 178, 255, 255, 128}},
     {{1, 52, 220, 246, 198, 199, 249, 220, 255, 255, 128},
      {124, 74, 191, 243, 183, 193, 250, 221, 255, 255, 128},
      {24, 71, 130, 219, 154, 170, 243, 182, 255, 255, 128}},
     {{1, 182, 225, 249, 219, 240, 255, 224, 128, 128, 128},
      {149, 150, 226, 252, 216, 205, 255, 171, 128, 128, 128},
      {28, 108, 170, 242, 183, 194, 254, 223, 255, 255, 128}},
     {{1, 81, 230, 252, 204, 203, 255, 192, 128, 128, 128},
      {123, 102, 209, 247, 188, 196, 255, 233, 128, 128, 128},
      {20, 95, 153, 243, 164, 173, 255, 203, 128, 128, 128}},
     {{1, 222, 248, 255, 216, 213, 128, 128, 128, 128, 128},
      {168, 175, 246, 252, 235, 205, 255, 255, 128, 128, 128},
      {47, 116, 215, 255, 211, 212, 255, 255, 128, 128, 128}},
     {{1, 121, 236, 253, 212, 214, 255, 255, 128, 128, 128},
      {141, 84, 213, 252, 201, 202, 255, 219, 128, 128, 128},
      {42, 80, 160, 240, 162, 185, 255, 205, 128, 128, 128}},
     {{1, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {244, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128},
      {238, 1, 255, 128, 128, 128, 128, 128, 128, 128, 128}}},
};

// RFC 6386 section 13.4: the probability that a frame header replaces each token probability.
constexpr Probabilities update_probabilities = {
    {{{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{176, 246, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {223, 241, 252, 255, 255, 255, 255, 255, 255, 255, 255},
      {249, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 244, 252, 255, 255, 255, 255, 255, 255, 255, 255},
      {234, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 246, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {239, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {251, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {251, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 253, 255, 254, 255, 255, 255, 255, 255, 255},
      {250, 255, 254, 255, 254, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
    {{{217, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {225, 252, 241, 253, 255, 255, 254, 255, 255, 255, 255},
      {234, 250, 241, 250, 253, 255, 253, 254, 255, 255, 255}},
     {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {223, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {238, 253, 254, 254, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 248, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {249, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {247, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {252, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
    {{{186, 251, 250, 255, 255, 255, 255, 255, 255, 255, 255},
      {234, 251, 244, 254, 255, 255, 255, 255, 255, 255, 255},
      {251, 251, 243, 253, 254, 255, 254, 255, 255, 255, 255}},
     {{255, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {236, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {251, 253, 253, 254, 254, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 254, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
    {{{248, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {250, 254, 252, 254, 255, 255, 255, 255, 255, 255, 255},
      {248, 254, 249, 253, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {246, 253, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {252, 254, 251, 254, 254, 255, 255, 255, 255, 255, 255}},
     {{255, 254, 252, 255, 255, 255, 255, 255, 255, 255, 255},
      {248, 254, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 255, 254, 254, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {245, 251, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {253, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 251, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {252, 253, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 254, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 252, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {249, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 254, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 253, 255, 255, 255, 255, 255, 255, 255, 255},
      {250, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}},
     {{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {254, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255},
      {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255}}},
};

// RFC 6386 section 11.5: a key frame's 4x4 mode probabilities, given the modes above and left.
constexpr std::uint8_t subblock_mode_probabilities[10][10][9] = {
    {{231, 120, 48, 89, 115, 113, 120, 152, 112},
     {152, 179, 64, 126, 170, 118, 46, 70, 95},
     {175, 69, 143, 80, 85, 82, 72, 155, 103},
     {56, 58, 10, 171, 218, 189, 17, 13, 152},
     {114, 26, 17, 163, 44, 195, 21, 10, 173},
     {121, 24, 80, 195, 26, 62, 44, 64, 85},
     {144, 71, 10, 38, 171, 213, 144, 34, 26},
     {170, 46, 55, 19, 136, 160, 33, 206, 71},
     {63, 20, 8, 114, 114, 208, 12, 9, 226},
     {81, 40, 11, 96, 182, 84, 29, 16, 36}},
    {{134, 183, 89, 137, 98, 101, 106, 165, 148},
     {72, 187, 100, 130, 157, 111, 32, 75, 80},
     {66, 102, 167, 99, 74, 62, 40, 234, 128},
     {41, 53, 9, 178, 241, 141, 26, 8, 107},
     {74, 43, 26, 146, 73, 166, 49, 23, 157},
     {65, 38, 105, 160, 51, 52, 31, 115, 128},
     {104, 79, 12, 27, 217, 255, 87, 17, 7},
     {87, 68, 71, 44, 114, 51, 15, 186, 23},
     {47, 41, 14, 110, 182, 183, 21, 17, 194},
     {66, 45, 25, 102, 197, 189, 23, 18, 22}},
    {{88, 88, 147, 150, 42, 46, 45, 196, 205},
     {43, 97, 183, 117, 85, 38, 35, 179, 61},
     {39, 53, 200, 87, 26, 21, 43, 232, 171},
     {56, 34, 51, 104, 114, 102, 29, 93, 77},
     {39, 28, 85, 171, 58, 165, 90, 98, 64},
     {34, 22, 116, 206, 23, 34, 43, 166, 73},
     {107, 54, 32, 26, 51, 1, 81, 43, 31},
     {68, 25, 106, 22, 64, 171, 36, 225, 114},
     {34, 19, 21, 102, 132, 188, 16, 76, 124},
     {62, 18, 78, 95, 85, 57, 50, 48, 51}},
    {{193, 101, 35, 159, 215, 111, 89, 46, 111},
     {60, 148, 31, 172, 219, 228, 21, 18, 111},
     {112, 113, 77, 85, 179, 255, 38, 120, 114},
     {40, 42, 1, 196, 245, 209, 10, 25, 109},
     {88, 43, 29, 140, 166, 213, 37, 43, 154},
     {61, 63, 30, 155, 67, 45, 68, 1, 209},
     {100, 80, 8, 43, 154, 1, 51, 26, 71},
     {142, 78, 78, 16, 255, 128, 34, 197, 171},
     {41, 40, 5, 102, 211, 183, 4, 1, 221},
     {51, 50, 17, 168, 209, 192, 23, 25, 82}},
    {{138, 31, 36, 171, 27, 166, 38, 44, 229},
     {67, 87, 58, 169, 82, 115, 26, 59, 179},
     {63, 59, 90, 180, 59, 166, 93, 73, 154},
     {40, 40, 21, 116, 143, 209, 34, 39, 175},
     {47, 15, 16, 183, 34, 223, 49, 45, 183},
     {46, 17, 33, 183, 6, 98, 15, 32, 183},
     {57, 46, 22, 24, 128, 1, 54, 17, 37},
     {65, 32, 73, 115, 28, 128, 23, 128, 205},
     {40, 3, 9, 115, 51, 192, 18, 6, 223},
     {87, 37, 9, 115, 59, 77, 64, 21, 47}},
    {{104, 55, 44, 218, 9, 54, 53, 130, 226},
     {64, 90, 70, 205, 40, 41, 23, 26, 57},
     {54, 57, 112, 184, 5, 41, 38, 166, 213},
     {30, 34, 26, 133, 152, 116, 10, 32, 134},
     {39, 19, 53, 221, 26, 114, 32, 73, 255},
     {31, 9, 65, 234, 2, 15, 1, 118, 73},
     {75, 32, 12, 51, 192, 255, 160, 43, 51},
     {88, 31, 35, 67, 102, 85, 55, 186, 85},
     {56, 21, 23, 111, 59, 205, 45, 37, 192},
     {55, 38, 70, 124, 73, 102, 1, 34, 98}},
    {{125, 98, 42, 88, 104, 85, 117, 175, 82},
     {95, 84, 53, 89, 128, 100, 113, 101, 45},
     {75, 79, 123, 47, 51, 128, 81, 171, 1},
     {57, 17, 5, 71, 102, 57, 53, 41, 49},
     {38, 33, 13, 121, 57, 73, 26, 1, 85},
     {41, 10, 67, 138, 77, 110, 90, 47, 114},
     {115, 21, 2, 10, 102, 255, 166, 23, 6},
     {101, 29, 16, 10, 85, 128, 101, 196, 26},
     {57, 18, 10, 102, 102, 213, 34, 20, 43},
     {117, 20, 15, 36, 163, 128, 68, 1, 26}},
    {{102, 61, 71, 37, 34, 53, 31, 243, 192},
     {69, 60, 71, 38, 73, 119, 28, 222, 37},
     {68, 45, 128, 34, 1, 47, 11, 245, 171},
     {62, 17, 19, 70, 146, 85, 55, 62, 70},
     {37, 43, 37, 154, 100, 163, 85, 160, 1},
     {63, 9, 92, 136, 28, 64, 32, 201, 85},
     {75, 15, 9, 9, 64, 255, 184, 119, 16},
     {86, 6, 28, 5, 64, 255, 25, 248, 1},
     {56, 8, 17, 132, 137, 255, 55, 116, 128},
     {58, 15, 20, 82, 135, 57, 26, 121, 40}},
    {{164, 50, 31, 137, 154, 133, 25, 35, 218},
     {51, 103, 44, 131, 131, 123, 31, 6, 158},
     {86, 40, 64, 135, 148, 224, 45, 183, 128},
     {22, 26, 17, 131, 240, 154, 14, 1, 209},
     {45, 16, 21, 91, 64, 222, 7, 1, 197},
     {56, 21, 39, 155, 60, 138, 23, 102, 213},
     {83, 12, 13, 54, 192, 255, 68, 47, 28},
     {85, 26, 85, 85, 128, 128, 32, 146, 171},
     {18, 11, 7, 63, 144, 171, 4, 4, 246},
     {35, 27, 10, 146, 174, 171, 12, 26, 128}},
    {{190, 80, 35, 99, 180, 80, 126, 54, 45},
     {85, 126, 47, 87, 176, 51, 41, 20, 32},
     {101, 75, 128, 139, 118, 146, 116, 128, 85},
     {56, 41, 15, 176, 236, 85, 37, 9, 62},
     {71, 30, 17, 119, 118, 255, 17, 18, 138},
     {101, 38, 60, 138, 55, 70, 43, 26, 142},
     {146, 36, 19, 30, 171, 255, 97, 27, 20},
     {138, 45, 61, 62, 219, 1, 81, 188, 64},
     {32, 41, 20, 117, 151, 142, 20, 21, 163},
     {112, 19, 12, 61, 195, 128, 48, 4, 24}},
};

/**
 * The 4x4 mode tree (RFC 6386 section 11.2): a positive entry indexes the next pair of nodes, and
 * any other is a leaf holding the negated mode, so B_DC_PRED's leaf is zero.
 */
constexpr std::array<std::int8_t, 18> subblock_mode_tree{
    -mode_dc, 1,        // DC, or deeper
    -mode_tm, 2,        // TM, or deeper
    -mode_ve, 3,        // VE, or deeper
    4,        6,        // {HE, RD, VR} or {LD, VL, HD, HU}
    -mode_he, 5,        // HE, or {RD, VR}
    -mode_rd, -mode_vr, //
    -mode_ld, 7,        // LD, or deeper
    -mode_vl, 8,        // VL, or {HD, HU}
    -mode_hd, -mode_hu, //
};

// RFC 6386 section 14.1: quantizer step sizes by index.
constexpr std::array<std::uint8_t, 128> dc_quantizer{
    4,   5,   6,   7,   8,   9,   10,  10,  11,  12,  13,  14,  15,  16,  17,  17,  18,  19,  20,
    20,  21,  21,  22,  22,  23,  23,  24,  25,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,
    35,  36,  37,  37,  38,  39,  40,  41,  42,  43,  44,  45,  46,  46,  47,  48,  49,  50,  51,
    52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,
    71,  72,  73,  74,  75,  76,  76,  77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  88,
    89,  91,  93,  95,  96,  98,  100, 101, 102, 104, 106, 108, 110, 112, 114, 116, 118, 122, 124,
    126, 128, 130, 132, 134, 136, 138, 140, 143, 145, 148, 151, 154, 157,
};
constexpr std::array<std::uint16_t, 128> ac_quantizer{
    4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,  22,
    23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,
    42,  43,  44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  60,  62,
    64,  66,  68,  70,  72,  74,  76,  78,  80,  82,  84,  86,  88,  90,  92,  94,  96,  98,  100,
    102, 104, 106, 108, 110, 112, 114, 116, 119, 122, 125, 128, 131, 134, 137, 140, 143, 146, 149,
    152, 155, 158, 161, 164, 167, 170, 173, 177, 181, 185, 189, 193, 197, 201, 205, 209, 213, 217,
    221, 225, 229, 234, 239, 245, 249, 254, 259, 264, 269, 274, 279, 284,
};

/** Coefficient positions in decoding order, and the probability band of each position. */
constexpr std::array<std::uint8_t, 16> zigzag{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15};
// One entry past the end, so the context lookup for "after the last coefficient" stays in range.
constexpr std::array<std::uint8_t, 17> band_of{0, 1, 2, 3, 6, 4, 5, 6, 6, 6, 6, 6, 6, 6, 6, 7, 0};

// Extra-bit probabilities of the DCT_CAT3..DCT_CAT6 tokens, each ending at zero.
constexpr std::uint8_t category3[] = {173, 148, 140, 0};
constexpr std::uint8_t category4[] = {176, 155, 140, 135, 0};
constexpr std::uint8_t category5[] = {180, 157, 141, 134, 130, 0};
constexpr std::uint8_t category6[] = {254, 254, 243, 230, 196, 177, 153, 140, 133, 130, 129, 0};
constexpr std::array<const std::uint8_t*, 4> categories{category3, category4, category5, category6};

/**
 * The boolean entropy decoder (RFC 6386 section 7), shaped like libwebp's so that it detects the
 * end of its data at the same moment: `bits_` counts the bits loaded beyond the eight a decision
 * compares against, and a decision that needs a byte past the end shifts in zeros and marks the
 * reader exhausted, which the caller then treats as truncation, as libwebp does.
 */
class BoolDecoder final {
  public:
    BoolDecoder() noexcept = default;
    explicit BoolDecoder(const std::span<const std::uint8_t> bytes) noexcept
        : next_(bytes.data()), end_(bytes.data() + bytes.size()) {}

    [[nodiscard]] bool exhausted() const noexcept {
        return exhausted_;
    }

    [[nodiscard]] std::uint32_t bit(const std::uint32_t probability) noexcept {
        if (bits_ < 0)
            load();
        const auto position = static_cast<std::uint32_t>(bits_);
        const std::uint32_t split = (range_ * probability) >> 8U;
        const auto value = static_cast<std::uint32_t>(value_ >> position);
        std::uint32_t range = split + 1U;
        std::uint32_t result = 0U;
        if (value > split) {
            range = range_ - split;
            value_ -= static_cast<std::uint64_t>(split + 1U) << position;
            result = 1U;
        }
        // Renormalize so the range is at least 128 again.
        const int shift = 8 - static_cast<int>(std::bit_width(range));
        range <<= static_cast<std::uint32_t>(shift);
        bits_ -= shift;
        range_ = range - 1U;
        return result;
    }

    /** An unsigned literal of `count` bits, most significant first, each at even odds. */
    [[nodiscard]] std::uint32_t literal(std::uint32_t count) noexcept {
        std::uint32_t value = 0U;
        while (count-- > 0U)
            value |= bit(128U) << count;
        return value;
    }

    /** A magnitude of `count` bits followed by a sign bit. */
    [[nodiscard]] int signed_literal(const std::uint32_t count) noexcept {
        const auto magnitude = static_cast<int>(literal(count));
        return bit(128U) != 0U ? -magnitude : magnitude;
    }

  private:
    void load() noexcept {
        if (next_ != end_) {
            value_ = value_ << 8U | *next_++;
            bits_ += 8;
        } else if (!exhausted_) {
            value_ <<= 8U;
            bits_ += 8;
            exhausted_ = true;
        } else {
            bits_ = 0;
        }
    }

    const std::uint8_t* next_ = nullptr;
    const std::uint8_t* end_ = nullptr;
    std::uint64_t value_ = 0U;
    /** The range minus one, in [127, 254] between decisions. */
    std::uint32_t range_ = 254U;
    int bits_ = -8;
    bool exhausted_ = false;
};

[[nodiscard]] constexpr int clamp_int(const int value, const int low, const int high) noexcept {
    return value < low ? low : value > high ? high : value;
}

[[nodiscard]] constexpr std::uint8_t clip_pixel(const int value) noexcept {
    return static_cast<std::uint8_t>(clamp_int(value, 0, 255));
}

// The transforms' fixed-point multiplies by sqrt(2)*cos(pi/8) and sqrt(2)*sin(pi/8). libwebp
// multiplies in int; the product is formed in 64 bits and wrapped back to 32, which matches it for
// every input and stays defined for the out-of-range ones a hostile stream can produce.
[[nodiscard]] constexpr int multiply_1(const int value) noexcept {
    return static_cast<int>(static_cast<std::int32_t>(static_cast<std::int64_t>(value) * 20091) >>
                            16) +
           value;
}
[[nodiscard]] constexpr int multiply_2(const int value) noexcept {
    return static_cast<int>(static_cast<std::int32_t>(static_cast<std::int64_t>(value) * 35468) >>
                            16);
}

constexpr std::ptrdiff_t work_stride = 32;

/** Adds the inverse DCT of one 4x4 block of dequantized coefficients to its prediction. */
void add_inverse_dct(const std::int16_t* const in, std::uint8_t* destination) noexcept {
    std::array<int, 16> columns{};
    for (std::size_t i = 0U; i < 4U; ++i) {
        const int a = in[i] + in[8U + i];
        const int b = in[i] - in[8U + i];
        const int c = multiply_2(in[4U + i]) - multiply_1(in[12U + i]);
        const int d = multiply_1(in[4U + i]) + multiply_2(in[12U + i]);
        columns[i * 4U + 0U] = a + d;
        columns[i * 4U + 1U] = b + c;
        columns[i * 4U + 2U] = b - c;
        columns[i * 4U + 3U] = a - d;
    }
    for (std::size_t i = 0U; i < 4U; ++i) {
        const int dc = columns[i] + 4;
        const int a = dc + columns[8U + i];
        const int b = dc - columns[8U + i];
        const int c = multiply_2(columns[4U + i]) - multiply_1(columns[12U + i]);
        const int d = multiply_1(columns[4U + i]) + multiply_2(columns[12U + i]);
        const std::array<int, 4> row{a + d, b + c, b - c, a - d};
        for (std::size_t x = 0U; x < 4U; ++x)
            destination[x] = clip_pixel(destination[x] + (row[x] >> 3));
        destination += work_stride;
    }
}

/**
 * The inverse Walsh-Hadamard transform of the second-order block, whose outputs are the DC
 * coefficients of the sixteen luma blocks. Stored to int16 as libwebp stores them.
 */
void inverse_wht(const std::array<std::int16_t, 16>& in, std::int16_t* out) noexcept {
    std::array<int, 16> temporary{};
    for (std::size_t i = 0U; i < 4U; ++i) {
        const int a0 = in[i] + in[12U + i];
        const int a1 = in[4U + i] + in[8U + i];
        const int a2 = in[4U + i] - in[8U + i];
        const int a3 = in[i] - in[12U + i];
        temporary[i] = a0 + a1;
        temporary[8U + i] = a0 - a1;
        temporary[4U + i] = a3 + a2;
        temporary[12U + i] = a3 - a2;
    }
    for (std::size_t i = 0U; i < 4U; ++i) {
        const int dc = temporary[i * 4U] + 3;
        const int a0 = dc + temporary[i * 4U + 3U];
        const int a1 = temporary[i * 4U + 1U] + temporary[i * 4U + 2U];
        const int a2 = temporary[i * 4U + 1U] - temporary[i * 4U + 2U];
        const int a3 = dc - temporary[i * 4U + 3U];
        out[0] = static_cast<std::int16_t>((a0 + a1) >> 3);
        out[16] = static_cast<std::int16_t>((a3 + a2) >> 3);
        out[32] = static_cast<std::int16_t>((a0 - a1) >> 3);
        out[48] = static_cast<std::int16_t>((a3 - a2) >> 3);
        out += 64;
    }
}

[[nodiscard]] constexpr std::uint8_t average2(const int a, const int b) noexcept {
    return static_cast<std::uint8_t>((a + b + 1) >> 1);
}
[[nodiscard]] constexpr std::uint8_t average3(const int a, const int b, const int c) noexcept {
    return static_cast<std::uint8_t>((a + 2 * b + c + 2) >> 2);
}

/** Fills a size x size block with one value. */
void fill(std::uint8_t* destination, const int size, const int value) noexcept {
    for (int y = 0; y < size; ++y)
        std::fill_n(destination + y * work_stride, size, static_cast<std::uint8_t>(value));
}

/**
 * Predicts a 16x16 luma or 8x8 chroma block in place from the edge samples around it. DC
 * prediction averages only the edges inside the frame (128 with neither); the other modes use the
 * frame's synthetic edges, 127 above and 129 to the left.
 */
void predict_block(std::uint8_t* const destination, const int size, const std::uint8_t mode,
                   const bool has_top, const bool has_left) noexcept {
    const std::uint8_t* const top = destination - work_stride;
    switch (mode) {
    case mode_dc: {
        const int shift = size == 16 ? 4 : 3;
        int sum = 0;
        if (has_top) {
            for (int x = 0; x < size; ++x)
                sum += top[x];
        }
        if (has_left) {
            for (int y = 0; y < size; ++y)
                sum += destination[y * work_stride - 1];
        }
        int value = 128;
        if (has_top && has_left)
            value = (sum + size) >> (shift + 1);
        else if (has_top || has_left)
            value = (sum + (size >> 1)) >> shift;
        fill(destination, size, value);
        break;
    }
    case mode_tm:
        for (int y = 0; y < size; ++y) {
            const int left = destination[y * work_stride - 1] - top[-1];
            for (int x = 0; x < size; ++x)
                destination[y * work_stride + x] = clip_pixel(top[x] + left);
        }
        break;
    case mode_ve:
        for (int y = 0; y < size; ++y)
            std::copy_n(top, size, destination + y * work_stride);
        break;
    default:
        for (int y = 0; y < size; ++y) {
            std::fill_n(destination + y * work_stride, size, destination[y * work_stride - 1]);
        }
        break;
    }
}

/** Predicts one 4x4 luma subblock in place (RFC 6386 section 12.3, as libwebp computes it). */
void predict_subblock(std::uint8_t* const d, const std::uint8_t mode) noexcept {
    const std::uint8_t* const top = d - work_stride;
    const int x0 = top[-1];
    const int a = top[0];
    const int b = top[1];
    const int c = top[2];
    const int e = top[3];
    const int f = top[4];
    const int g = top[5];
    const int h = top[6];
    const int k = top[7];
    const int i = d[-1];
    const int j = d[work_stride - 1];
    const int l = d[2 * work_stride - 1];
    const int m = d[3 * work_stride - 1];
    const auto at = [d](const int x, const int y) -> std::uint8_t& {
        return d[y * work_stride + x];
    };
    switch (mode) {
    case mode_dc: {
        int dc = 4;
        for (int n = 0; n < 4; ++n)
            dc += top[n] + d[n * work_stride - 1];
        fill(d, 4, dc >> 3);
        break;
    }
    case mode_tm:
        for (int y = 0; y < 4; ++y) {
            const int left = d[y * work_stride - 1] - x0;
            for (int x = 0; x < 4; ++x)
                at(x, y) = clip_pixel(top[x] + left);
        }
        break;
    case mode_ve: {
        const std::array<std::uint8_t, 4> values{average3(x0, a, b), average3(a, b, c),
                                                 average3(b, c, e), average3(c, e, f)};
        for (int y = 0; y < 4; ++y)
            std::copy(values.begin(), values.end(), d + y * work_stride);
        break;
    }
    case mode_he: {
        const std::array<std::uint8_t, 4> values{average3(x0, i, j), average3(i, j, l),
                                                 average3(j, l, m), average3(l, m, m)};
        for (int y = 0; y < 4; ++y)
            std::fill_n(d + y * work_stride, 4, values[static_cast<std::size_t>(y)]);
        break;
    }
    case mode_rd:
        at(0, 3) = average3(j, l, m);
        at(1, 3) = at(0, 2) = average3(i, j, l);
        at(2, 3) = at(1, 2) = at(0, 1) = average3(x0, i, j);
        at(3, 3) = at(2, 2) = at(1, 1) = at(0, 0) = average3(a, x0, i);
        at(3, 2) = at(2, 1) = at(1, 0) = average3(b, a, x0);
        at(3, 1) = at(2, 0) = average3(c, b, a);
        at(3, 0) = average3(e, c, b);
        break;
    case mode_vr:
        at(0, 0) = at(1, 2) = average2(x0, a);
        at(1, 0) = at(2, 2) = average2(a, b);
        at(2, 0) = at(3, 2) = average2(b, c);
        at(3, 0) = average2(c, e);
        at(0, 3) = average3(l, j, i);
        at(0, 2) = average3(j, i, x0);
        at(0, 1) = at(1, 3) = average3(i, x0, a);
        at(1, 1) = at(2, 3) = average3(x0, a, b);
        at(2, 1) = at(3, 3) = average3(a, b, c);
        at(3, 1) = average3(b, c, e);
        break;
    case mode_ld:
        at(0, 0) = average3(a, b, c);
        at(1, 0) = at(0, 1) = average3(b, c, e);
        at(2, 0) = at(1, 1) = at(0, 2) = average3(c, e, f);
        at(3, 0) = at(2, 1) = at(1, 2) = at(0, 3) = average3(e, f, g);
        at(3, 1) = at(2, 2) = at(1, 3) = average3(f, g, h);
        at(3, 2) = at(2, 3) = average3(g, h, k);
        at(3, 3) = average3(h, k, k);
        break;
    case mode_vl:
        at(0, 0) = average2(a, b);
        at(1, 0) = at(0, 2) = average2(b, c);
        at(2, 0) = at(1, 2) = average2(c, e);
        at(3, 0) = at(2, 2) = average2(e, f);
        at(0, 1) = average3(a, b, c);
        at(1, 1) = at(0, 3) = average3(b, c, e);
        at(2, 1) = at(1, 3) = average3(c, e, f);
        at(3, 1) = at(2, 3) = average3(e, f, g);
        // These two break the diagonal pattern; the format defines them so.
        at(3, 2) = average3(f, g, h);
        at(3, 3) = average3(g, h, k);
        break;
    case mode_hd:
        at(0, 0) = at(2, 1) = average2(i, x0);
        at(0, 1) = at(2, 2) = average2(j, i);
        at(0, 2) = at(2, 3) = average2(l, j);
        at(0, 3) = average2(m, l);
        at(3, 0) = average3(a, b, c);
        at(2, 0) = average3(x0, a, b);
        at(1, 0) = at(3, 1) = average3(i, x0, a);
        at(1, 1) = at(3, 2) = average3(j, i, x0);
        at(1, 2) = at(3, 3) = average3(l, j, i);
        at(1, 3) = average3(m, l, j);
        break;
    default: // mode_hu
        at(0, 0) = average2(i, j);
        at(2, 0) = at(0, 1) = average2(j, l);
        at(2, 1) = at(0, 2) = average2(l, m);
        at(1, 0) = average3(i, j, l);
        at(3, 0) = at(1, 1) = average3(j, l, m);
        at(3, 1) = at(1, 2) = average3(l, m, m);
        at(3, 2) = at(2, 2) = at(0, 3) = at(1, 3) = at(2, 3) = at(3, 3) =
            static_cast<std::uint8_t>(m);
        break;
    }
}

// The loop filters (RFC 6386 section 15), on a plane with `step` across the edge.

void filter2(std::uint8_t* const p, const std::ptrdiff_t step) noexcept {
    const int p1 = p[-2 * step];
    const int p0 = p[-step];
    const int q0 = p[0];
    const int q1 = p[step];
    const int a = 3 * (q0 - p0) + clamp_int(p1 - q1, -128, 127);
    const int a1 = clamp_int((a + 4) >> 3, -16, 15);
    const int a2 = clamp_int((a + 3) >> 3, -16, 15);
    p[-step] = clip_pixel(p0 + a2);
    p[0] = clip_pixel(q0 - a1);
}

void filter4(std::uint8_t* const p, const std::ptrdiff_t step) noexcept {
    const int p1 = p[-2 * step];
    const int p0 = p[-step];
    const int q0 = p[0];
    const int q1 = p[step];
    const int a = 3 * (q0 - p0);
    const int a1 = clamp_int((a + 4) >> 3, -16, 15);
    const int a2 = clamp_int((a + 3) >> 3, -16, 15);
    const int a3 = (a1 + 1) >> 1;
    p[-2 * step] = clip_pixel(p1 + a3);
    p[-step] = clip_pixel(p0 + a2);
    p[0] = clip_pixel(q0 - a1);
    p[step] = clip_pixel(q1 - a3);
}

void filter6(std::uint8_t* const p, const std::ptrdiff_t step) noexcept {
    const int p2 = p[-3 * step];
    const int p1 = p[-2 * step];
    const int p0 = p[-step];
    const int q0 = p[0];
    const int q1 = p[step];
    const int q2 = p[2 * step];
    const int a = clamp_int(3 * (q0 - p0) + clamp_int(p1 - q1, -128, 127), -128, 127);
    const int a1 = (27 * a + 63) >> 7;
    const int a2 = (18 * a + 63) >> 7;
    const int a3 = (9 * a + 63) >> 7;
    p[-3 * step] = clip_pixel(p2 + a3);
    p[-2 * step] = clip_pixel(p1 + a2);
    p[-step] = clip_pixel(p0 + a1);
    p[0] = clip_pixel(q0 - a1);
    p[step] = clip_pixel(q1 - a2);
    p[2 * step] = clip_pixel(q2 - a3);
}

[[nodiscard]] bool high_edge_variance(const std::uint8_t* const p, const std::ptrdiff_t step,
                                      const int threshold) noexcept {
    return std::abs(p[-2 * step] - p[-step]) > threshold || std::abs(p[step] - p[0]) > threshold;
}

[[nodiscard]] bool needs_filter(const std::uint8_t* const p, const std::ptrdiff_t step,
                                const int threshold) noexcept {
    return 4 * std::abs(p[-step] - p[0]) + std::abs(p[-2 * step] - p[step]) <= threshold;
}

[[nodiscard]] bool needs_filter2(const std::uint8_t* const p, const std::ptrdiff_t step,
                                 const int threshold, const int interior) noexcept {
    const int p3 = p[-4 * step];
    const int p2 = p[-3 * step];
    const int p1 = p[-2 * step];
    const int p0 = p[-step];
    const int q0 = p[0];
    const int q1 = p[step];
    const int q2 = p[2 * step];
    const int q3 = p[3 * step];
    if (4 * std::abs(p0 - q0) + std::abs(p1 - q1) > threshold)
        return false;
    return std::abs(p3 - p2) <= interior && std::abs(p2 - p1) <= interior &&
           std::abs(p1 - p0) <= interior && std::abs(q3 - q2) <= interior &&
           std::abs(q2 - q1) <= interior && std::abs(q1 - q0) <= interior;
}

/** The simple filter across one 16-sample edge; `along` steps between samples of the edge. */
void simple_edge(std::uint8_t* p, const std::ptrdiff_t across, const std::ptrdiff_t along,
                 const int limit) noexcept {
    const int threshold = 2 * limit + 1;
    for (int n = 0; n < 16; ++n, p += along) {
        if (needs_filter(p, across, threshold))
            filter2(p, across);
    }
}

/** The normal filter across one edge: `macroblock` edges move three samples a side, inner two. */
void normal_edge(std::uint8_t* p, const std::ptrdiff_t across, const std::ptrdiff_t along,
                 const int size, const int limit, const int interior, const int hev_threshold,
                 const bool macroblock) noexcept {
    const int threshold = 2 * limit + 1;
    for (int n = 0; n < size; ++n, p += along) {
        if (!needs_filter2(p, across, threshold, interior))
            continue;
        if (high_edge_variance(p, across, hev_threshold))
            filter2(p, across);
        else if (macroblock)
            filter6(p, across);
        else
            filter4(p, across);
    }
}

// libwebp's integer BT.601 conversion: 14-bit fixed point with a 6-bit fraction.
[[nodiscard]] constexpr int multiply_high(const int value, const int coefficient) noexcept {
    return (value * coefficient) >> 8;
}
[[nodiscard]] constexpr std::uint8_t clip_yuv(const int value) noexcept {
    constexpr int mask = (256 << 6) - 1;
    if ((value & ~mask) == 0)
        return static_cast<std::uint8_t>(value >> 6);
    return value < 0 ? std::uint8_t{0U} : std::uint8_t{255U};
}
void yuv_to_rgba(const int y, const int u, const int v, std::uint8_t* const out) noexcept {
    out[0] = clip_yuv(multiply_high(y, 19077) + multiply_high(v, 26149) - 14234);
    out[1] =
        clip_yuv(multiply_high(y, 19077) - multiply_high(u, 6419) - multiply_high(v, 13320) + 8708);
    out[2] = clip_yuv(multiply_high(y, 19077) + multiply_high(u, 33050) - 17685);
    out[3] = 255U;
}

/**
 * libwebp's "fancy" upsampling of one chroma plane for a pair of output rows: each output sample
 * weighs its four nearest chroma samples 9:3:3:1. `top` and `current` are the chroma rows above
 * and below the pair; `lower` may be null when only the upper row is wanted.
 */
void upsample_chroma(const std::uint8_t* const top, const std::uint8_t* const current,
                     const std::size_t length, std::uint8_t* const upper,
                     std::uint8_t* const lower) noexcept {
    const std::size_t last_pair = (length - 1U) >> 1U;
    std::uint32_t top_left = top[0];
    std::uint32_t left = current[0];
    upper[0] = static_cast<std::uint8_t>((3U * top_left + left + 2U) >> 2U);
    if (lower != nullptr)
        lower[0] = static_cast<std::uint8_t>((3U * left + top_left + 2U) >> 2U);
    for (std::size_t x = 1U; x <= last_pair; ++x) {
        const std::uint32_t above = top[x];
        const std::uint32_t here = current[x];
        const std::uint32_t average = top_left + above + left + here + 8U;
        const std::uint32_t diagonal_12 = (average + 2U * (above + left)) >> 3U;
        const std::uint32_t diagonal_03 = (average + 2U * (top_left + here)) >> 3U;
        upper[2U * x - 1U] = static_cast<std::uint8_t>((diagonal_12 + top_left) >> 1U);
        upper[2U * x] = static_cast<std::uint8_t>((diagonal_03 + above) >> 1U);
        if (lower != nullptr) {
            lower[2U * x - 1U] = static_cast<std::uint8_t>((diagonal_03 + left) >> 1U);
            lower[2U * x] = static_cast<std::uint8_t>((diagonal_12 + here) >> 1U);
        }
        top_left = above;
        left = here;
    }
    if ((length & 1U) == 0U) {
        upper[length - 1U] = static_cast<std::uint8_t>((3U * top_left + left + 2U) >> 2U);
        if (lower != nullptr)
            lower[length - 1U] = static_cast<std::uint8_t>((3U * left + top_left + 2U) >> 2U);
    }
}

struct Quantizer final {
    std::array<int, 2> y1{};
    std::array<int, 2> y2{};
    std::array<int, 2> uv{};
};

struct FilterStrength final {
    int limit = 0;
    int interior = 0;
    int hev_threshold = 0;
};

struct Macroblock final {
    std::uint8_t segment = 0U;
    bool skip = false;
    bool subblocks = false;
    std::array<std::uint8_t, 16> modes{};
    std::uint8_t chroma_mode = mode_dc;
};

struct MacroblockFilter final {
    FilterStrength strength;
    bool inner = false;
};

class Decoder final {
  public:
    Decoder(const std::span<const std::uint8_t> frame, const ImageDimensions dimensions)
        : frame_(frame), width_(dimensions.width), height_(dimensions.height),
          mb_width_((dimensions.width + 15U) >> 4U), mb_height_((dimensions.height + 15U) >> 4U) {}

    [[nodiscard]] std::vector<std::uint8_t> decode() {
        parse_header();
        y_stride_ = static_cast<std::size_t>(mb_width_) * 16U;
        uv_stride_ = static_cast<std::size_t>(mb_width_) * 8U;
        y_plane_.assign(y_stride_ * mb_height_ * 16U, 0U);
        u_plane_.assign(uv_stride_ * mb_height_ * 8U, 0U);
        v_plane_.assign(uv_stride_ * mb_height_ * 8U, 0U);
        filters_.assign(static_cast<std::size_t>(mb_width_) * mb_height_, MacroblockFilter{});
        top_modes_.assign(static_cast<std::size_t>(mb_width_) * 4U, mode_dc);
        top_nonzero_.assign(mb_width_, Contexts{});
        std::vector<Macroblock> row(mb_width_);
        std::array<std::int16_t, 384> coefficients{};
        for (std::uint32_t mb_y = 0U; mb_y < mb_height_; ++mb_y) {
            left_modes_.fill(mode_dc);
            left_nonzero_ = Contexts{};
            for (std::uint32_t mb_x = 0U; mb_x < mb_width_; ++mb_x)
                parse_modes(row[mb_x], mb_x);
            if (header_reader_.exhausted())
                fail("the first partition ends early");
            BoolDecoder& tokens = partitions_[mb_y & (partition_count_ - 1U)];
            for (std::uint32_t mb_x = 0U; mb_x < mb_width_; ++mb_x) {
                Macroblock& block = row[mb_x];
                bool skip = use_skip_probability_ && block.skip;
                if (!skip) {
                    skip = parse_residuals(block, mb_x, tokens, coefficients);
                } else {
                    coefficients.fill(0);
                    top_nonzero_[mb_x].luma.fill(0U);
                    top_nonzero_[mb_x].chroma = {};
                    left_nonzero_.luma.fill(0U);
                    left_nonzero_.chroma = {};
                    if (!block.subblocks) {
                        top_nonzero_[mb_x].dc = 0U;
                        left_nonzero_.dc = 0U;
                    }
                }
                if (tokens.exhausted())
                    fail("a token partition ends early");
                MacroblockFilter& filter = filters_[mb_y * mb_width_ + mb_x];
                filter.strength = strengths_[block.segment][block.subblocks ? 1U : 0U];
                filter.inner = block.subblocks || !skip;
                reconstruct(block, coefficients, mb_x, mb_y);
            }
        }
        if (filter_type_ != 0U)
            filter_frame();
        return convert();
    }

  private:
    struct Contexts final {
        std::array<std::uint8_t, 4> luma{};
        /** [plane: U, V][position] */
        std::array<std::array<std::uint8_t, 2>, 2> chroma{};
        std::uint8_t dc = 0U;
    };

    void parse_header() {
        const std::uint32_t tag = static_cast<std::uint32_t>(frame_[0]) |
                                  static_cast<std::uint32_t>(frame_[1]) << 8U |
                                  static_cast<std::uint32_t>(frame_[2]) << 16U;
        const std::size_t first_size = tag >> 5U;
        const std::span<const std::uint8_t> rest = frame_.subspan(10U);
        if (first_size > rest.size())
            fail("the first partition is truncated");
        header_reader_ = BoolDecoder(rest.first(first_size));
        BoolDecoder& reader = header_reader_;
        // The colour space and clamping type are informative; libwebp always clamps.
        static_cast<void>(reader.literal(2U));

        // Segmentation (section 9.3).
        bool use_segments = reader.bit(128U) != 0U;
        bool update_map = false;
        bool absolute = true;
        std::array<int, 4> segment_quantizer{};
        std::array<int, 4> segment_filter{};
        if (use_segments) {
            update_map = reader.bit(128U) != 0U;
            if (reader.bit(128U) != 0U) {
                absolute = reader.bit(128U) != 0U;
                for (int& value : segment_quantizer)
                    value = reader.bit(128U) != 0U ? reader.signed_literal(7U) : 0;
                for (int& value : segment_filter)
                    value = reader.bit(128U) != 0U ? reader.signed_literal(6U) : 0;
            }
            if (update_map) {
                for (std::uint8_t& probability : segment_probabilities_) {
                    probability = reader.bit(128U) != 0U
                                      ? static_cast<std::uint8_t>(reader.literal(8U))
                                      : std::uint8_t{255U};
                }
            }
        }
        update_segment_map_ = update_map;
        if (reader.exhausted())
            fail("the segment header is truncated");

        // Loop filter (section 9.6).
        const bool simple = reader.bit(128U) != 0U;
        const auto level = static_cast<int>(reader.literal(6U));
        const auto sharpness = static_cast<int>(reader.literal(3U));
        const bool use_deltas = reader.bit(128U) != 0U;
        std::array<int, 4> reference_delta{};
        std::array<int, 4> mode_delta{};
        if (use_deltas && reader.bit(128U) != 0U) {
            for (int& delta : reference_delta) {
                if (reader.bit(128U) != 0U)
                    delta = reader.signed_literal(6U);
            }
            for (int& delta : mode_delta) {
                if (reader.bit(128U) != 0U)
                    delta = reader.signed_literal(6U);
            }
        }
        // As libwebp decides: a zero frame level disables filtering whatever the segments say.
        filter_type_ = level == 0 ? 0U : simple ? 1U : 2U;
        if (reader.exhausted())
            fail("the filter header is truncated");

        // Token partitions (section 9.5): sizes of all but the last, then the partitions.
        partition_count_ = 1U << reader.literal(2U);
        std::span<const std::uint8_t> partitions = rest.subspan(first_size);
        const std::size_t sizes_bytes = 3U * (partition_count_ - 1U);
        if (partitions.size() < sizes_bytes)
            fail("the partition table is truncated");
        std::span<const std::uint8_t> data = partitions.subspan(sizes_bytes);
        for (std::size_t index = 0U; index + 1U < partition_count_; ++index) {
            const std::uint8_t* const size_bytes = partitions.data() + 3U * index;
            std::size_t size = static_cast<std::size_t>(size_bytes[0]) |
                               static_cast<std::size_t>(size_bytes[1]) << 8U |
                               static_cast<std::size_t>(size_bytes[2]) << 16U;
            // libwebp clamps a partition that overruns the chunk; the reader then reports it
            // exhausted if a macroblock needs the missing bytes.
            size = std::min(size, data.size());
            partitions_[index] = BoolDecoder(data.first(size));
            data = data.subspan(size);
        }
        if (data.empty())
            fail("the last token partition is empty");
        partitions_[partition_count_ - 1U] = BoolDecoder(data);

        // Quantizers (section 9.6, 14.1).
        const auto base = static_cast<int>(reader.literal(7U));
        const auto delta = [&reader] {
            return reader.bit(128U) != 0U ? reader.signed_literal(4U) : 0;
        };
        const int y1_dc = delta();
        const int y2_dc = delta();
        const int y2_ac = delta();
        const int uv_dc = delta();
        const int uv_ac = delta();
        for (std::size_t segment = 0U; segment < 4U; ++segment) {
            int q = base;
            if (use_segments) {
                q = segment_quantizer[segment] + (absolute ? 0 : base);
            } else if (segment > 0U) {
                quantizers_[segment] = quantizers_[0];
                continue;
            }
            const auto dc = [](const int index, const int limit) {
                return static_cast<int>(
                    dc_quantizer[static_cast<std::size_t>(clamp_int(index, 0, limit))]);
            };
            const auto ac = [](const int index) {
                return static_cast<int>(
                    ac_quantizer[static_cast<std::size_t>(clamp_int(index, 0, 127))]);
            };
            Quantizer& quantizer = quantizers_[segment];
            quantizer.y1 = {dc(q + y1_dc, 127), ac(q)};
            // x * 155 / 100, exactly, for every table value.
            quantizer.y2 = {dc(q + y2_dc, 127) * 2, std::max((ac(q + y2_ac) * 101581) >> 16, 8)};
            quantizer.uv = {dc(q + uv_dc, 117), ac(q + uv_ac)};
        }

        // Filter strengths per segment and per prediction kind (section 15.1, 9.6).
        if (filter_type_ != 0U) {
            for (std::size_t segment = 0U; segment < 4U; ++segment) {
                int base_level = level;
                if (use_segments)
                    base_level = segment_filter[segment] + (absolute ? 0 : level);
                for (std::size_t subblocks = 0U; subblocks < 2U; ++subblocks) {
                    int value = base_level;
                    if (use_deltas) {
                        value += reference_delta[0];
                        if (subblocks != 0U)
                            value += mode_delta[0];
                    }
                    value = clamp_int(value, 0, 63);
                    FilterStrength& strength = strengths_[segment][subblocks];
                    if (value == 0) {
                        strength = FilterStrength{};
                        continue;
                    }
                    int interior = value;
                    if (sharpness > 0) {
                        interior >>= sharpness > 4 ? 2 : 1;
                        interior = std::min(interior, 9 - sharpness);
                    }
                    interior = std::max(interior, 1);
                    strength.interior = interior;
                    strength.limit = 2 * value + interior;
                    strength.hev_threshold = value >= 40 ? 2 : value >= 15 ? 1 : 0;
                }
            }
        }

        // A key frame's probabilities start from the defaults (section 13).
        static_cast<void>(reader.bit(128U)); // refresh_entropy_probs: irrelevant to one frame
        for (std::size_t t = 0U; t < plane_types; ++t) {
            for (std::size_t b = 0U; b < bands; ++b) {
                for (std::size_t c = 0U; c < contexts; ++c) {
                    for (std::size_t p = 0U; p < nodes; ++p) {
                        probabilities_[t][b][c][p] =
                            reader.bit(update_probabilities[t][b][c][p]) != 0U
                                ? static_cast<std::uint8_t>(reader.literal(8U))
                                : default_probabilities[t][b][c][p];
                    }
                }
            }
        }
        use_skip_probability_ = reader.bit(128U) != 0U;
        if (use_skip_probability_)
            skip_probability_ = reader.literal(8U);
    }

    void parse_modes(Macroblock& block, const std::uint32_t mb_x) {
        BoolDecoder& reader = header_reader_;
        block.segment = 0U;
        if (update_segment_map_) {
            block.segment =
                reader.bit(segment_probabilities_[0]) == 0U
                    ? static_cast<std::uint8_t>(reader.bit(segment_probabilities_[1]))
                    : static_cast<std::uint8_t>(reader.bit(segment_probabilities_[2]) + 2U);
        }
        block.skip = use_skip_probability_ && reader.bit(skip_probability_) != 0U;
        block.subblocks = reader.bit(145U) == 0U;
        std::uint8_t* const top = top_modes_.data() + static_cast<std::size_t>(mb_x) * 4U;
        if (!block.subblocks) {
            const std::uint8_t mode = reader.bit(156U) != 0U
                                          ? (reader.bit(128U) != 0U ? mode_tm : mode_he)
                                          : (reader.bit(163U) != 0U ? mode_ve : mode_dc);
            block.modes.fill(mode);
            std::fill_n(top, 4, mode);
            left_modes_.fill(mode);
        } else {
            for (std::size_t y = 0U; y < 4U; ++y) {
                std::uint8_t left = left_modes_[y];
                for (std::size_t x = 0U; x < 4U; ++x) {
                    const std::uint8_t* const probability =
                        subblock_mode_probabilities[top[x]][left];
                    int node = subblock_mode_tree[reader.bit(probability[0])];
                    while (node > 0) {
                        const auto index = static_cast<std::size_t>(node);
                        node = subblock_mode_tree[2U * index + reader.bit(probability[index])];
                    }
                    left = static_cast<std::uint8_t>(-node);
                    top[x] = left;
                }
                std::copy_n(top, 4, block.modes.begin() + static_cast<std::ptrdiff_t>(y * 4U));
                left_modes_[y] = left;
            }
        }
        block.chroma_mode = reader.bit(142U) == 0U   ? mode_dc
                            : reader.bit(114U) == 0U ? mode_ve
                            : reader.bit(183U) != 0U ? mode_tm
                                                     : mode_he;
    }

    /** Reads the extra magnitude of a coefficient larger than one (section 13.2). */
    [[nodiscard]] static int large_value(BoolDecoder& reader, const std::uint8_t* const p) {
        if (reader.bit(p[3]) == 0U) {
            if (reader.bit(p[4]) == 0U)
                return 2;
            return 3 + static_cast<int>(reader.bit(p[5]));
        }
        if (reader.bit(p[6]) == 0U) {
            if (reader.bit(p[7]) == 0U)
                return 5 + static_cast<int>(reader.bit(159U));
            int value = 7 + 2 * static_cast<int>(reader.bit(165U));
            return value + static_cast<int>(reader.bit(145U));
        }
        const std::uint32_t high = reader.bit(p[8]);
        const std::uint32_t low = reader.bit(p[9U + high]);
        const std::uint32_t category = 2U * high + low;
        int value = 0;
        for (const std::uint8_t* extra = categories[category]; *extra != 0U; ++extra)
            value += value + static_cast<int>(reader.bit(*extra));
        return value + 3 + (8 << category);
    }

    /**
     * Reads one block's tokens from position `n` and stores its dequantized coefficients. Returns
     * the position after the last token, which neighbours use as context.
     */
    [[nodiscard]] int read_block(BoolDecoder& reader, const std::size_t type, const int context,
                                 const std::array<int, 2>& dequantize, int n,
                                 std::int16_t* const out) {
        const std::uint8_t* p = probabilities_[type][band_of[static_cast<std::size_t>(n)]][context];
        for (; n < 16; ++n) {
            if (reader.bit(p[0]) == 0U)
                return n; // end of block
            while (reader.bit(p[1]) == 0U) {
                p = probabilities_[type][band_of[static_cast<std::size_t>(++n)]][0];
                if (n == 16)
                    return 16;
            }
            const std::size_t next_band = band_of[static_cast<std::size_t>(n) + 1U];
            int value = 1;
            if (reader.bit(p[2]) == 0U) {
                p = probabilities_[type][next_band][1];
            } else {
                value = large_value(reader, p);
                p = probabilities_[type][next_band][2];
            }
            if (reader.bit(128U) != 0U)
                value = -value;
            out[zigzag[static_cast<std::size_t>(n)]] =
                static_cast<std::int16_t>(value * dequantize[n > 0 ? 1U : 0U]);
        }
        return 16;
    }

    /**
     * Reads a macroblock's coefficients (section 13). Returns whether every coefficient is zero,
     * which, like a skipped macroblock, leaves its inner edges unfiltered.
     */
    [[nodiscard]] bool parse_residuals(const Macroblock& block, const std::uint32_t mb_x,
                                       BoolDecoder& reader,
                                       std::array<std::int16_t, 384>& coefficients) {
        coefficients.fill(0);
        const Quantizer& quantizer = quantizers_[block.segment];
        Contexts& top = top_nonzero_[mb_x];
        Contexts& left = left_nonzero_;
        int first = 0;
        std::size_t luma_type = 3U;
        if (!block.subblocks) {
            std::array<std::int16_t, 16> dc{};
            const int nonzero =
                read_block(reader, 1U, top.dc + left.dc, quantizer.y2, 0, dc.data());
            top.dc = left.dc = nonzero > 0 ? std::uint8_t{1U} : std::uint8_t{0U};
            inverse_wht(dc, coefficients.data());
            first = 1;
            luma_type = 0U;
        }
        bool any = false;
        for (std::size_t y = 0U; y < 4U; ++y) {
            for (std::size_t x = 0U; x < 4U; ++x) {
                std::int16_t* const out = coefficients.data() + (y * 4U + x) * 16U;
                const int count = read_block(reader, luma_type, top.luma[x] + left.luma[y],
                                             quantizer.y1, first, out);
                top.luma[x] = left.luma[y] = count > first ? std::uint8_t{1U} : std::uint8_t{0U};
                any = any || count > 1 || out[0] != 0;
            }
        }
        for (std::size_t plane = 0U; plane < 2U; ++plane) {
            for (std::size_t y = 0U; y < 2U; ++y) {
                for (std::size_t x = 0U; x < 2U; ++x) {
                    std::int16_t* const out =
                        coefficients.data() + (16U + plane * 4U + y * 2U + x) * 16U;
                    const int count =
                        read_block(reader, 2U, top.chroma[plane][x] + left.chroma[plane][y],
                                   quantizer.uv, 0, out);
                    top.chroma[plane][x] = left.chroma[plane][y] =
                        count > 0 ? std::uint8_t{1U} : std::uint8_t{0U};
                    any = any || count > 1 || out[0] != 0;
                }
            }
        }
        return !any;
    }

    /**
     * Predicts a macroblock from its unfiltered neighbours and adds its residue. The work buffers
     * hold the macroblock with its edges as libwebp lays them out: the row above (with four
     * samples above-right), the column to the left, 127 above the frame and 129 left of it.
     */
    void reconstruct(const Macroblock& block, const std::array<std::int16_t, 384>& coefficients,
                     const std::uint32_t mb_x, const std::uint32_t mb_y) {
        std::array<std::uint8_t, 17U * work_stride> luma{};
        std::array<std::uint8_t, 9U * work_stride> chroma_u{};
        std::array<std::uint8_t, 9U * work_stride> chroma_v{};
        std::uint8_t* const y = luma.data() + work_stride + 8;
        std::uint8_t* const u = chroma_u.data() + work_stride + 8;
        std::uint8_t* const v = chroma_v.data() + work_stride + 8;
        const std::size_t px = static_cast<std::size_t>(mb_x) * 16U;
        const std::size_t py = static_cast<std::size_t>(mb_y) * 16U;
        const std::size_t cx = px / 2U;
        const std::size_t cy = py / 2U;
        if (mb_y == 0U) {
            std::fill_n(y - work_stride - 1, 21, std::uint8_t{127U});
            std::fill_n(u - work_stride - 1, 9, std::uint8_t{127U});
            std::fill_n(v - work_stride - 1, 9, std::uint8_t{127U});
        } else {
            const std::uint8_t* const above = y_plane_.data() + (py - 1U) * y_stride_;
            y[-work_stride - 1] = mb_x == 0U ? std::uint8_t{129U} : above[px - 1U];
            std::copy_n(above + px, 16, y - work_stride);
            if (mb_x + 1U == mb_width_)
                std::fill_n(y - work_stride + 16, 4, above[px + 15U]);
            else
                std::copy_n(above + px + 16U, 4, y - work_stride + 16);
            const std::uint8_t* const above_u = u_plane_.data() + (cy - 1U) * uv_stride_;
            const std::uint8_t* const above_v = v_plane_.data() + (cy - 1U) * uv_stride_;
            u[-work_stride - 1] = mb_x == 0U ? std::uint8_t{129U} : above_u[cx - 1U];
            v[-work_stride - 1] = mb_x == 0U ? std::uint8_t{129U} : above_v[cx - 1U];
            std::copy_n(above_u + cx, 8, u - work_stride);
            std::copy_n(above_v + cx, 8, v - work_stride);
        }
        for (std::size_t row = 0U; row < 16U; ++row) {
            y[static_cast<std::ptrdiff_t>(row) * work_stride - 1] =
                mb_x == 0U ? std::uint8_t{129U} : y_plane_[(py + row) * y_stride_ + px - 1U];
        }
        for (std::size_t row = 0U; row < 8U; ++row) {
            const auto offset = static_cast<std::ptrdiff_t>(row) * work_stride - 1;
            u[offset] =
                mb_x == 0U ? std::uint8_t{129U} : u_plane_[(cy + row) * uv_stride_ + cx - 1U];
            v[offset] =
                mb_x == 0U ? std::uint8_t{129U} : v_plane_[(cy + row) * uv_stride_ + cx - 1U];
        }

        if (block.subblocks) {
            // Subblocks on the right edge take their above-right samples from the row above the
            // macroblock, whichever row of subblocks they are in.
            for (const std::ptrdiff_t row : {3, 7, 11})
                std::copy_n(y - work_stride + 16, 4, y + row * work_stride + 16);
            for (std::size_t n = 0U; n < 16U; ++n) {
                std::uint8_t* const destination =
                    y + static_cast<std::ptrdiff_t>((n & 3U) * 4U) +
                    static_cast<std::ptrdiff_t>((n >> 2U) * 4U) * work_stride;
                predict_subblock(destination, block.modes[n]);
                add_inverse_dct(coefficients.data() + n * 16U, destination);
            }
        } else {
            predict_block(y, 16, block.modes[0], mb_y > 0U, mb_x > 0U);
            for (std::size_t n = 0U; n < 16U; ++n) {
                add_inverse_dct(coefficients.data() + n * 16U,
                                y + static_cast<std::ptrdiff_t>((n & 3U) * 4U) +
                                    static_cast<std::ptrdiff_t>((n >> 2U) * 4U) * work_stride);
            }
        }
        predict_block(u, 8, block.chroma_mode, mb_y > 0U, mb_x > 0U);
        predict_block(v, 8, block.chroma_mode, mb_y > 0U, mb_x > 0U);
        for (std::size_t n = 0U; n < 4U; ++n) {
            const std::ptrdiff_t offset = static_cast<std::ptrdiff_t>((n & 1U) * 4U) +
                                          static_cast<std::ptrdiff_t>((n >> 1U) * 4U) * work_stride;
            add_inverse_dct(coefficients.data() + (16U + n) * 16U, u + offset);
            add_inverse_dct(coefficients.data() + (20U + n) * 16U, v + offset);
        }

        for (std::size_t row = 0U; row < 16U; ++row) {
            std::copy_n(y + static_cast<std::ptrdiff_t>(row) * work_stride, 16,
                        y_plane_.data() + (py + row) * y_stride_ + px);
        }
        for (std::size_t row = 0U; row < 8U; ++row) {
            std::copy_n(u + static_cast<std::ptrdiff_t>(row) * work_stride, 8,
                        u_plane_.data() + (cy + row) * uv_stride_ + cx);
            std::copy_n(v + static_cast<std::ptrdiff_t>(row) * work_stride, 8,
                        v_plane_.data() + (cy + row) * uv_stride_ + cx);
        }
    }

    /**
     * Runs the loop filter over every macroblock in raster order, after reconstruction: intra
     * prediction reads unfiltered samples, and each macroblock's filter sees its left and upper
     * neighbours already filtered, as in libwebp.
     */
    void filter_frame() {
        const auto y_step = static_cast<std::ptrdiff_t>(y_stride_);
        const auto uv_step = static_cast<std::ptrdiff_t>(uv_stride_);
        for (std::uint32_t mb_y = 0U; mb_y < mb_height_; ++mb_y) {
            for (std::uint32_t mb_x = 0U; mb_x < mb_width_; ++mb_x) {
                const MacroblockFilter& filter = filters_[mb_y * mb_width_ + mb_x];
                const int limit = filter.strength.limit;
                if (limit == 0)
                    continue;
                std::uint8_t* const y = y_plane_.data() +
                                        static_cast<std::size_t>(mb_y) * 16U * y_stride_ +
                                        static_cast<std::size_t>(mb_x) * 16U;
                if (filter_type_ == 1U) {
                    if (mb_x > 0U)
                        simple_edge(y, 1, y_step, limit + 4);
                    if (filter.inner) {
                        for (std::ptrdiff_t x = 4; x < 16; x += 4)
                            simple_edge(y + x, 1, y_step, limit);
                    }
                    if (mb_y > 0U)
                        simple_edge(y, y_step, 1, limit + 4);
                    if (filter.inner) {
                        for (std::ptrdiff_t row = 4; row < 16; row += 4)
                            simple_edge(y + row * y_step, y_step, 1, limit);
                    }
                    continue;
                }
                const std::size_t chroma_offset = static_cast<std::size_t>(mb_y) * 8U * uv_stride_ +
                                                  static_cast<std::size_t>(mb_x) * 8U;
                std::uint8_t* const u = u_plane_.data() + chroma_offset;
                std::uint8_t* const v = v_plane_.data() + chroma_offset;
                const int interior = filter.strength.interior;
                const int hev = filter.strength.hev_threshold;
                if (mb_x > 0U) {
                    normal_edge(y, 1, y_step, 16, limit + 4, interior, hev, true);
                    normal_edge(u, 1, uv_step, 8, limit + 4, interior, hev, true);
                    normal_edge(v, 1, uv_step, 8, limit + 4, interior, hev, true);
                }
                if (filter.inner) {
                    for (std::ptrdiff_t x = 4; x < 16; x += 4)
                        normal_edge(y + x, 1, y_step, 16, limit, interior, hev, false);
                    normal_edge(u + 4, 1, uv_step, 8, limit, interior, hev, false);
                    normal_edge(v + 4, 1, uv_step, 8, limit, interior, hev, false);
                }
                if (mb_y > 0U) {
                    normal_edge(y, y_step, 1, 16, limit + 4, interior, hev, true);
                    normal_edge(u, uv_step, 1, 8, limit + 4, interior, hev, true);
                    normal_edge(v, uv_step, 1, 8, limit + 4, interior, hev, true);
                }
                if (filter.inner) {
                    for (std::ptrdiff_t row = 4; row < 16; row += 4)
                        normal_edge(y + row * y_step, y_step, 1, 16, limit, interior, hev, false);
                    normal_edge(u + 4 * uv_step, uv_step, 1, 8, limit, interior, hev, false);
                    normal_edge(v + 4 * uv_step, uv_step, 1, 8, limit, interior, hev, false);
                }
            }
        }
    }

    /**
     * Converts the visible area to RGBA as libwebp's EmitFancyRGB does: the first row and, for an
     * even height, the last use one chroma row; every other pair of rows shares the chroma rows
     * above and below it.
     */
    [[nodiscard]] std::vector<std::uint8_t> convert() const {
        std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width_) * height_ * 4U);
        std::vector<std::uint8_t> upper_u(width_);
        std::vector<std::uint8_t> upper_v(width_);
        std::vector<std::uint8_t> lower_u(width_);
        std::vector<std::uint8_t> lower_v(width_);
        const auto emit = [&](const std::size_t row, const std::vector<std::uint8_t>& chroma_u,
                              const std::vector<std::uint8_t>& chroma_v) {
            const std::uint8_t* const luma = y_plane_.data() + row * y_stride_;
            std::uint8_t* const out = rgba.data() + row * width_ * 4U;
            for (std::size_t x = 0U; x < width_; ++x)
                yuv_to_rgba(luma[x], chroma_u[x], chroma_v[x], out + x * 4U);
        };
        const auto u_row = [this](const std::size_t row) {
            return u_plane_.data() + row * uv_stride_;
        };
        const auto v_row = [this](const std::size_t row) {
            return v_plane_.data() + row * uv_stride_;
        };
        upsample_chroma(u_row(0U), u_row(0U), width_, upper_u.data(), nullptr);
        upsample_chroma(v_row(0U), v_row(0U), width_, upper_v.data(), nullptr);
        emit(0U, upper_u, upper_v);
        std::size_t row = 1U;
        for (; row + 1U < height_; row += 2U) {
            const std::size_t above = (row - 1U) / 2U;
            upsample_chroma(u_row(above), u_row(above + 1U), width_, upper_u.data(),
                            lower_u.data());
            upsample_chroma(v_row(above), v_row(above + 1U), width_, upper_v.data(),
                            lower_v.data());
            emit(row, upper_u, upper_v);
            emit(row + 1U, lower_u, lower_v);
        }
        if (height_ % 2U == 0U) {
            const std::size_t last = height_ / 2U - 1U;
            upsample_chroma(u_row(last), u_row(last), width_, upper_u.data(), nullptr);
            upsample_chroma(v_row(last), v_row(last), width_, upper_v.data(), nullptr);
            emit(height_ - 1U, upper_u, upper_v);
        }
        return rgba;
    }

    std::span<const std::uint8_t> frame_;
    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::uint32_t mb_width_ = 0U;
    std::uint32_t mb_height_ = 0U;

    BoolDecoder header_reader_;
    std::array<BoolDecoder, 8> partitions_{};
    std::uint32_t partition_count_ = 1U;
    std::array<std::uint8_t, 3> segment_probabilities_{255U, 255U, 255U};
    bool update_segment_map_ = false;
    std::uint32_t filter_type_ = 0U;
    std::array<Quantizer, 4> quantizers_{};
    std::array<std::array<FilterStrength, 2>, 4> strengths_{};
    Probabilities probabilities_{};
    bool use_skip_probability_ = false;
    std::uint32_t skip_probability_ = 0U;

    std::vector<std::uint8_t> top_modes_;
    std::array<std::uint8_t, 4> left_modes_{};
    std::vector<Contexts> top_nonzero_;
    Contexts left_nonzero_;
    std::vector<MacroblockFilter> filters_;

    std::size_t y_stride_ = 0U;
    std::size_t uv_stride_ = 0U;
    std::vector<std::uint8_t> y_plane_;
    std::vector<std::uint8_t> u_plane_;
    std::vector<std::uint8_t> v_plane_;
};

} // namespace

ImageDimensions inspect_vp8(const std::span<const std::uint8_t> frame) {
    if (frame.size() < 10U)
        fail("a VP8 frame is truncated");
    const std::uint32_t tag = static_cast<std::uint32_t>(frame[0]) |
                              static_cast<std::uint32_t>(frame[1]) << 8U |
                              static_cast<std::uint32_t>(frame[2]) << 16U;
    if ((tag & 1U) != 0U)
        fail("a VP8 frame is not a key frame");
    if (((tag >> 1U) & 7U) > 3U)
        fail("a VP8 frame has an unknown profile");
    if (((tag >> 4U) & 1U) == 0U)
        fail("a VP8 frame is not displayable");
    if ((tag >> 5U) >= frame.size())
        fail("a VP8 frame's first partition overruns it");
    if (frame[3] != 0x9DU || frame[4] != 0x01U || frame[5] != 0x2AU)
        fail("a VP8 frame lacks its start code");
    // The top two bits of each dimension ask for upscaling on display, which WebP ignores.
    const ImageDimensions dimensions{
        (static_cast<std::uint32_t>(frame[6]) | static_cast<std::uint32_t>(frame[7]) << 8U) &
            0x3FFFU,
        (static_cast<std::uint32_t>(frame[8]) | static_cast<std::uint32_t>(frame[9]) << 8U) &
            0x3FFFU,
    };
    validate_canvas(dimensions, "WebP");
    return dimensions;
}

std::vector<std::uint8_t> decode_vp8_rgba8(const std::span<const std::uint8_t> frame,
                                           const ImageDimensions expected) {
    const ImageDimensions dimensions = inspect_vp8(frame);
    if (dimensions != expected)
        fail("a VP8 frame's dimensions disagree with its container");
    return Decoder(frame, dimensions).decode();
}

} // namespace strata::resource::codec
