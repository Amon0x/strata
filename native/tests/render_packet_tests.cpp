#include <strata/render_packet.hpp>

#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string_view>

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
    result.opacity = 0.75;
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
    check(encoded.size() > 12U && encoded[8U] == 6U,
          "effect packet did not use render protocol v6");
    const host::RenderPacket packet = decoder.decode(encoded);
    check(packet.batches.size() == 4U, "effect packet changed its ordered batch count");
    const auto* backdrop = std::get_if<host::EffectBatch>(&packet.batches[0U]);
    const auto* content = std::get_if<host::EffectBatch>(&packet.batches[1U]);
    check(backdrop != nullptr && backdrop->kind == host::EffectBatchKind::backdrop &&
              backdrop->effect == "test:effect" && backdrop->parameter_count == 1U &&
              backdrop->parameters[0U] == 12.0,
          "backdrop effect batch lost its typed program state");
    check(content != nullptr && content->kind == host::EffectBatchKind::content_begin &&
              std::holds_alternative<host::DrawBatch>(packet.batches[2U]) &&
              std::holds_alternative<host::ContentEffectEndBatch>(packet.batches[3U]),
          "content effect isolation markers did not round-trip in order");
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
        effect_record + sizeof(std::uint32_t) + 4U * sizeof(std::uint32_t) + 8U * sizeof(double);
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

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count < 2)
            throw std::invalid_argument("resource root is required");
        test_effect_batches_round_trip(arguments[1]);
        test_unbalanced_content_effect_is_rejected(arguments[1]);
        test_repeated_epoch_still_validates_batches(arguments[1]);
        test_content_effect_depth_is_bounded(arguments[1]);
        test_repeated_epoch_rejects_batch_shape_changes(arguments[1]);
        std::cout << "strata_render_packet_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_render_packet_tests: " << error.what() << '\n';
        return 1;
    }
}
