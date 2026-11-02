#include "ui/render/vector_glyphs.hpp"

#include <algorithm>
#include <memory>
#include <set>
#include <unordered_set>

#include "base/task_pool.hpp"

namespace strata::ui {

VectorGlyphPool::VectorGlyphPool(std::string id, base::TaskOwner* const tasks)
    : id_(std::move(id)), tasks_(tasks) {
    if (tasks_ == nullptr) {
        owned_tasks_.emplace(base::TaskExecution::deterministic);
        tasks_ = &*owned_tasks_;
    }
}

const font::FontHintZones& VectorGlyphPool::FontSource::zones() const {
    std::call_once(initialized, [this] { hints = font::font_hint_zones(font); });
    return hints;
}

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
    const std::uint16_t units_per_em = source.font->font.units_per_em();
    const double em = std::max(1.0, static_cast<double>(units_per_em));
    const double tolerance = std::max(em / 4096.0, 1.0 / (32.0 * scale));
    const font::VectorGlyph vector =
        font::build_vector_glyph(*source.outline, units_per_em, source.font->zones(), tolerance);
    if (vector.empty())
        return std::nullopt;
    return font::fit_vector_glyph(vector, scale);
}

std::shared_ptr<const VectorGlyphPool::FontSource>
VectorGlyphPool::font_source(const std::string_view font_id, const font::OpenTypeFont& font) {
    auto source = fonts_.find(font_id);
    if (source == fonts_.end()) {
        source = fonts_.emplace(std::string(font_id),
                                 std::make_shared<const FontSource>(font)).first;
    }
    return source->second;
}

