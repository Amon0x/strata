#include "ui/render/surface_textures.hpp"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "resource/image_stream.hpp"
#include "ui/render/submission.hpp"

namespace strata::ui {

SurfaceTextures::SurfaceTextures(std::string host_namespace,
                                 std::vector<resource::TextureResource> static_textures)
    : host_namespace_(std::move(host_namespace)), static_(std::move(static_textures)) {
    rebuild();
}

bool SurfaceTextures::sync(const resource::RuntimeImageStore& images,
                           const resource::SvgImageRegistry* const registry) {
    // A streamed animation that moved to its next frame uploads it.
    for (auto& source : sources_) {
        if (source.stream != nullptr && source.revision != source.stream->revision()) {
            source.bytes = source.stream->pixels();
            source.revision = source.stream->revision();
            changed_ = true;
        }
    }
    const resource::LottieLibrary* const lottie = registry != nullptr ? registry->lottie() : nullptr;
    const std::uint64_t prepared_generation = registry != nullptr ? registry->texture_generation() : 0U;
    const std::uint64_t lottie_generation = lottie != nullptr ? lottie->image_generation() : 0U;
    if (synced_ && images.generation() == store_generation_ &&
        lottie_generation == lottie_generation_ && prepared_generation == prepared_generation_)
        return changed_;
    synced_ = true;
    store_generation_ = images.generation();
    lottie_generation_ = lottie_generation;
    prepared_generation_ = prepared_generation;
    prepared_.clear();
    if (registry != nullptr) {
        prepared_.reserve(registry->textures().size());
        for (const auto& texture : registry->textures()) {
            if (texture != nullptr)
                prepared_.push_back(texture);
        }
    }
    runtime_.clear();
    runtime_.reserve(images.images().size());
    for (const auto& [id, image] : images.images())
        runtime_.push_back(image);
    if (lottie != nullptr) {
        for (std::shared_ptr<const resource::RuntimeImage>& image : lottie->images())
            runtime_.push_back(std::move(image));
    }
    std::vector<resource::TextureResourceDescriptor> previous = std::move(descriptors_);
    rebuild(registry);
    changed_ = changed_ || previous != descriptors_;
    for (const auto& [host_id, revision] : resident_) {
        const auto found = by_host_id_.find(host_id);
        changed_ = changed_ || found == by_host_id_.end() ||
                   sources_[found->second].revision != revision;
    }
    return changed_;
}

void SurfaceTextures::rebuild(const resource::SvgImageRegistry* const registry) {
    descriptors_.clear();
    sources_.clear();
    by_host_id_.clear();
    descriptors_.reserve(static_.size() + prepared_.size() + runtime_.size());
    sources_.reserve(static_.size() + prepared_.size() + runtime_.size());
    const auto append = [this](const resource::TextureResource& texture) {
        descriptors_.push_back(texture.descriptor());
        if (texture.stream != nullptr) {
            sources_.push_back(Source{texture.stream->pixels(), texture.stream->revision(),
                                      texture.stream.get()});
        } else {
            sources_.push_back(Source{texture.pixels, 0U});
        }
    };
    for (const resource::TextureResource& texture : static_)
        append(texture);
    for (const auto& texture : prepared_)
        append(*texture);
    for (const std::shared_ptr<const resource::RuntimeImage>& image : runtime_) {
        const bool shadowed = std::ranges::any_of(static_, [&image](const auto& texture) {
            return texture.logical_id == image->id;
        });
        if (shadowed || (registry != nullptr && registry->declares(image->id)))
            continue;
        descriptors_.push_back(resource::TextureResourceDescriptor{
            image->id,
            host_namespace_ + "/image/" + image->id,
            image->sampling,
            resource::ImageEncoding::rgba8,
            image->dimensions,
        });
        sources_.push_back(Source{image->pixels, image->revision});
    }
    for (std::size_t index = 0U; index < descriptors_.size(); ++index)
        by_host_id_.emplace(descriptors_[index].host_id, index);
}

void SurfaceTextures::upload(const std::size_t index) {
    const resource::TextureResourceDescriptor* descriptor = &descriptors_[index];
    if (std::ranges::any_of(uploads_, [descriptor](const TextureUpload& upload) {
            return upload.descriptor == descriptor;
        })) {
        return;
    }
    const Source& source = sources_[index];
    if (source.bytes.size() != static_cast<std::size_t>(descriptor->dimensions.width) *
                                   descriptor->dimensions.height * 4U) {
        throw std::invalid_argument("image '" + descriptor->logical_id +
                                    "' has RGBA8 pixels that differ from its dimensions");
    }
    uploads_.push_back(TextureUpload{descriptor, source.bytes});
}

void SurfaceTextures::plan(const RenderSubmission& submission, const bool batches_changed) {
    uploads_.clear();
    releases_.clear();
    // Unchanged images leave every resident texture current.
    if (changed_) {
        for (const auto& [host_id, revision] : resident_) {
            const auto found = by_host_id_.find(host_id);
            if (found == by_host_id_.end())
                releases_.push_back(host_id);
            else if (sources_[found->second].revision != revision)
                upload(found->second);
        }
    }
    // Batches already committed sample only resident textures.
    if (!batches_changed)
        return;
    for (const SubmissionBatch& batch : submission.batches) {
        if (!batch.texture.has_value() || resident_.contains(*batch.texture))
            continue;
        if (const auto found = by_host_id_.find(*batch.texture); found != by_host_id_.end())
            upload(found->second);
    }
}

void SurfaceTextures::commit() {
    // Allocate new residency nodes before changing either existing revisions or pending operations.
    std::optional<decltype(resident_)> additions;
    for (const TextureUpload& upload : uploads_) {
        const std::string& id = upload.descriptor->host_id;
        if (resident_.contains(id) && !std::ranges::contains(releases_, id))
            continue;
        if (!additions.has_value()) additions.emplace();
        const auto index = static_cast<std::size_t>(upload.descriptor - descriptors_.data());
        additions->emplace(id, sources_[index].revision);
    }
    for (const std::string& host_id : releases_)
        resident_.erase(host_id);
    for (const TextureUpload& upload : uploads_) {
        const auto index = static_cast<std::size_t>(upload.descriptor - descriptors_.data());
        const auto existing = resident_.find(upload.descriptor->host_id);
        if (existing != resident_.end())
            existing->second = sources_[index].revision;
        else
            resident_.insert(additions->extract(upload.descriptor->host_id));
    }
    uploads_.clear();
    releases_.clear();
    changed_ = false;
}

std::vector<std::string> SurfaceTextures::resident() const {
    std::vector<std::string> result;
    result.reserve(resident_.size());
    for (const auto& [host_id, revision] : resident_)
        result.push_back(host_id);
    return result;
}

} // namespace strata::ui
