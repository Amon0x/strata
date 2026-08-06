#include <strata/render_packet.hpp>

#include <stdexcept>

#include <strata/strata.hpp>

namespace strata::host {
namespace {

struct ByteTarget final {
    std::vector<std::uint8_t>* bytes = nullptr;
    bool failed = false;
};

void capture_bytes(
    void* const user_data,
    const strata_bytes_view bytes
) noexcept {
    auto& target = *static_cast<ByteTarget*>(user_data);
    try {
        if (bytes.size == 0U) {
            target.bytes->clear();
        } else {
            target.bytes->assign(bytes.data, bytes.data + bytes.size);
        }
    } catch (...) {
        target.failed = true;
    }
}

void read_bytes(
    strata_surface* const surface,
    std::vector<std::uint8_t>& bytes,
    const bool release
) {
    ByteTarget target{&bytes, false};
    const strata_bytes_sink sink{sizeof(strata_bytes_sink), &target, &capture_bytes};
    require_ok(
        release
            ? strata_surface_prepare_release_packet(surface, &sink)
            : strata_surface_read_render_packet(surface, &sink),
        release ? "surface release packet preparation" : "render packet read"
    );
    if (target.failed) throw std::bad_alloc();
}

} // namespace

SurfacePacketStream::SurfacePacketStream(Surface& surface)
    : SurfacePacketStream(surface.native_handle()) {}

SurfacePacketStream::SurfacePacketStream(strata_surface* const surface) : surface_(surface) {
    if (surface == nullptr) {
        throw std::invalid_argument("Surface packet stream requires a live Surface");
    }
}

SurfacePacketStream::~SurfacePacketStream() = default;

SurfacePacketFrame SurfacePacketStream::frame(const std::int64_t time_nanoseconds) {
    if (surface_ == nullptr) {
        throw std::logic_error("Surface packet stream is not attached to a live Surface");
    }
    strata_surface_frame_info frame{};
    frame.struct_size = sizeof(frame);
    require_ok(
        strata_surface_frame(surface_, time_nanoseconds, &frame),
        "surface frame"
    );
    read_bytes(surface_, bytes_, false);
    const RenderPacket& packet = decoder_.decode(bytes_);
    return SurfacePacketFrame{frame, &packet, bytes_.size()};
}

const RenderPacket& SurfacePacketStream::prepare_release() {
    if (surface_ == nullptr) {
        throw std::logic_error("Surface packet stream is not attached to a live Surface");
    }
    read_bytes(surface_, bytes_, true);
    return decoder_.decode(bytes_);
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
    bytes_.clear();
}

} // namespace strata::host
