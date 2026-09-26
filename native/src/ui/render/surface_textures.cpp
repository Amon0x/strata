#include "ui/render/surface_textures.hpp"

#include <algorithm>
#include <utility>

#include "ui/render/submission.hpp"

namespace strata::ui {

SurfaceTextures::SurfaceTextures(std::string host_namespace,
                                 std::vector<resource::EncodedTextureResource> static_textures)
    : host_namespace_(std::move(host_namespace)), static_(std::move(static_textures)) {
    rebuild();
}

bool SurfaceTextures::sync(const resource::RuntimeImageStore& images) {
    if (synced_ && images.generation() == store_generation_)
        return changed_;
    synced_ = true;
    store_generation_ = images.generation();
    runtime_.clear();
    runtime_.reserve(images.images().size());
    for (const auto& [id, image] : images.images())
        runtime_.push_back(image);
    std::vector<resource::TextureResourceDescriptor> previous = std::move(descriptors_);
    rebuild();
    changed_ = changed_ || previous != descriptors_;
    for (const auto& [host_id, revision] : resident_) {
        const auto found = by_host_id_.find(host_id);
        changed_ = changed_ || found == by_host_id_.end() ||
                   sources_[found->second].revision != revision;
    }
    return changed_;
}

void SurfaceTextures::rebuild() {
    descriptors_.clear();
    sources_.clear();
    by_host_id_.clear();
    descriptors_.reserve(static_.size() + runtime_.size());
    sources_.reserve(static_.size() + runtime_.size());
    for (const resource::EncodedTextureResource& texture : static_) {
        descriptors_.push_back(texture.descriptor());
        sources_.push_back(Source{texture.bytes, 0U});
    }
    for (const std::shared_ptr<const resource::RuntimeImage>& image : runtime_) {
        const bool shadowed = std::ranges::any_of(static_, [&image](const auto& texture) {
            return texture.logical_id == image->id;
        });
        if (shadowed)
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
    uploads_.push_back(TextureUpload{descriptor, sources_[index].bytes});
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
    for (const std::string& host_id : releases_)
        resident_.erase(host_id);
    for (const TextureUpload& upload : uploads_) {
        const auto index = static_cast<std::size_t>(upload.descriptor - descriptors_.data());
        resident_.insert_or_assign(upload.descriptor->host_id, sources_[index].revision);
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
