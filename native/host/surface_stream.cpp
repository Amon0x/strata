#include <strata/render_packet.hpp>

#include <stdexcept>

#include <strata/strata.hpp>

namespace strata::host {

SurfacePacketStream::SurfacePacketStream(Surface& surface)
    : SurfacePacketStream(surface.native_handle()) {}

SurfacePacketStream::SurfacePacketStream(strata_surface* const surface) : surface_(surface) {
    if (surface == nullptr) {
        throw std::invalid_argument("Surface packet stream requires a live Surface");
    }
}

SurfacePacketStream::~SurfacePacketStream() = default;

SurfacePacketCommit frame_surface(strata_surface* const surface,
                                   const std::int64_t time_nanoseconds) {
    if (surface == nullptr) {
        throw std::logic_error("Surface packet stream is not attached to a live Surface");
    }
    strata_surface_frame_info frame{};
    frame.struct_size = sizeof(frame);
    require_ok(
        strata_surface_frame(surface, time_nanoseconds, &frame),
        "surface frame"
    );
    strata_render_packet* raw = nullptr;
    require_ok(strata_surface_acquire_render_packet(surface, &raw), "render packet acquisition");
    return SurfacePacketCommit{frame, EncodedRenderPacket(raw)};
}

SurfacePacketFrame SurfacePacketStream::frame(const std::int64_t time_nanoseconds) {
    SurfacePacketCommit commit = frame_surface(surface_, time_nanoseconds);
    const RenderPacket& packet = decoder_.decode(commit.encoded.bytes());
    return SurfacePacketFrame{commit.surface, &packet, std::move(commit.encoded)};
}

const RenderPacket& SurfacePacketStream::prepare_release() {
    if (surface_ == nullptr) {
        throw std::logic_error("Surface packet stream is not attached to a live Surface");
    }
    strata_render_packet* raw = nullptr;
    require_ok(strata_surface_prepare_release_packet(surface_, &raw), "release packet preparation");
    const EncodedRenderPacket encoded(raw);
    return decoder_.decode(encoded.bytes());
}

void SurfacePacketStream::acknowledge_release() {
    if (surface_ == nullptr) {
        throw std::logic_error("Surface packet stream is not attached to a live Surface");
    }
    require_ok(
        strata_surface_acknowledge_release_packet(surface_),
        "surface release packet acknowledgement"
    );
}

bool SurfacePacketStream::matches(const Surface& surface) const noexcept {
    return matches(surface.native_handle());
}

bool SurfacePacketStream::matches(const strata_surface* const surface) const noexcept {
    return surface_ == surface;
}

strata_surface* SurfacePacketStream::native_surface() const noexcept {
    return surface_;
}

void SurfacePacketStream::reset() noexcept {
    decoder_.reset();
}

} // namespace strata::host
