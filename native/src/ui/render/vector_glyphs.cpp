#include "ui/render/vector_glyphs.hpp"

#include <algorithm>
#include <memory>
#include <set>

#include "font/raster_pool.hpp"

namespace strata::ui {

std::shared_ptr<const font::GlyphOutline>
VectorGlyphPool::drawn_outline(const font::OpenTypeFont& font, const std::uint16_t glyph) {
    // A colour glyph's layers are drawn as an image; its own outline is only a fallback.
    std::shared_ptr<const font::GlyphOutline> outline =
        font.color_layers(glyph).empty() ? font.glyph_outline(glyph) : nullptr;
    return outline != nullptr && !outline->empty() ? outline : nullptr;
}

VectorGlyphPool::Levels VectorGlyphPool::build_levels(const font::GlyphOutline& outline,
                                                      const std::uint16_t units_per_em,
                                                      const font::FontHintZones& zones) {
    Levels levels;
    levels[0] = font::build_vector_glyph(outline, units_per_em, zones);
    if (levels[0].empty())
        return levels;
    // Coarser levels, each flattened twice as loosely, for the sizes a zoom shows it at where
    // the finest would read more segments than its pixels need.
    const double em = std::max(1.0, static_cast<double>(units_per_em));
    for (std::uint32_t level = 1U; level < vector_glyph_detail_levels; ++level) {
        levels[level] = font::build_vector_glyph(outline, units_per_em, zones,
                                                 em / 4096.0 * static_cast<double>(1U << level));
    }
    return levels;
}

std::optional<font::VectorGlyph> VectorGlyphPool::build_fitted(const Source& source,
                                                               const double scale) {
    // Never finer than the outline drawn at any size already is.
    const double em = std::max(1.0, static_cast<double>(source.units_per_em));
    const double tolerance = std::max(em / 4096.0, 1.0 / (32.0 * scale));
    const font::VectorGlyph vector =
        font::build_vector_glyph(*source.outline, source.units_per_em, *source.zones, tolerance);
    if (vector.empty())
        return std::nullopt;
    return font::fit_vector_glyph(vector, scale);
}

const font::FontHintZones& VectorGlyphPool::zones(const std::string_view font_id,
                                                  const font::OpenTypeFont& font) {
    auto zones = zones_.find(font_id);
    if (zones == zones_.end())
        zones = zones_.emplace(std::string(font_id), font::font_hint_zones(font)).first;
    return zones->second;
}

std::optional<std::uint32_t>
VectorGlyphPool::adopt(const std::string_view font_id, const std::uint16_t glyph,
                       std::shared_ptr<const font::GlyphOutline> outline,
                       const std::uint16_t units_per_em, const font::FontHintZones* const zones,
                       const Levels& levels) {
    std::optional<std::uint32_t> index;
    if (outline != nullptr && !levels[0].empty()) {
        index = add(levels[0]);
        sources_.emplace(*index, Source{std::move(outline), units_per_em, zones});
        for (std::uint32_t level = 1U; level < vector_glyph_detail_levels; ++level) {
            if (levels[level].empty())
                records_.push_back(records_[*index]);
            else
                static_cast<void>(add(levels[level]));
        }
    }
    indices_.emplace(std::make_tuple(std::string(font_id), glyph), index);
    return index;
}

std::optional<std::uint32_t> VectorGlyphPool::glyph(const std::string_view font_id,
                                                    const font::OpenTypeFont& font,
                                                    const std::uint16_t glyph) {
    if (const auto found = indices_.find(std::make_tuple(font_id, glyph)); found != indices_.end())
        return found->second;
    std::shared_ptr<const font::GlyphOutline> outline = drawn_outline(font, glyph);
    if (outline == nullptr)
        return adopt(font_id, glyph, nullptr, font.units_per_em(), nullptr, Levels{});
    const font::FontHintZones& font_zones = zones(font_id, font);
    const Levels levels = build_levels(*outline, font.units_per_em(), font_zones);
    return adopt(font_id, glyph, std::move(outline), font.units_per_em(), &font_zones, levels);
}

std::uint32_t VectorGlyphPool::fitted(const std::uint32_t index, const double scale) {
    const auto source = sources_.find(index);
    if (source == sources_.end() || !(scale > 0.0))
        return index;
    const auto key = std::make_pair(index, scale);
    if (const auto found = fitted_.find(key); found != fitted_.end())
        return found->second;
    const std::optional<font::VectorGlyph> vector = build_fitted(source->second, scale);
    const std::uint32_t result = vector.has_value() ? add(*vector) : index;
    fitted_.emplace(key, result);
    return result;
}

void VectorGlyphPool::prepare(const std::span<const VectorGlyphWant> wants) {
    // The glyphs new to the pool, each once, in the order wanted. Their outlines and zones are
    // read here: only the building, which reads nothing it shares, is spread over lanes.
    struct Build final {
        const VectorGlyphWant* want = nullptr;
        std::shared_ptr<const font::GlyphOutline> outline;
        const font::FontHintZones* zones = nullptr;
        Levels levels;
    };
    std::vector<Build> builds;
    std::set<std::tuple<std::string_view, std::uint16_t>> queued;
    for (const VectorGlyphWant& want : wants) {
        if (indices_.contains(std::make_tuple(want.font_id, want.glyph)) ||
            !queued.emplace(want.font_id, want.glyph).second) {
            continue;
        }
        Build build;
        build.want = &want;
        build.outline = drawn_outline(*want.font, want.glyph);
        if (build.outline != nullptr)
            build.zones = &zones(want.font_id, *want.font);
        builds.push_back(std::move(build));
    }
    // A glyph is five flattenings: a few are worth a lane.
    constexpr std::size_t parallel_glyphs = 4U;
    font::parallel_for(builds.size(), parallel_glyphs, [&builds](const std::size_t index) {
        Build& build = builds[index];
        if (build.outline != nullptr) {
            build.levels =
                build_levels(*build.outline, build.want->font->units_per_em(), *build.zones);
        }
    });
    for (Build& build : builds) {
        static_cast<void>(adopt(build.want->font_id, build.want->glyph, std::move(build.outline),
                                build.want->font->units_per_em(), build.zones, build.levels));
    }

    // Then the sizes the wanted glyphs are fitted at that are new.
    struct Fit final {
        std::uint32_t index = 0U;
        double scale = 0.0;
        const Source* source = nullptr;
        std::optional<font::VectorGlyph> vector;
    };
    std::vector<Fit> fits;
    std::set<std::pair<std::uint32_t, double>> queued_fits;
    for (const VectorGlyphWant& want : wants) {
        if (!(want.fit_scale > 0.0))
            continue;
        const auto found = indices_.find(std::make_tuple(want.font_id, want.glyph));
        if (found == indices_.end() || !found->second.has_value())
            continue;
        const auto key = std::make_pair(*found->second, want.fit_scale);
        const auto source = sources_.find(key.first);
        if (source == sources_.end() || fitted_.contains(key) || !queued_fits.insert(key).second)
            continue;
        fits.push_back(Fit{key.first, key.second, &source->second, std::nullopt});
    }
    constexpr std::size_t parallel_fits = 8U;
    font::parallel_for(fits.size(), parallel_fits, [&fits](const std::size_t index) {
        fits[index].vector = build_fitted(*fits[index].source, fits[index].scale);
    });
    for (const Fit& fit : fits) {
        fitted_.emplace(std::make_pair(fit.index, fit.scale),
                        fit.vector.has_value() ? add(*fit.vector) : fit.index);
    }
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
