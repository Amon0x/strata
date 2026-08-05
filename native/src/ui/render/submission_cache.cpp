#include "ui/render/submission.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

#include "font/atlas.hpp"
#include "ui/render/submission_internal.hpp"
#include "ui/text.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] bool requires_text_engine(const RenderCommandBuffer& commands) noexcept {
    for (const RenderCommand& command : commands.commands()) {
        if (std::holds_alternative<TextRunRenderCommand>(command)) return true;
    }
    return false;
}

} // namespace

RenderSubmissionCache::RenderSubmissionCache()
    : preparation_cache_(std::make_unique<submission_detail::PreparationCache>()) {}

RenderSubmissionCache::~RenderSubmissionCache() = default;

const RenderSubmission& RenderSubmissionCache::resolve(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine* const text_engine,
    const RenderSubmissionEnvironment& environment,
    const std::span<const resource::TextureResourceDescriptor> textures
) {
    const bool text_required = requires_text_engine(commands);
    if (text_required && text_engine == nullptr) {
        throw std::invalid_argument(
            "render submission contains text commands but has no TextEngine"
        );
    }
    const bool text_resources_match = text_engine == nullptr
        ? text_engine_ == nullptr
        : !glyph_atlas.reclamation_pending() && glyph_atlas_ == &glyph_atlas &&
          glyph_atlas_generation_ == glyph_atlas.generation() && text_engine_ == text_engine;
    if (submission_.has_value() && text_resources_match &&
        environment_ == environment &&
        commands_.commands() == commands.commands() &&
        std::ranges::equal(texture_descriptors_, textures)) {
        ++hits_;
        return *submission_;
    }

    if (!submission_.has_value()) submission_.emplace();
    submission_detail::update_cached(
        commands,
        glyph_atlas,
        text_engine,
        environment.display_scale,
        environment.framebuffer_width,
        environment.framebuffer_height,
        environment.logical_width,
        environment.logical_height,
        textures,
        *preparation_cache_,
        *submission_
    );
    commands_ = commands;
    environment_ = environment;
    texture_descriptors_.assign(textures.begin(), textures.end());
    glyph_atlas_ = text_engine != nullptr ? &glyph_atlas : nullptr;
    glyph_atlas_generation_ = text_engine != nullptr ? glyph_atlas.generation() : 0U;
    text_engine_ = text_engine;
    ++misses_;
    return *submission_;
}

const RenderSubmission& RenderSubmissionCache::resolve(
    const RenderCommandBuffer& commands,
    font::GlyphAtlas& glyph_atlas,
    const TextEngine& text_engine,
    const RenderSubmissionEnvironment& environment,
    const std::span<const resource::TextureResourceDescriptor> textures
) {
    return resolve(commands, glyph_atlas, &text_engine, environment, textures);
}

void RenderSubmissionCache::clear() noexcept {
    commands_.clear();
    environment_.reset();
    texture_descriptors_.clear();
    glyph_atlas_ = nullptr;
    glyph_atlas_generation_ = 0U;
    text_engine_ = nullptr;
    submission_.reset();
    preparation_cache_->clear();
}

std::size_t RenderSubmissionCache::hit_count() const noexcept { return hits_; }
std::size_t RenderSubmissionCache::miss_count() const noexcept { return misses_; }

} // namespace strata::ui
