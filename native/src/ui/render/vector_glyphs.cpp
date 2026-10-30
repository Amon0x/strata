#include "ui/render/vector_glyphs.hpp"

#include <algorithm>
#include <memory>

namespace strata::ui {

std::optional<std::uint32_t> VectorGlyphPool::glyph(const std::string_view font_id,
                                                    const font::OpenTypeFont& font,
                                                    const std::uint16_t glyph) {
    if (const auto found = indices_.find(std::make_tuple(font_id, glyph)); found != indices_.end())
        return found->second;
    std::optional<std::uint32_t> index;
    // A colour glyph's layers are drawn as an image; its own outline is only a fallback.
    const std::shared_ptr<const font::GlyphOutline> outline =
        font.color_layers(glyph).empty() ? font.glyph_outline(glyph) : nullptr;
    if (outline != nullptr && !outline->empty()) {
        auto zones = zones_.find(font_id);
        if (zones == zones_.end())
            zones = zones_.emplace(std::string(font_id), font::font_hint_zones(font)).first;
        const font::VectorGlyph vector =
            font::build_vector_glyph(*outline, font.units_per_em(), zones->second);
        if (!vector.empty()) {
            index = add(vector);
            sources_.emplace(*index, Source{outline, font.units_per_em(), &zones->second});
        }
    }
    indices_.emplace(std::make_tuple(std::string(font_id), glyph), index);
    return index;
}

std::uint32_t VectorGlyphPool::fitted(const std::uint32_t index, const double scale) {
    const auto source = sources_.find(index);
    if (source == sources_.end() || !(scale > 0.0))
        return index;
    const auto key = std::make_pair(index, scale);
    if (const auto found = fitted_.find(key); found != fitted_.end())
        return found->second;
    // Never finer than the outline drawn at any size already is.
    const double em = std::max(1.0, static_cast<double>(source->second.units_per_em));
    const double tolerance = std::max(em / 4096.0, 1.0 / (32.0 * scale));
    const font::VectorGlyph vector = font::build_vector_glyph(
        *source->second.outline, source->second.units_per_em, *source->second.zones, tolerance);
    const std::uint32_t result =
        vector.empty() ? index : add(font::fit_vector_glyph(vector, scale));
    fitted_.emplace(key, result);
    return result;
}

std::uint32_t VectorGlyphPool::add(const font::VectorGlyph& vector) {
    VectorGlyphRecord record;
    record.left = vector.left;
    record.bottom = vector.bottom;
    record.right = vector.right;
    record.top = vector.top;
    record.segment_base = static_cast<std::uint32_t>(segments_.size() / 4U);
    record.band_base = static_cast<std::uint32_t>(bands_.size());
    record.band_count = vector.band_count();
    record.segment_count = static_cast<std::uint32_t>(vector.segments.size());
    // Its whole segments, then its bands' pieces; the band offsets count from the first piece.
    for (const font::VectorSegment& segment : vector.segments)
        segments_.insert(segments_.end(), {segment.x0, segment.y0, segment.x1, segment.y1});
    for (const font::VectorSegment& piece : vector.band_pieces)
        segments_.insert(segments_.end(), {piece.x0, piece.y0, piece.x1, piece.y1});
    bands_.insert(bands_.end(), vector.band_offsets.begin(), vector.band_offsets.end());
    records_.push_back(record);
    return static_cast<std::uint32_t>(records_.size() - 1U);
}

VectorGlyphAppend VectorGlyphPool::pending() const {
    VectorGlyphAppend append;
    append.first_record = appended_records_;
    append.records.assign(records_.begin() + appended_records_, records_.end());
    append.first_segment = appended_segments_;
    append.segments.assign(segments_.begin() + appended_segments_ * 4U, segments_.end());
    append.first_band = appended_bands_;
    append.bands.assign(bands_.begin() + appended_bands_, bands_.end());
    return append;
}

void VectorGlyphPool::commit() noexcept {
    if (appended_records_ != records_.size())
        sent_ = true;
    appended_records_ = static_cast<std::uint32_t>(records_.size());
    appended_segments_ = static_cast<std::uint32_t>(segments_.size() / 4U);
    appended_bands_ = static_cast<std::uint32_t>(bands_.size());
}

void VectorGlyphPool::resend() noexcept {
    appended_records_ = 0U;
    appended_segments_ = 0U;
    appended_bands_ = 0U;
}

void VectorGlyphPool::clear() noexcept {
    indices_.clear();
    fitted_.clear();
    zones_.clear();
    sources_.clear();
    records_.clear();
    segments_.clear();
    bands_.clear();
    resend();
    sent_ = false;
}

} // namespace strata::ui
