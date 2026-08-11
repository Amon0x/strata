#include <strata/render_packet.hpp>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "font/atlas.hpp"
#include "resource/resource.hpp"
#include "runtime/value.hpp"
#include "ui/render.hpp"
#include "ui/render/packet.hpp"
#include "ui/text.hpp"

namespace {

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

strata::ui::EffectState effect(const strata::ui::EffectInput input) {
    strata::ui::EffectState result;
    result.id = "test:effect";
    result.input = input;
    if (input == strata::ui::EffectInput::backdrop) {
        result.backdrop_source = strata::ui::EffectBackdropSource::surface;
    }
    result.opacity = 0.75;
    result.refresh_rate = 120.0;
    result.parameters.push_back(
        strata::ui::MaterialParameter{"radius", strata::runtime::Value(12.0)});
    result.packed_parameters[0U] = 12.0;
    result.packed_parameter_count = 1U;
    return result;
}

std::vector<std::uint8_t> encode(const strata::ui::RenderCommandBuffer& commands,
                                 const std::filesystem::path& resources) {
    const std::shared_ptr<const strata::ui::TextEngine> text =
        strata::ui::TextEngine::load_control_font(
            resources, strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    strata::font::GlyphAtlas atlas("effect-packet-test");
    strata::ui::HostRenderPacketCache cache;
    return cache.encode(commands, 7U, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0);
}

std::uint32_t u32(const std::vector<std::uint8_t>& bytes, const std::size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint32_t)) {
        throw std::runtime_error("packet fixture offset is outside the encoded bytes");
    }
    std::uint32_t value = 0U;
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}

std::vector<std::size_t> batch_kind_offsets(const std::vector<std::uint8_t>& bytes) {
    const std::uint32_t resource_count = u32(bytes, 12U);
    const std::uint32_t batch_count = u32(bytes, 16U);
    const std::uint32_t vertex_bytes = u32(bytes, 40U);
    const std::uint32_t index_count = u32(bytes, 44U);
    std::size_t offset = 56U;
    for (std::uint32_t index = 0U; index < resource_count; ++index) {
        offset += sizeof(std::uint32_t);
        const std::uint32_t record_size = u32(bytes, offset);
        offset += sizeof(std::uint32_t) + record_size;
    }
    offset += vertex_bytes + static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);
    std::vector<std::size_t> result;
    result.reserve(batch_count);
    for (std::uint32_t index = 0U; index < batch_count; ++index) {
        result.push_back(offset);
        offset += sizeof(std::uint32_t);
        const std::uint32_t record_size = u32(bytes, offset);
        offset += sizeof(std::uint32_t) + record_size;
    }
    return result;
}

void test_effect_batches_round_trip(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{10.0, 12.0, 80.0, 44.0},
        ui::CornerRadii::all(9.0),
        effect(ui::EffectInput::backdrop),
    });
    commands.append(ui::ContentEffectPushRenderCommand{
        ui::Rect{20.0, 30.0, 100.0, 60.0},
        ui::CornerRadii::all(11.0),
        effect(ui::EffectInput::content),
    });
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{20.0, 30.0, 100.0, 60.0},
        ui::RenderColor{30U, 80U, 140U, 255U},
    });
    commands.append(ui::ContentEffectPopRenderCommand{});

    host::RenderPacketDecoder decoder;
    const std::vector<std::uint8_t> encoded = encode(commands, resources);
    check(encoded.size() > 12U &&
              encoded[8U] == static_cast<std::uint8_t>(STRATA_RENDER_PACKET_VERSION_CURRENT),
          "effect packet did not use render protocol v10");
    const host::RenderPacket packet = decoder.decode(encoded);
    check(packet.batches.size() == 4U, "effect packet changed its ordered batch count");
    const auto* backdrop = std::get_if<host::EffectBatch>(&packet.batches[0U]);
    const auto* content = std::get_if<host::EffectBatch>(&packet.batches[1U]);
    check(backdrop != nullptr && backdrop->kind == host::EffectBatchKind::backdrop &&
              backdrop->backdrop_source == host::EffectBackdropSource::surface &&
              backdrop->effect == "test:effect" && backdrop->parameter_count == 1U &&
              backdrop->parameters[0U] == 12.0 && backdrop->refresh_rate == 120.0,
          "backdrop effect batch lost its typed program state");
    check(content != nullptr && content->kind == host::EffectBatchKind::content_begin &&
              std::holds_alternative<host::DrawBatch>(packet.batches[2U]) &&
              std::holds_alternative<host::ContentEffectEndBatch>(packet.batches[3U]),
          "content effect isolation markers did not round-trip in order");
}