std::optional<std::uint32_t>
VectorGlyphPool::adopt(const std::string_view font_id, const std::uint16_t glyph,
                       std::shared_ptr<const font::GlyphOutline> outline,
                       std::shared_ptr<const FontSource> font,
                       const Levels& levels) {
    std::optional<std::uint32_t> index;
    if (outline != nullptr && !levels[0].empty()) {
        index = add(levels[0]);
        sources_.emplace(*index, Source{std::move(outline), std::move(font)});
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

base::ResourceResult<std::uint32_t> VectorGlyphPool::glyph(
    const std::string_view font_id, const font::OpenTypeFont& font, const std::uint16_t glyph) {
    if (const auto found = indices_.find(std::make_tuple(font_id, glyph)); found != indices_.end())
        return {found->second, {}};
    auto preparing = preparing_glyphs_.find(std::make_tuple(font_id, glyph));
    if (preparing == preparing_glyphs_.end()) {
        const VectorGlyphWant want{font_id, &font, glyph, 0.0};
        prepare(std::span(&want, 1U));
        return this->glyph(font_id, font, glyph);
    }
    const std::vector<Built>* const ready = preparing->second.batch.get();
    if (ready == nullptr)
        return {std::nullopt, preparing->second.batch.readiness()};
    const Built& built = ready->at(preparing->second.index);
    const auto index = adopt(font_id, glyph, built.source.outline, built.source.font, built.levels);
    preparing_glyphs_.erase(preparing);
    return {index, {}};
}

base::ResourceResult<std::uint32_t> VectorGlyphPool::fitted(const std::uint32_t index,
                                                          const double scale) {
    const auto source = sources_.find(index);
    if (source == sources_.end() || !(scale > 0.0))
        return {index, {}};
    const auto key = std::make_pair(index, scale);
    if (const auto found = fitted_.find(key); found != fitted_.end())
        return {found->second, {}};
    if (!preparing_fits_.contains(key))
        prepare_fits(std::span(&key, 1U));
    const auto preparing = preparing_fits_.find(key);
    const auto* ready = preparing->second.batch.get();
    // The unfitted outline is valid while its grid-fitted variant prepares.
    if (ready == nullptr)
        return {index, preparing->second.batch.readiness()};
    const auto& vector = ready->at(preparing->second.index);
    const std::uint32_t result = vector.has_value() ? add(*vector) : index;
    fitted_.emplace(key, result);
    preparing_fits_.erase(preparing);
    return {result, {}};
}

void VectorGlyphPool::prepare(const std::span<const VectorGlyphWant> wants) {
    struct Input final {
        std::shared_ptr<const FontSource> font;
        std::uint16_t glyph = 0U;
    };
    std::vector<Input> inputs;
    std::vector<std::tuple<std::string, std::uint16_t>> keys;
    std::set<std::tuple<std::string_view, std::uint16_t>> queued;
    for (const VectorGlyphWant& want : wants) {
        if (indices_.contains(std::make_tuple(want.font_id, want.glyph)) ||
            preparing_glyphs_.contains(std::make_tuple(want.font_id, want.glyph)) ||
            !queued.emplace(want.font_id, want.glyph).second) {
            continue;
        }
        inputs.push_back(Input{font_source(want.font_id, *want.font), want.glyph});
        keys.emplace_back(want.font_id, want.glyph);
    }
    if (!inputs.empty()) {
        auto batch = base::Pending<std::vector<Built>>::start(
            *tasks_, base::TaskKind::vector_glyph, base::TaskPriority::frame_critical, id_,
            [inputs = std::move(inputs)](const base::TaskCancellation& cancellation) {
                std::vector<Built> result(inputs.size());
                base::parallel_for(inputs.size(), 4U, [&](const std::size_t index) {
                    if (cancellation.cancelled())
                        return;
                    const Input& input = inputs[index];
                    Built& built = result[index];
                    built.source = Source{drawn_outline(input.font->font, input.glyph), input.font};
                    if (built.source.outline != nullptr) {
                        built.levels = build_levels(*built.source.outline,
                                                     input.font->font.units_per_em(),
                                                     input.font->zones());
                    }
                });
                return result;
            });
        for (std::size_t index = 0U; index < keys.size(); ++index) {
            preparing_glyphs_.emplace(std::move(keys[index]), Preparation<Built>{batch, index});
        }
    }
    std::vector<std::pair<std::uint32_t, double>> fits;
    for (const VectorGlyphWant& want : wants) {
        const auto ready = glyph(want.font_id, *want.font, want.glyph);
        if (!(want.fit_scale > 0.0))
            continue;
        if (ready.value.has_value())
            fits.emplace_back(*ready.value, want.fit_scale);
    }
    prepare_fits(fits);
}

void VectorGlyphPool::prepare_fits(
    const std::span<const std::pair<std::uint32_t, double>> wants) {
    std::vector<std::pair<Source, double>> inputs;
    std::vector<std::pair<std::uint32_t, double>> keys;
    std::set<std::pair<std::uint32_t, double>> queued;
    for (const auto& key : wants) {
        const auto source = sources_.find(key.first);
        if (source == sources_.end() || fitted_.contains(key) || preparing_fits_.contains(key) ||
            !queued.insert(key).second)
            continue;
        inputs.emplace_back(source->second, key.second);
        keys.push_back(key);
    }
    if (inputs.empty())
        return;
    auto batch = base::Pending<std::vector<std::optional<font::VectorGlyph>>>::start(
        *tasks_, base::TaskKind::vector_glyph, base::TaskPriority::frame_critical, id_,
        [inputs = std::move(inputs)](const base::TaskCancellation& cancellation) {
            std::vector<std::optional<font::VectorGlyph>> result(inputs.size());
            base::parallel_for(inputs.size(), 8U, [&](const std::size_t index) {
                if (!cancellation.cancelled())
                    result[index] = build_fitted(inputs[index].first, inputs[index].second);
            });
            return result;
        });
    for (std::size_t index = 0U; index < keys.size(); ++index) {
        preparing_fits_.emplace(
            keys[index], Preparation<std::optional<font::VectorGlyph>>{batch, index});
    }
}

void VectorGlyphPool::retain_preparations(const std::span<const std::uint64_t> resources) {
    if (preparing_glyphs_.empty() && preparing_fits_.empty())
        return;
    std::unordered_set<std::uint64_t> retained;
    retained.insert(resources.begin(), resources.end());
    std::unordered_set<std::uint64_t> cancelled;
    const auto unused = [&](const auto& item) {
        const auto& batch = item.second.batch;
        if (retained.contains(batch.id()))
            return false;
        if (cancelled.insert(batch.id()).second)
            batch.cancel();
        return true;
    };
    std::erase_if(preparing_glyphs_, unused);
    std::erase_if(preparing_fits_, unused);
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
    for (const auto& [key, preparation] : preparing_glyphs_) {
        static_cast<void>(key);
        preparation.batch.cancel();
    }
    for (const auto& [key, preparation] : preparing_fits_) {
        static_cast<void>(key);
        preparation.batch.cancel();
    }
    preparing_glyphs_.clear();
    preparing_fits_.clear();
    indices_.clear();
    fitted_.clear();
    fonts_.clear();
    sources_.clear();
    records_.clear();
    segments_.clear();
    bands_.clear();
    resend();
    sent_ = false;
}

} // namespace strata::ui
