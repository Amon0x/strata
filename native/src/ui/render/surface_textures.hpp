#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "resource/image.hpp"
#include "resource/runtime_images.hpp"

namespace strata::ui {

struct RenderSubmission;

/** One texture the host creates from this packet before drawing with it. */
struct TextureUpload final {
    const resource::TextureResourceDescriptor* descriptor = nullptr;
    std::span<const std::uint8_t> bytes;
};

/**
 * Every raster texture a Surface can draw: its static PNG images and the Runtime's published
 * images. Planning resolves ids against the whole table; a texture reaches the host only when a
 * planned batch samples it, again only when its image is replaced, and is released from the host
 * when its image is. A static image shadows a runtime image with the same id.
 */
class SurfaceTextures final {
  public:
    SurfaceTextures(std::string host_namespace,
                    std::vector<resource::EncodedTextureResource> static_textures);

    /** Adopts the runtime's images; true while the next packet must carry texture changes. */
    bool sync(const resource::RuntimeImageStore& images);
    [[nodiscard]] std::span<const resource::TextureResourceDescriptor> descriptors() const noexcept {
        return descriptors_;
    }
    /**
     * Plans this packet's uploads and releases for a submission planned against descriptors().
     * batches_changed is false only for a submission whose batches a committed packet carried.
     */
    void plan(const RenderSubmission& submission, bool batches_changed);
    [[nodiscard]] std::span<const TextureUpload> uploads() const noexcept { return uploads_; }
    [[nodiscard]] std::span<const std::string> releases() const noexcept { return releases_; }
    /** Records the planned operations once the host packet that carries them is retained. */
    void commit();
    /** Host ids the host currently holds, for the Surface's terminal release. */
    [[nodiscard]] std::vector<std::string> resident() const;

  private:
    struct Source final {
        std::span<const std::uint8_t> bytes;
        std::uint64_t revision = 0U;
    };
    void rebuild();
    void upload(std::size_t index);

    std::string host_namespace_;
    std::vector<resource::EncodedTextureResource> static_;
    /** Keeps the published pixels alive while they are planned for upload. */
    std::vector<std::shared_ptr<const resource::RuntimeImage>> runtime_;
    std::vector<resource::TextureResourceDescriptor> descriptors_;
    /** Parallel to descriptors_. */
    std::vector<Source> sources_;
    std::map<std::string, std::size_t, std::less<>> by_host_id_;
    /** Host id -> revision the host holds. */
    std::map<std::string, std::uint64_t, std::less<>> resident_;
    std::vector<TextureUpload> uploads_;
    std::vector<std::string> releases_;
    std::uint64_t store_generation_ = 0U;
    bool synced_ = false;
    bool changed_ = false;
};

} // namespace strata::ui
