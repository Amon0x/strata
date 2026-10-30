#include <strata/render_packet.hpp>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "compiler/module.hpp"
#include "font/atlas.hpp"
#include "resource/resource.hpp"
#include "resource/runtime_images.hpp"
#include "runtime/application.hpp"
#include "runtime/profiler.hpp"
#include "runtime/value.hpp"
#include "ui/render.hpp"
#include "ui/render/packet.hpp"
#include "ui/render/surface_packets.hpp"
#include "ui/surface.hpp"
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
    // Only a backdrop effect has a backdrop to measure.
    if (input == strata::ui::EffectInput::backdrop)
        result.probe = 0x1'0000'002AU;
    return result;
}

// Tone is defined once for every backend, so its fixed points are checked where it is defined.
constexpr bool toned(const float red, const float green, const float blue, const float tone,
                     const float expected_red, const float expected_green,
                     const float expected_blue) {
    float r = red;
    float g = green;
    float b = blue;
    strata::host::tone_color(r, g, b, tone);
    const auto near = [](const float left, const float right) {
        return left - right < 0.002F && right - left < 0.002F;
    };
    return near(r, expected_red) && near(g, expected_green) && near(b, expected_blue);
}
static_assert(toned(1.0F, 1.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F), "tone 0 must leave ink as authored");
static_assert(toned(1.0F, 1.0F, 1.0F, 1.0F, 0.043F, 0.071F, 0.125F),
              "white ink must mirror to dark ink at full tone");
static_assert(toned(0.04F, 0.07F, 0.13F, 1.0F, 0.04F, 0.07F, 0.13F), "dark ink must stay dark");
static_assert(toned(0.19F, 0.82F, 0.35F, 1.0F, 0.19F, 0.82F, 0.35F),
              "a saturated colour must keep its hue");

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
    const std::uint32_t group_count = u32(bytes, 56U);
    std::size_t offset = 60U + static_cast<std::size_t>(group_count) * 52U;
    for (std::uint32_t index = 0U; index < resource_count; ++index) {
        offset += sizeof(std::uint32_t);
        const std::uint32_t record_size = u32(bytes, offset);
        offset += sizeof(std::uint32_t) + record_size;
    }
    offset += vertex_bytes + static_cast<std::size_t>(index_count) * sizeof(std::uint32_t);
    // The material and effect id tables precede the batch records.
    for (int table = 0; table < 2; ++table) {
        const std::uint32_t count = u32(bytes, offset);
        offset += sizeof(std::uint32_t);
        for (std::uint32_t id = 0U; id < count; ++id)
            offset += sizeof(std::uint32_t) + u32(bytes, offset);
    }
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
          "effect packet did not use the current render protocol");
    const host::RenderPacket packet = decoder.decode(encoded);
    check(packet.batches.size() == 4U, "effect packet changed its ordered batch count");
    const auto* backdrop = std::get_if<host::EffectBatch>(&packet.batches[0U]);
    const auto* content = std::get_if<host::EffectBatch>(&packet.batches[1U]);
    check(backdrop != nullptr && backdrop->kind == host::EffectBatchKind::backdrop &&
              backdrop->backdrop_source == host::EffectBackdropSource::surface &&
              packet.effects[backdrop->effect] == "test:effect" &&
              backdrop->parameter_count == 1U &&
              backdrop->parameters[0U] == 12.0 && backdrop->refresh_rate == 120.0,
          "backdrop effect batch lost its typed program state");
    check(backdrop->probe == 0x1'0000'002AU && backdrop->tone == 0.0 && content != nullptr &&
              content->probe == 0U,
          "backdrop probe token did not round-trip on the backdrop effect alone");
    check(content != nullptr && content->kind == host::EffectBatchKind::content_begin &&
              std::holds_alternative<host::DrawBatch>(packet.batches[2U]) &&
              std::holds_alternative<host::ContentEffectEndBatch>(packet.batches[3U]),
          "content effect isolation markers did not round-trip in order");
}

/**
 * A content effect composited as a layer carries its operator and its built-in filters, their
 * lengths carried through the transform it is drawn under as its bounds are.
 */