void test_rounded_clip_batches_round_trip(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::TransformPushRenderCommand{
        2.0, 0.0, 5.0,
        0.0, 0.5, 7.0,
    });
    commands.append(ui::ClipPushRenderCommand{
        ui::Rect{8.0, 10.0, 120.0, 80.0},
        ui::CornerRadii::all(14.0),
    });
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{8.0, 10.0, 120.0, 80.0},
        ui::RenderColor{30U, 80U, 140U, 255U},
    });
    commands.append(ui::BlurRegionRenderCommand{
        ui::Rect{8.0, 10.0, 120.0, 80.0},
        2.0,
        1U,
    });
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{8.0, 10.0, 120.0, 80.0},
        {},
        effect(ui::EffectInput::backdrop),
    });
    commands.append(ui::ContentEffectPushRenderCommand{
        ui::Rect{8.0, 10.0, 120.0, 80.0},
        {},
        effect(ui::EffectInput::content),
    });
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{8.0, 10.0, 120.0, 80.0},
        ui::RenderColor{50U, 90U, 160U, 255U},
    });
    commands.append(ui::ContentEffectPopRenderCommand{});
    commands.append(ui::ClipPopRenderCommand{});
    commands.append(ui::TransformPopRenderCommand{});

    host::RenderPacketDecoder decoder;
    const host::RenderPacket packet = decoder.decode(encode(commands, resources));
    check(packet.batches.size() == 6U,
          "rounded clipping introduced an unnecessary isolation batch");
    const auto* draw = std::get_if<host::DrawBatch>(&packet.batches.front());
    check(draw != nullptr && draw->rounded_clips.size() == 1U,
          "rounded clip did not remain attached to its descendant draw");
    const host::RoundedClip& clip = draw->rounded_clips.front();
    check(clip.x == 8.0 && clip.y == 10.0 && clip.width == 120.0 &&
              clip.height == 80.0 &&
              clip.radii == std::array<double, 4U>{14.0, 14.0, 14.0, 14.0} &&
              clip.inverse_transform ==
                  std::array<double, 6U>{0.5, 0.0, -2.5, 0.0, 2.0, -14.0},
          "rounded clip geometry did not survive submission packet planning");
    const auto* blur = std::get_if<host::BlurBatch>(&packet.batches[1U]);
    const auto* backdrop = std::get_if<host::EffectBatch>(&packet.batches[2U]);
    check(blur != nullptr && blur->rounded_clips == draw->rounded_clips &&
              backdrop != nullptr && backdrop->rounded_clips == draw->rounded_clips,
          "rounded clip state did not reach blur/effect batches");
    const auto* content = std::get_if<host::EffectBatch>(&packet.batches[3U]);
    const auto* content_draw = std::get_if<host::DrawBatch>(&packet.batches[4U]);
    check(content != nullptr && content->kind == host::EffectBatchKind::content_begin &&
              content->rounded_clips == draw->rounded_clips &&
              content_draw != nullptr && content_draw->rounded_clips.empty() &&
              std::holds_alternative<host::ContentEffectEndBatch>(packet.batches[5U]),
          "content isolation applied an inherited rounded clip to both input and composite");
}

void test_large_scale_rounded_clip_round_trip(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::TransformPushRenderCommand{
        100'000'000.0, 0.0, 0.0,
        0.0, 100'000'000.0, 0.0,
    });
    commands.append(ui::ClipPushRenderCommand{
        ui::Rect{0.0, 0.0, 0.000001, 0.000001},
        ui::CornerRadii::all(0.0000002),
    });
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{0.0, 0.0, 0.000001, 0.000001},
        ui::RenderColor{255U, 255U, 255U, 255U},
    });
    commands.append(ui::ClipPopRenderCommand{});
    commands.append(ui::TransformPopRenderCommand{});

    host::RenderPacketDecoder decoder;
    const host::RenderPacket packet = decoder.decode(encode(commands, resources));
    const auto* draw = packet.batches.size() == 1U
        ? std::get_if<host::DrawBatch>(&packet.batches.front())
        : nullptr;
    check(draw != nullptr && draw->rounded_clips.size() == 1U &&
              draw->rounded_clips.front().inverse_transform[0U] == 0.00000001 &&
              draw->rounded_clips.front().inverse_transform[4U] == 0.00000001,
          "packet decoder rejected or changed a valid large-scale affine clip");
}

