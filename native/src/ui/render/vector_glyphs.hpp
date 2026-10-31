#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
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

/** A glyph a frame draws from its outline, and the scale it is fitted at there (0: none). */
struct VectorGlyphWant final {
    std::string_view font_id;
    const font::OpenTypeFont* font = nullptr;
    std::uint16_t glyph = 0U;
    /** Device pixels a font unit spans, as VectorGlyphPool::fitted takes it. */
    double fit_scale = 0.0;
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
    /**
     * Makes the wanted glyphs and fitted variants the pool lacks, spread over the raster pool's
     * lanes (font::parallel_for), so glyph and fitted then find them: a page of new text builds
     * its outlines at once rather than one after another as planning asks. Records are added in
     * the order wanted, so the buffers are the same however the work was spread.
     */
    void prepare(std::span<const VectorGlyphWant> wants);
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
    /** A glyph's records, its own and its coarser levels; its own empty when it has none. */
    using Levels = std::array<font::VectorGlyph, vector_glyph_detail_levels>;
    struct Source;

    [[nodiscard]] static std::shared_ptr<const font::GlyphOutline>
    drawn_outline(const font::OpenTypeFont& font, std::uint16_t glyph);
    [[nodiscard]] static Levels build_levels(const font::GlyphOutline& outline,
                                             std::uint16_t units_per_em,
                                             const font::FontHintZones& zones);
    /** The variant of `source` fitted at `scale`; nothing when its outline flattens to none. */
    [[nodiscard]] static std::optional<font::VectorGlyph> build_fitted(const Source& source,
                                                                       double scale);
    [[nodiscard]] const font::FontHintZones& zones(std::string_view font_id,
                                                   const font::OpenTypeFont& font);
    /** Adds a glyph's levels and indexes it; nothing when it has no records. */
    std::optional<std::uint32_t> adopt(std::string_view font_id, std::uint16_t glyph,
                                       std::shared_ptr<const font::GlyphOutline> outline,
                                       std::uint16_t units_per_em,
                                       const font::FontHintZones* zones, const Levels& levels);
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