void test_layer_composite_round_trip(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    ui::EffectState layer;
    layer.id = "strata:layer";
    layer.input = ui::EffectInput::content;
    layer.opacity = 0.5;
    layer.composite = ui::LayerComposite::multiply;
    layer.filters = {
        ui::LayerFilter{ui::LayerFilterKind::blur, {3.0, 0.0}},
        ui::LayerFilter{ui::LayerFilterKind::drop_shadow, {0.0, 0.0, 0.0, 0.5, 4.0, 2.0, 6.0, 0.0}},
        ui::LayerFilter{ui::LayerFilterKind::gradient_map,
                        {1.0, 2.0, 0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, 1.0}},
    };
    commands.append(ui::TransformPushRenderCommand{2.0, 0.0, 5.0, 0.0, 2.0, 7.0});
    commands.append(ui::ContentEffectPushRenderCommand{ui::Rect{0.0, 0.0, 40.0, 30.0}, {}, layer});
    commands.append(ui::SolidRectRenderCommand{ui::Rect{0.0, 0.0, 40.0, 30.0},
                                               ui::RenderColor{30U, 80U, 140U, 255U}});
    commands.append(ui::ContentEffectPopRenderCommand{});
    commands.append(ui::TransformPopRenderCommand{});

    host::RenderPacketDecoder decoder;
    const host::RenderPacket packet = decoder.decode(encode(commands, resources));
    const auto* content = std::get_if<host::EffectBatch>(&packet.batches.front());
    check(content != nullptr && content->kind == host::EffectBatchKind::content_begin &&
              content->composite == host::LayerComposite::multiply && content->opacity == 0.5 &&
              content->width == 80.0 && content->height == 60.0,
          "a layer's composite operator or bounds did not round-trip");
    check(content->filters.size() == 3U &&
              content->filters[0U].kind == host::LayerFilterKind::blur &&
              content->filters[0U].values == std::vector<double>{6.0, 0.0} &&
              content->filters[1U].kind == host::LayerFilterKind::drop_shadow &&
              content->filters[1U].value(4U) == 8.0 && content->filters[1U].value(5U) == 4.0 &&
              content->filters[1U].value(6U) == 12.0 &&
              content->filters[2U].kind == host::LayerFilterKind::gradient_map &&
              content->filters[2U].values.size() == 10U,
          "a layer's filters did not round-trip in presented lengths");
    check(content->filter_reach() == 12.0 + 8.0,
          "a layer's filter reach is not its widest blur or shadow");
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
        effect_record + 3U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    // The effect is named by a u16 table index and a reserved u16.
    const std::size_t opacity_offset = effect_id_length_offset + 2U * sizeof(std::uint16_t);
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
        effect_record + 3U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    // The effect is named by a u16 table index and a reserved u16.
    const std::size_t opacity_offset = effect_id_length_offset + 2U * sizeof(std::uint16_t);
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
        effect_record + 3U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    // The effect is named by a u16 table index and a reserved u16.
    const std::size_t opacity_offset = effect_id_length_offset + 2U * sizeof(std::uint16_t);
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
        effect_record + 3U * sizeof(std::uint32_t) +
        4U * sizeof(std::uint32_t) + 8U * sizeof(double);
    // The effect is named by a u16 table index and a reserved u16.
    const std::size_t opacity_offset = effect_id_length_offset + 2U * sizeof(std::uint16_t);
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

/**
 * An effect that does not ask for a refresh rate is evaluated every frame. A retained sample only
 * pays off when the display refreshes faster than the requested rate, so a default cap would make
 * every effect pay the cache copy and composite while almost never reusing it.
 */
void test_effect_refresh_defaults_to_unbounded(const std::filesystem::path& resources) {
    using namespace strata;
    check(host::default_effect_refresh_rate == 0.0 && host::EffectBatch{}.refresh_rate == 0.0,
          "decoded effect batches default to a rate-limited cache");
    ui::EffectState unspecified;
    unspecified.id = "test:effect";
    check(unspecified.refresh_rate == 0.0, "effect state defaults to a rate-limited cache");
    ui::RenderCommandBuffer commands;
    commands.append(ui::BackdropEffectRenderCommand{
        ui::Rect{10.0, 12.0, 80.0, 44.0},
        ui::CornerRadii::all(9.0),
        unspecified,
    });
    host::RenderPacketDecoder decoder;
    const host::RenderPacket& packet = decoder.decode(encode(commands, resources));
    const auto* batch = std::get_if<host::EffectBatch>(&packet.batches.at(0U));
    check(batch != nullptr && batch->refresh_rate == 0.0,
          "an effect without a refresh rate was encoded as rate limited");
}

/** Writes render packets byte by byte, for streams the UI encoder does not readily produce. */
class PacketWriter final {
  public:
    void u32(const std::uint32_t value) {
        for (std::uint32_t shift = 0U; shift < 32U; shift += 8U)
            bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
    void u64(const std::uint64_t value) {
        u32(static_cast<std::uint32_t>(value));
        u32(static_cast<std::uint32_t>(value >> 32U));
    }
    void text(const std::string_view value) {
        u32(static_cast<std::uint32_t>(value.size()));
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
    void record(const std::uint32_t kind, const PacketWriter& body) {
        u32(kind);
        u32(static_cast<std::uint32_t>(body.bytes.size()));
        bytes.insert(bytes.end(), body.bytes.begin(), body.bytes.end());
    }
    std::vector<std::uint8_t> bytes;
};

/** One textured quad: logical bounds (x, y, size) sampling texture UVs (u0, v0)-(u1, v1). */
struct TexturedQuad final {
    std::string texture;
    float x = 0.0F;
    float y = 0.0F;
    float size = 0.0F;
    float u0 = 0.0F;
    float v0 = 0.0F;
    float u1 = 1.0F;
    float v1 = 1.0F;
};

/**
 * A packet of textured quads. With `geometry`, a full payload for `epoch`; without, a retained
 * packet repeating it. Resources are (kind, writer of the record after its texture id).
 */
std::vector<std::uint8_t> textured_packet(
    const std::uint64_t epoch, const bool geometry, const std::vector<TexturedQuad>& quads,
    const std::vector<std::pair<std::uint32_t, PacketWriter>>& resources) {
    PacketWriter packet;
    packet.bytes.assign({'S', 'T', 'R', 'A', 'T', 'A', 'R', 'P'});
    packet.u32(STRATA_RENDER_PACKET_VERSION_CURRENT);
    packet.u32(static_cast<std::uint32_t>(resources.size()));
    packet.u32(geometry ? static_cast<std::uint32_t>(quads.size()) : 0U);
    packet.u64(epoch);
    packet.u64(epoch);
    packet.u32(geometry ? strata::host::packet_flag_geometry_payload : 0U);
    packet.u32(geometry ? static_cast<std::uint32_t>(quads.size() * 4U * 88U) : 0U);
    packet.u32(geometry ? static_cast<std::uint32_t>(quads.size() * 6U) : 0U);
    packet.u32(0U);
    packet.u32(0U);
    packet.u32(0U);
    for (const auto& [kind, body] : resources)
        packet.record(kind, body);
    if (!geometry)
        return packet.bytes;
    for (const TexturedQuad& quad : quads) {
        const std::array<std::array<float, 4U>, 4U> corners{{
            {quad.x, quad.y, quad.u0, quad.v0},
            {quad.x + quad.size, quad.y, quad.u1, quad.v0},
            {quad.x + quad.size, quad.y + quad.size, quad.u1, quad.v1},
            {quad.x, quad.y + quad.size, quad.u0, quad.v1},
        }};
        for (const std::array<float, 4U>& corner : corners) {
            std::array<std::uint8_t, 88U> vertex{};
            std::memcpy(vertex.data(), &corner[0U], sizeof(float));
            std::memcpy(vertex.data() + 4U, &corner[1U], sizeof(float));
            std::memcpy(vertex.data() + 12U, &corner[2U], sizeof(float));
            std::memcpy(vertex.data() + 16U, &corner[3U], sizeof(float));
            vertex[23U] = 255U;
            packet.bytes.insert(packet.bytes.end(), vertex.begin(), vertex.end());
        }
    }
    for (std::uint32_t quad = 0U; quad < quads.size(); ++quad) {
        for (const std::uint32_t index : {0U, 1U, 2U, 0U, 2U, 3U})
            packet.u32(index);
    }
    packet.u32(1U);
    packet.text("strata:unified_ui");
    packet.u32(0U);
    for (std::uint32_t quad = 0U; quad < quads.size(); ++quad) {
        PacketWriter draw;
        draw.u32(quad);
        for (const std::uint32_t value : {0U, 0U, 64U, 64U})
            draw.u32(value);
        draw.u32(0U);
        // Material 0, straight alpha, hard clip coverage.
        draw.bytes.insert(draw.bytes.end(), {0U, 0U, static_cast<std::uint8_t>(
                                                       STRATA_RENDER_BLEND_STRAIGHT_ALPHA),
                                             static_cast<std::uint8_t>(
                                                 STRATA_RENDER_CLIP_COVERAGE_HARD)});
        draw.u32(1U);
        draw.text(quads[quad].texture);
        draw.u32(quad * 4U);
        draw.u32(quad * 6U);
        draw.u32(6U);
        packet.record(0U, draw);
    }
    return packet.bytes;
}

PacketWriter atlas_record(const std::string_view texture, const std::uint32_t x,
                          const std::uint32_t y, const std::uint32_t width,
                          const std::uint32_t height, const bool upload) {
    PacketWriter record;
    record.text(texture);
    record.u32(strata::host::texture_format_rgba8);
    record.u32(x);
    record.u32(y);
    record.u32(width);
    record.u32(height);
    if (upload) {
        record.u32(width * height * 4U);
        record.bytes.resize(record.bytes.size() + static_cast<std::size_t>(width) * height * 4U,
                            0x7FU);
    }
    return record;
}

PacketWriter image_record(const std::string_view texture) {
    PacketWriter record;
    record.text(texture);
    record.u32(strata::host::texture_sampling_linear);
    record.u32(1U);
    record.u32(1U);
    record.u32(4U);
    record.bytes.insert(record.bytes.end(), {10U, 20U, 30U, 255U});
    return record;
}

/**
 * Replacing a texture changes what every draw sampling it shows, though no vertex moves. The
 * decoder reports those draws' bounds so a retained effect sample over them is refreshed, and only
 * those: an atlas upload into texels no draw samples leaves every sample current.
 */
void test_texture_changes_report_dirty_regions() {
    using namespace strata;
    const std::vector<TexturedQuad> quads{
        {"image", 4.0F, 4.0F, 8.0F, 0.0F, 0.0F, 1.0F, 1.0F},
        // Samples texels [0, 2) of an 8x8 atlas.
        {"atlas", 40.0F, 40.0F, 8.0F, 0.0F, 0.0F, 0.25F, 0.25F},
    };
    host::RenderPacketDecoder decoder;
    const host::RenderPacket& first = decoder.decode(textured_packet(
        1U, true, quads,
        {{host::resource_image, image_record("image")},
         {host::resource_create, atlas_record("atlas", 0U, 0U, 8U, 8U, false)}}));
    check(first.geometry_dirty_all, "a full geometry payload did not dirty everything");

    const auto covers = [](const host::GeometryDirtyRegion& region, const double x,
                           const double y, const double size) {
        return region.x <= x && region.y <= y && region.x + region.width >= x + size &&
               region.y + region.height >= y + size && region.x > x - 2.0 &&
               region.y > y - 2.0 && region.width < size + 4.0 && region.height < size + 4.0;
    };
    const host::RenderPacket& replaced = decoder.decode(textured_packet(
        1U, false, quads, {{host::resource_image, image_record("image")}}));
    check(!replaced.geometry_dirty_all && replaced.geometry_dirty_regions.size() == 1U &&
              covers(replaced.geometry_dirty_regions.front(), 4.0, 4.0, 8.0),
          "a replaced image did not dirty exactly the draw that samples it");

    const host::RenderPacket& unused_texels = decoder.decode(textured_packet(
        1U, false, quads,
        {{host::resource_upload, atlas_record("atlas", 5U, 5U, 3U, 3U, true)}}));
    check(!unused_texels.geometry_dirty_all && unused_texels.geometry_dirty_regions.empty(),
          "an atlas upload into texels no draw samples dirtied a draw");

    const host::RenderPacket& sampled_texels = decoder.decode(textured_packet(
        1U, false, quads,
        {{host::resource_upload, atlas_record("atlas", 1U, 1U, 1U, 1U, true)}}));
    check(!sampled_texels.geometry_dirty_all &&
              sampled_texels.geometry_dirty_regions.size() == 1U &&
              covers(sampled_texels.geometry_dirty_regions.front(), 40.0, 40.0, 8.0),
          "an atlas upload into sampled texels did not dirty the draw that samples them");

    const host::RenderPacket& unchanged = decoder.decode(textured_packet(1U, false, quads, {}));
    check(!unchanged.geometry_dirty_all && unchanged.geometry_dirty_regions.empty(),
          "a retained packet without resource changes reported dirty regions");
}

/**
 * A Surface frame becomes a complete host packet in the core: text reaches the glyph atlas, a
 * settled frame re-sends the retained epoch, the profiler records the packet, and the terminal
 * packet releases every atlas page the host was given.
 */
void test_surface_frames_encode_complete_packets(const std::filesystem::path& resources) {
    using namespace strata;
    runtime::ApplicationContext application("surface-packets",
                                            runtime::ApplicationBundle::create());
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{
            "surface-packets.strata",
            "overlay Main { root Panel(key: \"root\") { Text(text: \"Packets\") } }",
        },
        [](const std::string_view, const std::string_view path) -> compiler::ModuleSource {
            throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
        },
        0U);
    check(activation.activated(), "surface packet fixture did not activate");
    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 200;
    environment.logical_width = 320.0;
    environment.logical_height = 200.0;
    environment.reduced_motion = true;
    ui::Surface surface(
        "surface-packets", application, runtime::LayerRole::overlay, "Main", environment,
        ui::TextEngine::load_control_font(
            resources, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")));
    surface.profiler().set_capture_enabled(true);
    ui::SurfacePackets packets("strata:test/surface-packets", {});
    const resource::RuntimeImageStore images;
    host::RenderPacketDecoder decoder;

    const ui::SurfaceFrame first = surface.frame(1'000'000);
    const host::RenderPacket& full = decoder.decode(packets.encode(surface, first, images));
    check(full.frame_index == first.frame_index && full.full_geometry_payload &&
              !full.batches.empty() && !full.vertices.empty(),
          "the first Surface frame did not encode complete geometry");
    // Text is drawn from its outlines: its glyphs reach the host in the Surface's own buffers.
    std::vector<std::string> glyph_resources;
    for (const host::ResourceOperation& operation : full.resources) {
        if (operation.kind == host::resource_create ||
            operation.kind == host::resource_vector_glyphs) {
            glyph_resources.push_back(operation.texture);
        }
    }
    check(std::ranges::any_of(full.resources,
                              [](const host::ResourceOperation& operation) {
                                  return operation.kind == host::resource_vector_glyphs &&
                                         operation.texture.find("strata:test/surface-packets") !=
                                             std::string::npos &&
                                         !operation.vector_glyphs.records.empty();
                              }),
          "text did not append its glyphs to namespaced vector glyph buffers");
    const std::uint64_t epoch = full.geometry_epoch;
    const runtime::ProfilerSnapshot profile = surface.profiler().snapshot();
    check(std::ranges::any_of(profile.counters,
                              [](const runtime::ProfilerCounterSnapshot& counter) {
                                  return counter.counter == runtime::ProfilerCounter::packet_bytes &&
                                         counter.value > 0U;
                              }),
          "packet encoding was not recorded in the Surface profiler");

    const ui::SurfaceFrame settled = surface.frame(2'000'000);
    const host::RenderPacket& reused = decoder.decode(packets.encode(surface, settled, images));
    check(reused.frame_index == settled.frame_index && reused.geometry_epoch == epoch &&
              !reused.full_geometry_payload && packets.telemetry().geometry_reused,
          "a settled frame did not re-send the retained geometry epoch");

    const host::RenderPacket& terminal = decoder.decode(packets.prepare_release(surface));
    check(packets.release_prepared(), "the terminal packet was not recorded as prepared");
    for (const std::string& resource : glyph_resources) {
        check(std::ranges::any_of(terminal.resources,
                                  [&resource](const host::ResourceOperation& operation) {
                                      return operation.kind == host::resource_release &&
                                             operation.texture == resource;
                                  }),
              "the terminal packet did not release glyph resource " + resource);
    }
    check(&packets.prepare_release(surface) == &packets.packet(),
          "preparing the terminal packet twice did not return the same packet");
    bool refused = false;
    try {
        static_cast<void>(packets.encode(surface, surface.frame(3'000'000), images));
    } catch (const std::logic_error&) {
        refused = true;
    }
    check(refused, "a released Surface encoded another packet");
}

/**
 * The core resolves a draw's blend against its rounded clips and interns material ids; the
 * packet carries the result, so a backend applies it without a name or a rule of its own.
 */
void test_blend_and_clip_coverage_are_resolved_in_the_core(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    const auto material = [](const std::string_view blend) {
        ui::MaterialState state;
        state.id = "test:material";
        state.blend_mode = std::string(blend);
        return ui::MaterialPushRenderCommand{state};
    };
    const auto rect = [](const double x) {
        return ui::SolidRectRenderCommand{ui::Rect{x, 0.0, 10.0, 10.0},
                                          ui::RenderColor{10U, 20U, 30U, 255U}};
    };
    commands.append(material("opaque"));
    commands.append(rect(0.0));
    commands.append(ui::MaterialPopRenderCommand{});
    commands.append(ui::ClipPushRenderCommand{ui::Rect{0.0, 0.0, 200.0, 200.0},
                                              ui::CornerRadii::all(8.0)});
    commands.append(material("opaque"));
    commands.append(rect(20.0));
    commands.append(ui::MaterialPopRenderCommand{});
    commands.append(material("multiply"));
    commands.append(rect(40.0));
    commands.append(ui::MaterialPopRenderCommand{});
    commands.append(material("premultiplied_alpha"));
    commands.append(rect(60.0));
    commands.append(ui::MaterialPopRenderCommand{});
    commands.append(ui::ClipPopRenderCommand{});
    commands.append(material("multiply"));
    commands.append(rect(80.0));
    commands.append(ui::MaterialPopRenderCommand{});
    host::RenderPacketDecoder decoder;
    const host::RenderPacket packet = decoder.decode(encode(commands, resources));
    std::vector<std::pair<host::DrawBlend, host::ClipCoverage>> resolved;
    for (const host::SubmissionBatch& batch : packet.batches) {
        const auto* draw = std::get_if<host::DrawBatch>(&batch);
        check(draw != nullptr && packet.materials[draw->material] == "test:material",
              "a draw lost its interned material id");
        resolved.emplace_back(draw->blend, draw->clip_coverage);
    }
    check(packet.materials.size() == 1U, "a material id was interned more than once");
    using host::ClipCoverage;
    using host::DrawBlend;
    check(resolved == std::vector<std::pair<DrawBlend, ClipCoverage>>{
                          {DrawBlend::opaque, ClipCoverage::hard},
                          {DrawBlend::straight_alpha, ClipCoverage::straight_alpha},
                          {DrawBlend::clipped_multiply, ClipCoverage::multiply},
                          {DrawBlend::premultiplied_alpha, ClipCoverage::premultiplied_alpha},
                          {DrawBlend::multiply, ClipCoverage::hard},
                      },
          "the core did not adapt each blend to its rounded clips");
}

/** A typed field outside its domain is rejected, not reinterpreted. */
void test_typed_draw_fields_are_validated(const std::filesystem::path& resources) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::SolidRectRenderCommand{ui::Rect{0.0, 0.0, 10.0, 10.0},
                                               ui::RenderColor{10U, 20U, 30U, 255U}});
    const std::vector<std::uint8_t> encoded = encode(commands, resources);
    const std::vector<std::size_t> offsets = batch_kind_offsets(encoded);
    check(offsets.size() == 1U, "typed-field fixture changed its batch shape");
    // Kind, size, source order, scissor and the (empty) rounded clip count precede the material.
    const std::size_t material_offset = offsets.front() + 8U * sizeof(std::uint32_t);
    const auto rejects = [&encoded](const std::size_t offset, const std::uint8_t value) {
        std::vector<std::uint8_t> corrupted = encoded;
        corrupted[offset] = value;
        try {
            host::RenderPacketDecoder decoder;
            static_cast<void>(decoder.decode(corrupted));
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    check(!rejects(material_offset, 0U), "the typed-field fixture itself was rejected");
    check(rejects(material_offset, 1U), "a material index past the id table was accepted");
    check(rejects(material_offset + 2U, 7U), "an unknown draw blend was accepted");
    check(rejects(material_offset + 3U, 4U), "an unknown clip coverage was accepted");
}

/**
 * Whether a retained effect sample is current is decided once, by the decoder: the revision moves
 * when the effect changes or anything within its read reach does, and holds otherwise.
 */
void test_decoder_assigns_effect_sample_revisions(const std::filesystem::path& resources) {
    using namespace strata;
    const std::shared_ptr<const ui::TextEngine> text = ui::TextEngine::load_control_font(
        resources, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    font::GlyphAtlas atlas("sample-revision-test");
    ui::HostRenderPacketCache cache;
    host::RenderPacketDecoder decoder;
    ui::EffectState glass = effect(ui::EffectInput::backdrop);
    glass.read_reach = 10.0;
    std::uint64_t frame = 0U;
    const auto revision = [&](const ui::RenderColor far, const ui::RenderColor near,
                              const ui::EffectState& state) {
        ui::RenderCommandBuffer commands;
        // Far: 100px from the effect. Near: 6px right of its bounds, inside the 10px reach.
        commands.append(ui::SolidRectRenderCommand{ui::Rect{200.0, 150.0, 10.0, 10.0}, far});
        commands.append(ui::SolidRectRenderCommand{ui::Rect{86.0, 50.0, 10.0, 10.0}, near});
        commands.append(ui::BackdropEffectRenderCommand{ui::Rect{50.0, 50.0, 30.0, 30.0}, {}, state});
        const host::RenderPacket& packet = decoder.decode(
            cache.encode(commands, ++frame, {}, atlas, *text, 1.0, 320, 200, 320.0, 200.0));
        for (const host::SubmissionBatch& batch : packet.batches) {
            if (const auto* effect_batch = std::get_if<host::EffectBatch>(&batch))
                return std::pair{effect_batch->sample_revision, effect_batch->read_reach};
        }
        throw std::runtime_error("sample-revision fixture lost its effect");
    };
    const ui::RenderColor red{255U, 0U, 0U, 255U};
    const ui::RenderColor blue{0U, 0U, 255U, 255U};
    const auto [first, reach] = revision(red, red, glass);
    check(first != 0U && reach == 10.0, "the effect did not carry its revision and read reach");
    check(revision(red, red, glass).first == first, "an unchanged frame moved the revision");
    check(revision(blue, red, glass).first == first,
          "a change beyond the read reach moved the revision");
    const std::uint64_t near_change = revision(blue, blue, glass).first;
    check(near_change != first, "a change within the read reach kept the revision");
    glass.opacity = 0.5;
    check(revision(blue, blue, glass).first != near_change,
          "a change to the effect itself kept the revision");
}

} // namespace

int strata_test_render_packet(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 2)
            throw std::invalid_argument("resource root is required");
        test_effect_batches_round_trip(arguments[1]);
        test_layer_composite_round_trip(arguments[1]);
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
        test_effect_refresh_defaults_to_unbounded(arguments[1]);
        test_texture_changes_report_dirty_regions();
        test_surface_frames_encode_complete_packets(arguments[1]);
        test_blend_and_clip_coverage_are_resolved_in_the_core(arguments[1]);
        test_typed_draw_fields_are_validated(arguments[1]);
        test_decoder_assigns_effect_sample_revisions(arguments[1]);
        std::cout << "strata_render_packet_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_render_packet_tests: " << error.what() << '\n';
        return 1;
    }
}