void test_invalid_effect_refresh_rate_is_rejected(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{0.0, 0.0, 20.0, 20.0},
        {},
        effect(ui::EffectInput::backdrop),
    });
    std::vector<std::uint8_t> encoded = encode(commands, resources);
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == 1U, "refresh-rate fixture changed its batch shape");
    const std::size_t effect_record = offsets.front() + 2U * sizeof(std::uint32_t);
    const std::size_t effect_id_length_offset =
        effect_record + 2U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    const std::size_t opacity_offset =
        effect_id_length_offset + sizeof(std::uint32_t) + u32(encoded, effect_id_length_offset);
    const double invalid_refresh_rate = -1.0;
    std::memcpy(encoded.data() + opacity_offset + sizeof(double), &invalid_refresh_rate,
                sizeof(invalid_refresh_rate));
    bool rejected = false;
    try {
        host::RenderPacketDecoder decoder;
        static_cast<void>(decoder.decode(encoded));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "packet decoder accepted a negative effect refresh rate");
}

void test_invalid_effect_backdrop_source_is_rejected(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{0.0, 0.0, 20.0, 20.0},
        {},
        effect(ui::EffectInput::backdrop),
    });
    std::vector<std::uint8_t> encoded = encode(commands, resources);
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == 1U, "backdrop-source fixture changed its batch shape");
    const std::size_t effect_record = offsets.front() + 2U * sizeof(std::uint32_t);
    const std::size_t effect_id_length_offset =
        effect_record + 2U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    const std::size_t opacity_offset =
        effect_id_length_offset + sizeof(std::uint32_t) + u32(encoded, effect_id_length_offset);
    const std::uint32_t invalid_backdrop_source = 2U;
    std::memcpy(
        encoded.data() + opacity_offset + 2U * sizeof(double),
        &invalid_backdrop_source,
        sizeof(invalid_backdrop_source)
    );
    bool rejected = false;
    try {
        host::RenderPacketDecoder decoder;
        static_cast<void>(decoder.decode(encoded));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "packet decoder accepted an unknown effect backdrop source");
}

void test_content_effect_backdrop_source_is_rejected(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::ContentEffectPushRenderCommand{
        ui::Rect{0.0, 0.0, 20.0, 20.0},
        {},
        effect(ui::EffectInput::content),
    });
    commands.append(ui::ContentEffectPopRenderCommand{});
    std::vector<std::uint8_t> encoded = encode(commands, resources);
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == 2U, "content-source fixture changed its batch shape");
    const std::size_t effect_record = offsets.front() + 2U * sizeof(std::uint32_t);
    const std::size_t effect_id_length_offset =
        effect_record + 2U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    const std::size_t opacity_offset =
        effect_id_length_offset + sizeof(std::uint32_t) + u32(encoded, effect_id_length_offset);
    const std::uint32_t surface_backdrop_source =
        static_cast<std::uint32_t>(host::EffectBackdropSource::surface);
    std::memcpy(
        encoded.data() + opacity_offset + 2U * sizeof(double),
        &surface_backdrop_source,
        sizeof(surface_backdrop_source)
    );
    bool rejected = false;
    try {
        host::RenderPacketDecoder decoder;
        static_cast<void>(decoder.decode(encoded));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "packet decoder accepted SURFACE sampling on a content effect");
}

void test_unbalanced_content_effect_is_rejected(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::ContentEffectPushRenderCommand{
        ui::Rect{0.0, 0.0, 20.0, 20.0},
        {},
        effect(ui::EffectInput::content),
    });
    bool rejected = false;
    try {
        host::RenderPacketDecoder decoder;
        static_cast<void>(decoder.decode(encode(commands, resources)));
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "packet decoder accepted an unbalanced content effect stack");
}

