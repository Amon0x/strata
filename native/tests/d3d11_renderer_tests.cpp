#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <strata/d3d11.hpp>
#include <strata/render_packet.hpp>

namespace {

using Microsoft::WRL::ComPtr;
using namespace strata;

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

void write_float(std::vector<std::uint8_t>& bytes, const std::size_t vertex,
                 const std::size_t offset, const float value) {
    std::memcpy(bytes.data() + vertex * 88U + offset, &value, sizeof(value));
}

/** A 4x4 opaque quad of one colour, as the packet decoder would hand it to the renderer. */
[[nodiscard]] host::RenderPacket quad(const std::uint64_t epoch, const std::uint8_t red,
                                      const std::uint8_t green, const std::uint8_t blue) {
    host::RenderPacket packet;
    packet.geometry_epoch = epoch;
    packet.vertices.resize(4U * 88U);
    constexpr float positions[4][2]{{0.0F, 0.0F}, {4.0F, 0.0F}, {4.0F, 4.0F}, {0.0F, 4.0F}};
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        write_float(packet.vertices, vertex, 0U, positions[vertex][0]);
        write_float(packet.vertices, vertex, 4U, positions[vertex][1]);
        packet.vertices[vertex * 88U + 20U] = red;
        packet.vertices[vertex * 88U + 21U] = green;
        packet.vertices[vertex * 88U + 22U] = blue;
        packet.vertices[vertex * 88U + 23U] = 255U;
        write_float(packet.vertices, vertex, 84U, 1.0F);
    }
    packet.indices = {0U, 1U, 2U, 0U, 2U, 3U};
    packet.batches.emplace_back(host::DrawBatch{
        .source_order = 0U,
        .scissor = host::Scissor{0U, 0U, 4U, 4U},
        .material = "strata:unified_ui",
        .blend_mode = "opaque",
        .base_vertex = 0U,
        .first_index = 0U,
        .index_count = 6U,
    });
    return packet;
}

struct Fixture final {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11RenderTargetView> view;
    ComPtr<ID3D11Texture2D> staging;

    Fixture() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0U, nullptr, 0U,
                                     D3D11_SDK_VERSION, &device, nullptr, &context))) {
            throw std::runtime_error("could not create a WARP device");
        }
        D3D11_TEXTURE2D_DESC descriptor{};
        descriptor.Width = 4U;
        descriptor.Height = 4U;
        descriptor.MipLevels = 1U;
        descriptor.ArraySize = 1U;
        descriptor.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        descriptor.SampleDesc.Count = 1U;
        descriptor.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &texture)) &&
                  SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &view)),
              "could not create the render target");
        descriptor.Usage = D3D11_USAGE_STAGING;
        descriptor.BindFlags = 0U;
        descriptor.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(SUCCEEDED(device->CreateTexture2D(&descriptor, nullptr, &staging)),
              "could not create the readback texture");
    }

    [[nodiscard]] d3d11::RenderTarget target() const {
        return d3d11::RenderTarget{texture.Get(), view.Get(), 4U, 4U, 4.0, 4.0};
    }

    [[nodiscard]] std::array<int, 4> center() const {
        context->CopyResource(staging.Get(), texture.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        check(SUCCEEDED(context->Map(staging.Get(), 0U, D3D11_MAP_READ, 0U, &mapped)),
              "could not read the render target back");
        const auto* pixel =
            static_cast<const std::uint8_t*>(mapped.pData) + 2U * mapped.RowPitch + 2U * 4U;
        const std::array<int, 4> result{pixel[0], pixel[1], pixel[2], pixel[3]};
        context->Unmap(staging.Get(), 0U);
        return result;
    }
};

// Geometry patches step from the previous epoch only. When a frame fails after the decoder has
// applied its epoch, the next patch packet must not be laid over the older buffer.
void test_geometry_after_a_failed_frame() {
    Fixture fixture;
    d3d11::Renderer renderer(fixture.device.Get(), fixture.context.Get());
    d3d11::FrameOptions options;
    options.load_action = d3d11::TargetLoadAction::clear;

    host::RenderPacket blue = quad(1U, 0U, 0U, 255U);
    blue.full_geometry_payload = true;
    blue.geometry_dirty_all = true;
    static_cast<void>(renderer.render("layer", blue, fixture.target(), options));
    check(fixture.center() == std::array<int, 4>{0, 0, 255, 255}, "the first frame was not drawn");

    host::RenderPacket green = quad(2U, 0U, 255U, 0U);
    green.vertex_patches.push_back(host::GeometryPatch{0U, green.vertices});
    host::ResourceOperation missing_atlas;
    missing_atlas.kind = host::resource_upload;
    missing_atlas.texture = "missing";
    missing_atlas.width = 1U;
    missing_atlas.height = 1U;
    missing_atlas.bytes = {0U};
    green.resources.push_back(missing_atlas);
    bool failed = false;
    try {
        static_cast<void>(renderer.render("layer", green, fixture.target(), options));
    } catch (const std::exception&) {
        failed = true;
    }
    check(failed, "the frame with an impossible upload did not fail");

    host::RenderPacket next = quad(3U, 0U, 255U, 0U);
    next.index_patches.push_back(host::GeometryPatch{0U, {0U, 0U, 0U, 0U}});
    static_cast<void>(renderer.render("layer", next, fixture.target(), options));
    check(fixture.center() == std::array<int, 4>{0, 255, 0, 255},
          "a patch after a failed frame was applied over stale geometry");
}

// An embedding engine's render thread may never have initialised COM; PNG textures still decode.
void test_png_upload_without_com() {
    Fixture fixture;
    static constexpr std::array<std::uint8_t, 70> png{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9C, 0x63, 0x10, 0x50, 0x30, 0xF8, 0x0F, 0x00, 0x02, 0x04, 0x01, 0x60, 0x8D, 0xBC,
        0xBB, 0x71, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
    };
    std::string failure;
    std::thread worker([&fixture, &failure] {
        try {
            d3d11::Renderer renderer(fixture.device.Get(), fixture.context.Get());
            host::RenderPacket packet;
            host::ResourceOperation image;
            image.kind = host::resource_encoded_image;
            image.texture = "image";
            image.format = host::texture_encoding_png;
            image.sampling = 1U;
            image.width = 1U;
            image.height = 1U;
            image.bytes.assign(png.begin(), png.end());
            packet.resources.push_back(image);
            renderer.consume_resources(packet);
        } catch (const std::exception& error) {
            failure = error.what();
        }
    });
    worker.join();
    check(failure.empty(), "a PNG upload on a thread without COM failed: " + failure);
}

} // namespace

int main() {
    try {
        test_geometry_after_a_failed_frame();
        test_png_upload_without_com();
        std::cout << "strata_d3d11_renderer_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_d3d11_renderer_tests: " << error.what() << '\n';
        return 1;
    }
}
