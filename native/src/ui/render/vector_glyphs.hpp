#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "font/opentype.hpp"
#include "font/vector_glyph.hpp"

namespace strata::ui {

/**
 * One glyph's record in the vector-text glyph buffer (32 bytes, as packets carry it and the GPU
 * reads it: VectorGlyphRecord in gpu/shaders.hpp matches it): its bounds in font units, where its
 * segments and bands start in their buffers, and how many bands and segments it has. Its bands'
 * pieces follow its segments.
 */
struct VectorGlyphRecord final {
    float left = 0.0F;
    float bottom = 0.0F;
    float right = 0.0F;
    float top = 0.0F;
    std::uint32_t segment_base = 0U;
    std::uint32_t band_base = 0U;
    std::uint32_t band_count = 0U;
    std::uint32_t segment_count = 0U;
};
static_assert(sizeof(VectorGlyphRecord) == 32U);

/**
 * The records a glyph has, from its own on: its outline flattened for any size (within an em over
 * 4096 of its curves), then each next twice as loosely. The vertex stage draws the loosest whose
 * curves stay within a thirty-second of a device pixel at the size the glyph shows
 * (gpu/shaders.hpp).
 */
inline constexpr std::uint32_t vector_glyph_detail_levels = 5U;

/**
 * What the host appends to its vector-text buffers: the glyphs made since the last append, from
 * the first index of each buffer on (every buffer only grows, so what is appended once stays).
 * Segments count in four floats, bands in single indices.
 */
struct VectorGlyphAppend final {
    std::uint32_t first_record = 0U;
    std::vector<VectorGlyphRecord> records;
    std::uint32_t first_segment = 0U;
    std::vector<float> segments;
    std::uint32_t first_band = 0U;
    std::vector<std::uint32_t> bands;

    [[nodiscard]] bool empty() const noexcept {
        return records.empty();
    }
};

/**
 * A Surface's vector glyphs: each glyph's outline made into a font::VectorGlyph once, the first
 * time it is drawn, and its record, segments and bands appended to buffers the host keeps on the
 * GPU under `id`. A glyph is then drawn at any size, place, scale or rotation from its record
 * index, with nothing rasterized for it. Where it shows at the size it was laid out for, it draws
 * a variant fitted to the pixel grid at that size, made once the first time the size appears.
 */
class VectorGlyphPool final {
  public:
    explicit VectorGlyphPool(std::string id) : id_(std::move(id)) {}

    /** The host resource the buffers are kept under; draws of vector text name it as texture. */
    [[nodiscard]] const std::string& id() const noexcept {
        return id_;
    }
    /**
     * The glyph's record index (the first of its vector_glyph_detail_levels), made now if it is
     * new; nothing when the font draws it some other way: a colour glyph, or one it has no
     * TrueType outline for (bitmap and CFF fonts).
     */
    [[nodiscard]] std::optional<std::uint32_t>
    glyph(std::string_view font_id, const font::OpenTypeFont& font, std::uint16_t glyph);
    /**
     * The record of glyph `index` fitted to the pixel grid at `scale` device pixels a font unit
     * (font::fit_vector_glyph), made now if this size is new. It is flattened for that size, its
     * curves within a thirty-second of a pixel, so small text has a few segments where the
     * outline drawn at any size has many: each pixel reads fewer.
     */
    [[nodiscard]] std::uint32_t fitted(std::uint32_t index, double scale);
    [[nodiscard]] const VectorGlyphRecord& record(std::uint32_t index) const {
        return records_.at(index);
    }
    /** What was made since the last commit, for the host to append. */
    [[nodiscard]] VectorGlyphAppend pending() const;
    /** The pending glyphs are in a packet the host will receive. */
    void commit() noexcept;
    /** The host's buffers are gone (or about to be): the next append carries everything. */
    void resend() noexcept;
    /** The host holds buffers under `id`, which a terminal packet must release. */
    [[nodiscard]] bool sent() const noexcept {
        return sent_;
    }
    /** Drops every glyph, as the host releases the buffers. */
    void clear() noexcept;
    [[nodiscard]] std::size_t size() const noexcept {
        return records_.size();
    }

  private:
    [[nodiscard]] std::uint32_t add(const font::VectorGlyph& glyph);

    std::string id_;
    std::map<std::tuple<std::string, std::uint16_t>, std::optional<std::uint32_t>, std::less<>>
        indices_;
    std::map<std::pair<std::uint32_t, double>, std::uint32_t> fitted_;
    std::map<std::string, font::FontHintZones, std::less<>> zones_;
    /** What each glyph's fitted variants are made from, by its record index. */
    struct Source final {
        std::shared_ptr<const font::GlyphOutline> outline;
        std::uint16_t units_per_em = 0U;
        const font::FontHintZones* zones = nullptr;
    };
    std::map<std::uint32_t, Source> sources_;
    std::vector<VectorGlyphRecord> records_;
    std::vector<float> segments_;
    std::vector<std::uint32_t> bands_;
    /** Where the next append starts. */
    std::uint32_t appended_records_ = 0U;
    std::uint32_t appended_segments_ = 0U;
    std::uint32_t appended_bands_ = 0U;
    bool sent_ = false;
};

} // namespace strata::ui