void test_repeated_epoch_still_validates_batches(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::ContentEffectPushRenderCommand{
        ui::Rect{0.0, 0.0, 20.0, 20.0},
        {},
        effect(ui::EffectInput::content),
    });
    commands.append(ui::ContentEffectPopRenderCommand{});
    std::vector<std::uint8_t> encoded = encode(commands, resources);
    host::RenderPacketDecoder decoder;
    static_cast<void>(decoder.decode(encoded));
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == 2U, "repeated-epoch fixture changed its batch shape");
    const std::size_t effect_record = offsets.front() + 2U * sizeof(std::uint32_t);
    const std::size_t effect_id_length_offset =
        effect_record + 2U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    const std::size_t opacity_offset =
        effect_id_length_offset + sizeof(std::uint32_t) + u32(encoded, effect_id_length_offset);
    const double changed_opacity = 0.25;
    std::memcpy(encoded.data() + opacity_offset, &changed_opacity, sizeof(changed_opacity));
    const host::RenderPacket& changed = decoder.decode(encoded);
    const auto* changed_effect = std::get_if<host::EffectBatch>(&changed.batches.front());
    check(changed_effect != nullptr && changed_effect->opacity == changed_opacity,
          "repeated geometry epoch discarded a changed effect batch");
    const std::uint32_t unknown_kind = 99U;
    std::memcpy(encoded.data() + offsets.back(), &unknown_kind, sizeof(unknown_kind));
    bool rejected = false;
    try {
        static_cast<void>(decoder.decode(encoded));
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "repeated geometry epoch bypassed render batch validation");
}

void test_content_effect_depth_is_bounded(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    for (std::size_t depth = 0U; depth < host::maximum_content_effect_depth; ++depth) {
        commands.append(ui::ContentEffectPushRenderCommand{
            ui::Rect{0.0, 0.0, 20.0, 20.0},
            {},
            effect(ui::EffectInput::content),
        });
    }
    for (std::size_t depth = 0U; depth < host::maximum_content_effect_depth; ++depth) {
        commands.append(ui::ContentEffectPopRenderCommand{});
    }
    std::vector<std::uint8_t> encoded = encode(commands, resources);
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == host::maximum_content_effect_depth * 2U,
          "depth-limit fixture changed its balanced packet shape");
    const std::vector<std::uint8_t> extra_begin(
        encoded.begin() + static_cast<std::ptrdiff_t>(offsets[0U]),
        encoded.begin() + static_cast<std::ptrdiff_t>(offsets[1U]));
    encoded.insert(encoded.begin() +
                       static_cast<std::ptrdiff_t>(offsets[host::maximum_content_effect_depth]),
                   extra_begin.begin(), extra_begin.end());
    const std::uint32_t excessive_batch_count = static_cast<std::uint32_t>(offsets.size() + 1U);
    std::memcpy(encoded.data() + 16U, &excessive_batch_count, sizeof(excessive_batch_count));
    bool rejected = false;
    try {
        host::RenderPacketDecoder decoder;
        static_cast<void>(decoder.decode(encoded));
    } catch (const std::exception&) {
        rejected = true;
    }
    check(rejected, "packet decoder accepted excessive content-effect nesting");
}

void test_repeated_epoch_rejects_batch_shape_changes(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{0.0, 0.0, 20.0, 20.0},
        {},
        effect(ui::EffectInput::backdrop),
    });
    std::vector<std::uint8_t> encoded = encode(commands, resources);
    host::RenderPacketDecoder decoder;
    static_cast<void>(decoder.decode(encoded));
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == 1U, "batch-shape fixture changed its packet shape");
    const std::vector<std::uint8_t> duplicate(
        encoded.begin() + static_cast<std::ptrdiff_t>(offsets.front()), encoded.end());
    encoded.insert(encoded.end(), duplicate.begin(), duplicate.end());
    const std::uint32_t changed_batch_count = 2U;
    std::memcpy(encoded.data() + 16U, &changed_batch_count, sizeof(changed_batch_count));
    bool rejected = false;
    try {
        static_cast<void>(decoder.decode(encoded));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "repeated geometry epoch accepted a changed batch shape");
}

void test_incremental_geometry_patch_round_trip(const std::filesystem::path& resources) {
    using namespace strata;
    const std::shared_ptr<const ui::TextEngine> text =
        ui::TextEngine::load_control_font(
            resources,
            resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        );
    font::GlyphAtlas atlas("geometry-patch-test");
    ui::HostRenderPacketCache cache;
    host::RenderPacketDecoder decoder;
    ui::RenderCommandBuffer commands;
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{10.0, 20.0, 100.0, 50.0},
        ui::RenderColor{30U, 80U, 140U, 255U},
    });
    const std::vector<std::uint8_t> first_encoded =
        cache.encode(commands, 1U, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0);
    const host::RenderPacket first = decoder.decode(first_encoded);
    check(first.full_geometry_payload && first.vertex_patches.empty(),
          "initial render packet was not a complete geometry epoch");

    commands.clear();
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{14.0, 20.0, 100.0, 50.0},
        ui::RenderColor{30U, 80U, 140U, 255U},
    });
    const std::vector<std::uint8_t> patch_encoded =
        cache.encode(commands, 2U, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0);
    check((u32(patch_encoded, 36U) & host::packet_flag_geometry_patches) != 0U &&
              patch_encoded.size() < first_encoded.size(),
          "small geometry change was encoded as another full packet");
    std::vector<std::uint8_t> malformed = patch_encoded;
    malformed.push_back(0xFFU);
    bool rejected = false;
    try {
        static_cast<void>(decoder.decode(malformed));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "render decoder accepted trailing data after a geometry patch");
    const host::RenderPacket patched = decoder.decode(patch_encoded);
    check(!patched.full_geometry_payload && !patched.vertex_patches.empty() &&
              patched.index_patches.empty() && patched.vertices != first.vertices &&
              patched.indices == first.indices && !patched.geometry_dirty_all &&
              patched.geometry_dirty_regions.size() == 1U &&
              patched.geometry_dirty_regions.front().x == 10.0 &&
              patched.geometry_dirty_regions.front().y == 20.0 &&
              patched.geometry_dirty_regions.front().width == 104.0 &&
              patched.geometry_dirty_regions.front().height == 50.0,
          "retained geometry patch did not reconstruct the changed submission");

    const std::vector<std::uint8_t> reuse_encoded =
        cache.encode(commands, 3U, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0);
    const host::RenderPacket reused = decoder.decode(reuse_encoded);
    check(u32(reuse_encoded, 36U) == 0U && reused.vertex_patches.empty() &&
              reused.geometry_dirty_regions.empty() && !reused.geometry_dirty_all &&
              reused.vertices == patched.vertices && reused.geometry_epoch == patched.geometry_epoch,
          "compact reuse did not retain the patched geometry epoch");
}

void test_effect_batch_change_invalidates_retained_samples(
    const std::filesystem::path& resources
) {
    using namespace strata;
    const std::shared_ptr<const ui::TextEngine> text =
        ui::TextEngine::load_control_font(
            resources,
            resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        );
    font::GlyphAtlas atlas("effect-batch-patch-test");
    ui::HostRenderPacketCache cache;
    host::RenderPacketDecoder decoder;
    ui::RenderCommandBuffer commands;
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{0.0, 0.0, 100.0, 80.0},
        ui::RenderColor{30U, 80U, 140U, 255U},
    });
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{10.0, 12.0, 80.0, 44.0},
        ui::CornerRadii::all(9.0),
        effect(ui::EffectInput::backdrop),
    });
    static_cast<void>(decoder.decode(cache.encode(
        commands, 1U, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0
    )));

    ui::EffectState changed_effect = effect(ui::EffectInput::backdrop);
    changed_effect.opacity = 0.5;
    commands.clear();
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{0.0, 0.0, 100.0, 80.0},
        ui::RenderColor{30U, 80U, 140U, 255U},
    });
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{10.0, 12.0, 80.0, 44.0},
        ui::CornerRadii::all(9.0),
        std::move(changed_effect),
    });
    const host::RenderPacket& changed = decoder.decode(cache.encode(
        commands, 2U, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0
    ));
    check(
        !changed.full_geometry_payload && changed.geometry_dirty_all,
        "an effect-batch-only patch retained stale rate-limited samples"
    );
}

} // namespace

int strata_test_render_packet(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 2)
            throw std::invalid_argument("resource root is required");
        test_effect_batches_round_trip(arguments[1]);
        test_rounded_clip_batches_round_trip(arguments[1]);
        test_large_scale_rounded_clip_round_trip(arguments[1]);
        test_invalid_effect_refresh_rate_is_rejected(arguments[1]);
        test_invalid_effect_backdrop_source_is_rejected(arguments[1]);
        test_content_effect_backdrop_source_is_rejected(arguments[1]);
        test_unbalanced_content_effect_is_rejected(arguments[1]);
        test_repeated_epoch_still_validates_batches(arguments[1]);
        test_content_effect_depth_is_bounded(arguments[1]);
        test_repeated_epoch_rejects_batch_shape_changes(arguments[1]);
        test_incremental_geometry_patch_round_trip(arguments[1]);
        test_effect_batch_change_invalidates_retained_samples(arguments[1]);
        std::cout << "strata_render_packet_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_render_packet_tests: " << error.what() << '\n';
        return 1;
    }
}
