#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <strata/svg.hpp>

#include "data/json.hpp"
#include "font/atlas.hpp"
#include "font/opentype.hpp"
#include "font/raster.hpp"
#include "resource/image.hpp"
#include "resource/resource.hpp"
#include "runtime/application.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/behavior/input.hpp"
#include "ui/behavior/registry.hpp"
#include "ui/collection/model.hpp"
#include "ui/description.hpp"
#include "ui/inspection.hpp"
#include "ui/layout.hpp"
#include "ui/motion/catalog.hpp"
#include "ui/motion/players.hpp"
#include "ui/render.hpp"
#include "ui/render/packet.hpp"
#include "ui/render/paint_geometry.hpp"
#include "ui/render/path_geometry.hpp"
#include "ui/render/submission.hpp"
#include "ui/scheduler.hpp"
#include "ui/surface.hpp"
#include "ui/svg_image.hpp"
#include "ui/text.hpp"
#include "ui/tree.hpp"
#include "ui/widget/input.hpp"
#include "ui/widget/registry.hpp"

namespace {

static_assert(!std::is_copy_constructible_v<strata::ui::DescriptionMaterialization>);
static_assert(!std::is_copy_assignable_v<strata::ui::DescriptionMaterialization>);
static_assert(std::is_nothrow_move_constructible_v<strata::ui::DescriptionMaterialization>);
static_assert(!std::is_move_assignable_v<strata::ui::DescriptionMaterialization>);

void check(const bool condition, const std::string_view message) {
    if (!condition)
        throw std::runtime_error(std::string(message));
}

std::shared_ptr<const strata::ui::DescriptionChildren>
children(std::vector<std::shared_ptr<const strata::ui::DescriptionNode>> values) {
    return std::make_shared<const strata::ui::EagerDescriptionChildren>(std::move(values));
}

std::shared_ptr<const strata::ui::DescriptionNode>
node(std::string type, std::optional<std::string> key,
     std::vector<std::shared_ptr<const strata::ui::DescriptionNode>> nested = {},
     strata::ui::DescriptionNode::Properties properties = {}) {
    return strata::ui::DescriptionNode::create(std::move(type), std::move(key), "/fixture",
                                               "screen Main", std::move(properties),
                                               children(std::move(nested)));
}

strata::runtime::Value
object(std::initializer_list<std::pair<std::string, strata::runtime::Value>> fields) {
    return strata::runtime::Value(
        std::vector<std::pair<std::string, strata::runtime::Value>>(fields));
}

strata::ui::DescriptionNode::Properties layout_properties(strata::runtime::Value value) {
    return {{"$layout", strata::runtime::ExpressionValue(std::move(value))}};
}

void check_near(const double actual, const double expected, const std::string_view message) {
    if (std::abs(actual - expected) > 0.000'001) {
        throw std::runtime_error(std::string(message) + ": expected " + std::to_string(expected) +
                                 ", got " + std::to_string(actual));
    }
}

[[nodiscard]] bool rendered_text(const strata::ui::RenderCommandBuffer& commands,
                                 const std::string_view expected) {
    return std::ranges::any_of(commands.commands(), [expected](const auto& command) {
        const auto* text = std::get_if<strata::ui::TextRunRenderCommand>(&command);
        if (text == nullptr || text->glyphs.size() != expected.size())
            return false;
        for (std::size_t index = 0U; index < expected.size(); ++index) {
            if (text->glyphs[index].code_point !=
                static_cast<std::uint32_t>(static_cast<unsigned char>(expected[index]))) {
                return false;
            }
        }
        return true;
    });
}

[[nodiscard]] const strata::data::JsonValue*
semantic_child_named(const strata::data::JsonValue* node, const std::string_view name) {
    const strata::data::JsonValue* children = node != nullptr ? node->find("children") : nullptr;
    if (children == nullptr || children->array() == nullptr)
        return nullptr;
    const auto found = std::ranges::find_if(*children->array(), [name](const auto& child) {
        const strata::data::JsonValue* child_name = child.find("name");
        return child_name != nullptr && child_name->string() != nullptr &&
               *child_name->string() == name;
    });
    return found != children->array()->end() ? &*found : nullptr;
}

void test_collection_models() {
    using namespace strata::ui::collection;
    const std::vector<std::string> ordered{"zeta", "alpha", "gamma", "beta"};
    KeySet current(std::vector<std::string>{"zeta", "beta"});
    const SelectionTransition toggled =
        select(ordered, current, std::string("zeta"), "alpha", SelectionMode::multiple,
               SelectionModifiers{true, false});
    check(toggled.selected.values() == std::vector<std::string>({"zeta", "beta", "alpha"}),
          "collection toggle lost stable insertion order");
    const SelectionTransition ranged =
        select(ordered, toggled.selected, std::string("zeta"), "gamma", SelectionMode::multiple,
               SelectionModifiers{false, true});
    check(ranged.selected.values() == std::vector<std::string>({"zeta", "alpha", "gamma"}),
          "collection range selection did not follow source order");
    check(navigate(ordered, std::string("alpha"), Navigation::next, 10U, 2U) == "beta" &&
              navigate(ordered, std::string("gamma"), Navigation::first) == "zeta" &&
              navigate(ordered, std::string("gamma"), Navigation::last) == "beta",
          "shared collection navigation changed");
    const std::vector<Label> labels{
        {"zeta", "Zulu"},
        {"alpha", "Alpha"},
        {"gamma", "Gamma"},
        {"beta", "Beta"},
    };
    check(typeahead(labels, std::string("gamma"), "a") == "alpha",
          "collection typeahead did not wrap from the active item");
    check_near(reveal_offset(30.0, 100.0, 8.0, 28.0, 12.0), 0.0,
               "collection reveal leading inset changed");
    check_near(reveal_offset(30.0, 100.0, 150.0, 180.0), 80.0,
               "collection reveal trailing edge changed");
}

void test_bundled_font_metrics(const std::filesystem::path& resource_root) {
    const std::filesystem::path& resources = resource_root;
    const strata::font::OpenTypeFont font =
        strata::font::OpenTypeFont::parse(strata::resource::load_binary_resource(
            resources, strata::resource::ResourceId::parse("assets/strata/fonts/medium.ttf")));
    check(font.units_per_em() == 2'048U, "bundled medium font unitsPerEm changed");
    const strata::font::TextMetrics count = font.measure_utf8("Count 0", 12.0);
    const strata::font::TextMetrics increment = font.measure_utf8("Increment", 12.0);
    const strata::font::TextMetrics enabled = font.measure_utf8("Enabled", 12.0);
    check_near(count.width, 41.794921875, "bundled Count 0 shaping width changed");
    check_near(increment.width, 54.45703125, "bundled Increment GPOS width changed");
    check_near(enabled.width, 42.919921875, "bundled Enabled shaping width changed");
    check_near(count.height, 14.0625, "bundled control line height changed");

    const strata::font::TextMetrics missing = font.measure_utf8("▾  Expanded section", 12.0);
    const strata::font::TextMetrics omitted = font.measure_utf8("  Expanded section", 12.0);
    check_near(missing.width, omitted.width, "missing glyph advanced as .notdef");
    check(missing.glyph_count == omitted.glyph_count, "missing glyph was counted as resolved");
    const strata::font::ShapedText shaped = font.shape_utf8("▾  Expanded section", 12.0);
    check(!shaped.glyphs.empty(), "missing glyph suppressed the resolved suffix");
    check(shaped.glyphs.front().code_point == static_cast<std::uint32_t>('E'),
          "missing glyph emitted .notdef");
    check(shaped.glyphs.front().text_start_offset == 3U,
          "missing glyph lost canonical UTF-16 offsets");

    const std::uint16_t latin_a = font.glyph_id(static_cast<std::uint32_t>('A'));
    check(latin_a != 0U, "bundled font no longer maps Latin A");
    const std::shared_ptr<const strata::font::GlyphOutline> latin_a_outline =
        font.glyph_outline(latin_a);
    check(latin_a_outline != nullptr && !latin_a_outline->empty(),
          "native glyf decoder lost Latin A");
    check(latin_a_outline->bounds.has_value(), "native glyf decoder lost Latin A bounds");
    check(latin_a_outline == font.glyph_outline(latin_a),
          "native glyf decoder did not retain its per-font outline cache");
    const std::optional<strata::font::GlyphRasterBitmap> latin_a_raster =
        strata::font::rasterize_coverage(font, latin_a, 12.0, 1.0);
    check(latin_a_raster.has_value(), "native coverage rasterizer lost Latin A");
    check(latin_a_raster->bytes.size() ==
              static_cast<std::size_t>(latin_a_raster->width) * latin_a_raster->height,
          "native coverage raster byte count does not match its dimensions");
    check(std::ranges::any_of(latin_a_raster->bytes,
                              [](const std::uint8_t value) { return value != 0U; }),
          "native coverage raster emitted a transparent Latin A");
    const std::uint16_t latin_h = font.glyph_id(static_cast<std::uint32_t>('H'));
    check(latin_h != 0U, "bundled font no longer maps Latin H");
    strata::font::CoverageRasterConfig unhinted_coverage;
    unhinted_coverage.hinting = strata::font::GlyphHinting::none;
    strata::font::CoverageRasterConfig hinted_coverage;
    hinted_coverage.hinting = strata::font::GlyphHinting::full;
    const std::optional<strata::font::GlyphRasterBitmap> unhinted_h =
        strata::font::rasterize_coverage(font, latin_h, 11.3, 1.0,
                                         strata::font::SubpixelPhase{1U, 4U}, unhinted_coverage);
    const std::optional<strata::font::GlyphRasterBitmap> hinted_h =
        strata::font::rasterize_coverage(font, latin_h, 11.3, 1.0,
                                         strata::font::SubpixelPhase{1U, 4U}, hinted_coverage);
    const std::optional<strata::font::GlyphRasterBitmap> other_phase_h =
        strata::font::rasterize_coverage(font, latin_h, 11.3, 1.0,
                                         strata::font::SubpixelPhase{3U, 4U}, hinted_coverage);
    check(unhinted_h.has_value() && hinted_h.has_value() && other_phase_h.has_value(),
          "FreeType coverage backend rejected a valid hinted stem fixture");
    check(unhinted_h->bytes != hinted_h->bytes,
          "native TrueType hinting did not alter the small-pixel stem fixture");
    check(hinted_h->bytes != other_phase_h->bytes ||
              hinted_h->plane_bounds_layout_pixels.left !=
                  other_phase_h->plane_bounds_layout_pixels.left,
          "hinted coverage rasterization collapsed distinct horizontal subpixel phases");
    strata::font::CoverageRasterConfig undarkened_coverage;
    undarkened_coverage.stem_darkening_pixels = 0.0;
    undarkened_coverage.transfer_strength = 0.0;
    strata::font::CoverageRasterConfig darkened_coverage = undarkened_coverage;
    darkened_coverage.stem_darkening_pixels = 0.20;
    const auto undarkened_h = strata::font::rasterize_coverage(
        font, latin_h, 11.3, 1.0, strata::font::SubpixelPhase{2U, 4U}, undarkened_coverage);
    const auto darkened_h = strata::font::rasterize_coverage(
        font, latin_h, 11.3, 1.0, strata::font::SubpixelPhase{2U, 4U}, darkened_coverage);
    check(undarkened_h.has_value() && darkened_h.has_value(),
          "small-pixel optical darkening rejected a valid stem fixture");
    const auto coverage_sum = [](const strata::font::GlyphRasterBitmap& raster) {
        std::uint64_t sum = 0U;
        for (const std::uint8_t value : raster.bytes)
            sum += value;
        return sum;
    };
    check(coverage_sum(*darkened_h) > coverage_sum(*undarkened_h),
          "small-pixel optical darkening did not increase stem coverage");
    const std::optional<strata::font::GlyphRasterBitmap> latin_a_msdf =
        strata::font::rasterize_msdf(font, latin_a, 32.0);
    check(latin_a_msdf.has_value(), "native MSDF rasterizer lost Latin A");
    check(latin_a_msdf->bytes.size() ==
              static_cast<std::size_t>(latin_a_msdf->width) * latin_a_msdf->height * 4U,
          "native MSDF raster byte count does not match RGBA dimensions");
    bool has_multichannel_distance = false;
    for (std::size_t index = 0U; index < latin_a_msdf->bytes.size(); index += 4U) {
        if (latin_a_msdf->bytes[index] != latin_a_msdf->bytes[index + 1U] ||
            latin_a_msdf->bytes[index + 1U] != latin_a_msdf->bytes[index + 2U]) {
            has_multichannel_distance = true;
            break;
        }
    }
    check(has_multichannel_distance, "native MSDF raster collapsed to a monochrome SDF");
    strata::font::GlyphAtlas atlas("ui-test-surface");
    const std::optional<strata::font::GlyphAtlasEntry> first_entry =
        atlas.request_coverage("strata:fonts/default-medium", font, latin_a, 12.0, {});
    check(first_entry.has_value(), "native glyph atlas rejected a valid coverage glyph");
    strata::font::GlyphAtlas explicit_mode_atlas("ui-test-explicit-raster-mode");
    const auto default_large_entry =
        explicit_mode_atlas.request("strata:fonts/default-medium", font, latin_a, 72.0, {});
    const auto explicit_msdf_entry =
        explicit_mode_atlas.request("strata:fonts/default-medium", font, latin_a, 72.0, {}, 0U,
                                    strata::font::GlyphRasterMode::msdf);
    check(default_large_entry.has_value() && explicit_msdf_entry.has_value() &&
              default_large_entry->mode == strata::font::GlyphRasterMode::coverage &&
              explicit_msdf_entry->mode == strata::font::GlyphRasterMode::msdf,
          "glyph size implicitly selected MSDF or explicit MSDF opt-in was ignored");
    const std::optional<strata::font::GlyphAtlasEntry> cached_entry =
        atlas.request_coverage("strata:fonts/default-medium", font, latin_a, 12.0, {});
    check(cached_entry.has_value() && cached_entry->texture == first_entry->texture &&
              cached_entry->u == first_entry->u && cached_entry->v == first_entry->v,
          "native glyph atlas cache changed a stable entry");
    const std::vector<strata::font::AtlasOperation> atlas_operations = atlas.take_operations();
    check(atlas_operations.size() == 2U, "native glyph atlas did not plan one create and upload");
    check(atlas_operations[0].kind == strata::font::AtlasOperationKind::create &&
              atlas_operations[1].kind == strata::font::AtlasOperationKind::upload,
          "native glyph atlas resource operations are not dependency ordered");
    check(atlas.take_operations().empty(),
          "native glyph atlas replayed consumed upload operations");
    atlas.adopt_display_scale(2.0);
    const std::vector<strata::font::AtlasOperation> scale_operations = atlas.take_operations();
    check(scale_operations.size() == 1U &&
              scale_operations.front().kind == strata::font::AtlasOperationKind::release,
          "native glyph atlas did not release scale-dependent coverage pages");

    const std::uint16_t latin_b = font.glyph_id(static_cast<std::uint32_t>('B'));
    check(latin_b != 0U, "bundled font lost the batched atlas fixture");
    strata::font::GlyphAtlas batched_atlas("ui-test-batched-atlas");
    check(!batched_atlas.begin_frame_preparation(),
          "empty batched atlas unexpectedly advanced at frame preparation");
    const std::array<strata::font::GlyphAtlasWarmupRequest, 2U> batched_requests{
        strata::font::GlyphAtlasWarmupRequest{
            "strata:fonts/default-medium",
            &font,
            latin_a,
            12.0,
            {},
            0U,
            strata::font::GlyphRasterMode::coverage,
        },
        strata::font::GlyphAtlasWarmupRequest{
            "strata:fonts/default-medium",
            &font,
            latin_b,
            12.0,
            {},
            0U,
            strata::font::GlyphRasterMode::coverage,
        },
    };
    check(batched_atlas.warm(batched_requests),
          "batched atlas rejected valid grayscale warmup requests");
    const std::vector<strata::font::AtlasOperation> batched_operations =
        batched_atlas.take_operations();
    check(batched_operations.size() == 2U &&
              batched_operations[0].kind == strata::font::AtlasOperationKind::create &&
              batched_operations[1].kind == strata::font::AtlasOperationKind::upload &&
              !batched_operations[1].bytes.empty(),
          "frame warmup did not consolidate grayscale glyphs into one page upload");
    batched_atlas.end_frame_preparation();

    std::vector<strata::font::GlyphAtlasWarmupRequest> parallel_requests;
    for (std::uint32_t code_point = 'D'; code_point <= 'T'; ++code_point) {
        const std::uint16_t glyph = font.glyph_id(code_point);
        check(glyph != 0U, "bundled font lost a parallel atlas fixture glyph");
        parallel_requests.push_back(strata::font::GlyphAtlasWarmupRequest{
            "strata:fonts/default-medium",
            &font,
            glyph,
            13.0,
            {},
            0U,
            strata::font::GlyphRasterMode::coverage,
        });
    }
    strata::font::GlyphAtlas parallel_atlas("ui-test-parallel-atlas");
    check(!parallel_atlas.begin_frame_preparation(),
          "empty parallel atlas unexpectedly advanced at frame preparation");
    check(parallel_atlas.warm(parallel_requests) &&
              parallel_atlas.cached_glyph_count() == parallel_requests.size(),
          "bounded parallel warmup lost a coverage glyph");
    const std::vector<strata::font::AtlasOperation> parallel_operations =
        parallel_atlas.take_operations();
    check(parallel_operations.size() == 2U &&
              parallel_operations[0].kind == strata::font::AtlasOperationKind::create &&
              parallel_operations[1].kind == strata::font::AtlasOperationKind::upload &&
              !parallel_operations[1].bytes.empty(),
          "bounded parallel warmup did not publish one coherent atlas upload");
    parallel_atlas.end_frame_preparation();

    const std::uint16_t latin_c = font.glyph_id(static_cast<std::uint32_t>('C'));
    check(latin_c != 0U, "bundled font lost atlas lifecycle fixtures");
    strata::font::GlyphAtlasConfig bounded_config;
    bounded_config.maximum_cached_glyphs = 1U;
    strata::font::GlyphAtlas bounded_atlas("ui-test-bounded-atlas", bounded_config);
    check(bounded_atlas.request_coverage("strata:fonts/default-medium", font, latin_a, 12.0, {})
              .has_value(),
          "bounded atlas rejected its initial glyph");
    const std::uint64_t bounded_generation = bounded_atlas.generation();
    try {
        static_cast<void>(
            bounded_atlas.request_coverage("strata:fonts/default-medium", font, latin_b, 12.0, {}));
        check(false, "bounded atlas recycled while published geometry could reference its pages");
    } catch (const strata::font::FontError&) {
    }
    check(bounded_atlas.reclamation_pending() && bounded_atlas.generation() == bounded_generation,
          "bounded atlas capacity pressure mutated a generation outside safe preparation");
    check(bounded_atlas.begin_frame_preparation() &&
              bounded_atlas.generation() == bounded_generation + 1U,
          "bounded atlas did not reclaim its scheduled generation at frame preparation");
    check(bounded_atlas.request_coverage("strata:fonts/default-medium", font, latin_b, 12.0, {})
              .has_value(),
          "bounded atlas did not reuse reclaimed capacity during warmup");
    bounded_atlas.end_frame_preparation();
    check(!bounded_atlas.frame_preparation_active() &&
              bounded_atlas.generation_recycle_count() == 1U,
          "bounded atlas did not close or account for its preparation phase");

    strata::font::GlyphAtlas one_recycle_atlas("ui-test-one-recycle-atlas", bounded_config);
    check(!one_recycle_atlas.begin_frame_preparation(),
          "empty atlas unexpectedly advanced at frame preparation");
    check(one_recycle_atlas.request_coverage("strata:fonts/default-medium", font, latin_a, 12.0, {})
                  .has_value() &&
              one_recycle_atlas
                  .request_coverage("strata:fonts/default-medium", font, latin_b, 12.0, {})
                  .has_value(),
          "atlas did not admit one bounded in-preparation recycle");
    const std::uint64_t recycled_generation = one_recycle_atlas.generation();
    try {
        static_cast<void>(one_recycle_atlas.request_coverage("strata:fonts/default-medium", font,
                                                             latin_c, 12.0, {}));
        check(false, "atlas exceeded its bounded per-preparation recycle budget");
    } catch (const strata::font::FontError&) {
    }
    check(one_recycle_atlas.reclamation_pending() &&
              one_recycle_atlas.generation() == recycled_generation,
          "atlas mutated after exhausting its preparation recycle budget");
    one_recycle_atlas.end_frame_preparation();

    const std::uint16_t e_acute = font.glyph_id(0x00E9U);
    if (e_acute != 0U) {
        const std::shared_ptr<const strata::font::GlyphOutline> compound =
            font.glyph_outline(e_acute);
        check(compound != nullptr && !compound->empty(),
              "native glyf decoder lost a composite glyph");
    }
}

void test_svg_image_projection_and_compound_fill() {
    using namespace strata::ui;
    const strata::svg::Document document = strata::svg::parse(R"SVG(
<svg width="20" height="10" viewBox="0 0 10 10">
  <rect width="10" height="10" fill="currentColor"/>
</svg>)SVG");
    std::vector<RenderCommand> commands;
    append_svg_image(commands, document, Rect{0.0, 0.0, 100.0, 50.0}, TextureRegion{},
                     RenderColor{70U, 120U, 240U, 200U}, 0.5);
    check(commands.size() == 5U &&
              std::holds_alternative<ClipPushRenderCommand>(commands.front()) &&
              std::holds_alternative<TransformPushRenderCommand>(commands[1]) &&
              std::holds_alternative<TransformPopRenderCommand>(commands[3]) &&
              std::holds_alternative<ClipPopRenderCommand>(commands.back()),
          "SVG image projection did not preserve its viewport clip and transform");
    const auto* projected = std::get_if<PathRenderCommand>(&commands[2]);
    check(projected != nullptr && projected->shape.fill.has_value(),
          "SVG image projection did not emit vector path geometry");
    const RenderColor projected_color = projected->shape.fill->representative();
    check(projected_color == RenderColor{70U, 120U, 240U, 100U},
          "SVG currentColor did not resolve through Image tint and opacity");
    const auto* transform = std::get_if<TransformPushRenderCommand>(&commands[1]);
    check(transform != nullptr && std::abs(transform->m00 - 5.0) < 1.0e-9 &&
              std::abs(transform->m11 - 5.0) < 1.0e-9 && std::abs(transform->m02 - 25.0) < 1.0e-9,
          "SVG default preserveAspectRatio did not center the viewBox");

    Path compound;
    compound.move_to(Point{0.0, 0.0});
    compound.line_to(Point{1.0, 0.0});
    compound.line_to(Point{1.0, 1.0});
    compound.line_to(Point{0.0, 1.0});
    compound.close();
    compound.move_to(Point{0.25, 0.25});
    compound.line_to(Point{0.75, 0.25});
    compound.line_to(Point{0.75, 0.75});
    compound.line_to(Point{0.25, 0.75});
    compound.close();
    const PaintMesh mesh = tessellate_shape(
        PathShape{
            std::move(compound),
            Paint(RenderColor{255U, 255U, 255U, 255U}),
            std::nullopt,
            std::nullopt,
            PathFillRule::evenodd,
        },
        Size{100.0, 100.0}, 1.0);
    const auto covered = [](const PaintMesh& candidate, const Point point) {
        const auto cross = [](const Point first, const Point second, const Point third) {
            return (second.x - first.x) * (third.y - first.y) -
                   (second.y - first.y) * (third.x - first.x);
        };
        for (std::size_t index = 0U; index + 2U < candidate.indices.size(); index += 3U) {
            const Point a = candidate.vertices[candidate.indices[index]].normalized;
            const Point b = candidate.vertices[candidate.indices[index + 1U]].normalized;
            const Point c = candidate.vertices[candidate.indices[index + 2U]].normalized;
            const double first = cross(a, b, point);
            const double second = cross(b, c, point);
            const double third = cross(c, a, point);
            if (!((first < 0.0 || second < 0.0 || third < 0.0) &&
                  (first > 0.0 || second > 0.0 || third > 0.0))) {
                return true;
            }
        }
        return false;
    };
    check(covered(mesh, Point{0.1, 0.1}) && !covered(mesh, Point{0.5, 0.5}),
          "compound even-odd path tessellation filled its hole");

    Path open_triangle;
    open_triangle.move_to(Point{0.0, 0.0});
    open_triangle.line_to(Point{1.0, 0.0});
    open_triangle.line_to(Point{0.5, 1.0});
    const PaintMesh open_mesh = tessellate_shape(
        PathShape{
            std::move(open_triangle),
            Paint(RenderColor{255U, 255U, 255U, 255U}),
            std::nullopt,
            std::nullopt,
            PathFillRule::nonzero,
        },
        Size{100.0, 100.0}, 1.0);
    check(covered(open_mesh, Point{0.5, 0.25}),
          "an open filled SVG subpath was not implicitly closed");

    const auto overlapping_rectangles = [] {
        Path path;
        path.move_to(Point{0.0, 0.0});
        path.line_to(Point{0.75, 0.0});
        path.line_to(Point{0.75, 1.0});
        path.line_to(Point{0.0, 1.0});
        path.close();
        path.move_to(Point{0.25, 0.0});
        path.line_to(Point{1.0, 0.0});
        path.line_to(Point{1.0, 1.0});
        path.line_to(Point{0.25, 1.0});
        path.close();
        return path;
    };
    const PaintMesh overlap_evenodd = tessellate_shape(
        PathShape{
            overlapping_rectangles(),
            Paint(RenderColor{255U, 255U, 255U, 255U}),
            std::nullopt,
            std::nullopt,
            PathFillRule::evenodd,
        },
        Size{100.0, 100.0}, 1.0);
    const PaintMesh overlap_nonzero = tessellate_shape(
        PathShape{
            overlapping_rectangles(),
            Paint(RenderColor{255U, 255U, 255U, 255U}),
            std::nullopt,
            std::nullopt,
            PathFillRule::nonzero,
        },
        Size{100.0, 100.0}, 1.0);
    check(covered(overlap_evenodd, Point{0.1, 0.5}) && !covered(overlap_evenodd, Point{0.5, 0.5}),
          "intersecting even-odd subpaths did not cancel their overlap");
    check(covered(overlap_nonzero, Point{0.5, 0.5}),
          "intersecting nonzero subpaths lost their overlap");

    Path bow_tie;
    bow_tie.move_to(Point{0.0, 0.0});
    bow_tie.line_to(Point{1.0, 1.0});
    bow_tie.line_to(Point{0.0, 1.0});
    bow_tie.line_to(Point{1.0, 0.0});
    bow_tie.close();
    const PaintMesh bow_tie_mesh = tessellate_shape(
        PathShape{
            std::move(bow_tie),
            Paint(RenderColor{255U, 255U, 255U, 255U}),
            std::nullopt,
            std::nullopt,
            PathFillRule::evenodd,
        },
        Size{100.0, 100.0}, 1.0);
    check(covered(bow_tie_mesh, Point{0.5, 0.2}) && covered(bow_tie_mesh, Point{0.5, 0.8}) &&
              !covered(bow_tie_mesh, Point{0.1, 0.5}),
          "self-intersecting even-odd path tessellation changed its winding regions");

    Path translucent_corner;
    translucent_corner.move_to(Point{0.2, 0.2});
    translucent_corner.line_to(Point{0.8, 0.2});
    translucent_corner.line_to(Point{0.8, 0.8});
    const PaintMesh translucent_stroke = tessellate_shape(
        PathShape{
            std::move(translucent_corner),
            std::nullopt,
            Paint(RenderColor{255U, 255U, 255U, 128U}),
            StrokeStyle{20.0, PathCap::butt, PathJoin::round, 4.0},
        },
        Size{100.0, 100.0}, 1.0);
    const auto coverage_count = [](const PaintMesh& candidate, const Point point) {
        const auto cross = [](const Point first, const Point second, const Point third) {
            return (second.x - first.x) * (third.y - first.y) -
                   (second.y - first.y) * (third.x - first.x);
        };
        std::size_t result = 0U;
        for (std::size_t index = 0U; index + 2U < candidate.indices.size(); index += 3U) {
            const Point a = candidate.vertices[candidate.indices[index]].normalized;
            const Point b = candidate.vertices[candidate.indices[index + 1U]].normalized;
            const Point c = candidate.vertices[candidate.indices[index + 2U]].normalized;
            const double first = cross(a, b, point);
            const double second = cross(b, c, point);
            const double third = cross(c, a, point);
            if (!((first < 0.0 || second < 0.0 || third < 0.0) &&
                  (first > 0.0 || second > 0.0 || third > 0.0))) {
                ++result;
            }
        }
        return result;
    };
    check(coverage_count(translucent_stroke, Point{0.74, 0.26}) == 1U,
          "translucent stroke geometry over-composited its joined segments");

    Path excessive_curves;
    for (std::size_t contour = 0U; contour < 33U; ++contour) {
        excessive_curves.move_to(Point{0.0, 0.5});
        excessive_curves.cubic_to(Point{0.0, 1.0e8}, Point{1.0, -1.0e8}, Point{1.0, 0.5});
    }
    bool aggregate_limit_rejected = false;
    try {
        static_cast<void>(tessellate_shape(
            PathShape{
                std::move(excessive_curves),
                Paint(RenderColor{255U, 255U, 255U, 255U}),
            },
            Size{100.0, 100.0}, 1.0));
    } catch (const std::invalid_argument&) {
        aggregate_limit_rejected = true;
    }
    check(aggregate_limit_rejected,
          "UI path flattening did not enforce its aggregate point budget");
}

void test_bundled_texture_descriptor(const std::filesystem::path& resource_root) {
    const std::filesystem::path& resources = resource_root;
    const strata::resource::ResourceBytes png = strata::resource::load_binary_resource(
        resources,
        strata::resource::ResourceId::parse("assets/strata/textures/ui/icons/chevron-down.png"));
    const strata::resource::ImageDimensions dimensions = strata::resource::inspect_png(png);
    check(dimensions.width == 32U && dimensions.height == 32U,
          "native PNG descriptor lost the bundled icon dimensions");
    std::vector<std::uint8_t> corrupt = png;
    corrupt[0] = 0U;
    try {
        static_cast<void>(strata::resource::inspect_png(corrupt));
        check(false, "native PNG descriptor accepted a corrupt signature");
    } catch (const std::invalid_argument&) {
    }
}

std::uint32_t packet_u32(const std::vector<std::uint8_t>& packet, std::size_t& offset) {
    check(offset + 4U <= packet.size(), "render packet truncated a u32 field");
    std::uint32_t value = 0U;
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        value |= static_cast<std::uint32_t>(packet[offset++]) << (byte * 8U);
    }
    return value;
}

std::uint64_t packet_u64(const std::vector<std::uint8_t>& packet, std::size_t& offset) {
    check(offset + 8U <= packet.size(), "render packet truncated a u64 field");
    std::uint64_t value = 0U;
    for (std::size_t byte = 0U; byte < 8U; ++byte) {
        value |= static_cast<std::uint64_t>(packet[offset++]) << (byte * 8U);
    }
    return value;
}

void test_render_packet_batches_every_portable_command() {
    using namespace strata::ui;
    RenderCommandBuffer commands;
    commands.append(SolidRectRenderCommand{});
    commands.append(RoundedRectRenderCommand{
        {},
        {},
        {},
        RenderBorder{1.0, {}, true},
        1.0,
    });
    commands.append(BorderRenderCommand{});
    commands.append(ImageRenderCommand{});
    commands.append(NinePatchRenderCommand{});
    commands.append(TextRunRenderCommand{
        {},
        {},
        12.0,
        {LogicalGlyph{"strata:regular", 2U, 65U, 0U, 1U, 1.0, 2.0, 3.0, 4.0, 5.0}},
    });
    commands.append(CustomMeshRenderCommand{
        {},
        "fixture.mesh",
        MeshGeometry{
            {
                MeshVertex{0.5, 0.0, 0.0, 0.5, 0.0},
                MeshVertex{1.0, 1.0, 0.0, 1.0, 1.0},
                MeshVertex{0.0, 1.0, 0.0, 0.0, 1.0},
            },
            {0U, 1U, 2U},
        },
        std::string("fixture.texture"),
        MaterialState{
            "fixture.material",
            "straight_alpha",
            0.75,
            {MaterialParameter{"amount", strata::runtime::Value(0.5)}},
        },
        1.0,
    });
    commands.append(PathRenderCommand{
        Rect{0.0, 0.0, 32.0, 32.0},
        PathShape{
            Path::parse("M 0 0 L 1 0 L 1 1 Z"),
            Paint(strata::runtime::ColorValue{255U, 255U, 255U, 255U}),
            Paint(strata::runtime::ColorValue{0U, 0U, 0U, 255U}),
            StrokeStyle{2.0, PathCap::round, PathJoin::bevel, 4.0, {4.0, 2.0}, 1.0},
        },
    });
    commands.append(BlurRegionRenderCommand{});
    commands.append(ShadowRenderCommand{});
    EffectState effect{
        "fixture.effect",
        EffectInput::backdrop,
        0.8,
        default_effect_refresh_rate,
        true,
        {MaterialParameter{"amount", strata::runtime::Value(0.5)}},
    };
    effect.packed_parameters[0U] = 0.5;
    effect.packed_parameter_count = 1U;
    commands.append(BackdropEffectRenderCommand{{}, {}, effect});
    effect.input = EffectInput::content;
    commands.append(ContentEffectPushRenderCommand{{}, {}, effect});
    commands.append(ContentEffectPopRenderCommand{});
    commands.append(ClipPushRenderCommand{});
    commands.append(ClipPopRenderCommand{});
    commands.append(TransformPushRenderCommand{});
    commands.append(TransformPopRenderCommand{});
    commands.append(MaterialPushRenderCommand{MaterialState{"fixture.material"}});
    commands.append(MaterialPopRenderCommand{});

    const std::vector<std::uint8_t> packet = encode_render_packet(commands, 42U);
    check(packet.size() >= 24U &&
              std::string_view(reinterpret_cast<const char*>(packet.data()), 8U) == "STRATARP",
          "render packet lost its fixed magic");
    std::size_t offset = 8U;
    check(packet_u32(packet, offset) == 3U, "render packet version changed");
    check(packet_u32(packet, offset) == 19U, "render packet command count changed");
    check(packet_u64(packet, offset) == 42U, "render packet frame identity changed");
    for (std::uint32_t expected_kind = 0U; expected_kind < 19U; ++expected_kind) {
        check(packet_u32(packet, offset) == expected_kind, "render packet command kind changed");
        const std::uint32_t payload_size = packet_u32(packet, offset);
        check(offset + payload_size <= packet.size(),
              "render packet command payload escaped its record");
        offset += payload_size;
    }
    check(offset == packet.size(), "render packet has unframed trailing bytes");
}

void test_render_submission_cache(const std::filesystem::path& resource_root) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{1.0, 2.0, 40.0, 20.0},
        ui::RenderColor{10U, 20U, 30U, 255U},
    });
    const std::shared_ptr<const ui::TextEngine> text_engine = ui::TextEngine::load_control_font(
        resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    font::GlyphAtlas atlas("submission-cache-test");
    ui::RenderSubmissionCache cache;
    const ui::RenderSubmissionEnvironment environment{1.0, 640, 480, 640.0, 480.0};

    const ui::RenderSubmission* first = &cache.resolve(commands, atlas, *text_engine, environment);
    check(cache.hit_count() == 0U && cache.miss_count() == 1U && !first->vertex_bytes.empty() &&
              !first->indices.empty(),
          "render submission cache did not populate from native geometry");
    const ui::RenderSubmission* settled =
        &cache.resolve(commands, atlas, *text_engine, environment);
    check(settled == first && cache.hit_count() == 1U && cache.miss_count() == 1U,
          "settled render submission did not reuse surface-owned geometry");

    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{2.0, 3.0, 4.0, 5.0},
        ui::RenderColor{255U, 255U, 255U, 255U},
    });
    static_cast<void>(cache.resolve(commands, atlas, *text_engine, environment));
    check(cache.hit_count() == 1U && cache.miss_count() == 2U,
          "render command mutation did not invalidate cached geometry");
    static_cast<void>(
        cache.resolve(commands, atlas, *text_engine,
                      ui::RenderSubmissionEnvironment{2.0, 1'280, 960, 640.0, 480.0}));
    check(cache.hit_count() == 1U && cache.miss_count() == 3U,
          "render environment mutation did not invalidate cached geometry");
    atlas.invalidate_resources();
    static_cast<void>(cache.resolve(commands, atlas, *text_engine, environment));
    check(cache.hit_count() == 1U && cache.miss_count() == 4U,
          "glyph-atlas generation mutation did not invalidate cached geometry");
    const std::shared_ptr<const ui::TextEngine> replacement_text_engine =
        ui::TextEngine::load_control_font(
            resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    static_cast<void>(cache.resolve(commands, atlas, *replacement_text_engine, environment));
    check(cache.hit_count() == 1U && cache.miss_count() == 5U,
          "text-engine replacement did not invalidate cached geometry");
    cache.clear();
    static_cast<void>(cache.resolve(commands, atlas, *text_engine, environment));
    check(cache.miss_count() == 6U, "explicit resource invalidation retained cached geometry");

    const std::uint16_t visible_glyph = text_engine->control_font().glyph_id('A');
    const std::uint16_t clipped_glyph = text_engine->control_font().glyph_id('B');
    check(visible_glyph != 0U && clipped_glyph != 0U, "submission culling fixture lost its glyphs");
    ui::RenderCommandBuffer culled_text;
    culled_text.append(ui::TextRunRenderCommand{
        {10.0, 10.0},
        {},
        12.0,
        {ui::LogicalGlyph{
            "strata:fonts/default-medium",
            visible_glyph,
            'A',
            0U,
            1U,
            0.0,
            9.0,
            7.0,
        }},
        ui::FontRasterization::grayscale,
        ui::Rect{10.0, 10.0, 20.0, 20.0},
    });
    culled_text.append(ui::TextRunRenderCommand{
        {10.0, 600.0},
        {},
        12.0,
        {ui::LogicalGlyph{
            "strata:fonts/default-medium",
            clipped_glyph,
            'B',
            0U,
            1U,
            0.0,
            9.0,
            7.0,
        }},
        ui::FontRasterization::grayscale,
        ui::Rect{10.0, 600.0, 20.0, 20.0},
    });
    font::GlyphAtlas culled_atlas("submission-culling-test");
    ui::RenderSubmissionCache culled_cache;
    const ui::RenderSubmission& culled_submission =
        culled_cache.resolve(culled_text, culled_atlas, *text_engine, environment);
    check(culled_submission.planned_draws == 1U && culled_atlas.cached_glyph_count() == 1U,
          "submission warmup rasterized a text run outside the viewport");
}

void test_authored_material_scope_is_fill_local() {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::MaterialPushRenderCommand{
        ui::MaterialState{"fixture:surface"},
    });
    commands.append(ui::RoundedRectRenderCommand{
        ui::Rect{0.0, 0.0, 120.0, 40.0},
        ui::CornerRadii::all(8.0),
        ui::Paint(runtime::ColorValue{24U, 24U, 42U, 255U}),
        std::nullopt,
        1.0,
    });
    commands.append(ui::BorderRenderCommand{
        ui::Rect{0.0, 0.0, 120.0, 40.0},
        ui::RenderBorder{1.0, ui::RenderColor{92U, 102U, 118U, 255U}, true},
        ui::CornerRadii::all(8.0),
    });
    commands.append(ui::MaterialPopRenderCommand{});
    commands.append(ui::RoundedRectRenderCommand{
        ui::Rect{8.0, 8.0, 40.0, 24.0},
        ui::CornerRadii::all(4.0),
        ui::Paint(runtime::ColorValue{50U, 60U, 74U, 255U}),
        std::nullopt,
        1.0,
    });

    font::GlyphAtlas atlas("material-scope-test");
    const ui::RenderSubmission submission =
        ui::build_render_submission(commands, atlas, nullptr, 1.0, 640, 480, 640.0, 480.0);
    check(submission.planned_draws == 3U && submission.batches.size() == 2U &&
              submission.batches[0].material == "fixture:surface" &&
              submission.batches[1].material == "strata:unified_ui",
          "an authored surface material escaped into a border or later sibling fill");
    constexpr std::size_t first_vertex_draw_mode_offset = 24U + 14U * sizeof(float);
    check(submission.vertex_bytes.size() >= first_vertex_draw_mode_offset + sizeof(float),
          "authored rounded fill emitted no draw data");
    float draw_mode = 0.0F;
    std::memcpy(&draw_mode, submission.vertex_bytes.data() + first_vertex_draw_mode_offset,
                sizeof(draw_mode));
    check(draw_mode == 2.0F, "an authored fill replaced the rounded primitive's geometry mode");
}

float submission_vertex_component(const std::vector<std::uint8_t>& vertices,
                                  const std::size_t vertex, const std::size_t component) {
    constexpr std::size_t vertex_bytes = 88U;
    constexpr std::size_t component_bytes = sizeof(float);
    check(component < 2U, "submission vertex test requested a non-position component");
    const std::size_t offset = vertex * vertex_bytes + component * component_bytes;
    check(offset + component_bytes <= vertices.size(), "submission vertex position was truncated");
    float result = 0.0F;
    std::memcpy(&result, vertices.data() + offset, component_bytes);
    return result;
}

void test_shadow_submission_extends_beyond_the_source_shape() {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::ShadowRenderCommand{
        ui::Rect{100.0, 50.0, 40.0, 20.0},
        ui::CornerRadii::all(8.0),
        ui::RenderColor{0U, 0U, 0U, 96U},
        10.0,
        2.0,
    });
    font::GlyphAtlas atlas("shadow-outset-test");
    const ui::RenderSubmission submission =
        ui::build_render_submission(commands, atlas, nullptr, 1.0, 640, 480, 640.0, 480.0);
    check(submission.vertex_bytes.size() == 4U * 88U && submission.indices.size() == 6U,
          "exterior shadow did not produce one expanded quad");
    float minimum_x = std::numeric_limits<float>::max();
    float minimum_y = std::numeric_limits<float>::max();
    float maximum_x = std::numeric_limits<float>::lowest();
    float maximum_y = std::numeric_limits<float>::lowest();
    for (std::size_t vertex = 0U; vertex < 4U; ++vertex) {
        const float x = submission_vertex_component(submission.vertex_bytes, vertex, 0U);
        const float y = submission_vertex_component(submission.vertex_bytes, vertex, 1U);
        minimum_x = std::min(minimum_x, x);
        minimum_y = std::min(minimum_y, y);
        maximum_x = std::max(maximum_x, x);
        maximum_y = std::max(maximum_y, y);
    }
    check(
        std::abs(minimum_x - 88.0F) < 0.0001F &&
            std::abs(maximum_x - 152.0F) < 0.0001F &&
            std::abs(minimum_y - 38.0F) < 0.0001F &&
            std::abs(maximum_y - 82.0F) < 0.0001F,
        "shadow geometry remained clipped to its source rectangle"
    );
    constexpr std::size_t draw_data_offset = 24U;
    const auto draw_data = [&submission](const std::size_t component) {
        float result = 0.0F;
        std::memcpy(
            &result,
            submission.vertex_bytes.data() + draw_data_offset + component * sizeof(float),
            sizeof(result)
        );
        return result;
    };
    check(
        draw_data(0U) == 40.0F && draw_data(1U) == 20.0F &&
            draw_data(8U) == 64.0F && draw_data(9U) == 44.0F,
        "shadow submission lost its independent source and expanded dimensions"
    );
}

void check_translation_reused_geometry(const strata::ui::RenderSubmission& actual,
                                       const strata::ui::RenderSubmission& expected) {
    constexpr std::size_t vertex_bytes = 88U;
    check(actual.vertex_bytes.size() == expected.vertex_bytes.size() &&
              actual.vertex_bytes.size() % vertex_bytes == 0U &&
              actual.indices == expected.indices &&
              actual.batches.size() == expected.batches.size(),
          "translation reuse changed submission topology");
    const std::size_t vertex_count = actual.vertex_bytes.size() / vertex_bytes;
    for (std::size_t vertex = 0U; vertex < vertex_count; ++vertex) {
        constexpr float position_tolerance = 0.000'01F;
        check(std::abs(submission_vertex_component(actual.vertex_bytes, vertex, 0U) -
                       submission_vertex_component(expected.vertex_bytes, vertex, 0U)) <=
                  position_tolerance,
              "translation reuse changed a vertex x position beyond float precision");
        check(std::abs(submission_vertex_component(actual.vertex_bytes, vertex, 1U) -
                       submission_vertex_component(expected.vertex_bytes, vertex, 1U)) <=
                  position_tolerance,
              "translation reuse changed a vertex y position beyond float precision");
        check(std::equal(actual.vertex_bytes.begin() +
                             static_cast<std::ptrdiff_t>(vertex * vertex_bytes + 8U),
                         actual.vertex_bytes.begin() +
                             static_cast<std::ptrdiff_t>((vertex + 1U) * vertex_bytes),
                         expected.vertex_bytes.begin() +
                             static_cast<std::ptrdiff_t>(vertex * vertex_bytes + 8U)),
              "translation reuse changed non-position vertex data");
    }
}

void test_render_submission_translation_reuse(const std::filesystem::path& resource_root) {
    using namespace strata;
    const std::shared_ptr<const ui::TextEngine> text_engine = ui::TextEngine::load_control_font(
        resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    const std::uint16_t glyph = text_engine->control_font().glyph_id('A');
    check(glyph != 0U, "translation reuse fixture lost its text glyph");
    const ui::LogicalGlyph logical{
        "strata:fonts/default-medium", glyph, 'A', 0U, 1U, 0.0, 9.0, 7.0,
    };
    const auto commands = [&logical](const double x, const double y, const ui::Point text_origin) {
        ui::RenderCommandBuffer result;
        result.append(ui::TransformPushRenderCommand{1.0, 0.0, x, 0.0, 1.0, y});
        result.append(ui::SolidRectRenderCommand{
            ui::Rect{1.0, 2.0, 40.0, 20.0},
            ui::RenderColor{10U, 20U, 30U, 255U},
        });
        result.append(ui::TextRunRenderCommand{
            text_origin,
            ui::RenderColor{240U, 241U, 242U, 255U},
            12.0,
            {logical},
        });
        result.append(ui::TransformPopRenderCommand{});
        return result;
    };
    const ui::RenderSubmissionEnvironment environment{1.25, 800, 600, 640.0, 480.0};
    font::GlyphAtlas atlas("submission-translation-reuse-test");
    ui::RenderSubmissionCache retained;
    const ui::RenderCommandBuffer initial = commands(2.2, 3.4, ui::Point{4.1, 5.3});
    static_cast<void>(retained.resolve(initial, atlas, *text_engine, environment));

    const ui::RenderCommandBuffer moved = commands(7.7, 11.2, ui::Point{6.6, 8.9});
    const ui::RenderSubmission& reused = retained.resolve(moved, atlas, *text_engine, environment);
    ui::RenderSubmissionCache fresh;
    const ui::RenderSubmission& rebuilt = fresh.resolve(moved, atlas, *text_engine, environment);
    check_translation_reused_geometry(reused, rebuilt);
}

void test_native_nine_patch_geometry(const std::filesystem::path& resource_root) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::NinePatchRenderCommand{
        ui::Rect{10.0, 20.0, 30.0, 40.0},
        "fixture:nine-patch",
        ui::TextureRegion{},
        ui::Edges{4.0, 4.0, 4.0, 4.0},
        ui::Edges{5.0, 5.0, 5.0, 5.0},
        ui::RenderColor{255U, 255U, 255U, 255U},
    });
    const std::shared_ptr<const ui::TextEngine> text_engine = ui::TextEngine::load_control_font(
        resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    std::vector<resource::EncodedTextureResource> textures{
        resource::EncodedTextureResource{
            "fixture:nine-patch",
            "fixture/nine-patch.png",
            resource::TextureSampling::linear,
            resource::ImageEncoding::png,
            resource::ImageDimensions{16U, 16U},
            {0U},
        },
    };
    std::vector<resource::TextureResourceDescriptor> texture_descriptors{
        textures.front().descriptor(),
    };
    font::GlyphAtlas atlas("nine-patch-test");
    ui::RenderSubmissionCache cache;
    const ui::RenderSubmission& submission = cache.resolve(
        commands, atlas, *text_engine, ui::RenderSubmissionEnvironment{1.0, 640, 480, 640.0, 480.0},
        texture_descriptors);
    check(submission.vertex_bytes.size() == 36U * 88U && submission.indices.size() == 54U &&
              submission.batches.size() == 1U && submission.batches.front().index_count == 54U,
          "native nine-patch did not encode its complete 3x3 geometry");
    const std::vector<std::uint8_t> initial_geometry = submission.vertex_bytes;
    static_cast<void>(cache.resolve(commands, atlas, *text_engine,
                                    ui::RenderSubmissionEnvironment{1.0, 640, 480, 640.0, 480.0},
                                    texture_descriptors));
    check(cache.hit_count() == 1U && cache.miss_count() == 1U,
          "stable texture descriptors did not reuse nine-patch geometry");
    texture_descriptors.front().dimensions.width = 32U;
    const ui::RenderSubmission& resized_texture = cache.resolve(
        commands, atlas, *text_engine, ui::RenderSubmissionEnvironment{1.0, 640, 480, 640.0, 480.0},
        texture_descriptors);
    check(cache.hit_count() == 1U && cache.miss_count() == 2U &&
              resized_texture.vertex_bytes != initial_geometry,
          "texture descriptor mutation reused stale nine-patch geometry");

    ui::HostRenderPacketCache packet_cache;
    textures.front().dimensions = texture_descriptors.front().dimensions;
    static_cast<void>(packet_cache.encode(commands, 1U, textures, atlas, *text_engine, 1.0, 640,
                                          480, 640.0, 480.0));
    commands.append(ui::SolidRectRenderCommand{
        ui::Rect{0.0, 0.0, 1.0, 1.0},
        ui::RenderColor{255U, 255U, 255U, 255U},
    });
    const std::vector<std::uint8_t>& descriptor_only_packet =
        packet_cache.encode(commands, 2U, {}, atlas, *text_engine, 1.0, 640, 480, 640.0, 480.0);
    check(!descriptor_only_packet.empty(),
          "one-shot texture upload discarded descriptors required by later geometry");

    const std::uint16_t teardown_glyph = text_engine->control_font().glyph_id('A');
    check(teardown_glyph != 0U, "control font lost the atlas teardown fixture");
    check(atlas
              .request_coverage("strata:fonts/control", text_engine->control_font(), teardown_glyph,
                                12.0, {})
              .has_value(),
          "atlas teardown fixture did not allocate a live page");
    // Publish the page once so teardown models a resource that can genuinely be host-owned.
    static_cast<void>(
        packet_cache.encode(commands, 3U, {}, atlas, *text_engine, 1.0, 640, 480, 640.0, 480.0));
    const std::vector<std::uint8_t>& release_packet =
        packet_cache.prepare_resource_release(4U, atlas, textures);
    std::size_t release_offset = 8U;
    check(release_packet.size() >= release_offset &&
              std::string_view(reinterpret_cast<const char*>(release_packet.data()),
                               release_offset) == "STRATARP",
          "surface teardown packet lost its fixed magic");
    check(packet_u32(release_packet, release_offset) == 8U,
          "surface teardown did not use the host render packet protocol");
    check(packet_u32(release_packet, release_offset) == 2U,
          "surface teardown did not combine its live atlas and static texture releases");
    check(packet_u32(release_packet, release_offset) == 0U,
          "surface teardown packet unexpectedly retained draw batches");
    check(packet_u64(release_packet, release_offset) == 4U,
          "surface teardown packet lost its frame identity");
    check(packet_u64(release_packet, release_offset) != 0U,
          "surface teardown packet lost its geometry epoch");
    check(packet_u32(release_packet, release_offset) == 1U,
          "surface teardown packet lost its full-geometry marker");
    check(packet_u32(release_packet, release_offset) == 0U &&
              packet_u32(release_packet, release_offset) == 0U &&
              packet_u32(release_packet, release_offset) == 0U &&
              packet_u32(release_packet, release_offset) == 0U,
          "surface teardown packet retained render geometry or draw accounting");
    std::vector<std::string> released_textures;
    for (std::size_t index = 0U; index < 2U; ++index) {
        check(packet_u32(release_packet, release_offset) == 2U,
              "surface teardown packet did not encode a release operation");
        const std::uint32_t release_payload_size = packet_u32(release_packet, release_offset);
        const std::size_t payload_end = release_offset + release_payload_size;
        const std::uint32_t texture_size = packet_u32(release_packet, release_offset);
        check(release_offset + texture_size == payload_end && payload_end <= release_packet.size(),
              "surface teardown release escaped its framed payload");
        released_textures.emplace_back(
            reinterpret_cast<const char*>(release_packet.data() + release_offset), texture_size);
        release_offset = payload_end;
    }
    check(release_offset == release_packet.size() &&
              std::ranges::find(released_textures, textures.front().host_id) !=
                  released_textures.end() &&
              std::ranges::any_of(released_textures,
                                  [](const std::string& texture) {
                                      return texture.starts_with("strata:native-font-atlas/");
                                  }),
          "surface teardown lost or duplicated its static/atlas resource ownership");

    font::GlyphAtlas reload_atlas("static-image-reload-test");
    ui::HostRenderPacketCache reload_cache;
    const std::vector<std::uint8_t>& before_reload = reload_cache.encode(
        commands, 1U, textures, reload_atlas, *text_engine, 1.0, 640, 480, 640.0, 480.0);
    std::size_t before_offset = 8U;
    static_cast<void>(packet_u32(before_reload, before_offset));
    static_cast<void>(packet_u32(before_reload, before_offset));
    static_cast<void>(packet_u32(before_reload, before_offset));
    static_cast<void>(packet_u64(before_reload, before_offset));
    const std::uint64_t before_epoch = packet_u64(before_reload, before_offset);
    ui::HostRenderResourceInvalidationPlan invalidation = reload_cache.plan_resource_invalidation();
    reload_cache.commit_resource_invalidation(std::move(invalidation));
    ui::HostRenderResourceInvalidationPlan repeated_invalidation =
        reload_cache.plan_resource_invalidation();
    reload_cache.commit_resource_invalidation(std::move(repeated_invalidation));
    const std::vector<std::uint8_t>& after_reload = reload_cache.encode(
        commands, 2U, textures, reload_atlas, *text_engine, 1.0, 640, 480, 640.0, 480.0);
    std::size_t after_offset = 8U;
    check(packet_u32(after_reload, after_offset) == 8U,
          "static image reload packet version changed");
    check(packet_u32(after_reload, after_offset) == 2U,
          "repeated static image reload dropped its pending release or replacement");
    static_cast<void>(packet_u32(after_reload, after_offset));
    static_cast<void>(packet_u64(after_reload, after_offset));
    check(packet_u64(after_reload, after_offset) > before_epoch,
          "resource invalidation reused a prior geometry epoch");
    for (std::size_t field = 0U; field < 5U; ++field) {
        static_cast<void>(packet_u32(after_reload, after_offset));
    }
    check(packet_u32(after_reload, after_offset) == 2U,
          "static image reload did not release the prior host id first");
    const std::uint32_t release_size = packet_u32(after_reload, after_offset);
    after_offset += release_size;
    check(packet_u32(after_reload, after_offset) == 3U,
          "static image reload did not create the replacement after release");
}

void test_native_custom_mesh_geometry(const std::filesystem::path& resource_root) {
    using namespace strata;
    ui::RenderCommandBuffer commands;
    commands.append(ui::CustomMeshRenderCommand{
        ui::Rect{10.0, 20.0, 30.0, 40.0},
        "fixture:triangle",
        ui::MeshGeometry{
            {
                ui::MeshVertex{0.5, 0.0, 0.0, 0.5, 0.0},
                ui::MeshVertex{1.0, 1.0, 0.0, 1.0, 1.0},
                ui::MeshVertex{0.0, 1.0, 0.0, 0.0, 1.0},
            },
            {0U, 1U, 2U},
        },
        std::nullopt,
        std::nullopt,
        1.0,
    });
    const std::shared_ptr<const ui::TextEngine> text_engine = ui::TextEngine::load_control_font(
        resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    font::GlyphAtlas atlas("custom-mesh-test");
    ui::RenderSubmissionCache cache;
    const ui::RenderSubmission& submission =
        cache.resolve(commands, atlas, *text_engine,
                      ui::RenderSubmissionEnvironment{1.0, 640, 480, 640.0, 480.0});
    check(submission.vertex_bytes.size() == 3U * 88U && submission.indices.size() == 3U &&
              submission.batches.size() == 1U && submission.batches.front().index_count == 3U,
          "native custom mesh was not encoded from its owned vertex/index geometry");
}

void test_gradient_paint_authoring_and_tessellation() {
    using namespace strata;
    const auto stop = [](const runtime::ColorValue color, const double offset) {
        return runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"color", runtime::Value(color)},
            {"offset", runtime::Value(offset)},
        });
    };
    const runtime::Value authored(std::vector<std::pair<std::string, runtime::Value>>{
        {"angle", runtime::Value(90.0)},
        {"kind", runtime::Value("linear")},
        {"stops", runtime::Value(std::vector<runtime::Value>{
                      stop(runtime::ColorValue{0U, 0U, 0U, 255U}, 0.0),
                      stop(runtime::ColorValue{255U, 255U, 255U, 255U}, 1.0),
                  })},
    });
    const std::optional<ui::Paint> paint = ui::paint_from_value(&authored);
    check(paint.has_value() && paint->is_gradient(),
          "authored linear gradient was not read as a paint");
    const ui::Gradient& gradient = *paint->gradient();
    check(gradient.sample(0.5) == runtime::ColorValue{128U, 128U, 128U, 255U},
          "gradient sampling is not a linear ramp between its stops");
    check(gradient.sample(-1.0) == gradient.stops.front().color &&
              gradient.sample(2.0) == gradient.stops.back().color,
          "clamped gradient did not hold its terminal stops");
    // 90 degrees points right, and the axis is resolved against the shape so a wide shape keeps
    // the authored visual angle rather than a skewed one.
    const auto [start, end] = gradient.axis(ui::Size{200.0, 50.0});
    const auto near = [](const double left, const double right) {
        return std::abs(left - right) < 0.000001;
    };
    check(near(start.x, 0.0) && near(start.y, 0.5) && near(end.x, 1.0) && near(end.y, 0.5),
          "authored gradient angle did not resolve to a horizontal axis");
    const ui::PaintMesh mesh = ui::tessellate_gradient(gradient, ui::Size{200.0, 50.0}, 1.0);
    check(mesh.vertices.size() == 4U && mesh.indices.size() == 6U,
          "a two-stop linear gradient must tessellate into one band");
    const bool spans_shape = std::ranges::all_of(mesh.vertices, [](const ui::PaintVertex& vertex) {
        return vertex.normalized.x >= -0.000001 && vertex.normalized.x <= 1.000001 &&
               vertex.normalized.y >= -0.000001 && vertex.normalized.y <= 1.000001;
    });
    check(spans_shape, "linear gradient tessellation left the shape it fills");
    check(mesh.vertices.front().color == gradient.stops.front().color &&
              mesh.vertices.back().color == gradient.stops.back().color,
          "gradient band vertices did not carry their stop colours");

    ui::Gradient radial;
    radial.kind = ui::GradientKind::radial;
    radial.stops = gradient.stops;
    const ui::PaintMesh rings = ui::tessellate_gradient(radial, ui::Size{80.0, 80.0}, 2.0);
    check(!rings.indices.empty() && rings.indices.size() % 3U == 0U,
          "radial gradient tessellation did not produce triangles");
    const bool indexed = std::ranges::all_of(rings.indices, [&rings](const std::uint32_t index) {
        return index < rings.vertices.size();
    });
    check(indexed, "radial gradient tessellation produced out-of-range indices");

    ui::RenderCommandBuffer commands;
    commands.append(ui::RoundedRectRenderCommand{
        ui::Rect{0.0, 0.0, 120.0, 60.0},
        ui::CornerRadii::all(8.0),
        *paint,
        std::nullopt,
        1.0,
    });
    const ui::RenderCommand faded =
        ui::render_command_with_opacity(commands.commands().front(), 0.5);
    const ui::Paint& faded_fill = std::get<ui::RoundedRectRenderCommand>(faded).fill;
    check(faded_fill.is_gradient() && faded_fill.gradient()->stops.back().color.alpha == 128U,
          "opacity was not applied to every gradient stop");
}

void test_vector_shape_tessellation() {
    using namespace strata;
    const ui::Path parsed = ui::Path::parse("M 0 0 L 1 0 L 1 1 Z");
    check(parsed.segments().size() == 4U && parsed.segments()[2].to == ui::Point{1.0, 1.0} &&
              parsed.segments().back().verb == ui::PathVerb::close,
          "compact path parsing did not produce the authored outline");
    bool rejected = false;
    try {
        static_cast<void>(ui::Path::parse("M 0 0 S 1 1"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    check(rejected, "an unsupported path command must be rejected");

    ui::PathShape filled;
    filled.path = parsed;
    filled.fill = ui::Paint(runtime::ColorValue{255U, 0U, 0U, 255U});
    const ui::PaintMesh fill_mesh = ui::tessellate_shape(filled, ui::Size{100.0, 100.0}, 1.0);
    check(fill_mesh.indices.size() >= 3U && fill_mesh.indices.size() % 3U == 0U,
          "a filled triangle must tessellate into triangles");
    const bool feathered = std::ranges::any_of(
        fill_mesh.vertices, [](const ui::PaintVertex& vertex) { return vertex.color.alpha == 0U; });
    check(feathered, "a filled shape must carry its own antialiasing ramp");

    ui::PathShape stroked;
    stroked.path = ui::Path::arc(ui::Point{0.5, 0.5}, 0.4, 0.4, -90.0, 270.0);
    stroked.stroke = ui::Paint(runtime::ColorValue{0U, 255U, 0U, 255U});
    stroked.stroke_style =
        ui::StrokeStyle{4.0, ui::PathCap::round, ui::PathJoin::round, 4.0, {}, 0.0};
    const ui::PaintMesh stroke_mesh = ui::tessellate_shape(stroked, ui::Size{120.0, 120.0}, 2.0);
    check(!stroke_mesh.indices.empty() && stroke_mesh.indices.size() % 3U == 0U,
          "a stroked arc must tessellate into triangles");
    const bool indexed =
        std::ranges::all_of(stroke_mesh.indices, [&stroke_mesh](const std::uint32_t index) {
            return index < stroke_mesh.vertices.size();
        });
    check(indexed, "stroke tessellation produced out-of-range indices");

    // Dashing is compared with butt caps so the measure is the painted run itself rather than the
    // per-dash cap geometry a round cap would add.
    ui::PathShape solid = stroked;
    solid.stroke_style->cap = ui::PathCap::butt;
    ui::PathShape dashed = solid;
    dashed.stroke_style->dash = {8.0, 8.0};
    const auto painted_area = [](const ui::PaintMesh& mesh) {
        double total = 0.0;
        for (std::size_t index = 0U; index + 2U < mesh.indices.size(); index += 3U) {
            const ui::PaintVertex& a = mesh.vertices[mesh.indices[index]];
            const ui::PaintVertex& b = mesh.vertices[mesh.indices[index + 1U]];
            const ui::PaintVertex& c = mesh.vertices[mesh.indices[index + 2U]];
            if (a.color.alpha == 0U || b.color.alpha == 0U || c.color.alpha == 0U)
                continue;
            total +=
                std::abs((b.normalized.x - a.normalized.x) * (c.normalized.y - a.normalized.y) -
                         (c.normalized.x - a.normalized.x) * (b.normalized.y - a.normalized.y)) *
                0.5;
        }
        return total;
    };
    const double solid_area =
        painted_area(ui::tessellate_shape(solid, ui::Size{120.0, 120.0}, 2.0));
    const double dashed_area =
        painted_area(ui::tessellate_shape(dashed, ui::Size{120.0, 120.0}, 2.0));
    check(dashed_area > 0.0 && dashed_area < solid_area * 0.75,
          "a dashed stroke must paint less of its outline than a solid one");

    ui::RenderCommandBuffer commands;
    commands.append(ui::PathRenderCommand{ui::Rect{4.0, 8.0, 120.0, 120.0}, stroked});
    const std::shared_ptr<const ui::TextEngine> text_engine = nullptr;
    font::GlyphAtlas atlas("vector-shape-test");
    ui::RenderSubmissionCache cache;
    const ui::RenderSubmission& submission =
        cache.resolve(commands, atlas, text_engine.get(),
                      ui::RenderSubmissionEnvironment{1.0, 640, 480, 640.0, 480.0});
    check(submission.batches.size() == 1U &&
              submission.batches.front().material == "strata:unified_ui" &&
              submission.batches.front().index_count ==
                  ui::tessellate_shape(stroked, ui::Size{120.0, 120.0}, 1.0).indices.size(),
          "a shape command was not planned into one solid-material batch");
}

void test_keyed_reconciliation_and_detach_cleanup() {
    using namespace strata::ui;
    RetainedTree tree(10U);
    const auto first =
        node("Panel", "root",
             {
                 node("Text", "a", {},
                      {{"text", strata::runtime::ExpressionValue(strata::runtime::Value("A"))}}),
                 node("Text", "b", {},
                      {{"text", strata::runtime::ExpressionValue(strata::runtime::Value("B"))}}),
             });
    const ReconcileStats initial = tree.reconcile(first);
    check(initial.created == 3U && initial.materialized == 2U,
          "initial retained materialization changed");
    const std::uint64_t a_identity = tree.find_key("a")->identity();
    const std::uint64_t b_identity = tree.find_key("b")->identity();
    check(tree.find_key("a")->structural_path() == "/0", "initial structural path changed");
    check(tree.find_source("/fixture") != nullptr && tree.find_source("/fixture")->size() == 3U,
          "source index changed");
    tree.clear_dirty();

    std::uint64_t detached = 0U;
    tree.find_key("a")->add_cleanup([&detached] { ++detached; });
    check(tree.find_key("b")->set_retained_value("selection", strata::runtime::Value(2.0)),
          "retained value write failed");
    const auto reordered =
        node("Panel", "root",
             {
                 node("Text", "b", {},
                      {{"text", strata::runtime::ExpressionValue(strata::runtime::Value("B2"))}}),
                 node("Text", "a", {},
                      {{"text", strata::runtime::ExpressionValue(strata::runtime::Value("A"))}}),
             });
    const ReconcileStats reorder = tree.reconcile(reordered);
    check(reorder.created == 0U && reorder.reused == 3U &&
              tree.find_key("a")->identity() == a_identity &&
              tree.find_key("b")->identity() == b_identity &&
              *tree.find_key("b")->retained_value("selection")->number() == 2.0,
          "keyed reorder lost stable identity or retained state");
    check(tree.find_key("b")->structural_path() == "/0",
          "reordered structural path was not refreshed");
    check(tree.find_key("b")->dirty().contains(DirtyReason::text),
          "text property change missed its dirty reason");

    const auto removed = node("Panel", "root", {node("Text", "b")});
    const ReconcileStats removal = tree.reconcile(removed);
    check(removal.detached == 1U && detached == 1U && tree.find_key("a") == nullptr,
          "detached cleanup was not exact");
    tree.clear();
    check(detached == 1U, "detached cleanup ran more than once");
}

void test_exit_retention_and_prune_ownership() {
    using namespace strata::ui;
    const auto owns_scope = [](const std::shared_ptr<const DescriptionNode>& source,
                               std::string scope) {
        auto result = std::make_shared<DescriptionNode>(*source);
        result->materialization_result = std::make_shared<const DescriptionMaterialization>(
            strata::runtime::StateScopeSet{std::move(scope)},
            std::vector<strata::runtime::RuntimeDiagnostic>{}, 0U, 0U);
        return std::shared_ptr<const DescriptionNode>(std::move(result));
    };
    const std::shared_ptr<const DescriptionNode> exiting_child =
        owns_scope(node("Text", "exiting.child"), "scope.exiting.child");
    const std::shared_ptr<const DescriptionNode> exiting =
        owns_scope(node("Panel", "exiting", {exiting_child}), "scope.exiting");
    const std::shared_ptr<const DescriptionNode> stable =
        owns_scope(node("Text", "stable"), "scope.stable");
    RetainedTree tree;
    static_cast<void>(tree.reconcile(node("Panel", "root",
                                          {
                                              exiting,
                                              stable,
                                          })));
    check(attached_description_state_scopes(tree) ==
              strata::runtime::StateScopeSet{
                  "scope.exiting",
                  "scope.exiting.child",
                  "scope.stable",
              },
          "attached description state-scope collection lost an attached subtree");
    std::uint64_t cleanup_count = 0U;
    tree.find_key("exiting")->add_cleanup([&cleanup_count] { ++cleanup_count; });

    static_cast<void>(
        tree.reconcile(node("Panel", "root", {stable}), [](const RetainedNode& candidate) {
            return candidate.description().key == "exiting";
        }));
    check(tree.root()->children().size() == 2U,
          "exit boundary was not retained after attached children");
    check(tree.root()->children()[0] != nullptr && tree.root()->children()[1] != nullptr,
          "exit reconciliation left a null retained owner");
    check(tree.find_key("exiting")->lifecycle() == RetainedLifecycle::exiting &&
              tree.find_key("stable")->lifecycle() == RetainedLifecycle::attached,
          "exit reconciliation corrupted retained lifecycle state");
    check(attached_description_state_scopes(tree) == strata::runtime::StateScopeSet{"scope.stable"},
          "an EXITING lifecycle boundary retained its own or descendant state scopes");

    check(tree.prune_exiting([](const RetainedNode&) { return false; }) == 0U &&
              tree.root()->children().size() == 2U && tree.root()->children()[0] != nullptr &&
              tree.root()->children()[1] != nullptr,
          "a no-op exit prune moved ownership out of the retained tree");
    check(tree.prune_exiting([](const RetainedNode&) { return true; }) == 2U &&
              tree.root()->children().size() == 1U && tree.find_key("exiting") == nullptr &&
              tree.find_key("stable") != nullptr && cleanup_count == 1U,
          "completed exit pruning did not detach exactly one retained subtree");
}

void test_lazy_materialization_and_noop_reconcile() {
    using namespace strata::ui;
    std::uint64_t generated = 0U;
    auto generated_children = std::make_shared<const GeneratedDescriptionChildren>(
        1'000U, [&generated](const std::size_t index) {
            ++generated;
            return DescriptionNode::create("Row", "row." + std::to_string(index));
        });
    auto root = DescriptionNode::create("List", "list", {}, {}, {}, generated_children);
    auto ranged = std::make_shared<DescriptionNode>(*root);
    ranged->materialization = MaterializationRange{100U, 110U};

    RetainedTree tree;
    const ReconcileStats initial = tree.reconcile(ranged);
    check(initial.created == 11U && generated == 10U,
          "lazy description materialized outside its range");
    const std::shared_ptr<const DescriptionNode> cached = generated_children->at(104U);
    tree.clear_dirty();
    const ReconcileStats settled = tree.reconcile(ranged);
    check(!settled.changed() && settled.reused == 11U && generated == 10U &&
              generated_children->at(104U) == cached,
          "settled reconciliation re-evaluated an immutable generated index");
    check(tree.dirty_count() == 0U, "settled reconciliation created dirty work");

    std::weak_ptr<const DescriptionNode> evicted;
    {
        const std::shared_ptr<const DescriptionNode> transient = generated_children->at(500U);
        evicted = transient;
    }
    check(evicted.expired(), "unretained generated row remained strongly cached");
    static_cast<void>(generated_children->at(500U));
    check(generated == 12U, "expired generated row did not invalidate its cache entry");
}

void test_retained_virtual_realization_window() {
    using namespace strata;
    using namespace strata::ui;

    std::size_t generated = 0U;
    const auto provider = std::make_shared<const GeneratedDescriptionChildren>(
        100U, [&generated](const std::size_t index) {
            ++generated;
            auto row = std::make_shared<DescriptionNode>(
                *DescriptionNode::create("Row", "row." + std::to_string(index)));
            row->materialization_result = std::make_shared<const DescriptionMaterialization>(
                runtime::StateScopeSet{"scope." + std::to_string(index)});
            return std::shared_ptr<const DescriptionNode>(std::move(row));
        });
    const auto lazy_root = [&provider](const runtime::Value& revision) {
        auto root = std::make_shared<DescriptionNode>(
            *DescriptionNode::create("VirtualList", "virtual", {}, {},
                                     {{"revision", runtime::ExpressionValue(revision)}}, provider));
        root->materialization = MaterializationRange{};
        return std::shared_ptr<const DescriptionNode>(std::move(root));
    };
    const auto projected_theme = std::make_shared<const Theme>();
    const auto rows = [](const std::shared_ptr<const DescriptionChildren>& source,
                         const MaterializationRange range) {
        std::vector<RealizedDescriptionChild> result;
        for (std::size_t index = range.start; index < range.end_exclusive; ++index) {
            result.push_back(RealizedDescriptionChild{index, source->at(index)});
        }
        return result;
    };

    RetainedTree tree;
    static_cast<void>(tree.reconcile(lazy_root(runtime::Value(0.0))));
    check(tree.root() != nullptr && tree.root()->children().empty() &&
              !tree.root()->realized_range().has_value(),
          "lazy owner did not start with an unrealized retained window");
    const ReconcileStats first = tree.realize_children(
        tree.root()->identity(), provider, projected_theme, std::nullopt, 7U,
        MaterializationRange{10U, 13U}, rows(provider, MaterializationRange{10U, 13U}));
    check(first.created == 3U && generated == 3U &&
              tree.root()->realized_range() ==
                  std::optional<MaterializationRange>(MaterializationRange{10U, 13U}),
          "collection-local realization did not attach exactly its requested window");
    const std::uint64_t overlap_identity = tree.find_key("row.12")->identity();

    const ReconcileStats owner_update = tree.reconcile(lazy_root(runtime::Value(1.0)));
    check(owner_update.updated == 1U && tree.root()->children().size() == 3U &&
              tree.find_key("row.12")->identity() == overlap_identity &&
              tree.root()->realized_range() ==
                  std::optional<MaterializationRange>(MaterializationRange{10U, 13U}),
          "ordinary owner reconciliation detached or replaced its retained realization window");

    std::vector<RealizedDescriptionChild> forward;
    for (std::size_t index = 12U; index < 15U; ++index) {
        std::shared_ptr<const DescriptionNode> description =
            tree.root()->realized_child_description(index, provider, projected_theme, std::nullopt,
                                                    7U);
        if (description == nullptr)
            description = provider->at(index);
        forward.push_back(RealizedDescriptionChild{index, std::move(description)});
    }
    static_cast<void>(tree.realize_children(tree.root()->identity(), provider, projected_theme,
                                            std::nullopt, 7U, MaterializationRange{12U, 15U},
                                            std::move(forward)));
    check(generated == 5U && tree.find_key("row.12")->identity() == overlap_identity &&
              attached_description_state_scopes(tree).contains("scope.10") &&
              attached_description_state_scopes(tree).contains("scope.11"),
          "range movement regenerated overlap or lost its keyed retained identity");

    std::vector<RealizedDescriptionChild> reverse;
    for (std::size_t index = 10U; index < 13U; ++index) {
        std::shared_ptr<const DescriptionNode> description =
            tree.root()->realized_child_description(index, provider, projected_theme, std::nullopt,
                                                    7U);
        if (description == nullptr)
            description = provider->at(index);
        reverse.push_back(RealizedDescriptionChild{index, std::move(description)});
    }
    static_cast<void>(tree.realize_children(tree.root()->identity(), provider, projected_theme,
                                            std::nullopt, 7U, MaterializationRange{10U, 13U},
                                            std::move(reverse)));
    check(generated == 5U && tree.find_key("row.12")->identity() == overlap_identity,
          "bounded warm realization cache regenerated a recently exited row description");

    std::size_t replacement_generated = 0U;
    const auto replacement = std::make_shared<const GeneratedDescriptionChildren>(
        100U, [&replacement_generated](const std::size_t index) {
            ++replacement_generated;
            return DescriptionNode::create("Row", "row." + std::to_string(index));
        });
    static_cast<void>(tree.realize_children(tree.root()->identity(), replacement, projected_theme,
                                            std::nullopt, 8U, MaterializationRange{10U, 13U},
                                            rows(replacement, MaterializationRange{10U, 13U})));
    check(replacement_generated == 3U &&
              tree.root()->realization_current(replacement, projected_theme, std::nullopt, 8U,
                                               MaterializationRange{10U, 13U}) &&
              tree.find_key("row.12")->identity() == overlap_identity &&
              !attached_description_state_scopes(tree).contains("scope.10"),
          "provider/theme generation refresh reused stale descriptions or discarded stable keys");

    static_cast<void>(tree.reconcile(
        DescriptionNode::create("VirtualList", "virtual", {}, {}, {},
                                std::make_shared<const EagerDescriptionChildren>(
                                    std::vector<std::shared_ptr<const DescriptionNode>>{
                                        DescriptionNode::create("Row", "row.eager"),
                                    }))));
    check(!tree.root()->realized_range().has_value() &&
              tree.root()->warm_realization_state_scopes().empty() &&
              tree.root()->children().size() == 1U && tree.find_key("row.eager") != nullptr,
          "leaving virtual mode retained its collection-local window or warm state");
}

void test_lazy_range_convergence_state_machine() {
    using namespace strata::ui;
    using namespace strata::ui::surface_detail;

    const LazyRangeSignature first{
        LazyRangeState{"/lazy", 2U, {0U, 1U}, {1U, 2U}},
    };
    const LazyRangeSignature second{
        LazyRangeState{"/lazy", 2U, {1U, 2U}, {0U, 1U}},
    };
    LazyConvergenceTracker cycle;
    check(cycle.observe(first) == LazyConvergenceStatus::progress &&
              cycle.observe(second) == LazyConvergenceStatus::progress &&
              cycle.observe(first) == LazyConvergenceStatus::cycle &&
              cycle.observed_state_count() == 2U && cycle.known_state_bound() == 37U,
          "lazy convergence did not prove a repeated canonical range cycle");

    LazyConvergenceTracker fixed;
    const LazyRangeSignature fixed_signature{
        LazyRangeState{"/b", 4U, {2U, 4U}, {2U, 4U}},
        LazyRangeState{"/a", 0U, {}, {}},
    };
    check(fixed.observe(fixed_signature) == LazyConvergenceStatus::fixed_point &&
              fixed.observe(fixed_signature) == LazyConvergenceStatus::cycle,
          "lazy convergence did not recognize an all-producer fixed point");

    LazyConvergenceTracker generic_reconcile;
    const LazyRangeSignature no_lazy_producers;
    check(generic_reconcile.observe(no_lazy_producers) == LazyConvergenceStatus::fixed_point &&
              generic_reconcile.observe(no_lazy_producers) == LazyConvergenceStatus::cycle &&
              generic_reconcile.observed_state_count() == 1U &&
              generic_reconcile.known_state_bound() == 1U,
          "fixed ranges did not bound a perpetually changing generic reconciliation");
}

void test_materialization_publication_identity_and_eviction() {
    using namespace strata;
    using namespace strata::ui;
    using namespace strata::ui::surface_detail;

    DescriptionMaterialization move_source({}, {}, 7U, 5U);
    const std::uint64_t transferred_id = move_source.transaction_id();
    DescriptionMaterialization move_target(std::move(move_source));
    check(transferred_id != 0U && move_source.transaction_id() == 0U &&
              move_target.transaction_id() == transferred_id &&
              move_target.evaluated_expressions == 7U && move_target.described_nodes == 5U,
          "materialization move did not transfer one exclusive logical transaction identity");

    std::size_t evaluations = 0U;
    auto generated =
        std::make_shared<const GeneratedDescriptionChildren>(1U, [&evaluations](const std::size_t) {
            ++evaluations;
            auto row = std::make_shared<DescriptionNode>(
                *DescriptionNode::create("Row", "transaction.row"));
            row->materialization_result = std::make_shared<const DescriptionMaterialization>(
                runtime::StateScopeSet{"transaction.scope"},
                std::vector<runtime::RuntimeDiagnostic>{}, 3U, 2U);
            return std::shared_ptr<const DescriptionNode>(std::move(row));
        });
    MaterializationPublicationLedger ledger;
    std::uint64_t first_id = 0U;
    {
        const std::shared_ptr<const DescriptionNode> first = generated->at(0U);
        first_id = first->materialization_result->transaction_id();
        check(ledger.claim(first->materialization_result) ==
                      MaterializationPublicationClaim::publish &&
                  ledger.claim(first->materialization_result) ==
                      MaterializationPublicationClaim::already_published &&
                  ledger.tracked_count() == 1U,
              "live materialization transaction was not published exactly once");
    }
    ledger.purge_expired();
    check(ledger.tracked_count() == 0U,
          "expired materialization publication retained an unbounded tombstone");
    std::uint64_t previous_id = first_id;
    for (std::size_t iteration = 0U; iteration < 32U; ++iteration) {
        {
            const std::shared_ptr<const DescriptionNode> rematerialized = generated->at(0U);
            const std::uint64_t next_id = rematerialized->materialization_result->transaction_id();
            check(next_id > previous_id &&
                      ledger.claim(rematerialized->materialization_result) ==
                          MaterializationPublicationClaim::publish &&
                      ledger.tracked_count() == 1U,
                  "evicted row rematerialization did not publish a fresh stable transaction "
                  "identity");
            previous_id = next_id;
        }
        ledger.purge_expired();
        check(ledger.tracked_count() == 0U,
              "materialization publication ledger grew under eviction churn");
    }
    check(evaluations == 33U,
          "evicted generated rows did not rematerialize exactly once per visit");
}

void test_variable_virtual_extents_and_stable_anchor() {
    using namespace strata;
    using namespace strata::ui;
    using strata::ui::collection::VirtualItemExtents;

    const VirtualItemExtents prefixes({10.0, 30.0, 20.0});
    check_near(prefixes.total(), 60.0, "virtual extent prefix total changed");
    check(prefixes.first_index_ending_after(10.0) == 1U,
          "virtual extent end lookup lost its strict boundary");
    check(prefixes.end_index_starting_before(40.0) == 2U,
          "virtual extent start lookup lost its strict boundary");
    check(prefixes.index_at(39.0) == std::optional<std::size_t>(1U),
          "virtual extent point lookup changed");

    const auto virtual_list = [](const std::vector<std::pair<std::string, double>>& rows) {
        std::vector<std::shared_ptr<const DescriptionNode>> children;
        std::vector<runtime::Value> keys;
        children.reserve(rows.size());
        keys.reserve(rows.size());
        for (const auto& [key, height] : rows) {
            children.push_back(node("Panel", key, {},
                                    layout_properties(object({
                                        {"height", runtime::Value(height)},
                                        {"width", runtime::Value(100.0)},
                                    }))));
            keys.emplace_back(runtime::KeyValue{key});
        }
        DescriptionNode::Properties properties = layout_properties(object({
            {"kind", runtime::Value("SCROLL")},
            {"virtualItemCount", runtime::Value(static_cast<double>(rows.size()))},
            {"virtualItemExtent", runtime::Value(20.0)},
            {"virtualItemKeys", runtime::Value(std::move(keys))},
            {"virtualMeasureItemExtents", runtime::Value(true)},
            {"virtualOverscan", runtime::Value(0.0)},
        }));
        properties.emplace("scrollOffset", runtime::ExpressionValue(object({
                                               {"x", runtime::Value(0.0)},
                                               {"y", runtime::Value(25.0)},
                                           })));
        return node("VirtualList", "variable.virtual", std::move(children), std::move(properties));
    };

    RetainedTree tree;
    static_cast<void>(tree.reconcile(virtual_list({{"a", 10.0}, {"b", 30.0}, {"c", 20.0}})));
    LayoutEngine layout;
    const LayoutEnvironment environment{
        0U,    Rect{0.0, 0.0, 100.0, 20.0}, 1.0,
        {},    PointSnapPolicy::nearest,    RectangleSnapPolicy::outward,
        false,
    };
    const LayoutRecord* first = layout.layout(tree, environment).find(tree.root()->identity());
    check(first != nullptr, "variable virtual list was not arranged");
    check_near(first->content_size.height, 60.0, "measured virtual content extent changed");
    check(first->visible_range == std::optional<VisibleRange>(VisibleRange{1U, 3U}),
          "variable prefix viewport lookup changed");

    static_cast<void>(tree.reconcile(virtual_list({{"a", 20.0}, {"b", 30.0}, {"c", 20.0}})));
    const LayoutRecord* resized = layout.layout(tree, environment).find(tree.root()->identity());
    check(resized != nullptr, "resized virtual list was not arranged");
    check_near(resized->scroll_offset.y, 35.0,
               "measured extent update did not preserve the keyed within-row anchor");

    static_cast<void>(tree.reconcile(virtual_list({
        {"x", 5.0},
        {"a", 20.0},
        {"b", 30.0},
        {"c", 20.0},
    })));
    const LayoutRecord* inserted = layout.layout(tree, environment).find(tree.root()->identity());
    check(inserted != nullptr, "inserted virtual list was not arranged");
    check_near(inserted->scroll_offset.y, 40.0,
               "keyed insertion did not preserve the virtual viewport anchor");
    check(inserted->virtual_items != nullptr && inserted->virtual_items->count() == 4U &&
              inserted->virtual_items->index_of_key("b") == std::optional<std::size_t>(2U) &&
              inserted->virtual_item_keys.size() < inserted->virtual_items->count(),
          "virtual layout retained an all-item key snapshot instead of an observed key window");
}

void test_layered_layout_uses_independent_axes() {
    using namespace strata;
    using namespace strata::ui;

    const auto explicitly_justified = node(
        "Panel",
        "layer.explicit",
        {node(
            "Panel",
            "layer.explicit.child",
            {},
            layout_properties(object({
                {"height", runtime::Value(10.0)},
                {"width", runtime::Value(10.0)},
            }))
        )},
        layout_properties(object({
            {"alignItems", runtime::Value("END")},
            {"height", runtime::Value(80.0)},
            {"justifyContent", runtime::Value("START")},
            {"kind", runtime::Value("PANEL")},
            {"width", runtime::Value(100.0)},
        }))
    );
    RetainedTree explicit_tree;
    static_cast<void>(explicit_tree.reconcile(explicitly_justified));
    LayoutEngine explicit_layout;
    const LayoutEnvironment environment{
        0U, Rect{0.0, 0.0, 100.0, 80.0}, 1.0,
        {}, PointSnapPolicy::nearest, RectangleSnapPolicy::outward, false,
    };
    const LayoutResult& explicit_result =
        explicit_layout.layout(explicit_tree, environment);
    const LayoutRecord* explicit_child = explicit_result.find(
        explicit_tree.find_key("layer.explicit.child")->identity()
    );
    check(
        explicit_child != nullptr && explicit_child->bounds.x == 90.0 &&
            explicit_child->bounds.y == 0.0,
        "layered layout did not apply alignItems horizontally and justifyContent vertically"
    );

    const auto compatible = node(
        "Panel",
        "layer.compatible",
        {node(
            "Panel",
            "layer.compatible.child",
            {},
            layout_properties(object({
                {"height", runtime::Value(10.0)},
                {"width", runtime::Value(10.0)},
            }))
        )},
        layout_properties(object({
            {"alignItems", runtime::Value("CENTER")},
            {"height", runtime::Value(80.0)},
            {"kind", runtime::Value("PANEL")},
            {"width", runtime::Value(100.0)},
        }))
    );
    RetainedTree compatible_tree;
    static_cast<void>(compatible_tree.reconcile(compatible));
    LayoutEngine compatible_layout;
    const LayoutResult& compatible_result =
        compatible_layout.layout(compatible_tree, environment);
    const LayoutRecord* compatible_child = compatible_result.find(
        compatible_tree.find_key("layer.compatible.child")->identity()
    );
    check(
        compatible_child != nullptr && compatible_child->bounds.x == 45.0 &&
            compatible_child->bounds.y == 35.0,
        "layered layout broke the legacy single-axis centering fallback"
    );
}

void test_anchored_portal_is_out_of_flow_and_flips() {
    using namespace strata;
    using namespace strata::ui;

    const auto anchor = node("Panel", "anchor", {},
                             layout_properties(object({
                                 {"height", runtime::Value(20.0)},
                                 {"width", runtime::Value(100.0)},
                             })));
    const auto popup = node("Panel", "popup", {},
                            layout_properties(object({
                                {"anchorAlign", runtime::Value("END")},
                                {"anchorFlip", runtime::Value(true)},
                                {"anchorGap", runtime::Value(5.0)},
                                {"anchorShift", runtime::Value(true)},
                                {"anchorSide", runtime::Value("BOTTOM")},
                                {"anchorTarget", runtime::Value("anchor")},
                                {"height", runtime::Value(40.0)},
                                {"kind", runtime::Value("PORTAL")},
                                {"matchAnchorWidth", runtime::Value(true)},
                                {"width", runtime::Value(80.0)},
                            })));
    const auto point_popup = node("Panel", "point.popup", {},
                                  layout_properties(object({
                                      {"anchorFlip", runtime::Value(true)},
                                      {"anchorGap", runtime::Value(5.0)},
                                      {"anchorPoint", object({
                                                          {"x", runtime::Value(190.0)},
                                                          {"y", runtime::Value(95.0)},
                                                      })},
                                      {"anchorShift", runtime::Value(true)},
                                      {"anchorSide", runtime::Value("BOTTOM")},
                                      {"height", runtime::Value(20.0)},
                                      {"kind", runtime::Value("PORTAL")},
                                      {"width", runtime::Value(30.0)},
                                  })));
    const auto root = node("Panel", "root", {popup, point_popup, anchor},
                           layout_properties(object({
                               {"alignItems", runtime::Value("START")},
                               {"height", runtime::Value(100.0)},
                               {"justifyContent", runtime::Value("END")},
                               {"kind", runtime::Value("COLUMN")},
                               {"width", runtime::Value(200.0)},
                           })));

    RetainedTree tree;
    static_cast<void>(tree.reconcile(root));
    LayoutEngine layout;
    const LayoutEnvironment environment{
        0U,    Rect{0.0, 0.0, 200.0, 100.0}, 1.0,
        {},    PointSnapPolicy::nearest,     RectangleSnapPolicy::outward,
        false,
    };
    const LayoutResult& result = layout.layout(tree, environment);
    const LayoutRecord* anchor_record = result.find(tree.find_key("anchor")->identity());
    const LayoutRecord* popup_record = result.find(tree.find_key("popup")->identity());
    const LayoutRecord* point_record = result.find(tree.find_key("point.popup")->identity());
    check(anchor_record != nullptr && popup_record != nullptr,
          "anchored portal fixture did not arrange both records");
    check_near(anchor_record->bounds.y, 80.0, "portal participated in its parent's column flow");
    check_near(popup_record->bounds.x, 0.0, "matched portal width did not preserve END alignment");
    check_near(popup_record->bounds.width, 100.0,
               "portal did not match its anchor width before arrangement");
    check_near(popup_record->bounds.y, 35.0,
               "portal did not flip to the larger collision-free side");
    check(popup_record->detached_from_parent_clip,
          "anchored portal stopped detaching from its parent clip");
    check(point_record != nullptr, "point-anchored portal was not arranged");
    check_near(point_record->bounds.x, 170.0,
               "point-anchored portal did not shift inside the viewport");
    check_near(point_record->bounds.y, 70.0,
               "point-anchored portal did not flip above its pointer anchor");
}

void test_virtualization_cache_queries_only_observed_keys() {
    using namespace strata;
    using namespace strata::ui::collection;

    class CountingKeySequence final : public runtime::KeyedSequence {
      public:
        CountingKeySequence(const std::uint64_t generation, const std::size_t count)
            : generation_(generation), count_(count) {}

        [[nodiscard]] std::uint64_t generation() const noexcept override {
            return generation_;
        }
        [[nodiscard]] std::size_t count() const noexcept override {
            return count_;
        }
        [[nodiscard]] std::string key_at(const std::size_t index) const override {
            if (index >= count_)
                throw std::out_of_range("counting key index is outside the sequence");
            ++key_queries;
            return "row." + std::to_string(index);
        }
        [[nodiscard]] std::optional<std::size_t>
        index_of_key(const std::string_view key) const override {
            ++lookup_queries;
            constexpr std::string_view prefix = "row.";
            if (!key.starts_with(prefix))
                return std::nullopt;
            const std::string suffix(key.substr(prefix.size()));
            if (suffix.empty() || !std::ranges::all_of(suffix, [](const unsigned char value) {
                    return value >= '0' && value <= '9';
                })) {
                return std::nullopt;
            }
            const std::size_t index = static_cast<std::size_t>(std::stoull(suffix));
            return index < count_ ? std::optional<std::size_t>(index) : std::nullopt;
        }

        mutable std::size_t key_queries = 0U;
        mutable std::size_t lookup_queries = 0U;

      private:
        std::uint64_t generation_;
        std::size_t count_;
    };

    auto initial = std::make_shared<CountingKeySequence>(1U, 10'000U);
    VirtualizationCache cache;
    static_cast<void>(cache.resolve(7U, VirtualExtentRequest{initial, 20.0, std::nullopt, false},
                                    65.0, std::span<const VirtualMeasurement>{}));
    check(initial->key_queries == 0U && initial->lookup_queries == 0U,
          "initial virtual extent resolution enumerated an unobserved key domain");
    static_cast<void>(cache.resolve(7U, VirtualExtentRequest{initial, 20.0, std::nullopt, false},
                                    65.0, std::span<const VirtualMeasurement>{}));
    check(initial->key_queries == 0U && initial->lookup_queries == 0U,
          "warm virtual extent resolution performed key work");

    auto replacement = std::make_shared<CountingKeySequence>(2U, 10'000U);
    const VirtualExtentResolution anchored =
        cache.resolve(7U, VirtualExtentRequest{replacement, 20.0, std::nullopt, false}, 65.0,
                      std::span<const VirtualMeasurement>{});
    check(initial->key_queries == 1U && replacement->key_queries == 0U &&
              replacement->lookup_queries == 1U && !anchored.anchor_changed,
          "virtual generation replacement queried more than the retained anchor key");
    auto sorted = std::make_shared<CountingKeySequence>(3U, 10'000U);
    const VirtualExtentResolution reset =
        cache.resolve(7U, VirtualExtentRequest{sorted, 20.0, std::nullopt, false, true}, 65.0,
                      std::span<const VirtualMeasurement>{});
    check(reset.anchor_changed && reset.offset == 0.0 && replacement->key_queries == 0U &&
              sorted->lookup_queries == 0U,
          "explicit sort/reset policy preserved an unrelated virtual anchor");
}

void test_retained_layout_cache_scale_and_invalidation() {
    using namespace strata;
    using namespace strata::ui;
    const auto fixed = [](const double width, const double height) {
        return layout_properties(object({
            {"height", runtime::Value(height)},
            {"width", runtime::Value(width)},
        }));
    };
    const auto fill = [](const double weight, const double height) {
        return layout_properties(object({
            {"height", runtime::Value(height)},
            {"width", object({{"weight", runtime::Value(weight)}})},
        }));
    };
    RetainedTree tree;
    static_cast<void>(tree.reconcile(node("Panel", "layout.root",
                                          {
                                              node("Panel", "layout.fixed", {}, fixed(20.0, 10.0)),
                                              node("Panel", "layout.fill", {}, fill(1.0, 8.0)),
                                          },
                                          layout_properties(object({
                                              {"alignItems", runtime::Value("STRETCH")},
                                              {"gap", runtime::Value(3.0)},
                                              {"kind", runtime::Value("ROW")},
                                              {"padding", runtime::Value(2.0)},
                                          })))));

    LayoutEngine layout;
    LayoutEnvironment environment{0U,   Rect{0.0, 0.0, 103.0, 30.0}, 1.5,
                                  {},   PointSnapPolicy::nearest,    RectangleSnapPolicy::outward,
                                  false};
    const LayoutResult& first = layout.layout(tree, environment);
    check(first.operations.measured_nodes == 3U && first.operations.arranged_nodes == 3U,
          "initial retained layout did not visit exactly its materialized nodes");
    const LayoutRecord* fixed_record = first.find(tree.find_key("layout.fixed")->identity());
    const LayoutRecord* fill_record = first.find(tree.find_key("layout.fill")->identity());
    check(fixed_record != nullptr && fill_record != nullptr,
          "layout records lost keyed retained identities");
    check_near(fixed_record->bounds.x, 2.0, "row fixed child x changed");
    check_near(fill_record->bounds.x, 25.0, "row fill child x changed");
    check_near(fill_record->bounds.width, 76.0, "row fill allocation changed");
    check_near(fixed_record->bounds.height, 26.0, "row stretch changed");
    check(!tree.find_key("layout.fixed")->dirty().contains(DirtyReason::layout),
          "layout stage did not consume its retained dirty reason");
    check_near(fixed_record->snapped_bounds.right() * environment.scale,
               std::round(fixed_record->snapped_bounds.right() * environment.scale),
               "fractional-scale rectangle edge was not pixel aligned");

    const LayoutResult& settled = layout.layout(tree, environment);
    check(settled.operations.measured_nodes == 0U && settled.operations.arranged_nodes == 0U,
          "settled retained layout performed active work");
    const std::uint64_t dirty_generation = tree.dirty_generation(DirtyReason::layout);
    static_cast<void>(tree.mark(tree.find_key("layout.fixed")->identity(), DirtyReason::layout));
    check(tree.dirty_generation(DirtyReason::layout) == dirty_generation + 1U,
          "typed layout dirty generation did not advance");
    const LayoutResult& invalidated = layout.layout(tree, environment);
    check(invalidated.operations.measured_nodes == 2U &&
              invalidated.operations.measurement_cache_hits == 1U &&
              invalidated.operations.arranged_nodes == 2U,
          "subtree layout invalidation did not reuse unaffected measurement and placement caches");
}

void test_retained_layout_cache_translates_unchanged_subtrees() {
    using namespace strata;
    using namespace strata::ui;
    const auto description = [](const double leading_height) {
        return node("Panel", "translation.root",
                    {
                        node("Panel", "translation.leading", {},
                             layout_properties(object({
                                 {"height", runtime::Value(leading_height)},
                                 {"width", runtime::Value(20.0)},
                             }))),
                        node("Panel", "translation.trailing",
                             {
                                 node("Panel", "translation.inner", {},
                                      layout_properties(object({
                                          {"height", runtime::Value(5.0)},
                                          {"width", runtime::Value(5.0)},
                                      }))),
                             },
                             layout_properties(object({
                                 {"clip", runtime::Value(true)},
                                 {"height", runtime::Value(12.0)},
                                 {"width", runtime::Value(20.0)},
                             }))),
                    },
                    layout_properties(object({
                        {"clip", runtime::Value(true)},
                        {"kind", runtime::Value("COLUMN")},
                    })));
    };

    RetainedTree tree;
    static_cast<void>(tree.reconcile(description(10.0)));
    LayoutEngine layout;
    const LayoutEnvironment environment{
        0U,    Rect{0.0, 0.0, 100.0, 60.0}, 1.5,
        {},    PointSnapPolicy::nearest,    RectangleSnapPolicy::outward,
        false,
    };
    const LayoutResult& initial = layout.layout(tree, environment);
    check(initial.operations.arranged_nodes == 4U, "initial translation fixture skipped layout");
    const std::uint64_t trailing_identity = tree.find_key("translation.trailing")->identity();
    const std::uint64_t inner_identity = tree.find_key("translation.inner")->identity();
    const LayoutRecord initial_trailing = *initial.find(trailing_identity);
    const LayoutRecord initial_inner = *initial.find(inner_identity);

    static_cast<void>(tree.reconcile(description(13.25)));
    const LayoutResult& shifted = layout.layout(tree, environment);
    const LayoutRecord* shifted_trailing = shifted.find(trailing_identity);
    const LayoutRecord* shifted_inner = shifted.find(inner_identity);
    check(shifted.operations.arranged_nodes == 2U && shifted_trailing != nullptr &&
              shifted_inner != nullptr,
          "translated arrangement cache did not skip the unchanged trailing subtree");
    check_near(shifted_trailing->bounds.y, initial_trailing.bounds.y + 3.25,
               "translated arrangement cache misplaced its subtree root");
    check_near(shifted_inner->bounds.y, initial_inner.bounds.y + 3.25,
               "translated arrangement cache misplaced a descendant");
    check(initial_trailing.local_clip.has_value() && initial_inner.clip.has_value() &&
              shifted_trailing->local_clip.has_value() && shifted_inner->clip.has_value() &&
              shifted_trailing->local_clip->y == initial_trailing.local_clip->y + 3.25 &&
              shifted_inner->clip->y == initial_inner.clip->y + 3.25,
          "translated arrangement cache left clipping geometry behind");
    check_near(shifted_trailing->snapped_bounds.y * environment.scale,
               std::round(shifted_trailing->snapped_bounds.y * environment.scale),
               "translated arrangement cache did not resnap moved geometry");

    const double shifted_y = shifted_trailing->bounds.y;
    static_cast<void>(tree.reconcile(description(14.5)));
    const LayoutResult& shifted_again = layout.layout(tree, environment);
    check(shifted_again.operations.arranged_nodes == 2U,
          "translated arrangement cache did not advance its retained placement");
    check_near(shifted_again.find(trailing_identity)->bounds.y, shifted_y + 1.25,
               "successive translated arrangement reuse accumulated the wrong offset");
}

void test_explicit_container_size_constrains_descendants() {
    using namespace strata;
    using namespace strata::ui;

    const auto fill_child = [](const std::string& key) {
        return node("Panel", key, {},
                    layout_properties(object({
                        {"height", runtime::Value(10.0)},
                        {"width", object({{"weight", runtime::Value(1.0)}})},
                    })));
    };
    RetainedTree tree;
    static_cast<void>(tree.reconcile(
        node("Panel", "constraint.root",
             {
                 node("Column", "constraint.shell",
                      {
                          node("Row", "constraint.row",
                               {
                                   fill_child("constraint.first"),
                                   fill_child("constraint.second"),
                               },
                               layout_properties(object({
                                   {"height", runtime::Value(10.0)},
                                   {"kind", runtime::Value("ROW")},
                                   {"width", object({{"weight", runtime::Value(1.0)}})},
                               }))),
                      },
                      layout_properties(object({
                          {"height", runtime::Value(30.0)},
                          {"kind", runtime::Value("COLUMN")},
                          {"padding", runtime::Value(10.0)},
                          {"width", runtime::Value(100.0)},
                      }))),
             })));

    LayoutEngine layout;
    const LayoutResult& result = layout.layout(tree, LayoutEnvironment{
                                                         0U,
                                                         Rect{0.0, 0.0, 300.0, 100.0},
                                                         1.0,
                                                         {},
                                                         PointSnapPolicy::nearest,
                                                         RectangleSnapPolicy::outward,
                                                         false,
                                                     });
    const LayoutRecord* shell = result.find(tree.find_key("constraint.shell")->identity());
    const LayoutRecord* row = result.find(tree.find_key("constraint.row")->identity());
    const LayoutRecord* second = result.find(tree.find_key("constraint.second")->identity());
    check(shell != nullptr && row != nullptr && second != nullptr,
          "explicit-size constraint fixture lost a layout record");
    check_near(shell->bounds.width, 100.0, "explicit container width changed");
    check_near(row->bounds.width, 80.0, "container padding did not constrain its fill child");
    check(second->bounds.right() <= shell->bounds.right() - 10.0,
          "row descendants escaped their explicitly sized container");
}

void test_scroll_content_box_constrains_fill_child() {
    using namespace strata;
    using namespace strata::ui;

    RetainedTree tree;
    static_cast<void>(
        tree.reconcile(node("Scroll", "scroll.box",
                            {
                                node("Panel", "scroll.fill", {},
                                     layout_properties(object({
                                         {"height", object({{"weight", runtime::Value(1.0)}})},
                                         {"width", object({{"weight", runtime::Value(1.0)}})},
                                     }))),
                            },
                            layout_properties(object({
                                {"contentPadding", runtime::Value(8.0)},
                                {"kind", runtime::Value("SCROLL")},
                                {"scrollVertical", runtime::Value(true)},
                                {"scrollbarGutter", runtime::Value(8.0)},
                                {"viewportInsets", runtime::Value(1.0)},
                            })))));

    LayoutEngine layout;
    const LayoutResult& result = layout.layout(tree, LayoutEnvironment{
                                                         0U,
                                                         Rect{0.0, 0.0, 100.0, 100.0},
                                                         1.0,
                                                         {},
                                                         PointSnapPolicy::nearest,
                                                         RectangleSnapPolicy::outward,
                                                         false,
                                                     });
    const LayoutRecord* scroll = result.find(tree.find_key("scroll.box")->identity());
    const LayoutRecord* child = result.find(tree.find_key("scroll.fill")->identity());
    check(scroll != nullptr && child != nullptr, "scroll content-box fixture lost layout records");
    check_near(child->bounds.x, scroll->content_bounds.x,
               "scroll fill child did not start at its content box");
    check_near(child->bounds.y, scroll->content_bounds.y,
               "scroll fill child did not start at its content box");
    check_near(child->bounds.width, scroll->content_bounds.width,
               "scroll fill child escaped the non-scrolling content axis");
}

void test_exiting_child_retains_placement_without_affecting_flow() {
    using namespace strata;
    using namespace strata::ui;

    const auto fill = [](const std::string& key) {
        return node("Panel", key, {},
                    layout_properties(object({
                        {"height", object({{"weight", runtime::Value(1.0)}})},
                        {"width", object({{"weight", runtime::Value(1.0)}})},
                    })));
    };
    const auto fixed = [](const std::string& key) {
        return node("Panel", key, {},
                    layout_properties(object({
                        {"height", runtime::Value(20.0)},
                        {"width", runtime::Value(20.0)},
                    })));
    };
    const auto row = [&](std::shared_ptr<const DescriptionNode> content) {
        return node("Row", "exit.flow.root", {std::move(content), fixed("exit.flow.stable")},
                    layout_properties(object({{"kind", runtime::Value("ROW")}})));
    };

    RetainedTree tree;
    static_cast<void>(tree.reconcile(row(fill("exit.flow.old"))));
    LayoutEngine layout;
    const LayoutEnvironment environment{
        0U,    Rect{0.0, 0.0, 100.0, 20.0}, 1.0,
        {},    PointSnapPolicy::nearest,    RectangleSnapPolicy::outward,
        false,
    };
    const LayoutResult& initial = layout.layout(tree, environment);
    const Rect old_bounds = initial.find(tree.find_key("exit.flow.old")->identity())->bounds;
    const Rect stable_bounds = initial.find(tree.find_key("exit.flow.stable")->identity())->bounds;

    static_cast<void>(tree.reconcile(row(fill("exit.flow.new")), [](const RetainedNode& candidate) {
        return candidate.description().key == "exit.flow.old";
    }));
    const LayoutResult& transitioned = layout.layout(tree, environment);
    const LayoutRecord* old_record = transitioned.find(tree.find_key("exit.flow.old")->identity());
    const LayoutRecord* new_record = transitioned.find(tree.find_key("exit.flow.new")->identity());
    const LayoutRecord* stable_record =
        transitioned.find(tree.find_key("exit.flow.stable")->identity());
    check(old_record != nullptr && new_record != nullptr && stable_record != nullptr,
          "exit flow fixture lost attached or retained presentation records");
    check(old_record->bounds == old_bounds,
          "EXITING child did not retain its historical presentation placement");
    check(stable_record->bounds == stable_bounds,
          "EXITING child changed an attached sibling's flow allocation");
    check_near(new_record->bounds.width, old_bounds.width,
               "replacement fill child did not receive the vacated flow allocation");
}

void test_nested_scroll_pin_uses_nearest_scroll_offset() {
    using namespace strata;
    using namespace strata::ui;
    DescriptionNode::Properties pinned = layout_properties(object({
        {"height", runtime::Value(20.0)},
        {"width", runtime::Value(20.0)},
    }));
    pinned.emplace("scrollPin", runtime::ExpressionValue(object({
                                    {"vertical", runtime::Value(true)},
                                })));
    DescriptionNode::Properties scroll = layout_properties(object({
        {"kind", runtime::Value("SCROLL")},
    }));
    scroll.emplace("scrollOffset", runtime::ExpressionValue(object({
                                       {"x", runtime::Value(0.0)},
                                       {"y", runtime::Value(50.0)},
                                   })));
    RetainedTree tree;
    static_cast<void>(
        tree.reconcile(node("Scroll", "pin.root",
                            {
                                node("Panel", "pin.container",
                                     {
                                         node("Panel", "pin.target", {}, std::move(pinned)),
                                     },
                                     layout_properties(object({
                                         {"height", runtime::Value(200.0)},
                                         {"width", object({{"weight", runtime::Value(1.0)}})},
                                     }))),
                            },
                            std::move(scroll))));
    LayoutEngine layout;
    const LayoutResult& result = layout.layout(tree, LayoutEnvironment{
                                                         0U,
                                                         Rect{0.0, 0.0, 100.0, 100.0},
                                                         1.0,
                                                         {},
                                                         PointSnapPolicy::nearest,
                                                         RectangleSnapPolicy::outward,
                                                         false,
                                                     });
    const LayoutRecord* pinned_record = result.find(tree.find_key("pin.target")->identity());
    check(pinned_record != nullptr, "nested scroll pin layout record is missing");
    check_near(pinned_record->bounds.y, 0.0,
               "nested scroll pin did not cancel nearest scroll offset");
}

void test_wrapped_linear_and_intrinsic_grid_layout() {
    using namespace strata;
    using namespace strata::ui;
    const auto environment = LayoutEnvironment{
        0U,    Rect{0.0, 0.0, 200.0, 200.0}, 1.0,
        {},    PointSnapPolicy::nearest,     RectangleSnapPolicy::outward,
        false,
    };
    const auto sized = [](const std::string& key, const double width, const double height) {
        return node("Panel", key, {},
                    layout_properties(object({
                        {"height", runtime::Value(height)},
                        {"width", runtime::Value(width)},
                    })));
    };

    RetainedTree row_tree;
    static_cast<void>(
        row_tree.reconcile(node("Panel", "wrap.row.root",
                                {
                                    node("Row", "wrap.row",
                                         {
                                             sized("wrap.row.first", 40.0, 10.0),
                                             sized("wrap.row.second", 40.0, 20.0),
                                             sized("wrap.row.third", 40.0, 30.0),
                                         },
                                         layout_properties(object({
                                             {"alignContent", runtime::Value("SPACE_BETWEEN")},
                                             {"gap", runtime::Value(5.0)},
                                             {"height", runtime::Value(100.0)},
                                             {"justifyContent", runtime::Value("END")},
                                             {"kind", runtime::Value("ROW")},
                                             {"width", runtime::Value(100.0)},
                                             {"wrap", runtime::Value(true)},
                                         }))),
                                })));
    LayoutEngine row_layout;
    const LayoutResult& row_result = row_layout.layout(row_tree, environment);
    const LayoutRecord* row_first =
        row_result.find(row_tree.find_key("wrap.row.first")->identity());
    const LayoutRecord* row_second =
        row_result.find(row_tree.find_key("wrap.row.second")->identity());
    const LayoutRecord* row_third =
        row_result.find(row_tree.find_key("wrap.row.third")->identity());
    check(row_first != nullptr && row_second != nullptr && row_third != nullptr,
          "wrapped row children were not arranged");
    check_near(row_first->bounds.x, 15.0, "wrapped row first-line justification changed");
    check_near(row_second->bounds.x, 60.0, "wrapped row main-axis gap changed");
    check_near(row_third->bounds.x, 60.0, "wrapped row second-line justification changed");
    check_near(row_third->bounds.y, 70.0, "wrapped row align-content distribution changed");

    RetainedTree column_tree;
    static_cast<void>(
        column_tree.reconcile(node("Panel", "wrap.column.root",
                                   {
                                       node("Column", "wrap.column",
                                            {
                                                sized("wrap.column.first", 10.0, 30.0),
                                                sized("wrap.column.second", 20.0, 30.0),
                                                sized("wrap.column.third", 30.0, 30.0),
                                            },
                                            layout_properties(object({
                                                {"alignContent", runtime::Value("END")},
                                                {"gap", runtime::Value(5.0)},
                                                {"height", runtime::Value(70.0)},
                                                {"justifyContent", runtime::Value("END")},
                                                {"kind", runtime::Value("COLUMN")},
                                                {"width", runtime::Value(100.0)},
                                                {"wrap", runtime::Value(true)},
                                            }))),
                                   })));
    LayoutEngine column_layout;
    const LayoutResult& column_result = column_layout.layout(column_tree, environment);
    const LayoutRecord* column_first =
        column_result.find(column_tree.find_key("wrap.column.first")->identity());
    const LayoutRecord* column_third =
        column_result.find(column_tree.find_key("wrap.column.third")->identity());
    check(column_first != nullptr && column_third != nullptr,
          "wrapped column children were not arranged");
    check_near(column_first->bounds.x, 45.0, "wrapped column align-content offset changed");
    check_near(column_first->bounds.y, 5.0, "wrapped column first-line justification changed");
    check_near(column_third->bounds.x, 70.0, "wrapped column cross-axis gap changed");
    check_near(column_third->bounds.y, 40.0, "wrapped column second-line justification changed");

    const auto grid_item = [](const std::string& key, const double width, const double height,
                              const double column, const double row, const double column_span,
                              const double row_span) {
        return node("Panel", key, {},
                    layout_properties(object({
                        {"alignSelf", runtime::Value("START")},
                        {"columnSpan", runtime::Value(column_span)},
                        {"gridColumn", runtime::Value(column)},
                        {"gridRow", runtime::Value(row)},
                        {"height", runtime::Value(height)},
                        {"justifySelf", runtime::Value("START")},
                        {"rowSpan", runtime::Value(row_span)},
                        {"width", runtime::Value(width)},
                    })));
    };
    DescriptionNode::Properties grid_properties = layout_properties(object({
        {"gap", runtime::Value(5.0)},
        {"kind", runtime::Value("GRID")},
    }));
    grid_properties.emplace("columns",
                            runtime::ExpressionValue(runtime::Value(std::vector<runtime::Value>{
                                object({
                                    {"max", runtime::Value(60.0)},
                                    {"min", runtime::Value(50.0)},
                                    {"preferred", runtime::Value("content")},
                                }),
                                runtime::Value("content"),
                            })));
    grid_properties.emplace("rows",
                            runtime::ExpressionValue(runtime::Value(std::vector<runtime::Value>{
                                runtime::Value("content"), runtime::Value("content")})));
    RetainedTree grid_tree;
    static_cast<void>(grid_tree.reconcile(
        node("Panel", "grid.root",
             {
                 node("Grid", "grid.intrinsic",
                      {
                          grid_item("grid.first", 30.0, 80.0, 0.0, 0.0, 1.0, 2.0),
                          grid_item("grid.second", 40.0, 20.0, 1.0, 0.0, 1.0, 1.0),
                          grid_item("grid.span", 120.0, 30.0, 0.0, 1.0, 2.0, 1.0),
                      },
                      std::move(grid_properties)),
             })));
    LayoutEngine grid_layout;
    const LayoutResult& grid_result = grid_layout.layout(grid_tree, environment);
    const LayoutRecord* grid = grid_result.find(grid_tree.find_key("grid.intrinsic")->identity());
    const LayoutRecord* grid_second =
        grid_result.find(grid_tree.find_key("grid.second")->identity());
    const LayoutRecord* grid_span = grid_result.find(grid_tree.find_key("grid.span")->identity());
    check(grid != nullptr && grid_second != nullptr && grid_span != nullptr,
          "intrinsic grid nodes were not arranged");
    check_near(grid->bounds.width, 120.0, "spanning contribution did not size intrinsic columns");
    check_near(grid->bounds.height, 80.0, "spanning contribution did not size intrinsic rows");
    check_near(grid_second->bounds.x, 65.0, "clamped content track offset changed");
    check_near(grid_span->bounds.width, 120.0, "spanning grid cell extent changed");
}

void test_content_size_motion_interrupts_and_settles() {
    using namespace strata;
    using namespace strata::ui;
    const auto moving_child = [](const double width) {
        DescriptionNode::Properties properties = layout_properties(object({
            {"height", runtime::Value(20.0)},
        }));
        properties.emplace("animateContentSize",
                           runtime::ExpressionValue(object({
                               {"clip", runtime::Value(true)},
                               {"durationNanos", runtime::Value(180'000'000.0)},
                               {"height", runtime::Value(false)},
                               {"width", runtime::Value(true)},
                           })));
        return node("Panel", "motion.child",
                    {
                        node("Spacer", "motion.content", {},
                             layout_properties(object({
                                 {"height", runtime::Value(20.0)},
                                 {"width", runtime::Value(width)},
                             }))),
                    },
                    std::move(properties));
    };
    const auto moving_root = [&moving_child](const double width) {
        return node("Panel", "motion.root",
                    {
                        moving_child(width),
                        node("Spacer", "motion.stable", {},
                             layout_properties(object({
                                 {"height", runtime::Value(10.0)},
                                 {"width", runtime::Value(10.0)},
                             }))),
                    });
    };
    RetainedTree tree;
    static_cast<void>(tree.reconcile(moving_root(40.0)));
    LayoutEngine layout;
    LayoutEnvironment environment{
        0U,    Rect{0.0, 0.0, 200.0, 80.0}, 1.0,
        {},    PointSnapPolicy::nearest,    RectangleSnapPolicy::outward,
        false,
    };
    environment.frame_time_nanos = 0;
    const LayoutResult& initial = layout.layout(tree, environment);
    check_near(initial.find(tree.find_key("motion.child")->identity())->bounds.width, 40.0,
               "initial content size should not animate from zero");

    static_cast<void>(tree.reconcile(moving_root(100.0)));
    const LayoutResult& started = layout.layout(tree, environment);
    check_near(started.find(tree.find_key("motion.child")->identity())->bounds.width, 40.0,
               "content-size motion did not start from its measured value");
    environment.frame_time_nanos = 90'000'000;
    const LayoutResult& midpoint = layout.layout(tree, environment);
    check_near(midpoint.find(tree.find_key("motion.child")->identity())->bounds.width, 70.0,
               "content-size motion midpoint changed");
    check(midpoint.operations.measured_nodes == 2U &&
              midpoint.operations.measurement_cache_hits == 2U,
          "content-size motion invalidated measurement outside its ancestor frontier");

    static_cast<void>(tree.reconcile(moving_root(60.0)));
    const LayoutResult& interrupted = layout.layout(tree, environment);
    check_near(interrupted.find(tree.find_key("motion.child")->identity())->bounds.width, 70.0,
               "content-size retarget did not interrupt from the current value");
    environment.frame_time_nanos = 180'000'000;
    const LayoutResult& interrupted_midpoint = layout.layout(tree, environment);
    check_near(interrupted_midpoint.find(tree.find_key("motion.child")->identity())->bounds.width,
               65.0, "interrupted content-size midpoint changed");

    environment.frame_time_nanos = 181'000'000;
    environment.reduced_motion = true;
    const LayoutResult& reduced = layout.layout(tree, environment);
    check_near(reduced.find(tree.find_key("motion.child")->identity())->bounds.width, 60.0,
               "reduced motion did not snap measured content size");
    environment.frame_time_nanos = 200'000'000;
    const LayoutResult& settled = layout.layout(tree, environment);
    check(settled.operations.measured_nodes == 0U && settled.operations.arranged_nodes == 0U,
          "completed content-size motion retained active layout work");
}

void test_scroll_virtual_range_clipping_and_safe_insets() {
    using namespace strata;
    using namespace strata::ui;
    auto generated =
        std::make_shared<const GeneratedDescriptionChildren>(1'000U, [](const std::size_t index) {
            return DescriptionNode::create("Row", "virtual." + std::to_string(index), {}, {},
                                           layout_properties(object({
                                               {"height", runtime::Value(20.0)},
                                               {"width", object({{"weight", runtime::Value(1.0)}})},
                                           })));
        });
    auto description = DescriptionNode::create("List", "virtual.root", {}, {},
                                               layout_properties(object({
                                                   {"clip", runtime::Value(true)},
                                                   {"kind", runtime::Value("SCROLL")},
                                                   {"virtualItemCount", runtime::Value(1'000.0)},
                                                   {"virtualItemExtent", runtime::Value(20.0)},
                                                   {"virtualOverscan", runtime::Value(1.0)},
                                               })),
                                               generated);
    auto ranged = std::make_shared<DescriptionNode>(*description);
    ranged->materialization = MaterializationRange{100U, 110U};
    ranged->properties.emplace("scrollOffset", runtime::ExpressionValue(object({
                                                   {"x", runtime::Value(0.0)},
                                                   {"y", runtime::Value(2'000.0)},
                                               })));
    RetainedTree tree;
    const ReconcileStats reconciled = tree.reconcile(ranged);
    check(reconciled.materialized == 10U, "virtual layout materialized off-range descriptions");

    LayoutEngine layout;
    const LayoutResult& result = layout.layout(tree, LayoutEnvironment{
                                                         3U,
                                                         Rect{0.0, 0.0, 120.0, 116.0},
                                                         1.5,
                                                         Edges{10.0, 8.0, 10.0, 8.0},
                                                         PointSnapPolicy::nearest,
                                                         RectangleSnapPolicy::outward,
                                                         true,
                                                     });
    const LayoutRecord* record = result.find(tree.root()->identity());
    check(record != nullptr && record->viewport.has_value() && record->clip.has_value(),
          "scroll layout did not publish viewport and clip metadata");
    check_near(record->bounds.x, 10.0, "safe left inset changed");
    check_near(record->bounds.y, 8.0, "safe top inset changed");
    check_near(record->content_size.height, 20'000.0, "virtual content extent changed");
    check(record->visible_range == std::optional<VisibleRange>(VisibleRange{99U, 106U}),
          "virtual visible range or overscan changed");
    check(record->materialized_child_indices.front() == 100U &&
              record->materialized_child_indices.back() == 109U,
          "virtual layout lost source indexes for materialized children");
}

void test_active_work_scheduler_and_detach_cleanup() {
    using namespace strata::ui;
    RetainedTree tree;
    static_cast<void>(tree.reconcile(node("Panel", "scheduler.root",
                                          {
                                              node("Panel", "scheduler.owner"),
                                          })));
    RetainedNode* owner = tree.find_key("scheduler.owner");
    check(owner != nullptr, "scheduler fixture owner is missing");

    std::size_t failures = 0U;
    WorkScheduler scheduler(
        [&failures](WorkKind, std::uint64_t, std::string_view, std::string_view) { ++failures; });
    scheduler.bind_owner(*owner);
    std::size_t animation_ticks = 0U;
    static_cast<void>(scheduler.schedule(
        WorkKind::animation, owner->identity(), "fade",
        [&animation_ticks](const std::int64_t) { return ++animation_ticks < 2U; }));
    static_cast<void>(scheduler.schedule(
        WorkKind::widget, owner->identity(), "broken",
        [](const std::int64_t) -> bool { throw std::runtime_error("fixture"); }));
    const WorkTickResult first = scheduler.tick(10);
    check(first.visited == 2U && first.failed == 1U && first.remaining == 1U && failures == 1U,
          "active-work scheduler did not isolate callback failure or retain animation work");
    const WorkTickResult second = scheduler.tick(20);
    check(second.visited == 1U && second.completed == 1U && second.remaining == 0U,
          "active-work scheduler did not retire completed animation work");
    const WorkTickResult settled = scheduler.tick(30);
    check(settled.visited == 0U && settled.remaining == 0U,
          "settled active-work scheduler polled the retained tree");

    static_cast<void>(scheduler.schedule(WorkKind::service, owner->identity(), "owner-service",
                                         [](const std::int64_t) { return true; }));
    static_cast<void>(tree.reconcile(node("Panel", "scheduler.root")));
    check(scheduler.active_count() == 0U,
          "retained detach did not cancel identity-owned scheduled work exactly once");

    WorkScheduler isolated;
    static_cast<void>(isolated.schedule(WorkKind::service, 1U, "isolated",
                                        [](const std::int64_t) { return true; }));
    check(isolated.active_count() == 1U && scheduler.active_count() == 0U,
          "active work leaked across runtime scheduler instances");
}

void test_portable_description_and_declaration_state(const std::filesystem::path& resource_root,
                                                     const std::filesystem::path& fixture_root) {
    using namespace strata;
    const std::filesystem::path scenario_directory = fixture_root;
    const auto load = [](const std::filesystem::path& root, const std::string& name) {
        return resource::load_utf8_resource(root, resource::ResourceId::parse(name));
    };
    const data::JsonValue schemas = data::parse_json(load(scenario_directory, "schemas.json"));
    const auto bundle = runtime::ApplicationBundle::create(&schemas);
    runtime::ApplicationContext application("description", bundle);
    static_cast<void>(application.host().adopt(runtime::HostSnapshot::from_json(
        "initial", 10U,
        data::parse_json(R"({"app":{"title":"Initial host","generation":10,"editor":"seed"}})"))));
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(compiler::ModuleSource{"app/main.strata",
                                                           load(scenario_directory, "main.strata")},
                                    no_imports, 0U)
              .activated(),
          "description fixture did not activate");

    ui::DescriptionBuilder builder(application);
    ui::DescriptionBuildResult description = builder.build(runtime::LayerRole::overlay, "Main");
    check(description.root != nullptr && description.diagnostics.empty(),
          "portable description produced diagnostics");
    ui::RetainedTree tree;
    const ui::ReconcileStats initial = tree.reconcile(description.root);
    check(initial.created >= 10U, "portable description did not materialize its widget tree");
    check(tree.find_key("dynamic.root") != nullptr &&
              tree.find_key("dynamic.increment") != nullptr &&
              tree.find_key("dynamic.count") != nullptr,
          "portable description key index is incomplete");
    ui::LayoutEngine layout;
    const ui::LayoutResult& laid_out = layout.layout(tree, ui::LayoutEnvironment{
                                                               0U,
                                                               ui::Rect{0.0, 0.0, 960.0, 720.0},
                                                               1.0,
                                                               {},
                                                               ui::PointSnapPolicy::nearest,
                                                               ui::RectangleSnapPolicy::outward,
                                                               false,
                                                           });
    const ui::LayoutRecord* dynamic_root = laid_out.find(tree.find_key("dynamic.root")->identity());
    check(dynamic_root != nullptr && dynamic_root->kind == ui::LayoutKind::column,
          "portable style declaration did not reach native layout");
    check_near(dynamic_root->content_bounds.x, 12.0, "portable style padding changed");
    const auto count_text = tree.find_key("dynamic.count")->description().properties.find("text");
    check(count_text != tree.find_key("dynamic.count")->description().properties.end() &&
              count_text->second.value() != nullptr &&
              *count_text->second.value()->string() == "Count 0",
          "derived description did not observe declaration state");
    const auto action_property =
        tree.find_key("dynamic.increment")->description().properties.find("onClick");
    check(action_property != tree.find_key("dynamic.increment")->description().properties.end() &&
              action_property->second.action() != nullptr,
          "typed widget action was lost while describing portable IR");
    const runtime::ActionDispatchOutcome adjusted = application.dispatch(
        runtime::ActionEvent{"activate", std::string("dynamic.increment"), runtime::Value{}},
        *(*action_property->second.action())->action,
        tree.find_key("dynamic.increment")->description().state_scope);
    check(adjusted.status == runtime::ActionDispatchStatus::handled,
          "described framework action did not dispatch");

    ui::DescriptionBuildResult updated = builder.build(runtime::LayerRole::overlay, "Main");
    const std::uint64_t count_identity = tree.find_key("dynamic.count")->identity();
    tree.clear_dirty();
    const ui::ReconcileStats update = tree.reconcile(updated.root);
    const auto updated_text = tree.find_key("dynamic.count")->description().properties.find("text");
    check(update.created == 0U && tree.find_key("dynamic.count")->identity() == count_identity &&
              updated_text->second.value() != nullptr &&
              *updated_text->second.value()->string() == "Count 1" &&
              tree.find_key("dynamic.count")->dirty().contains(ui::DirtyReason::text),
          "state-driven description update lost identity or text invalidation");

    runtime::ApplicationContext surface_application("surface", bundle);
    static_cast<void>(surface_application.host().adopt(runtime::HostSnapshot::from_json(
        "initial", 10U,
        data::parse_json(R"({"app":{"title":"Initial host","generation":10,"editor":"seed"}})"))));
    check(surface_application
              .compile_and_activate(compiler::ModuleSource{"app/main.strata",
                                                           load(scenario_directory, "main.strata")},
                                    no_imports, 0U)
              .activated(),
          "surface fixture did not activate");
    ui::SurfaceEnvironment surface_environment;
    surface_environment.framebuffer_width = 960;
    surface_environment.framebuffer_height = 720;
    surface_environment.logical_width = 960.0;
    surface_environment.logical_height = 720.0;
    surface_environment.reduced_motion = true;
    surface_environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    std::string host_clipboard = "clipboard value";
    std::vector<bool> ime_active_updates;
    std::vector<runtime::HostServiceRect> ime_rect_updates;
    runtime::HostServices host_services(
        [] { return true; },
        [&host_clipboard] { return runtime::HostClipboardRead{0U, host_clipboard}; },
        [&host_clipboard](const std::string_view text) {
            host_clipboard.assign(text);
            return 0U;
        },
        [] { return true; },
        [&ime_active_updates](const bool active) {
            ime_active_updates.push_back(active);
            return 0U;
        },
        [&ime_rect_updates](const runtime::HostServiceRect rect) {
            ime_rect_updates.push_back(rect);
            return 0U;
        });
    constexpr std::string_view public_surface_id = "native-dynamic-session";
    constexpr std::string_view private_host_owner = "native-runtime-7/surface-owner-3";
    ui::Surface surface(
        std::string(public_surface_id), surface_application, runtime::LayerRole::overlay, "Main",
        surface_environment,
        ui::TextEngine::load_control_font(
            resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")),
        ui::WidgetRegistry{}, ui::BehaviorRegistry{}, &host_services, ui::Theme{},
        std::string(private_host_owner));
    const ui::SurfaceFrame first_frame = surface.frame(5'000'000);
    check(first_frame.operations.rebuilds == 1U,
          "initial surface frame did not rebuild exactly once");
    check(surface.tree().root()->structural_path() == "/",
          "surface retained root path is not canonical");
    check(surface.tree().find_key("dynamic.root")->structural_path() == "/0/0",
          "authored surface structural path includes a non-protocol wrapper");
    const data::JsonValue inspection = ui::inspect_surface(surface);
    const data::JsonValue* inspected_root = inspection.find("root");
    check(inspected_root != nullptr && inspected_root->find("componentType") != nullptr &&
              *inspected_root->find("componentType")->string() == "SurfaceLayers",
          "canonical native surface inspection lost its synthetic root");
    const data::JsonValue* inspected_editors = inspection.find("editors");
    check(inspected_editors != nullptr && inspected_editors->array() != nullptr &&
              inspected_editors->array()->size() == 1U &&
              inspected_editors->array()->front().find("key") != nullptr &&
              inspected_editors->array()->front().find("key")->string() != nullptr &&
              *inspected_editors->array()->front().find("key")->string() == "dynamic.editor",
          "inspection editors projected ordinary static Text instead of only writable controls");
    check(surface.inspect_select("dynamic.editor"), "inspector could not select a retained editor");
    static_cast<void>(surface.frame(5'000'000));
    check(std::ranges::any_of(surface.render_commands().commands(),
                              [](const ui::RenderCommand& command) {
                                  const auto* border =
                                      std::get_if<ui::BorderRenderCommand>(&command);
                                  return border != nullptr && border->border.width == 2.0 &&
                                         border->border.color ==
                                             ui::RenderColor{255U, 191U, 76U, 255U};
                              }),
          "inspector selection did not append its visible layout-bounds overlay");
    surface.inspect_clear();
    static_cast<void>(surface.frame(5'000'000));
    const ui::SurfaceFrame settled_frame = surface.frame(5'000'000);
    check(settled_frame.operations.rebuilds == 0U &&
              settled_frame.operations.layout_measured_nodes == 0U &&
              settled_frame.operations.layout_arranged_nodes == 0U &&
              settled_frame.operations.render.nodes_visited == 0U,
          "settled native surface performed tree, layout, or render traversal work");
    const ui::RetainedNode* stable_hover_node = surface.tree().find_key("dynamic.modal");
    const ui::LayoutRecord* stable_hover_layout =
        stable_hover_node != nullptr ? surface.layout().find(stable_hover_node->identity())
                                     : nullptr;
    check(stable_hover_layout != nullptr, "stable pointer fixture has no arranged modal");
    const ui::Point stable_hover{
        stable_hover_layout->bounds.x + 2.0,
        stable_hover_layout->bounds.y + 2.0,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        stable_hover,
        ui::PointerEventType::move,
        0,
        0,
    }));
    static_cast<void>(surface.frame(6'000'000));
    const std::uint64_t stable_semantics_generation = surface.semantics().generation();
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        ui::Point{stable_hover.x + 0.25, stable_hover.y + 0.25},
        ui::PointerEventType::move,
        0,
        0,
    }));
    const ui::SurfaceFrame stable_pointer_frame = surface.frame(7'000'000);
    check(stable_pointer_frame.operations.input_events_processed == 1U &&
              stable_pointer_frame.operations.rebuilds == 0U &&
              stable_pointer_frame.operations.layout_measured_nodes == 0U &&
              stable_pointer_frame.operations.layout_arranged_nodes == 0U &&
              stable_pointer_frame.operations.render.nodes_visited == 0U &&
              surface.semantics().generation() == stable_semantics_generation,
          "pointer motion inside one retained target traversed layout, semantics, or rendering");
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        ui::Point{stable_hover.x + 0.5, stable_hover.y + 0.5},
        ui::PointerEventType::move,
        0,
        0,
    }));
    const ui::SurfaceFrame cached_pointer_frame = surface.frame(8'000'000);
    check(cached_pointer_frame.operations.input_events_processed == 1U &&
              cached_pointer_frame.operations.pointer_geometry_rebuilds == 0U &&
              cached_pointer_frame.operations.input_fast_path_frames == 1U &&
              cached_pointer_frame.operations.render.nodes_visited == 0U,
          "stable pointer motion rebuilt its retained geometry snapshot");
    const ui::InputOperationResult blocked = surface.input().click("dynamic.increment");
    check(blocked.events.empty() && blocked.action_outcomes.empty() &&
              surface.input().focused_key() == "dynamic.modal",
          "modal input ownership did not redirect background activation");
    const ui::InputOperationResult close_modal = surface.input().click("dynamic.modal.close");
    const runtime::Value* modal_open = surface_application.state().find(runtime::StateAddress{
        "dsl:app/main.strata:overlay Main/state:/overlay/Main/body/1",
        "modalOpen",
    });
    check(close_modal.events.size() >= 2U && modal_open != nullptr &&
              modal_open->boolean() != nullptr && !*modal_open->boolean(),
          "modal close action did not mutate its exact declaration-owned state");
    static_cast<void>(surface.frame(21'666'667));
    static_cast<void>(surface.input().click("dynamic.editor"));
    static_cast<void>(surface.frame(22'000'000));
    check(ime_active_updates == std::vector<bool>{true} && ime_rect_updates.size() == 1U &&
              ime_rect_updates.front().width > 0.0 && ime_rect_updates.front().height > 0.0,
          "focused editor did not publish one post-layout host IME caret rectangle");
    host_services.request_ime(public_surface_id, std::nullopt);
    check(ime_active_updates == std::vector<bool>{true},
          "public Surface id incorrectly aliased private host-service ownership");
    surface.input().invalidate_host_geometry();
    static_cast<void>(surface.frame(22'100'000));
    check(ime_rect_updates.size() == 2U,
          "host geometry invalidation did not address the private IME owner");
    static_cast<void>(surface.input().key("A", ui::KeyModifiers{false, true, false, false}));
    static_cast<void>(surface.input().key("V", ui::KeyModifiers{false, true, false, false}));
    static_cast<void>(surface.input().ime_preedit("ime", 1U, 2U));
    const ui::InputOperationResult committed = surface.input().text("✓");
    const data::JsonValue* committed_source =
        !committed.events.empty() ? committed.events.front().find("source") : nullptr;
    const data::JsonValue* committed_surface_id =
        committed_source != nullptr ? committed_source->find("surfaceId") : nullptr;
    check(committed.events.size() == 1U && committed_surface_id != nullptr &&
              committed_surface_id->string() != nullptr &&
              *committed_surface_id->string() == public_surface_id &&
              *committed_surface_id->string() != private_host_owner &&
              surface.input().edited_text(surface.tree().find_key("dynamic.editor")->identity()) !=
                  nullptr &&
              *surface.input().edited_text(surface.tree().find_key("dynamic.editor")->identity()) ==
                  "clipboard value✓",
          "clipboard/IME editor sequence lost public identity or portable editing state");
    static_cast<void>(surface.input().key("A", ui::KeyModifiers{false, true, false, false}));
    static_cast<void>(surface.input().key("C", ui::KeyModifiers{false, true, false, false}));
    check(host_clipboard == "clipboard value✓",
          "editor copy did not reach the installed host clipboard");
    surface.cancel_interactions();
    check(ime_active_updates == std::vector<bool>{true, false},
          "focus cancellation did not synchronously release the runtime-owned host IME");
    host_services.request_ime("sibling-surface", runtime::HostServiceRect{12.0, 24.0, 2.0, 18.0});
    check(ime_active_updates == std::vector<bool>{true, false, true},
          "sibling surface did not acquire shared host IME ownership");
    surface.cancel_interactions();
    check(ime_active_updates == std::vector<bool>{true, false, true},
          "empty-focus cancellation deactivated a sibling surface's host IME");
    host_services.request_ime("sibling-surface", std::nullopt);
    check(ime_active_updates == std::vector<bool>{true, false, true, false},
          "owning sibling did not release the shared host IME");
    constexpr std::string_view teardown_public_id = "teardown-public-surface";
    constexpr std::string_view teardown_private_owner = "native-runtime-9/surface-owner-1";
    host_services.request_ime(teardown_private_owner,
                              runtime::HostServiceRect{4.0, 8.0, 2.0, 16.0});
    {
        ui::Surface teardown_surface(std::string(teardown_public_id), surface_application,
                                     runtime::LayerRole::overlay, "Main", surface_environment,
                                     std::shared_ptr<const ui::TextEngine>{}, ui::WidgetRegistry{},
                                     ui::BehaviorRegistry{}, &host_services, ui::Theme{},
                                     std::string(teardown_private_owner));
        host_services.request_ime(teardown_public_id, std::nullopt);
        check(ime_active_updates == std::vector<bool>{true, false, true, false, true},
              "teardown public id incorrectly released the private host owner");
    }
    check(ime_active_updates == std::vector<bool>{true, false, true, false, true, false},
          "InputRouter teardown did not release its private host-service owner");
}

void test_repeater_description_is_data_backed() {
    using namespace strata;
    const auto collection_property = [](const std::uint64_t rebuilds,
                                        const std::uint64_t cache_hits) {
        auto view = std::shared_ptr<runtime::CollectionViewValue>(new runtime::CollectionViewValue{
            runtime::CollectionViewImmutableIdentity{
                runtime::Value(std::vector<runtime::Value>{runtime::Value("A")}),
                1U,
                1U,
                0U,
                1U,
                "window",
                rebuilds,
            },
        });
        view->cache_hits.store(cache_hits, std::memory_order_relaxed);
        return runtime::ExpressionValue(
            std::shared_ptr<const runtime::CollectionViewValue>(std::move(view)));
    };
    const runtime::ExpressionValue retained_property = collection_property(1U, 0U);
    const runtime::ExpressionValue rebuilt_property = collection_property(2U, 0U);
    const runtime::ExpressionValue cache_hit_property = collection_property(1U, 9U);
    check(!ui::expression_value_equal(retained_property, rebuilt_property) &&
              runtime::capture_expression_dependency(retained_property) !=
                  runtime::capture_expression_dependency(rebuilt_property) &&
              ui::expression_value_equal(retained_property, cache_hit_property) &&
              runtime::capture_expression_dependency(retained_property) ==
                  runtime::capture_expression_dependency(cache_hit_property),
          "CollectionView retained/dependency identity diverged on rebuilds or cacheHits");

    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("lazy-repeater-description", bundle);
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    const std::string source = R"(
component CollectionMetadataRepeater(view: any) {
  Repeater(key: "metadata.repeater", estimatedItemExtent: 24) {
    for item in [{ label: "row" }] {
      Panel(key: format("{0}{1}", view.total == 1 ? "one." : "many.", item.label))
    }
  }
}
overlay CollectionMetadata {
  root CollectionMetadataRepeater(view: window(env.rows, 0, 1))
}
overlay DirectCollectionMetadata {
  root Repeater(key: "direct.metadata.repeater", estimatedItemExtent: 24) {
    for item in window(env.rows, 0, 1) {
      Panel(key: format("{0}", item))
    }
  }
}
component LexicalSourceRepeater(item: list<string>) {
  Repeater(key: "lexical.source.repeater", estimatedItemExtent: 24) {
    for item in item {
      Panel(key: item)
    }
  }
}
overlay LexicalCollectionSource {
  state sourceItems = ["A"];
  root LexicalSourceRepeater(item: sourceItems)
}
overlay LazyRepeater {
  state unrelated = 0;
  state identitySuffix = "";
  root Repeater(key: "lazy.repeater", estimatedItemExtent: 24, layout: { width: 100, height: 48 }) {
    for item, itemIndex in [
      { key: "duplicate", label: "zero" }, { key: "duplicate", label: "one" },
      { key: "duplicate", label: "two" }, { key: "duplicate", label: "three" },
      { key: "duplicate", label: "four" }, { key: "duplicate", label: "five" }
    ] where itemIndex % 2 == 0 {
      Panel(key: format("{0}{1}", upper(item.label), identitySuffix), layout: { width: 100, height: item.label == "two" ? 36 : 24 }) {
        state rowFlag = false;
        Text(text: item.label)
      }
    }
  }
}
overlay DuplicateRepeater {
  root Repeater(key: "duplicate.repeater", estimatedItemExtent: 24) {
    for item in [
      { label: "same" }, { label: "SAME" }
    ] {
      Panel(key: upper(item.label))
    }
  }
}
overlay InvalidRepeater {
  root Repeater(key: "invalid.repeater", estimatedItemExtent: 24) {
    for item in [{ label: "" }] {
      Panel(key: item.label)
    }
  }
}
overlay RetainedRepeater {
  root Repeater(key: "retained.repeater", estimatedItemExtent: 40, layout: { width: 120, height: 40 }) {
    for item in [{ label: "pane" }] {
      SplitPane(key: upper(item.label), defaultRatio: 0.25, layout: { kind: "ROW", width: 120, height: 40 }) {
        Panel() { Text(text: "Left") }
        Panel() { Text(text: "Right") }
      }
    }
  }
}
overlay DiagnosticRepeater {
  root Repeater(key: "diagnostic.repeater", estimatedItemExtent: 24, layout: { width: 100, height: 24 }) {
    for item in [{ label: "diagnostic" }] {
      Panel(key: upper(item.label)) {
        Text(text: format("bad {0}", -item["missing"]))
      }
    }
  }
}
overlay IndexedDependencyRepeater {
  root Repeater(key: "indexed.repeater", estimatedItemExtent: 24) {
    for item in [{ label: "zero" }] {
      Panel(key: format("{0}", env["compact"] ? env["viewport"] : upper(item.label)))
    }
  }
}
)";
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"lazy-repeater.strata", source}, no_imports, 0U);
    std::string activation_failure = "no diagnostic";
    if (!activation.diagnostics.empty()) {
        const runtime::ActivationDiagnostic& diagnostic = activation.diagnostics.front();
        activation_failure = diagnostic.code + ": " + diagnostic.message;
        if (diagnostic.range.has_value()) {
            activation_failure += " at " + diagnostic.range->source_id + ":" +
                                  std::to_string(diagnostic.range->start.line) + ":" +
                                  std::to_string(diagnostic.range->start.column);
        }
    }
    check(activation.activated(), "lazy repeater fixture did not activate: " + activation_failure);
    ui::DescriptionBuilder builder(application);
    ui::RetainedTree retained;
    std::weak_ptr<const runtime::IndexableSequence> retained_sequence;
    {
        ui::DescriptionBuildResult built =
            builder.build(runtime::LayerRole::overlay, "LazyRepeater");
        check(built.diagnostics.empty(), "lazy repeater description produced diagnostics");
        std::shared_ptr<const ui::DescriptionNode> repeater = built.root->children->at(0U);
        const auto repeater_layout_property = repeater->properties.find("$layout");
        const runtime::Value* repeater_layout =
            repeater_layout_property != repeater->properties.end()
                ? repeater_layout_property->second.value()
                : nullptr;
        check(repeater->type == "VirtualList" && repeater->children->size() == 3U &&
                  repeater->materialization ==
                      std::optional<ui::MaterializationRange>(ui::MaterializationRange{}) &&
                  repeater->virtual_sequence != nullptr &&
                  repeater->virtual_sequence->count() == 3U &&
                  repeater->virtual_sequence->key_at(1U) == "TWO" &&
                  repeater->virtual_sequence->source_index_at(1U) == 2U &&
                  repeater->virtual_sequence->index_of_key("FOUR") ==
                      std::optional<std::size_t>(2U) &&
                  repeater->virtual_sequence->index_of_key("duplicate") == std::nullopt &&
                  repeater->virtual_sequence->item_at(1U).field("label") != nullptr &&
                  *repeater->virtual_sequence->item_at(1U).field("label")->string() == "two" &&
                  repeater_layout != nullptr &&
                  repeater_layout->field("virtualItemKeys") == nullptr &&
                  built.described_nodes == 2U,
              "Repeater did not retain its authored canonical root-key domain");
        std::shared_ptr<const ui::DescriptionNode> third = repeater->children->at(1U);
        check(third->key == std::optional<std::string>("TWO") &&
                  third->materialization_key == std::optional<std::string>("TWO") &&
                  third->children->size() == 1U && third->materialization_result != nullptr &&
                  third->materialization_result->owned_state_scopes.size() == 1U &&
                  third->materialization_result->evaluated_expressions != 0U &&
                  third->materialization_result->described_nodes == 2U,
              "lazy Repeater row did not materialize with its canonical computed identity");

        static_cast<void>(retained.reconcile(built.root));
        builder.set_retained_tree(&retained);
        const std::optional<runtime::StateScopeResolution> unrelated =
            application.resolve_state_scope("overlay LazyRepeater", "unrelated");
        check(unrelated.has_value() && unrelated->declaration != nullptr &&
                  application.state().write(unrelated->address,
                                            runtime::StateSlot{
                                                "unrelated",
                                                unrelated->declaration->type_id,
                                                runtime::Value(0.0),
                                                unrelated->declaration_scope,
                                            },
                                            runtime::Value(1.0)),
              "unrelated Repeater fixture state did not change");
        ui::DescriptionBuildResult settled =
            builder.build(runtime::LayerRole::overlay, "LazyRepeater");
        const std::shared_ptr<const ui::DescriptionNode> settled_repeater =
            settled.root->children->at(0U);
        check(settled_repeater->virtual_sequence == repeater->virtual_sequence &&
                  settled.evaluated_expressions + 9U <= built.evaluated_expressions,
              "unrelated state invalidated Repeater filtering or key extraction");
        const std::optional<runtime::StateScopeResolution> identity_suffix =
            application.resolve_state_scope("overlay LazyRepeater", "identitySuffix");
        check(identity_suffix.has_value() && identity_suffix->declaration != nullptr &&
                  application.state().write(identity_suffix->address,
                                            runtime::StateSlot{
                                                "identitySuffix",
                                                identity_suffix->declaration->type_id,
                                                runtime::Value(""),
                                                identity_suffix->declaration_scope,
                                            },
                                            runtime::Value("!")),
              "identity dependency fixture state did not change");
        ui::DescriptionBuildResult identity_changed =
            builder.build(runtime::LayerRole::overlay, "LazyRepeater");
        check(identity_changed.root->children->at(0U)->virtual_sequence !=
                      repeater->virtual_sequence &&
                  identity_changed.root->children->at(0U)->virtual_sequence->key_at(1U) == "TWO!",
              "referenced lexical identity value did not invalidate the Repeater sequence");
        check(application.state().write(identity_suffix->address,
                                        runtime::StateSlot{
                                            "identitySuffix",
                                            identity_suffix->declaration->type_id,
                                            runtime::Value(""),
                                            identity_suffix->declaration_scope,
                                        },
                                        runtime::Value("")),
              "identity dependency fixture state did not reset");
        retained_sequence = repeater->virtual_sequence;
        static_cast<void>(retained.reconcile(settled.root));
    }

    bool duplicate_rejected = false;
    try {
        static_cast<void>(builder.build(runtime::LayerRole::overlay, "DuplicateRepeater"));
    } catch (const std::invalid_argument& error) {
        duplicate_rejected =
            std::string_view(error.what()).find("duplicated") != std::string_view::npos;
    }
    check(duplicate_rejected, "duplicate computed Repeater root keys were not rejected");

    bool invalid_key_rejected = false;
    try {
        static_cast<void>(builder.build(runtime::LayerRole::overlay, "InvalidRepeater"));
    } catch (const std::invalid_argument& error) {
        invalid_key_rejected =
            std::string_view(error.what()).find("non-empty") != std::string_view::npos;
    }
    check(invalid_key_rejected, "empty computed Repeater root keys were not rejected");

    {
        ui::RetainedTree metadata_retained;
        builder.set_retained_tree(nullptr);
        builder.set_contextual_host_roots({
            {"env", object({
                        {"rows", runtime::Value(std::vector<runtime::Value>{runtime::Value("A")})},
                    })},
        });
        ui::DescriptionBuildResult one =
            builder.build(runtime::LayerRole::overlay, "CollectionMetadata");
        const std::shared_ptr<const ui::DescriptionNode> one_repeater = one.root->children->at(0U);
        const auto one_view =
            one_repeater->virtual_sequence_generation->lexical_dependencies.find("view");
        check(one_repeater->virtual_sequence->key_at(0U) == "one.row" &&
                  one_view !=
                      one_repeater->virtual_sequence_generation->lexical_dependencies.end() &&
                  one_view->second.kind == runtime::ExpressionDependencyValueKind::collection &&
                  one_view->second.collection.total == 1U,
              "Repeater did not stamp external CollectionView metadata");
        static_cast<void>(metadata_retained.reconcile(one.root));
        builder.set_retained_tree(&metadata_retained);

        builder.set_contextual_host_roots({
            {"env", object({
                        {"rows", runtime::Value(std::vector<runtime::Value>{
                                     runtime::Value("A"),
                                     runtime::Value("B"),
                                 })},
                    })},
        });
        ui::DescriptionBuildResult two =
            builder.build(runtime::LayerRole::overlay, "CollectionMetadata");
        const std::shared_ptr<const ui::DescriptionNode> two_repeater = two.root->children->at(0U);
        check(two_repeater->virtual_sequence != one_repeater->virtual_sequence &&
                  two_repeater->virtual_sequence->key_at(0U) == "many.row" &&
                  two_repeater->virtual_sequence_generation->lexical_dependencies.at("view")
                          .collection.total == 2U,
              "equal Repeater items hid a changed external CollectionView total");
        static_cast<void>(metadata_retained.reconcile(two.root));
        builder.set_retained_tree(&metadata_retained);

        builder.set_contextual_host_roots({
            {"env", object({
                        {"rows", runtime::Value(std::vector<runtime::Value>{
                                     runtime::Value("A"),
                                     runtime::Value("C"),
                                 })},
                    })},
        });
        ui::DescriptionBuildResult same_metadata =
            builder.build(runtime::LayerRole::overlay, "CollectionMetadata");
        check(same_metadata.root->children->at(0U)->virtual_sequence ==
                      two_repeater->virtual_sequence &&
                  same_metadata.root->children->at(0U)->virtual_sequence->key_at(0U) == "many.row",
              "unchanged CollectionView metadata invalidated the Repeater generation");
        static_cast<void>(metadata_retained.reconcile(same_metadata.root));
        builder.set_retained_tree(&metadata_retained);

        ui::DescriptionBuildResult stable_cache_hit =
            builder.build(runtime::LayerRole::overlay, "CollectionMetadata");
        check(stable_cache_hit.root->children->at(0U)->virtual_sequence ==
                      two_repeater->virtual_sequence &&
                  stable_cache_hit.root->children->at(0U)->virtual_sequence->key_at(0U) ==
                      "many.row",
              "a stable CollectionView helper cache hit invalidated the Repeater generation");
    }

    {
        ui::RetainedTree direct_source_retained;
        builder.set_retained_tree(nullptr);
        builder.set_contextual_host_roots({
            {"env", object({
                        {"rows", runtime::Value(std::vector<runtime::Value>{runtime::Value("A")})},
                        {"unrelated", runtime::Value("first")},
                    })},
        });
        ui::DescriptionBuildResult one =
            builder.build(runtime::LayerRole::overlay, "DirectCollectionMetadata");
        const std::shared_ptr<const ui::DescriptionNode> one_repeater = one.root->children->at(0U);
        check(one_repeater->virtual_sequence->key_at(0U) == "A" &&
                  one_repeater->virtual_sequence_generation->source.kind ==
                      runtime::ExpressionDependencyValueKind::collection &&
                  one_repeater->virtual_sequence_generation->source.collection.total == 1U &&
                  one_repeater->virtual_sequence_generation->lexical_dependencies.empty() &&
                  one_repeater->virtual_sequence_generation->host_dependencies.size() == 1U &&
                  one_repeater->virtual_sequence_generation->host_dependencies.begin()
                      ->second.contextual &&
                  one_repeater->virtual_sequence_generation->host_dependencies.begin()
                          ->second.path.size() == 2U,
              "direct Repeater source evaluation did not retain its exact host/view stamp");
        static_cast<void>(direct_source_retained.reconcile(one.root));
        builder.set_retained_tree(&direct_source_retained);

        builder.set_contextual_host_roots({
            {"env", object({
                        {"rows", runtime::Value(std::vector<runtime::Value>{
                                     runtime::Value("A"),
                                     runtime::Value("B"),
                                 })},
                        {"unrelated", runtime::Value("second")},
                    })},
        });
        ui::DescriptionBuildResult two =
            builder.build(runtime::LayerRole::overlay, "DirectCollectionMetadata");
        const std::shared_ptr<const ui::DescriptionNode> two_repeater = two.root->children->at(0U);
        check(two_repeater->virtual_sequence != one_repeater->virtual_sequence &&
                  two_repeater->virtual_sequence->key_at(0U) == "A" &&
                  two_repeater->virtual_sequence_generation->source.collection.items ==
                      one_repeater->virtual_sequence_generation->source.collection.items &&
                  two_repeater->virtual_sequence_generation->source.collection.total == 2U,
              "direct Repeater source collapsed changed metadata into equal visible items");
        static_cast<void>(direct_source_retained.reconcile(two.root));
        builder.set_retained_tree(&direct_source_retained);

        builder.set_contextual_host_roots({
            {"env", object({
                        {"rows", runtime::Value(std::vector<runtime::Value>{
                                     runtime::Value("A"),
                                     runtime::Value("B"),
                                 })},
                        {"unrelated", runtime::Value("third")},
                    })},
        });
        ui::DescriptionBuildResult stable =
            builder.build(runtime::LayerRole::overlay, "DirectCollectionMetadata");
        check(stable.root->children->at(0U)->virtual_sequence == two_repeater->virtual_sequence,
              "stable direct-source cache hit or unrelated host sibling rebuilt the Repeater");
    }

    {
        ui::RetainedTree lexical_source_retained;
        builder.set_retained_tree(nullptr);
        ui::DescriptionBuildResult one =
            builder.build(runtime::LayerRole::overlay, "LexicalCollectionSource");
        const std::shared_ptr<const ui::DescriptionNode> one_repeater = one.root->children->at(0U);
        const auto lexical =
            one_repeater->virtual_sequence_generation->lexical_dependencies.find("item");
        check(one_repeater->virtual_sequence->count() == 1U &&
                  one_repeater->virtual_sequence->key_at(0U) == "A" &&
                  one_repeater->virtual_sequence_generation->source.kind ==
                      runtime::ExpressionDependencyValueKind::scalar &&
                  lexical !=
                      one_repeater->virtual_sequence_generation->lexical_dependencies.end() &&
                  lexical->second.kind == runtime::ExpressionDependencyValueKind::scalar,
              "direct lexical Repeater source was not included in its dependency stamp");
        static_cast<void>(lexical_source_retained.reconcile(one.root));
        builder.set_retained_tree(&lexical_source_retained);

        const std::optional<runtime::StateScopeResolution> source_items =
            application.resolve_state_scope("overlay LexicalCollectionSource", "sourceItems");
        check(source_items.has_value() && source_items->declaration != nullptr &&
                  application.state().write(
                      source_items->address,
                      runtime::StateSlot{
                          "sourceItems",
                          source_items->declaration->type_id,
                          runtime::Value(std::vector<runtime::Value>{runtime::Value("A")}),
                          source_items->declaration_scope,
                      },
                      runtime::Value(std::vector<runtime::Value>{
                          runtime::Value("A"),
                          runtime::Value("B"),
                      })),
              "direct lexical Repeater source fixture state did not change");
        ui::DescriptionBuildResult two =
            builder.build(runtime::LayerRole::overlay, "LexicalCollectionSource");
        check(two.root->children->at(0U)->virtual_sequence != one_repeater->virtual_sequence &&
                  two.root->children->at(0U)->virtual_sequence->count() == 2U,
              "changed direct lexical Repeater source reused a stale sequence");
    }

    {
        ui::RetainedTree indexed_retained;
        builder.set_contextual_host_roots({
            {"env", object({
                        {"unrelated", runtime::Value(0.0)},
                        {"viewport", runtime::Value("compact.a")},
                    })},
        });
        ui::DescriptionBuildResult missing =
            builder.build(runtime::LayerRole::overlay, "IndexedDependencyRepeater");
        const std::shared_ptr<const ui::DescriptionNode> missing_repeater =
            missing.root->children->at(0U);
        check(missing_repeater->virtual_sequence->key_at(0U) == "ZERO" &&
                  missing_repeater->virtual_sequence_generation->host_dependencies.size() == 1U &&
                  !missing_repeater->virtual_sequence_generation->host_dependencies.begin()
                       ->second.value.has_value(),
              "missing indexed contextual dependency was not recorded distinctly");
        static_cast<void>(indexed_retained.reconcile(missing.root));
        builder.set_retained_tree(&indexed_retained);

        builder.set_contextual_host_roots({
            {"env", object({
                        {"compact", runtime::Value{}},
                        {"unrelated", runtime::Value(0.0)},
                        {"viewport", runtime::Value("compact.a")},
                    })},
        });
        ui::DescriptionBuildResult explicit_null =
            builder.build(runtime::LayerRole::overlay, "IndexedDependencyRepeater");
        check(explicit_null.root->children->at(0U)->virtual_sequence !=
                  missing_repeater->virtual_sequence,
              "explicit null reused a generation stamped with a missing indexed path");
        static_cast<void>(indexed_retained.reconcile(explicit_null.root));
        builder.set_retained_tree(&indexed_retained);

        builder.set_contextual_host_roots({
            {"env", object({
                        {"compact", runtime::Value(false)},
                        {"unrelated", runtime::Value(1.0)},
                        {"viewport", runtime::Value("compact.a")},
                    })},
        });
        ui::DescriptionBuildResult false_guard =
            builder.build(runtime::LayerRole::overlay, "IndexedDependencyRepeater");
        static_cast<void>(indexed_retained.reconcile(false_guard.root));
        builder.set_retained_tree(&indexed_retained);
        const std::shared_ptr<const runtime::IndexableSequence> false_sequence =
            false_guard.root->children->at(0U)->virtual_sequence;

        builder.set_contextual_host_roots({
            {"env", object({
                        {"compact", runtime::Value(false)},
                        {"unrelated", runtime::Value(2.0)},
                        {"viewport", runtime::Value("compact.b")},
                    })},
        });
        ui::DescriptionBuildResult short_circuited =
            builder.build(runtime::LayerRole::overlay, "IndexedDependencyRepeater");
        check(short_circuited.root->children->at(0U)->virtual_sequence == false_sequence,
              "an unread conditional host branch invalidated the Repeater generation");

        builder.set_contextual_host_roots({
            {"env", object({
                        {"compact", runtime::Value(true)},
                        {"unrelated", runtime::Value(2.0)},
                        {"viewport", runtime::Value("compact.b")},
                    })},
        });
        ui::DescriptionBuildResult selected_branch =
            builder.build(runtime::LayerRole::overlay, "IndexedDependencyRepeater");
        check(selected_branch.root->children->at(0U)->virtual_sequence != false_sequence &&
                  selected_branch.root->children->at(0U)->virtual_sequence->key_at(0U) ==
                      "compact.b" &&
                  selected_branch.root->children->at(0U)
                          ->virtual_sequence_generation->host_dependencies.size() == 2U,
              "a selected indexed host branch did not retrace its exact dependency set");
        builder.set_contextual_host_roots({});
        builder.set_retained_tree(&retained);
    }

    {
        ui::DescriptionBuildResult diagnostic_build =
            builder.build(runtime::LayerRole::overlay, "DiagnosticRepeater");
        const std::shared_ptr<const ui::DescriptionNode> diagnostic_repeater =
            diagnostic_build.root->children->at(0U);
        const std::shared_ptr<const ui::DescriptionNode> first_diagnostic_row =
            diagnostic_repeater->children->at(0U);
        const std::shared_ptr<const ui::DescriptionNode> second_diagnostic_row =
            diagnostic_repeater->children->at(0U);
        check(first_diagnostic_row == second_diagnostic_row &&
                  first_diagnostic_row->materialization_result != nullptr &&
                  !first_diagnostic_row->materialization_result->diagnostics.empty() &&
                  second_diagnostic_row->materialization_result != nullptr &&
                  second_diagnostic_row->materialization_result ==
                      first_diagnostic_row->materialization_result &&
                  second_diagnostic_row->materialization_result->diagnostics.size() ==
                      first_diagnostic_row->materialization_result->diagnostics.size() &&
                  second_diagnostic_row->materialization_result->evaluated_expressions ==
                      first_diagnostic_row->materialization_result->evaluated_expressions &&
                  second_diagnostic_row->materialization_result->described_nodes ==
                      first_diagnostic_row->materialization_result->described_nodes,
              "lazy row materialization diagnostics or counters accumulated across requests");
    }

    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 160;
    environment.framebuffer_height = 80;
    environment.logical_width = 160.0;
    environment.logical_height = 80.0;
    {
        ui::Surface surface("lazy-row-state", application, runtime::LayerRole::overlay,
                            "LazyRepeater", environment);
        const ui::SurfaceFrame first_frame = surface.frame(0);
        const ui::RetainedNode* row = surface.tree().find_key("ZERO");
        check(row != nullptr && first_frame.operations.evaluated_expressions != 0U &&
                  first_frame.operations.described_nodes != 0U,
              "Surface did not publish lazy row work at the frame boundary");
        const std::optional<runtime::StateScopeResolution> row_state =
            application.resolve_state_scope(row->description().state_scope, "rowFlag");
        check(row_state.has_value() && row_state->declaration != nullptr &&
                  application.state().write(row_state->address,
                                            runtime::StateSlot{
                                                "rowFlag",
                                                row_state->declaration->type_id,
                                                runtime::Value(false),
                                                row_state->declaration_scope,
                                            },
                                            runtime::Value(true)),
              "materialized Repeater row state was not writable");
        static_cast<void>(surface.frame(16'666'667));
        const runtime::Value* retained_row_state = application.state().find(row_state->address);
        check(retained_row_state != nullptr && retained_row_state->boolean() != nullptr &&
                  *retained_row_state->boolean(),
              "lazy row owned state was pruned and reinitialized during rebuild");
    }
    {
        ui::Surface surface("lazy-retained-hook", application, runtime::LayerRole::overlay,
                            "RetainedRepeater", environment);
        static_cast<void>(surface.frame(0));
        const ui::RetainedNode* pane = surface.tree().find_key("PANE");
        check(pane != nullptr, "repeated SplitPane did not materialize");
        check(surface.tree().set_retained_value(pane->identity(), "strata.gesture.splitRatio",
                                                runtime::Value(0.75)),
              "repeated SplitPane retained ratio fixture did not change");
        surface.invalidate();
        static_cast<void>(surface.frame(16'666'667));
        pane = surface.tree().find_key("PANE");
        const runtime::Value* first_layout =
            pane != nullptr && !pane->children().empty()
                ? pane->children().front()->description().properties.at("$layout").value()
                : nullptr;
        const runtime::Value* first_width =
            first_layout != nullptr ? first_layout->field("width") : nullptr;
        check(first_width != nullptr && first_width->field("weight") != nullptr &&
                  first_width->field("weight")->number() != nullptr &&
                  std::abs(*first_width->field("weight")->number() - 0.75) < 0.000'001,
              "lazy row widget hook did not receive its retained-node snapshot");
    }
    {
        ui::Surface surface("lazy-diagnostic-publication", application, runtime::LayerRole::overlay,
                            "DiagnosticRepeater", environment);
        const ui::SurfaceFrame diagnosed = surface.frame(0);
        const auto diagnostic = std::ranges::find(
            diagnosed.diagnostics.records, std::string_view("STRATA.DSL.RUNTIME_TYPE_MISMATCH"),
            [](const runtime::RuntimeDiagnosticRecord& record) {
                return std::string_view(record.diagnostic.code);
            });
        check(diagnostic != diagnosed.diagnostics.records.end() &&
                  diagnostic->occurrence_count == 1U,
              "overlapping lazy row materialization published its transaction more than once");
        const std::size_t occurrence_count = diagnostic->occurrence_count;
        const ui::SurfaceFrame settled = surface.frame(16'666'667);
        const auto settled_diagnostic = std::ranges::find(
            settled.diagnostics.records, std::string_view("STRATA.DSL.RUNTIME_TYPE_MISMATCH"),
            [](const runtime::RuntimeDiagnosticRecord& record) {
                return std::string_view(record.diagnostic.code);
            });
        check(settled_diagnostic != settled.diagnostics.records.end() &&
                  settled_diagnostic->occurrence_count == occurrence_count &&
                  settled.operations.evaluated_expressions == 0U &&
                  settled.operations.described_nodes == 0U,
              "settled frame republished or accumulated lazy materialization work");
    }

    std::shared_ptr<const ui::DescriptionNode> detached_repeater;
    {
        ui::DescriptionBuilder temporary_builder(application);
        ui::DescriptionBuildResult temporary =
            temporary_builder.build(runtime::LayerRole::overlay, "LazyRepeater");
        detached_repeater = temporary.root->children->at(0U);
    }
    const std::shared_ptr<const ui::DescriptionNode> detached_row =
        detached_repeater->children->at(1U);
    check(detached_row->key == std::optional<std::string>("TWO"),
          "lazy Repeater description borrowed its destroyed builder");

    ui::DescriptionBuilder snapshot_builder(application);
    {
        ui::RetainedTree temporary_retained;
        ui::DescriptionBuildResult temporary =
            snapshot_builder.build(runtime::LayerRole::overlay, "LazyRepeater");
        static_cast<void>(temporary_retained.reconcile(temporary.root));
        snapshot_builder.set_retained_tree(&temporary_retained);
    }
    ui::DescriptionBuildResult after_retained_destruction =
        snapshot_builder.build(runtime::LayerRole::overlay, "LazyRepeater");
    check(after_retained_destruction.root->children->at(0U)->virtual_sequence->key_at(1U) == "TWO",
          "description sequence reuse borrowed its destroyed retained tree");

    const std::string replacement_source = R"(
overlay LazyRepeater {
  root Repeater(key: "lazy.repeater", estimatedItemExtent: 24) {
    for item in [{ label: "seven" }, { label: "eight" }] {
      Panel(key: upper(item.label))
    }
  }
}
)";
    check(application
              .compile_and_activate(
                  compiler::ModuleSource{"lazy-repeater-replacement.strata", replacement_source},
                  no_imports, 1U)
              .activated(),
          "replacement Repeater fixture did not activate");
    ui::DescriptionBuildResult replacement =
        builder.build(runtime::LayerRole::overlay, "LazyRepeater");
    check(replacement.root->children->at(0U)->virtual_sequence->key_at(0U) == "SEVEN",
          "active-unit replacement reused an obsolete Repeater sequence");
    static_cast<void>(retained.reconcile(replacement.root));
    check(retained_sequence.expired(),
          "retained Repeater sequence cache outlived active-unit replacement reconciliation");
}

void test_surface_contextual_environment(const std::filesystem::path& resource_root) {
    using namespace strata;
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("surface-environment", bundle);
    const std::string source = R"(
component EnvironmentPanel() {
  Panel(key: "environment.root") {
    Text(key: "environment.viewport", text: env.viewport)
    Text(key: "environment.orientation", text: env.orientation)
    Text(key: "environment.pointer", text: env.pointerPrecision)
    Text(key: "environment.density", text: env.density)
    Text(key: "environment.width", text: env.width)
    if env.compact {
      Text(key: "environment.compact", text: "compact")
    } else {
      if env.wide {
        Text(key: "environment.wide", text: "wide")
      } else {
        Text(key: "environment.regular", text: "regular")
      }
    }
  }
}
overlay Environment {
  root EnvironmentPanel()
}
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(compiler::ModuleSource{"environment.strata", source},
                                    no_imports, 0U)
              .activated(),
          "surface contextual environment fixture did not activate");

    ui::SurfaceEnvironment compact_environment;
    compact_environment.framebuffer_width = 960;
    compact_environment.framebuffer_height = 1'440;
    compact_environment.logical_width = 480.0;
    compact_environment.logical_height = 720.0;
    compact_environment.safe_insets = ui::Edges{10.0, 0.0, 10.0, 0.0};
    compact_environment.density = ui::SurfaceDensity::compact;
    compact_environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::coarse, true, true, false, false, false,
    };
    ui::SurfaceEnvironment wide_environment;
    wide_environment.framebuffer_width = 2'400;
    wide_environment.framebuffer_height = 1'400;
    wide_environment.logical_width = 1'200.0;
    wide_environment.logical_height = 700.0;
    wide_environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, true,
    };
    const std::shared_ptr<const ui::TextEngine> text_engine = ui::TextEngine::load_control_font(
        resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf"));
    ui::Surface compact("environment-compact", application, runtime::LayerRole::overlay,
                        "Environment", compact_environment, text_engine);
    ui::Surface wide("environment-wide", application, runtime::LayerRole::overlay, "Environment",
                     wide_environment, text_engine);

    const auto property = [](const ui::Surface& surface,
                             const std::string_view key) -> const runtime::Value* {
        const ui::RetainedNode* retained = surface.tree().find_key(key);
        if (retained == nullptr)
            return nullptr;
        const auto found = retained->description().properties.find("text");
        return found != retained->description().properties.end() ? found->second.value() : nullptr;
    };
    const ui::SurfaceFrame compact_frame = compact.frame(1'000'000);
    const ui::SurfaceFrame wide_frame = wide.frame(1'000'000);
    check(compact_frame.diagnostics.records.empty() && wide_frame.diagnostics.records.empty(),
          "surface contextual environment produced runtime diagnostics");
    check(property(compact, "environment.viewport") != nullptr &&
              *property(compact, "environment.viewport")->string() == "compact" &&
              *property(compact, "environment.orientation")->string() == "portrait" &&
              *property(compact, "environment.pointer")->string() == "COARSE" &&
              *property(compact, "environment.density")->string() == "compact" &&
              *property(compact, "environment.width")->number() == 460.0 &&
              compact.tree().find_key("environment.compact") != nullptr,
          std::string("compact surface env mismatch: viewport=") +
              (property(compact, "environment.viewport") != nullptr
                   ? runtime::display_string(*property(compact, "environment.viewport"))
                   : "<missing>") +
              ", orientation=" +
              (property(compact, "environment.orientation") != nullptr
                   ? runtime::display_string(*property(compact, "environment.orientation"))
                   : "<missing>") +
              ", pointer=" +
              (property(compact, "environment.pointer") != nullptr
                   ? runtime::display_string(*property(compact, "environment.pointer"))
                   : "<missing>") +
              ", density=" +
              (property(compact, "environment.density") != nullptr
                   ? runtime::display_string(*property(compact, "environment.density"))
                   : "<missing>") +
              ", width=" +
              (property(compact, "environment.width") != nullptr
                   ? runtime::display_string(*property(compact, "environment.width"))
                   : "<missing>") +
              ", branch=" +
              (compact.tree().find_key("environment.compact") != nullptr ? "compact" : "other"));
    check(property(wide, "environment.viewport") != nullptr &&
              *property(wide, "environment.viewport")->string() == "wide" &&
              *property(wide, "environment.orientation")->string() == "landscape" &&
              *property(wide, "environment.pointer")->string() == "FINE" &&
              wide.tree().find_key("environment.wide") != nullptr,
          "wide surface did not resolve its isolated env binding");

    ui::SurfaceEnvironment local_preferences = compact.environment();
    local_preferences.density = ui::SurfaceDensity::comfortable;
    check(compact.adopt_environment_preferences(local_preferences),
          "local environment preference did not advance");
    ui::SurfaceEnvironment regular_environment = compact_environment;
    regular_environment.generation = 2U;
    regular_environment.framebuffer_width = 1'280;
    regular_environment.logical_width = 640.0;
    check(compact.adopt_environment(regular_environment),
          "new external environment was rejected after a local preference change");
    static_cast<void>(compact.frame(17'666'667));
    wide.invalidate();
    static_cast<void>(wide.frame(17'666'667));
    check(*property(compact, "environment.viewport")->string() == "regular" &&
              compact.tree().find_key("environment.regular") != nullptr &&
              *property(wide, "environment.viewport")->string() == "wide" &&
              wide.tree().find_key("environment.wide") != nullptr,
          "contextual host roots leaked between native surfaces");
}

void test_async_collection_key_isolation_and_lazy_tree_publication(
    const std::filesystem::path& resource_root) {
    using namespace strata;
    const data::JsonValue schemas = data::parse_json(resource::load_utf8_resource(
        resource_root, resource::ResourceId::parse("assets/strata/ui/demo_surface.schemas.json")));
    const auto bundle = runtime::ApplicationBundle::create(&schemas);
    runtime::ApplicationContext application("async-collection-integration", bundle);
    check(
        application.host().adopt(bundle->host_snapshot(
            "async-collection-host", 1U,
            data::parse_json(
                R"({"data":{"treeItems":[{"key":"data.tree.folder.0","label":"Folder 0","parentKey":null,"mayHaveChildren":true,"childrenLoaded":false}],"tableRows":[],"gridEntries":[]}})"))),
        "initial lazy-tree host snapshot was not adopted");

    std::vector<runtime::AsyncRequest> requests;
    application.configure_async_host(runtime::AsyncHostAdapter{
        [&requests](const runtime::AsyncRequest& request) { requests.push_back(request); },
        {},
    });
    const std::string source = R"(
component AsyncTreeRow(key: key, label: string, selected: boolean, expanded: boolean, loading: boolean) {
  Panel(key: key, layout: { kind: "ROW", width: { weight: 1 }, height: { weight: 1 } }) {
    Text(text: label)
  }
}
component AsyncListRow(item: record, key: key, index: number) {
  Text(key: key, text: item.label)
}
overlay AsyncCollections {
  state queryText: string = "sample";
  Panel(key: "async.root", layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 }, gap: 8, padding: 8 }) {
    TextBox(key: "query.input", bind: queryText, layout: { width: 180, height: 28 })
    Button(
      key: "query.start", label: "Query",
      onClick: action("async.query", binding: "asyncResults", payload: queryText),
      layout: { width: 100, height: 28 }
    )
    VirtualList(
      key: "query.virtual",
      items: asyncResults,
      itemTemplate: AsyncListRow,
      itemExtent: 28,
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 100 }
    )
    TreeView(
      key: "lazy.tree",
      items: data.treeItems,
      rowTemplate: AsyncTreeRow,
      rowHeight: 28,
      onLoadChildren: action("async.query", binding: "asyncTreeChildren", payload: event.value),
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 120 }
    )
    when asyncResults.status {
      "IDLE" -> { Text(key: "query.status.idle", text: "Idle") }
      "LOADING" -> { Text(key: "query.status.loading", text: "Loading") }
      "READY" -> {
        Repeater(key: "query.authored") {
          for item in asyncResults.value {
            Text(key: format("query.authored.{0}", item.key), text: item.label)
          }
        }
      }
      "FAILED" -> { Text(key: "query.status.failed", text: "Failed") }
    }
    TreeView(
      key: "query.tree",
      items: asyncResults,
      rowTemplate: AsyncTreeRow,
      rowHeight: 28,
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 100 }
    )
  }
}
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"async-collections.strata", source}, no_imports, 0U);
    if (!activation.activated()) {
        std::string message = "async collection integration fixture did not activate";
        for (const runtime::ActivationDiagnostic& diagnostic : activation.diagnostics) {
            message += " [" + diagnostic.code + ": " + diagnostic.message + "]";
        }
        throw std::runtime_error(message);
    }

    {
        ui::DescriptionBuilder builder(application);
        const ui::DescriptionBuildResult idle =
            builder.build(runtime::LayerRole::overlay, "AsyncCollections");
        const std::shared_ptr<const ui::DescriptionNode> root = idle.root->children->at(0U);
        const std::shared_ptr<const ui::DescriptionNode> virtual_list = root->children->at(2U);
        const std::shared_ptr<const ui::DescriptionNode> query_tree = root->children->at(5U);
        check(virtual_list->children->size() == 1U && query_tree->children->size() == 1U &&
                  virtual_list->children->at(0U)->key ==
                      std::optional<std::string>("query.virtual.$async-loading") &&
                  query_tree->children->at(0U)->key ==
                      std::optional<std::string>("query.tree.$async-loading"),
              "async collection placeholders were not isolated by collection key");
    }

    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 640;
    environment.framebuffer_height = 520;
    environment.logical_width = 640.0;
    environment.logical_height = 520.0;
    environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    ui::Surface surface(
        "async-collection-integration", application, runtime::LayerRole::overlay,
        "AsyncCollections", environment,
        ui::TextEngine::load_control_font(
            resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")));
    static_cast<void>(surface.frame(1'000'000'000));

    const ui::RetainedNode* folder = surface.tree().find_key("data.tree.folder.0");
    const ui::LayoutRecord* folder_layout =
        folder != nullptr ? surface.layout().find(folder->identity()) : nullptr;
    check(folder_layout != nullptr, "unloaded lazy-tree folder was not arranged");
    const ui::Point disclosure{
        folder_layout->bounds.x + 3.0,
        folder_layout->bounds.y + folder_layout->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        disclosure,
        ui::PointerEventType::press,
        31,
        0,
    }));
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        disclosure,
        ui::PointerEventType::release,
        31,
        0,
    }));
    const ui::SurfaceFrame expanded = surface.frame(1'016'666'667);
    check(std::ranges::any_of(expanded.lifecycle_input.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* type = event.find("type");
                                  return type != nullptr && type->string() != nullptr &&
                                         *type->string() == "tree-children-requested";
                              }) &&
              requests.size() == 1U && requests.front().binding == "asyncTreeChildren" &&
              application.async().active_count() == 1U,
          "expanding an unloaded folder did not retain its typed async child request");
    static_cast<void>(surface.frame(1'033'333'334));
    check(application.async().active_count() == 1U,
          "expanded lazy-tree ownership cancelled active host work on the next frame");

    check(
        application.host().adopt(bundle->host_snapshot(
            "async-collection-host", 2U,
            data::parse_json(
                R"({"data":{"treeItems":[{"key":"data.tree.folder.0","label":"Folder 0","parentKey":null,"mayHaveChildren":true,"childrenLoaded":true},{"key":"data.tree.asset.0.0","label":"Asset 0","parentKey":"data.tree.folder.0","mayHaveChildren":false,"childrenLoaded":true}],"tableRows":[],"gridEntries":[]}})"))),
        "loaded lazy-tree host snapshot was not adopted");
    const runtime::Value lazy_result = runtime::value_from_json(data::parse_json(
        R"([{"key":"data.tree.asset.0.0","label":"Asset 0","parentKey":"data.tree.folder.0","mayHaveChildren":false,"childrenLoaded":true}])"));
    check(application.async().post_succeed(requests.front().id, lazy_result),
          "lazy-tree async completion was not accepted");
    static_cast<void>(surface.frame(1'050'000'001));
    const runtime::Value lazy_state = application.async().state("asyncTreeChildren");
    const runtime::Value* lazy_value = lazy_state.field("value");
    check(surface.tree().find_key("data.tree.asset.0.0") != nullptr &&
              lazy_state.field("status") != nullptr &&
              *lazy_state.field("status")->string() == "READY" && lazy_value != nullptr &&
              lazy_value->list() != nullptr &&
              lazy_value->list()->values.front().field("key")->key() != nullptr &&
              lazy_value->list()->values.front().field("parentKey")->key() != nullptr,
          "typed async completion or immutable host republish did not materialize lazy-tree "
          "children");

    const runtime::ActionDispatchOutcome query = surface.dispatch_action(
        "async.query",
        runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
            {"binding", runtime::Value("asyncResults")},
            {"payload", runtime::Value("sample")},
            {"debounceMillis", runtime::Value(0.0)},
        }),
        "query", std::optional<std::string>("async.root"), runtime::Value{});
    check(query.status == runtime::ActionDispatchStatus::handled,
          "async result query action was not handled");
    static_cast<void>(surface.frame(1'066'666'668));
    const auto result_request = std::ranges::find_if(
        requests, [](const runtime::AsyncRequest& item) { return item.binding == "asyncResults"; });
    check(result_request != requests.end(), "async result query did not reach the host adapter");
    check(application.async().post_succeed(result_request->id,
                                           runtime::value_from_json(data::parse_json(
                                               R"([{"key":"async.0","label":"Result 0"}])"))),
          "async result completion was not accepted");
    static_cast<void>(surface.frame(1'083'333'335));
    const runtime::Value result_state = application.async().state("asyncResults");
    check(result_state.field("status") != nullptr &&
              *result_state.field("status")->string() == "READY" &&
              result_state.field("value") != nullptr &&
              result_state.field("value")->list() != nullptr &&
              result_state.field("value")->list()->values.size() == 1U,
          "async result service did not publish its completed row");
    static_cast<void>(surface.frame(1'100'000'002));
    check(surface.tree().find_key("query.authored.async.0") != nullptr &&
              surface.tree().find_key("async.0") != nullptr &&
              surface.tree().find_key("query.virtual.$item.async.0") != nullptr,
          "READY async collections did not retain isolated authored, tree, and virtual keys");

    std::int64_t query_frame_time = 1'116'666'669;
    const auto start_query = [&surface, &requests, &query_frame_time](std::string payload) {
        const runtime::ActionDispatchOutcome outcome = surface.dispatch_action(
            "async.query",
            runtime::Value(std::vector<std::pair<std::string, runtime::Value>>{
                {"binding", runtime::Value("asyncResults")},
                {"payload", runtime::Value(std::move(payload))},
                {"debounceMillis", runtime::Value(0.0)},
            }),
            "query", std::optional<std::string>("async.root"), runtime::Value{});
        check(outcome.status == runtime::ActionDispatchStatus::handled,
              "repeated async query action was not handled");
        static_cast<void>(surface.frame(query_frame_time));
        query_frame_time += 16'666'667;
        return requests.back().id;
    };
    const std::uint64_t empty_request = start_query("empty");
    check(application.async().post_succeed(empty_request,
                                           runtime::Value(std::vector<runtime::Value>{})),
          "empty async completion was not accepted");
    static_cast<void>(surface.frame(query_frame_time));
    query_frame_time += 16'666'667;
    const runtime::Value empty_state = application.async().state("asyncResults");
    check(empty_state.field("status") != nullptr &&
              *empty_state.field("status")->string() == "READY" &&
              empty_state.field("value") != nullptr &&
              empty_state.field("value")->list() != nullptr &&
              empty_state.field("value")->list()->values.empty(),
          "empty async result retained a previous READY value");

    const std::uint64_t failed_request = start_query("fail");
    check(application.async().post_fail(failed_request,
                                        runtime::AsyncFailure{
                                            "The simulated source rejected the query.",
                                            "SIMULATED_FAILURE",
                                        }),
          "failed async completion was not accepted");
    static_cast<void>(surface.frame(query_frame_time));
    query_frame_time += 16'666'667;
    const runtime::Value failed_state = application.async().state("asyncResults");
    check(failed_state.field("status") != nullptr &&
              *failed_state.field("status")->string() == "FAILED" &&
              failed_state.field("error") != nullptr &&
              failed_state.field("error")->field("code") != nullptr &&
              *failed_state.field("error")->field("code")->string() == "SIMULATED_FAILURE",
          "failed async result retained a previous READY value");

    const std::uint64_t repeated_request = start_query("sample-again");
    check(application.async().post_succeed(repeated_request,
                                           runtime::value_from_json(data::parse_json(
                                               R"([{"key":"async.0","label":"Result again"}])"))),
          "repeated READY async completion was not accepted");
    static_cast<void>(surface.frame(query_frame_time));
    query_frame_time += 16'666'667;
    static_cast<void>(surface.frame(query_frame_time));
    check(surface.tree().find_key("query.authored.async.0") != nullptr &&
              surface.tree().find_key("async.0") != nullptr &&
              surface.tree().find_key("query.virtual.$item.async.0") != nullptr,
          "repeated READY transition lost isolated async collection rows");

    static_cast<void>(surface.input().click("query.input"));
    static_cast<void>(surface.frame(query_frame_time += 16'666'667));
    static_cast<void>(surface.input().key("A", ui::KeyModifiers{false, true, false, false}));
    static_cast<void>(surface.input().text("empty"));
    static_cast<void>(surface.frame(query_frame_time += 16'666'667));
    static_cast<void>(surface.input().click("query.start"));
    check(!requests.empty() && requests.back().payload.string() != nullptr &&
              *requests.back().payload.string() == "empty",
          "bound TextBox query action retained the previous authored value");
}

void test_native_collection_interaction_and_templates(const std::filesystem::path& resource_root) {
    using namespace strata;
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("collection-core", bundle);
    const std::string source = R"(
component CollectionTreeRow(key: key, label: string, selected: boolean, expanded: boolean, loading: boolean) {
  Panel(key: key, layout: { kind: "ROW", width: { weight: 1 }, height: { weight: 1 } }) {
    Text(text: label)
  }
}
component CollectionGridItem(key: key, label: string, selected: boolean) {
  Panel(key: key, variant: selected ? "primary" : "secondary", layout: { width: { weight: 1 }, height: { weight: 1 } }) {
    Text(text: label)
  }
}
component CollectionScoreCell(key: key, value: number, selected: boolean) {
  Panel(key: key, layout: { width: { weight: 1 }, height: { weight: 1 } }) {
    Text(text: format("Score {0}", value))
  }
}
overlay Collections {
  Panel(key: "collections.root", layout: { kind: "COLUMN", width: { weight: 1 }, height: { weight: 1 }, gap: 8, padding: 8 }) {
    TreeView(
      key: "collections.tree",
      items: [
        { key: "tree.root", label: "Root", parentKey: null, mayHaveChildren: true, childrenLoaded: true },
        { key: "tree.folder", label: "Folder", parentKey: null, mayHaveChildren: true, childrenLoaded: true },
        { key: "tree.folder.child", label: "Folder child", parentKey: "tree.folder", mayHaveChildren: false, childrenLoaded: true },
        { key: "tree.child", label: "Child", parentKey: "tree.root", mayHaveChildren: false, childrenLoaded: true }
      ],
      selectionMode: "MULTIPLE", rowTemplate: CollectionTreeRow,
      onDrop: action("focus.request", key: "tree.root"),
      hoverExpandDelay: 50ms,
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 100 }
    )
    Table(
      key: "collections.table",
      columns: [
        { id: "name", header: "Name", size: "FIXED", width: 140, pinned: true },
        { id: "score", header: "Score", size: "FIXED", width: 120 },
        { id: "status", header: "Status", size: "FIXED", width: 120 }
      ],
      rows: sortBy([
        { key: "row.alpha", cells: { name: "Alpha", score: 10, status: "Ready" } },
        { key: "row.beta", cells: { name: "Beta", score: 20, status: "Busy" } }
      ], row -> row.cells.name, false),
      selectionMode: "MULTIPLE", focusMode: "CELL",
      cellTemplates: [{ columnId: "score", template: CollectionScoreCell }],
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 120 }
    )
    ItemGrid(
      key: "collections.grid",
      entries: [
        { kind: "GROUP", key: "grid.group", label: "Group" },
        { kind: "ITEM", key: "grid.alpha", label: "Alpha", value: "alpha" },
        { kind: "ITEM", key: "grid.beta", label: "Beta", value: "beta" },
        { kind: "ITEM", key: "grid.gamma", label: "Gamma", value: "gamma" },
        { kind: "ITEM", key: "grid.delta", label: "Delta", value: "delta" },
        { kind: "ITEM", key: "grid.epsilon", label: "Epsilon", value: "epsilon" },
        { kind: "ITEM", key: "grid.zeta", label: "Zeta", value: "zeta" },
        { kind: "ITEM", key: "grid.eta", label: "Eta", value: "eta" },
        { kind: "ITEM", key: "grid.theta", label: "Theta", value: "theta" }
      ],
      selectionMode: "MULTIPLE", columns: 2, cellWidth: 100, cellHeight: 58, gap: 8,
      itemTemplate: CollectionGridItem,
      layout: { kind: "SCROLL", width: { weight: 1 }, height: 120 }
    )
  }
}
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"collections.strata", source}, no_imports, 0U);
    if (!activation.activated()) {
        std::string message = "native collection fixture did not activate";
        for (const runtime::ActivationDiagnostic& diagnostic : activation.diagnostics) {
            message += " [" + diagnostic.code + ": " + diagnostic.message + "]";
        }
        throw std::runtime_error(message);
    }

    std::shared_ptr<const ui::DescriptionNode> detached_tree_description;
    std::shared_ptr<const ui::DescriptionNode> detached_table_description;
    std::shared_ptr<const ui::DescriptionNode> detached_grid_description;
    {
        ui::DescriptionBuilder temporary_builder(application);
        ui::DescriptionBuildResult detached =
            temporary_builder.build(runtime::LayerRole::overlay, "Collections");
        const std::shared_ptr<const ui::DescriptionNode> collection_root =
            detached.root->children->at(0U);
        detached_tree_description = collection_root->children->at(0U);
        detached_table_description = collection_root->children->at(1U);
        detached_grid_description = collection_root->children->at(2U);
    }
    const auto derived_rows = detached_table_description->properties.find("rows");
    check(derived_rows != detached_table_description->properties.end() &&
              derived_rows->second.collection() != nullptr &&
              *derived_rows->second.collection() != nullptr &&
              (*derived_rows->second.collection())->items.list() != nullptr &&
              (*derived_rows->second.collection())->items.list()->values.size() == 2U,
          "sortBy did not preserve its materialized collection view on the Table description");
    const auto tree_items = detached_tree_description->properties.find("items");
    const runtime::Value* tree_layout = detached_tree_description->properties.at("$layout").value();
    const runtime::Value* tree_virtual_count =
        tree_layout != nullptr ? tree_layout->field("virtualItemCount") : nullptr;
    check(tree_items != detached_tree_description->properties.end() &&
              tree_items->second.data_value() != nullptr &&
              tree_items->second.data_value()->list() != nullptr &&
              tree_items->second.data_value()->list()->values.size() == 4U &&
              tree_virtual_count != nullptr && tree_virtual_count->number() != nullptr &&
              *tree_virtual_count->number() == 2.0,
          "TreeView did not derive its visible virtual rows from collection data");
    check(detached_tree_description->children->size() >= 1U,
          "TreeView did not populate generated collection children");
    check(detached_table_description->children->size() >= 2U &&
              detached_table_description->virtual_sequence != nullptr &&
              detached_table_description->virtual_sequence->count() == 2U &&
              detached_table_description->properties.at("$layout").value()->field(
                  "virtualItemKeys") == nullptr,
          "Table did not retain its direct keyed provider or populate generated children");
    check(detached_grid_description->children->size() == 5U &&
              detached_grid_description->children->at(0U)->materialization_result != nullptr &&
              detached_grid_description->virtual_item_members != nullptr &&
              detached_grid_description->virtual_item_members->size() == 5U &&
              detached_grid_description->virtual_item_extents != nullptr &&
              detached_grid_description->virtual_item_extents->size() == 5U &&
              detached_grid_description->properties.at("$layout").value()->field(
                  "virtualItemMembers") == nullptr &&
              detached_grid_description->properties.at("$layout").value()->field(
                  "virtualItemExtents") == nullptr,
          "ItemGrid did not retain direct band metadata or generated rows");
    const std::shared_ptr<const ui::DescriptionNode> first_tree_row =
        detached_tree_description->children->at(0U);
    const std::shared_ptr<const ui::DescriptionNode> repeated_tree_row =
        detached_tree_description->children->at(0U);
    const std::shared_ptr<const ui::DescriptionNode> first_table_row =
        detached_table_description->children->at(0U);
    const std::shared_ptr<const ui::DescriptionNode> second_table_row =
        detached_table_description->children->at(1U);
    check(first_tree_row == repeated_tree_row &&
              first_tree_row->properties.at("$treeDepth").value()->number() != nullptr &&
              *first_tree_row->properties.at("$treeDepth").value()->number() == 0.0 &&
              first_tree_row->properties.at("$treeExpandable").value()->boolean() != nullptr &&
              *first_tree_row->properties.at("$treeExpandable").value()->boolean() &&
              first_tree_row->materialization_result != nullptr &&
              first_table_row->materialization_result != nullptr &&
              second_table_row->materialization_result != nullptr &&
              first_table_row->materialization_result != second_table_row->materialization_result,
          "native generated rows borrowed their builder or shared per-index transactions");

    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 640;
    environment.framebuffer_height = 420;
    environment.logical_width = 640.0;
    environment.logical_height = 420.0;
    environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    ui::Surface surface(
        "collection-core", application, runtime::LayerRole::overlay, "Collections", environment,
        ui::TextEngine::load_control_font(
            resource_root, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")));
    const ui::SurfaceFrame initial_frame = surface.frame(1'000'000'000);
    const ui::RetainedNode* initial_tree = surface.tree().find_key("collections.tree");
    const ui::RetainedNode* initial_root = surface.tree().find_key("tree.root");
    const ui::RetainedNode* initial_folder = surface.tree().find_key("tree.folder");
    check(initial_tree != nullptr && initial_root != nullptr && initial_folder != nullptr &&
              initial_root->description().type == "Panel" &&
              surface.tree().find_key("tree.child") == nullptr,
          "tree component template or collapsed materialization was not native");
    const auto initial_drop_behavior =
        std::ranges::find(initial_root->description().behaviors,
                          std::string_view("strata.drop-target"), &ui::DescriptionBehavior::id);
    check(initial_drop_behavior != initial_root->description().behaviors.end() &&
              initial_drop_behavior->action != nullptr,
          "TreeView onDrop was not projected onto its generated row targets");
    const data::JsonValue* initial_tree_semantics =
        surface.semantics().find(initial_tree->identity());
    check(surface.layout().find(initial_root->identity()) != nullptr &&
              surface.layout().find(initial_folder->identity()) != nullptr &&
              semantic_child_named(initial_tree_semantics, "Root") != nullptr &&
              semantic_child_named(initial_tree_semantics, "Folder") != nullptr &&
              rendered_text(surface.render_commands(), "Root") &&
              rendered_text(surface.render_commands(), "Folder") &&
              initial_frame.operations.render.nodes_visited != 0U,
          "initial lazy Tree rows did not reach layout, semantics, and rendering in one frame");
    check(surface.tree().find_key("row.alpha.cell.score") != nullptr &&
              surface.tree().find_key("row.alpha.cell.score")->description().type == "Panel" &&
              surface.tree().find_key("grid.alpha") != nullptr &&
              surface.tree().find_key("grid.alpha")->description().type == "Panel",
          "table/grid component templates were not instantiated");

    const std::uint64_t initial_root_identity = initial_root->identity();
    const std::uint64_t initial_table_identity = surface.tree().find_key("row.alpha")->identity();
    const std::uint64_t initial_grid_identity = surface.tree().find_key("grid.alpha")->identity();
    const ui::SurfaceFrame warm_frame = surface.frame(1'016'666'667);
    check(surface.tree().find_key("tree.root")->identity() == initial_root_identity &&
              surface.tree().find_key("row.alpha")->identity() == initial_table_identity &&
              surface.tree().find_key("grid.alpha")->identity() == initial_grid_identity &&
              warm_frame.operations.rebuilds == 0U &&
              warm_frame.operations.layout_measured_nodes == 0U &&
              warm_frame.operations.layout_arranged_nodes == 0U &&
              warm_frame.operations.render.nodes_visited == 0U,
          "shared lazy collection path lost row identity or scheduled a perpetual warm frame");

    std::int64_t frame_time = 1'016'666'667;
    const auto pointers = [&surface,
                           &frame_time](const std::initializer_list<ui::PointerInputEvent> events) {
        for (const ui::PointerInputEvent& event : events) {
            static_cast<void>(surface.input().enqueue_pointer(event));
        }
        frame_time += 16'666'667;
        return surface.frame(frame_time).lifecycle_input;
    };
    const ui::RetainedNode* root = surface.tree().find_key("tree.root");
    const ui::LayoutRecord* root_layout =
        root != nullptr ? surface.layout().find(root->identity()) : nullptr;
    check(root_layout != nullptr, "tree root template was not arranged");
    const ui::Point disclosure{root_layout->bounds.x + 3.0,
                               root_layout->bounds.y + root_layout->bounds.height * 0.5};
    const ui::InputOperationResult expanded = pointers({
        ui::PointerInputEvent{disclosure, ui::PointerEventType::press, 7, 0},
        ui::PointerInputEvent{disclosure, ui::PointerEventType::release, 7, 0},
    });
    check(std::ranges::any_of(expanded.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* type = event.find("type");
                                  return type != nullptr && type->string() != nullptr &&
                                         *type->string() == "tree-expansion-changed";
                              }),
          "tree disclosure did not emit its typed expansion event");
    check(surface.tree().find_key("tree.child") != nullptr,
          "retained tree expansion did not materialize the child template");
    const ui::RetainedNode* leaf = surface.tree().find_key("tree.child");
    const auto leaf_drop_behavior =
        std::ranges::find(leaf->description().behaviors, std::string_view("strata.drop-target"),
                          &ui::DescriptionBehavior::id);
    const runtime::Value* leaf_axis = leaf_drop_behavior != leaf->description().behaviors.end()
                                          ? leaf_drop_behavior->options.field("insertionAxis")
                                          : nullptr;
    check(leaf_axis != nullptr && leaf_axis->string() != nullptr &&
              *leaf_axis->string() == "VERTICAL",
          "tree leaf target advertised an invalid on-placement drop");
    const ui::LayoutRecord* expanded_root_layout =
        surface.layout().find(surface.tree().find_key("tree.root")->identity());
    const ui::LayoutRecord* expanded_child_layout =
        surface.layout().find(surface.tree().find_key("tree.child")->identity());
    const ui::LayoutRecord* expanded_folder_layout =
        surface.layout().find(surface.tree().find_key("tree.folder")->identity());
    check(expanded_root_layout != nullptr && expanded_child_layout != nullptr &&
              expanded_folder_layout != nullptr &&
              expanded_child_layout->bounds.y < expanded_folder_layout->bounds.y,
          "TreeView did not project appended children immediately after their expanded parent");
    static_cast<void>(surface.input().key("C"));
    const runtime::Value* tree_active =
        surface.tree().find_key("collections.tree")->retained_value("strata.collection.active");
    check(tree_active != nullptr && tree_active->key() != nullptr &&
              tree_active->key()->value == "tree.child",
          "tree typeahead did not select its wrapped match; active=" +
              (tree_active != nullptr && tree_active->key() != nullptr ? tree_active->key()->value
                                                                       : std::string("<none>")));

    const ui::RetainedNode* tree_folder = surface.tree().find_key("tree.folder");
    root = surface.tree().find_key("tree.root");
    root_layout = root != nullptr ? surface.layout().find(root->identity()) : nullptr;
    const ui::LayoutRecord* folder_layout =
        tree_folder != nullptr ? surface.layout().find(tree_folder->identity()) : nullptr;
    check(root_layout != nullptr && folder_layout != nullptr,
          "tree drag hover fixture rows were not arranged");
    const ui::Point root_center{
        root_layout->bounds.x + root_layout->bounds.width * 0.5,
        root_layout->bounds.y + root_layout->bounds.height * 0.5,
    };
    const ui::Point folder_center{
        folder_layout->bounds.x + folder_layout->bounds.width * 0.5,
        folder_layout->bounds.y + folder_layout->bounds.height * 0.5,
    };
    static_cast<void>(pointers({
        ui::PointerInputEvent{root_center, ui::PointerEventType::press, 19, 0},
        ui::PointerInputEvent{folder_center, ui::PointerEventType::move, 19, 0},
    }));
    static_cast<void>(surface.frame(frame_time += 16'666'667));
    const ui::SurfaceFrame hover_expanded = surface.frame(frame_time += 50'000'001);
    check(surface.tree().find_key("tree.folder.child") != nullptr &&
              std::ranges::any_of(hover_expanded.lifecycle_input.events,
                                  [](const data::JsonValue& event) {
                                      const data::JsonValue* type = event.find("type");
                                      return type != nullptr && type->string() != nullptr &&
                                             *type->string() == "tree-expansion-changed";
                                  }),
          "stationary tree drag hover did not expand its collapsed target");
    static_cast<void>(pointers({
        ui::PointerInputEvent{folder_center, ui::PointerEventType::release, 19, 0},
    }));

    const ui::RetainedNode* row_alpha = surface.tree().find_key("row.alpha");
    const ui::LayoutRecord* row_alpha_layout =
        row_alpha != nullptr ? surface.layout().find(row_alpha->identity()) : nullptr;
    check(row_alpha_layout != nullptr, "table row was not arranged");
    const ui::Point row_point{row_alpha_layout->bounds.x + 30.0,
                              row_alpha_layout->bounds.y + row_alpha_layout->bounds.height * 0.5};
    static_cast<void>(pointers({
        ui::PointerInputEvent{row_point, ui::PointerEventType::press, 7, 0},
        ui::PointerInputEvent{row_point, ui::PointerEventType::release, 7, 0},
    }));
    const runtime::Value* table_selected =
        surface.tree().find_key("collections.table")->retained_value("strata.collection.selected");
    check(table_selected != nullptr && table_selected->list() != nullptr &&
              table_selected->list()->values.size() == 1U &&
              table_selected->list()->values.front().key() != nullptr &&
              table_selected->list()->values.front().key()->value == "row.alpha",
          "table pointer selection did not retain its stable row key");

    const ui::RetainedNode* table = surface.tree().find_key("collections.table");
    const ui::LayoutRecord* table_layout =
        table != nullptr ? surface.layout().find(table->identity()) : nullptr;
    check(table_layout != nullptr, "table was not arranged");
    const ui::Rect table_viewport = table_layout->viewport.value_or(table_layout->bounds);
    const ui::Point divider{table_viewport.x + 140.0, table_viewport.y + 12.0};
    const ui::InputOperationResult resized = pointers({
        ui::PointerInputEvent{divider, ui::PointerEventType::press, 7, 0},
        ui::PointerInputEvent{
            ui::Point{divider.x + 24.0, divider.y},
            ui::PointerEventType::move,
            7,
            0,
        },
        ui::PointerInputEvent{
            ui::Point{divider.x + 24.0, divider.y},
            ui::PointerEventType::release,
            7,
            0,
        },
    });
    check(std::ranges::any_of(resized.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* type = event.find("type");
                                  return type != nullptr && type->string() != nullptr &&
                                         *type->string() == "table-column-width-changed";
                              }) &&
              surface.tree()
                      .find_key("collections.table")
                      ->retained_value("strata.table.columnWidths") != nullptr,
          "table divider gesture did not retain and emit its native width");

    static_cast<void>(surface.frame(frame_time += 16'666'667));
    const ui::Point score_header{
        table_viewport.x + 164.0 + 60.0,
        table_viewport.y + 12.0,
    };
    const ui::Point status_tail{
        table_viewport.x + 164.0 + 120.0 + 116.0,
        table_viewport.y + 12.0,
    };
    const ui::InputOperationResult reordered = pointers({
        ui::PointerInputEvent{score_header, ui::PointerEventType::press, 31, 0},
        ui::PointerInputEvent{status_tail, ui::PointerEventType::move, 31, 0},
        ui::PointerInputEvent{status_tail, ui::PointerEventType::release, 31, 0},
    });
    const runtime::Value* table_order =
        surface.tree().find_key("collections.table")->retained_value("strata.table.columnOrder");
    check(std::ranges::any_of(reordered.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* type = event.find("type");
                                  return type != nullptr && type->string() != nullptr &&
                                         *type->string() == "table-column-reordered";
                              }) &&
              table_order != nullptr && table_order->list() != nullptr &&
              table_order->list()->values.size() == 3U &&
              table_order->list()->values[0].string() != nullptr &&
              *table_order->list()->values[0].string() == "name" &&
              table_order->list()->values[1].string() != nullptr &&
              *table_order->list()->values[1].string() == "status" &&
              table_order->list()->values[2].string() != nullptr &&
              *table_order->list()->values[2].string() == "score",
          "table header drag did not commit its constrained stable column order");

    static_cast<void>(surface.frame(frame_time += 16'666'667));
    table = surface.tree().find_key("collections.table");
    const data::JsonValue* table_semantics =
        table != nullptr ? surface.semantics().find(table->identity()) : nullptr;
    const data::JsonValue* semantic_children_value =
        table_semantics != nullptr ? table_semantics->find("children") : nullptr;
    const data::JsonValue::Array* semantic_children =
        semantic_children_value != nullptr ? semantic_children_value->array() : nullptr;
    check(semantic_children != nullptr && semantic_children->size() >= 3U &&
              (*semantic_children)[0].find("name") != nullptr &&
              (*semantic_children)[0].find("name")->string() != nullptr &&
              *(*semantic_children)[0].find("name")->string() == "Name" &&
              (*semantic_children)[1].find("name") != nullptr &&
              (*semantic_children)[1].find("name")->string() != nullptr &&
              *(*semantic_children)[1].find("name")->string() == "Status" &&
              (*semantic_children)[2].find("name") != nullptr &&
              (*semantic_children)[2].find("name")->string() != nullptr &&
              *(*semantic_children)[2].find("name")->string() == "Score",
          "table semantics diverged from the effective retained column order");

    const ui::Point resized_divider{table_viewport.x + 164.0, table_viewport.y + 12.0};
    static_cast<void>(pointers({
        ui::PointerInputEvent{resized_divider, ui::PointerEventType::press, 37, 0},
        ui::PointerInputEvent{
            ui::Point{resized_divider.x + 18.0, resized_divider.y},
            ui::PointerEventType::move,
            37,
            0,
        },
    }));
    check(surface.tree()
                      .find_key("collections.table")
                      ->retained_value("strata.table.headerSession") != nullptr &&
              surface.tree()
                      .find_key("collections.table")
                      ->retained_value("strata.table.headerSession")
                      ->object() != nullptr,
          "table resize did not retain a cancellable header gesture");
    static_cast<void>(surface.input().key("Escape"));
    static_cast<void>(surface.frame(frame_time += 16'666'667));
    const runtime::Value* cancelled_widths =
        surface.tree().find_key("collections.table")->retained_value("strata.table.columnWidths");
    const auto cancelled_name =
        cancelled_widths != nullptr && cancelled_widths->list() != nullptr
            ? std::ranges::find_if(cancelled_widths->list()->values,
                                   [](const runtime::Value& value) {
                                       const runtime::Value* id = value.field("id");
                                       return id != nullptr && id->string() != nullptr &&
                                              *id->string() == "name";
                                   })
            : std::vector<runtime::Value>::const_iterator{};
    check(cancelled_widths != nullptr && cancelled_widths->list() != nullptr &&
              cancelled_name != cancelled_widths->list()->values.end() &&
              cancelled_name->field("width") != nullptr &&
              cancelled_name->field("width")->number() != nullptr &&
              std::abs(*cancelled_name->field("width")->number() - 164.0) < 0.000'001 &&
              (surface.tree()
                       .find_key("collections.table")
                       ->retained_value("strata.table.headerSession") == nullptr ||
               surface.tree()
                       .find_key("collections.table")
                       ->retained_value("strata.table.headerSession")
                       ->object() == nullptr),
          "Escape did not atomically restore and clear the native table header gesture");

    static_cast<void>(surface.frame(frame_time += 16'666'667));
    const ui::RetainedNode* grid_alpha = surface.tree().find_key("grid.alpha");
    const ui::RetainedNode* grid_beta = surface.tree().find_key("grid.beta");
    const ui::LayoutRecord* alpha_layout =
        grid_alpha != nullptr ? surface.layout().find(grid_alpha->identity()) : nullptr;
    const ui::LayoutRecord* beta_layout =
        grid_beta != nullptr ? surface.layout().find(grid_beta->identity()) : nullptr;
    check(alpha_layout != nullptr && beta_layout != nullptr, "grid items were not arranged");
    const ui::Point gap{
        (alpha_layout->bounds.right() + beta_layout->bounds.x) * 0.5,
        alpha_layout->bounds.y + alpha_layout->bounds.height * 0.5,
    };
    const ui::Point beta_center{
        beta_layout->bounds.x + beta_layout->bounds.width * 0.5,
        beta_layout->bounds.y + beta_layout->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        gap,
        ui::PointerEventType::press,
        7,
        0,
    }));
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        beta_center,
        ui::PointerEventType::move,
        7,
        0,
    }));
    const ui::SurfaceFrame marquee_preview = surface.frame(frame_time += 16'666'667);
    const auto marquee_render_bounds = [&surface]() -> std::optional<ui::Rect> {
        for (const ui::RenderCommand& command : surface.render_commands().commands()) {
            const auto* rectangle = std::get_if<ui::RoundedRectRenderCommand>(&command);
            if (rectangle == nullptr ||
                rectangle->fill != ui::Paint(ui::RenderColor{70U, 137U, 230U, 50U}) ||
                !rectangle->border.has_value() ||
                rectangle->border->color != ui::RenderColor{104U, 169U, 255U, 220U}) {
                continue;
            }
            return rectangle->bounds;
        }
        return std::nullopt;
    };
    const std::optional<ui::Rect> first_marquee_bounds = marquee_render_bounds();
    const runtime::Value* marquee =
        surface.tree().find_key("collections.grid")->retained_value("strata.collection.marquee");
    const runtime::Value* preview_selection =
        marquee != nullptr ? marquee->field("lastSelection") : nullptr;
    const runtime::Value* grid_selected =
        surface.tree().find_key("collections.grid")->retained_value("strata.collection.selected");
    check(marquee_preview.operations.rebuilds == 0U &&
              (grid_selected == nullptr || grid_selected->list() == nullptr ||
               std::ranges::none_of(grid_selected->list()->values,
                                    [](const runtime::Value& value) {
                                        return value.key() != nullptr &&
                                               value.key()->value == "grid.beta";
                                    })) &&
              preview_selection != nullptr && preview_selection->list() != nullptr &&
              std::ranges::any_of(preview_selection->list()->values,
                                  [](const runtime::Value& value) {
                                      return value.key() != nullptr &&
                                             value.key()->value == "grid.beta";
                                  }),
          "grid marquee preview rebuilt or prematurely committed its provisional selection");
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        ui::Point{beta_center.x + 1.0, beta_center.y},
        ui::PointerEventType::move,
        7,
        0,
    }));
    const ui::SurfaceFrame marquee_continued = surface.frame(frame_time += 16'666'667);
    const std::optional<ui::Rect> continued_marquee_bounds = marquee_render_bounds();
    check(marquee_continued.operations.rebuilds == 0U &&
              marquee_continued.operations.layout_measured_nodes == 0U &&
              marquee_continued.operations.layout_arranged_nodes == 0U &&
              marquee_continued.operations.render.fragments_built == 0U &&
              marquee_continued.operations.render.nodes_visited != 0U &&
              marquee_continued.operations.render.commands_emitted != 0U &&
              first_marquee_bounds.has_value() && continued_marquee_bounds.has_value() &&
              first_marquee_bounds->right() != continued_marquee_bounds->right(),
          "presentation-only marquee movement reused a stale frame or rebuilt widget content");
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        beta_center,
        ui::PointerEventType::release,
        7,
        0,
    }));
    const ui::SurfaceFrame marquee_commit = surface.frame(frame_time += 16'666'667);
    grid_selected =
        surface.tree().find_key("collections.grid")->retained_value("strata.collection.selected");
    check(marquee_commit.operations.rebuilds == 1U && grid_selected != nullptr &&
              grid_selected->list() != nullptr &&
              std::ranges::any_of(grid_selected->list()->values,
                                  [](const runtime::Value& value) {
                                      return value.key() != nullptr &&
                                             value.key()->value == "grid.beta";
                                  }),
          "grid marquee release did not commit its provisional selection exactly once");
    const ui::RetainedNode* grid = surface.tree().find_key("collections.grid");
    const ui::LayoutRecord* grid_layout =
        grid != nullptr ? surface.layout().find(grid->identity()) : nullptr;
    check(grid_layout != nullptr && grid_layout->viewport.has_value(),
          "grid marquee auto-scroll fixture did not expose a scroll viewport");
    const ui::DescriptionNode* const retained_surface_description =
        &surface.tree().root()->description();
    const ui::DescriptionNode* const retained_grid_description = &grid->description();
    const double pre_wheel_scroll = grid_layout->scroll_offset.y;
    static_cast<void>(surface.input().enqueue_scroll(ui::ScrollInputEvent{
        gap,
        0.0,
        -1.0,
    }));
    static_cast<void>(surface.frame(frame_time += 16'666'667));
    grid = surface.tree().find_key("collections.grid");
    grid_layout = grid != nullptr ? surface.layout().find(grid->identity()) : nullptr;
    check(grid_layout != nullptr && grid_layout->scroll_offset.y > pre_wheel_scroll,
          "wheel scroll was swallowed by the input-only surface fast path");
    const double initial_grid_scroll = grid_layout->scroll_offset.y;
    const ui::Point lower_edge{
        gap.x,
        grid_layout->viewport->bottom() - 1.0,
    };
    static_cast<void>(pointers({
        ui::PointerInputEvent{gap, ui::PointerEventType::press, 23, 0},
        ui::PointerInputEvent{lower_edge, ui::PointerEventType::move, 23, 0},
    }));
    const std::uint64_t pre_auto_scroll_layout_generation = surface.layout().generation;
    static_cast<void>(surface.frame(frame_time += 50'000'000));
    grid = surface.tree().find_key("collections.grid");
    grid_layout = grid != nullptr ? surface.layout().find(grid->identity()) : nullptr;
    check(grid_layout != nullptr && grid_layout->scroll_offset.y > initial_grid_scroll &&
              surface.layout().generation == pre_auto_scroll_layout_generation + 1U &&
              &surface.tree().root()->description() == retained_surface_description &&
              &grid->description() == retained_grid_description &&
              grid->realized_range() ==
                  std::optional<ui::MaterializationRange>(ui::MaterializationRange{
                      grid_layout->visible_range->start,
                      grid_layout->visible_range->end_exclusive,
                  }),
          "stationary marquee did not advance through collection-local retained realization");
    surface.cancel_interactions();
    static_cast<void>(surface.frame(frame_time += 16'666'667));
    grid_selected =
        surface.tree().find_key("collections.grid")->retained_value("strata.collection.selected");
    marquee =
        surface.tree().find_key("collections.grid")->retained_value("strata.collection.marquee");
    check(grid_selected != nullptr && grid_selected->list() != nullptr &&
              grid_selected->list()->values.size() == 1U &&
              grid_selected->list()->values.front().key() != nullptr &&
              grid_selected->list()->values.front().key()->value == "grid.beta" &&
              (marquee == nullptr || marquee->object() == nullptr),
          "surface cancellation did not preserve committed selection and clear marquee preview");
}

void test_motion_timing_and_indeterminate_progress() {
    using namespace strata;
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("motion-contract", bundle);
    const std::string source = R"(
animation Counted {
  from { opacity: 0 }
  to { opacity: 1 }
  duration: 100ms;
  delay: 20ms;
  easing: "linear";
  repeat: { count: 3 };
  reverse: true;
  fillMode: "BACKWARDS";
}
animation LazyEnter {
  from { width: 0 }
  to { width: 120 }
  duration: 100ms;
  trigger: "ENTER";
}
animation HoverChannel {
  from { opacity: 0.7 }
  to { opacity: 1 }
  duration: 100ms;
}
animation FocusVisibleChannel {
  from { radius: 4 }
  to { radius: 8 }
  duration: 100ms;
}
animation FocusVisibleTrigger {
  from { opacity: 0 }
  to { opacity: 1 }
  duration: 100ms;
  trigger: "FOCUS_VISIBLE";
}
overlay Motion {
  root Progress(key: "motion.progress", indeterminate: true)
}
overlay InteractionMotion {
  root Button(
    key: "motion.hover", label: "Hover",
    motions: [
      { id: "motion.hover.channel", interaction: "HOVER", animation: HoverChannel },
      { id: "motion.focus-visible.channel", interaction: "FOCUS_VISIBLE", animation: FocusVisibleChannel }
    ]
  )
}
overlay ReorderMotion {
  root Panel(
    key: "reorder.container",
    layout: { kind: "ROW", width: 200, height: 40, gap: 8 },
    behaviors: [{ id: "strata.reorder-target", options: { payloadType: "reorder.item", axis: "HORIZONTAL" } }]
  ) {
    Panel(
      key: "reorder.alpha", layout: { width: 60, height: 40 },
      behaviors: [{ id: "strata.drag-source", options: { payloadType: "reorder.item", payload: "alpha", slop: 2, allowedOperations: ["MOVE"] } }]
    )
    Panel(key: "reorder.beta", layout: { width: 60, height: 40 })
  }
}
overlay ReorderMotionVerbose {
  root Panel(
    key: "reorder.verbose.container",
    layout: { kind: "ROW", width: 200, height: 40, gap: 8 },
    behaviors: [{ id: "strata.reorder-target", options: { payloadType: "reorder.item", axis: "HORIZONTAL" } }]
  ) {
    Panel(
      key: "reorder.verbose.alpha", layout: { width: 60, height: 40 },
      behaviors: [{ id: "strata.drag-source", options: { payloadType: "reorder.item", payload: "alpha", slop: 2, emitMoves: true, allowedOperations: ["MOVE"] } }]
    )
    Panel(key: "reorder.verbose.beta", layout: { width: 60, height: 40 })
  }
}
overlay LazyMotion {
  root Repeater(
    key: "motion.lazy", estimatedItemExtent: 24,
    layout: { width: 120, height: 24 }
  ) {
    for item in [{ label: "late" }] {
      Panel(
        key: upper(item.label), enter: LazyEnter,
        layout: { width: 120, height: 24 }
      )
    }
  }
}
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(compiler::ModuleSource{"motion.strata", source}, no_imports, 0U)
              .activated(),
          "motion timing fixture did not activate");

    ui::MotionCatalog catalog;
    catalog.bind(application.active_unit());
    const ui::CompiledMotion* counted = catalog.find("Counted");
    const ui::CompiledMotion* focus_visible_trigger = catalog.find("FocusVisibleTrigger");
    check(counted != nullptr && focus_visible_trigger != nullptr &&
              focus_visible_trigger->trigger == ui::MotionTrigger::focus_visible,
          "authored motion or focus-visible trigger was not retained in the native catalog");
    check(counted->timing.fill_mode == ui::MotionFillMode::backwards &&
              counted->timing.repeat.kind == ui::MotionRepeatKind::count &&
              counted->timing.repeat.iterations == 3U && counted->timing.reverse,
          "authored fill/repeat/reverse timing did not survive portable lowering");

    ui::motion_detail::TimelinePlayer player;
    const auto delayed = player.advance(*counted, true, ui::MotionDirection::forward, 0, false);
    check(delayed.running &&
              delayed.computed.number(ui::MotionProperty::opacity).value_or(-1.0) == 0.0,
          "backwards fill did not present the initial keyframe during delay");
    const auto alternating =
        player.advance(*counted, true, ui::MotionDirection::forward, 145'000'000, false);
    check_near(alternating.raw_progress, 0.75,
               "repeat reversal did not alternate iteration direction");
    check_near(alternating.computed.number(ui::MotionProperty::opacity).value_or(-1.0), 0.75,
               "repeat reversal sampled the wrong authored value");
    const auto unfilled_terminal =
        player.advance(*counted, true, ui::MotionDirection::forward, 320'000'000, false);
    check(unfilled_terminal.finished && unfilled_terminal.computed.values.empty(),
          "backwards-only fill leaked authored values after the final iteration");
    ui::CompiledMotion retained_terminal = *counted;
    retained_terminal.timing.fill_mode = ui::MotionFillMode::both;
    ui::motion_detail::TimelinePlayer retained_player;
    static_cast<void>(
        retained_player.advance(retained_terminal, true, ui::MotionDirection::forward, 0, false));
    const auto terminal = retained_player.advance(retained_terminal, true,
                                                  ui::MotionDirection::forward, 320'000'000, false);
    check_near(terminal.computed.number(ui::MotionProperty::opacity).value_or(-1.0), 1.0,
               "forwards fill did not retain the terminal direction of a counted repeat");
    ui::CompiledMotion even_alternating = retained_terminal;
    even_alternating.timing.repeat.iterations = 2U;
    ui::motion_detail::TimelinePlayer even_player;
    static_cast<void>(
        even_player.advance(even_alternating, true, ui::MotionDirection::forward, 0, false));
    const auto even_terminal = even_player.advance(
        even_alternating, true, ui::MotionDirection::forward, 220'000'000, false);
    check(even_terminal.finished &&
              even_player.progress() ==
                  ui::motion_terminal_progress(even_alternating, ui::MotionDirection::forward) &&
              even_player.progress() == 0.0,
          "even counted reverse terminal diverged from canonical exit completion");
    ui::motion_detail::TimelinePlayer reverse_even_player;
    static_cast<void>(reverse_even_player.advance(even_alternating, true,
                                                  ui::MotionDirection::reverse, 0, false));
    static_cast<void>(reverse_even_player.advance(
        even_alternating, true, ui::MotionDirection::reverse, 220'000'000, false));
    check(reverse_even_player.progress() ==
                  ui::motion_terminal_progress(even_alternating, ui::MotionDirection::reverse) &&
              reverse_even_player.progress() == 1.0,
          "reverse even counted terminal diverged from canonical exit completion");

    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 180;
    environment.logical_width = 320.0;
    environment.logical_height = 180.0;
    ui::Surface lazy_motion("lazy-motion", application, runtime::LayerRole::overlay, "LazyMotion",
                            environment);
    const ui::SurfaceFrame lazy_first = lazy_motion.frame(1'000'000);
    const ui::RetainedNode* late_row = lazy_motion.tree().find_key("LATE");
    const auto enter_channel =
        [&lazy_motion](const ui::RetainedNode* row) -> const ui::MotionInspectionChannel* {
        const auto* channels =
            row != nullptr ? lazy_motion.motion().inspection_channels(row->identity()) : nullptr;
        if (channels == nullptr)
            return nullptr;
        const auto found = std::ranges::find(*channels, std::string_view("strata.trigger.enter"),
                                             &ui::MotionInspectionChannel::id);
        return found != channels->end() ? &*found : nullptr;
    };
    const ui::MotionInspectionChannel* late_enter = enter_channel(late_row);
    check(late_enter != nullptr && late_enter->running && late_enter->progress == 0.0 &&
              lazy_motion.motion().active_count() == 1U &&
              lazy_first.operations.motion_running_players == 1U,
          "later lazy row did not discover and schedule its enter motion in the same frame");
    const ui::SurfaceFrame lazy_same_time = lazy_motion.frame(1'000'000);
    const ui::MotionInspectionChannel* same_time_enter = enter_channel(late_row);
    check(same_time_enter != nullptr && same_time_enter->progress == 0.0 &&
              lazy_same_time.operations.motion_mutated_nodes == 0U,
          "same-timestamp lazy convergence advanced or double-counted an existing motion");
    static_cast<void>(lazy_motion.frame(51'000'000));
    const ui::MotionInspectionChannel* advanced_enter = enter_channel(late_row);
    check(advanced_enter != nullptr && advanced_enter->progress > 0.0 &&
              advanced_enter->progress < 1.0,
          "discovered lazy enter motion did not schedule a later temporal frame");

    ui::Surface surface("motion-progress", application, runtime::LayerRole::overlay, "Motion",
                        environment);
    static_cast<void>(surface.frame(0));
    const ui::RetainedNode* progress_node = surface.tree().find_key("motion.progress");
    check(progress_node != nullptr, "indeterminate Progress was not retained");
    const auto progress_sample = [&surface, progress_node]() -> const ui::MotionInspectionChannel* {
        const auto* channels = surface.motion().inspection_channels(progress_node->identity());
        if (channels == nullptr)
            return nullptr;
        const auto found =
            std::ranges::find(*channels, std::string_view("strata.progress.indeterminate"),
                              &ui::MotionInspectionChannel::id);
        return found != channels->end() ? &*found : nullptr;
    };
    static_cast<void>(surface.frame(300'000'000));
    check(progress_sample() != nullptr && progress_sample()->running,
          "indeterminate Progress did not retain a running motion timeline");
    check_near(progress_sample()->progress, 0.25,
               "indeterminate Progress did not advance per frame");

    environment = surface.environment();
    environment.reduced_motion = true;
    check(surface.adopt_environment_preferences(environment), "reduced motion was not adopted");
    static_cast<void>(surface.frame(300'000'000));
    static_cast<void>(surface.frame(900'000'000));
    check(progress_sample() != nullptr && !progress_sample()->running,
          "reduced motion did not pause the indeterminate Progress timeline");
    check_near(progress_sample()->progress, 0.25,
               "reduced motion changed the presented progress frame");

    environment = surface.environment();
    environment.reduced_motion = false;
    check(surface.adopt_environment_preferences(environment), "standard motion was not restored");
    static_cast<void>(surface.frame(900'000'000));
    check_near(progress_sample()->progress, 0.25, "Progress resumed with reduced-motion catch-up");
    static_cast<void>(surface.frame(1'200'000'000));
    check_near(progress_sample()->progress, 0.5, "Progress did not resume from its frozen frame");

    ui::Surface interaction_motion("interaction-motion", application, runtime::LayerRole::overlay,
                                   "InteractionMotion", environment);
    static_cast<void>(interaction_motion.frame(1'300'000'000));
    const ui::RetainedNode* hover_node = interaction_motion.tree().find_key("motion.hover");
    const ui::LayoutRecord* hover_layout =
        hover_node != nullptr ? interaction_motion.layout().find(hover_node->identity()) : nullptr;
    check(hover_layout != nullptr, "hover interaction motion fixture was not arranged");
    const ui::Point hover_center{
        hover_layout->bounds.x + hover_layout->bounds.width * 0.5,
        hover_layout->bounds.y + hover_layout->bounds.height * 0.5,
    };
    static_cast<void>(interaction_motion.input().enqueue_pointer(ui::PointerInputEvent{
        hover_center,
        ui::PointerEventType::move,
        0,
        0,
    }));
    const ui::SurfaceFrame hover_started = interaction_motion.frame(1'310'000'000);
    const auto hover_sample = [&interaction_motion,
                               hover_node]() -> const ui::MotionInspectionChannel* {
        const auto* channels =
            interaction_motion.motion().inspection_channels(hover_node->identity());
        if (channels == nullptr)
            return nullptr;
        const auto found = std::ranges::find(*channels, std::string_view("motion.hover.channel"),
                                             &ui::MotionInspectionChannel::id);
        return found != channels->end() ? &*found : nullptr;
    };
    check(hover_sample() != nullptr && hover_sample()->running && hover_sample()->progress == 0.0 &&
              hover_started.operations.motion_running_players == 1U,
          "queued hover did not discover its interaction motion in the input frame");
    static_cast<void>(interaction_motion.frame(1'360'000'000));
    check_near(hover_sample()->progress, 0.5,
               "post-input motion discovery advanced the interaction clock more than once");

    const auto focus_visible_sample = [&interaction_motion,
                                       hover_node]() -> const ui::MotionInspectionChannel* {
        const auto* channels =
            interaction_motion.motion().inspection_channels(hover_node->identity());
        if (channels == nullptr)
            return nullptr;
        const auto found =
            std::ranges::find(*channels, std::string_view("motion.focus-visible.channel"),
                              &ui::MotionInspectionChannel::id);
        return found != channels->end() ? &*found : nullptr;
    };
    static_cast<void>(interaction_motion.input().enqueue_click("motion.hover"));
    static_cast<void>(interaction_motion.frame(1'370'000'000));
    check(interaction_motion.input().focused(hover_node->identity()) &&
              !interaction_motion.input().focus_visible(hover_node->identity()) &&
              focus_visible_sample() != nullptr && !focus_visible_sample()->running &&
              focus_visible_sample()->progress == 0.0,
          "pointer focus activated the keyboard-only focus-visible motion");
    static_cast<void>(interaction_motion.input().key("tab"));
    static_cast<void>(interaction_motion.frame(1'380'000'000));
    check(interaction_motion.input().focus_visible(hover_node->identity()) &&
              focus_visible_sample() != nullptr && focus_visible_sample()->running &&
              focus_visible_sample()->progress == 0.0,
          "keyboard traversal did not activate the focus-visible motion");
    static_cast<void>(interaction_motion.frame(1'430'000'000));
    check_near(focus_visible_sample()->progress, 0.5,
               "focus-visible motion did not advance after keyboard activation");
    static_cast<void>(interaction_motion.input().enqueue_click("motion.hover"));
    static_cast<void>(interaction_motion.frame(1'430'000'000));
    check(!interaction_motion.input().focus_visible(hover_node->identity()) &&
              focus_visible_sample() != nullptr && focus_visible_sample()->running,
          "pointer modality did not reverse the active focus-visible motion");
    check_near(focus_visible_sample()->progress, 0.5,
               "pointer-driven focus-visible reversal jumped away from its displayed value");

    ui::Surface reorder_motion("reorder-motion", application, runtime::LayerRole::overlay,
                               "ReorderMotion", environment);
    static_cast<void>(reorder_motion.frame(1'400'000'000));
    const ui::InputOperationResult reorder =
        reorder_motion.input().drag("reorder.alpha", "reorder.container");
    check(std::ranges::any_of(reorder.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* type = event.find("type");
                                  const data::JsonValue* phase = event.find("phase");
                                  const data::JsonValue* recipient = event.find("recipient");
                                  const data::JsonValue* insertion = event.find("insertionIndex");
                                  const data::JsonValue* before = event.find("beforeKey");
                                  const data::JsonValue* after = event.find("afterKey");
                                  return type != nullptr && type->string() != nullptr &&
                                         *type->string() == "drag" && phase != nullptr &&
                                         phase->string() != nullptr && *phase->string() == "drop" &&
                                         recipient != nullptr && recipient->string() != nullptr &&
                                         *recipient->string() == "target" && insertion != nullptr &&
                                         insertion->integer() != nullptr &&
                                         *insertion->integer() == 2 && before != nullptr &&
                                         before->is_null() && after != nullptr &&
                                         after->string() != nullptr &&
                                         *after->string() == "reorder.beta";
                              }) &&
              std::ranges::none_of(reorder.events,
                                   [](const data::JsonValue& event) {
                                       const data::JsonValue* phase = event.find("phase");
                                       return phase != nullptr && phase->string() != nullptr &&
                                              *phase->string() == "move";
                                   }),
          "reorder drop metadata was incomplete or default drag published high-frequency moves");
    const ui::SurfaceFrame reorder_repaint = reorder_motion.frame(1'416'666'667);
    check(reorder_repaint.operations.rebuilds == 0U,
          "retained reorder preview invalidated the declarative description");

    ui::Surface verbose_reorder("reorder-motion-verbose", application, runtime::LayerRole::overlay,
                                "ReorderMotionVerbose", environment);
    static_cast<void>(verbose_reorder.frame(1'400'000'000));
    const ui::InputOperationResult verbose =
        verbose_reorder.input().drag("reorder.verbose.alpha", "reorder.verbose.container");
    check(std::ranges::any_of(verbose.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* phase = event.find("phase");
                                  return phase != nullptr && phase->string() != nullptr &&
                                         *phase->string() == "move";
                              }),
          "drag source emitMoves opt-in did not publish move lifecycle events");
}

void test_component_slot_projection() {
    using namespace strata;
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("slot-projection", bundle);
    const std::string source = R"(
component Frame() {
  defaults: { Text: { variant: "compact" } }
  Panel {
    Slot(name: "header") { Text(key: "projection.default", text: "default") }
    Slot(name: "content")
  }
}

overlay Projection {
  root Frame() {
    Slot(name: "header") { Text(key: "projection.named", text: "named") }
    Text(key: "projection.raw", text: "raw")
  }
}
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    const runtime::ActivationResult activation = application.compile_and_activate(
        compiler::ModuleSource{"projection.strata", source}, no_imports, 0U);
    check(activation.activated(), "component slot projection fixture did not activate");

    ui::DescriptionBuilder builder(application);
    const ui::DescriptionBuildResult description =
        builder.build(runtime::LayerRole::overlay, "Projection");
    check(description.diagnostics.empty(), "component slot projection produced diagnostics");
    ui::RetainedTree tree;
    static_cast<void>(tree.reconcile(description.root));
    check(tree.find_key("projection.named") != nullptr &&
              tree.find_key("projection.raw") != nullptr &&
              tree.find_key("projection.default") == nullptr,
          "named/default/shorthand component slot projection changed");
    const auto variant =
        tree.find_key("projection.named")->description().properties.find("variant");
    check(variant != tree.find_key("projection.named")->description().properties.end() &&
              variant->second.value() != nullptr && variant->second.value()->string() != nullptr &&
              *variant->second.value()->string() == "compact",
          "component widget defaults did not apply to projected caller content");
}

void test_component_cache_tracks_exact_retained_dependencies() {
    using namespace strata;
    const data::JsonValue schemas = data::parse_json(R"({
      "extensionPackages":[],
      "widgets":{"registry":"component-cache","required":[],"definitions":[]},
      "actions":{"registry":"component-cache","required":[],"definitions":[]},
      "host":[
        {"path":"alpha","nullable":false,"type":{"kind":"object","allowUnknownFields":false,"valueNullable":false,"fields":[{"name":"value","type":{"kind":"string"},"required":true,"nullable":false}]}},
        {"path":"beta","nullable":false,"type":{"kind":"object","allowUnknownFields":false,"valueNullable":false,"fields":[{"name":"value","type":{"kind":"string"},"required":true,"nullable":false}]}}
      ]
    })");
    const auto bundle = runtime::ApplicationBundle::create(&schemas);
    runtime::ApplicationContext application("component-retained-cache", bundle);
    static_cast<void>(application.host().adopt(bundle->host_snapshot(
        "component-cache-alpha", 1U, data::parse_json(R"({"alpha":{"value":"stable"}})"))));
    static_cast<void>(application.host().adopt(bundle->host_snapshot(
        "component-cache-beta", 1U, data::parse_json(R"({"beta":{"value":"first"}})"))));
    const std::string source = R"(
component StableLeaf() {
  Text(key: "cache.stable", text: "stable")
}

component HostAlphaLeaf() {
  Text(key: "cache.host.alpha", text: alpha.value)
}

component HostBetaLeaf() {
  Text(key: "cache.host.beta", text: beta.value)
}

component RetainedLeaf() {
  SplitPane(key: "cache.split", defaultRatio: 0.25, layout: { kind: "ROW", width: 120, height: 40 }) {
    Panel(key: "cache.left")
    Panel(key: "cache.right")
  }
}

component CacheRoot() {
  Panel(key: "cache.root") {
    StableLeaf()
    HostAlphaLeaf()
    HostBetaLeaf()
    RetainedLeaf()
  }
}

overlay Main { root CacheRoot() }
overlay Other { root Text(key: "cache.other", text: "other") }
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(
                  compiler::ModuleSource{"component-retained-cache.strata", source}, no_imports, 0U)
              .activated(),
          "component retained-dependency cache fixture did not activate");

    const auto find_description =
        [](const auto& self, const std::shared_ptr<const ui::DescriptionNode>& node,
           const std::string_view key) -> std::shared_ptr<const ui::DescriptionNode> {
        if (node->key.has_value() && *node->key == key)
            return node;
        for (std::size_t index = 0U; index < node->children->size(); ++index) {
            if (auto found = self(self, node->children->at(index), key); found != nullptr) {
                return found;
            }
        }
        return nullptr;
    };

    ui::DescriptionBuilder builder(application);
    ui::DescriptionBuildResult first = builder.build(runtime::LayerRole::overlay, "Main");
    check(first.diagnostics.empty(), "component retained-dependency fixture produced diagnostics");
    const std::shared_ptr<const ui::DescriptionNode> stable_before =
        find_description(find_description, first.root, "cache.stable");
    const std::shared_ptr<const ui::DescriptionNode> split_before =
        find_description(find_description, first.root, "cache.split");
    const std::shared_ptr<const ui::DescriptionNode> alpha_before =
        find_description(find_description, first.root, "cache.host.alpha");
    const std::shared_ptr<const ui::DescriptionNode> beta_before =
        find_description(find_description, first.root, "cache.host.beta");
    ui::RetainedTree tree;
    static_cast<void>(tree.reconcile(first.root));
    const ui::RetainedNode* const retained_split = tree.find_key("cache.split");
    check(retained_split != nullptr &&
              tree.set_retained_value(retained_split->identity(), "strata.gesture.splitRatio",
                                      runtime::Value(0.75)),
          "component retained-dependency fixture did not change its retained ratio");

    builder.set_retained_tree(&tree);
    ui::DescriptionBuildResult second = builder.build(runtime::LayerRole::overlay, "Main");
    const std::shared_ptr<const ui::DescriptionNode> stable_after =
        find_description(find_description, second.root, "cache.stable");
    const std::shared_ptr<const ui::DescriptionNode> split_after =
        find_description(find_description, second.root, "cache.split");
    check(stable_before != nullptr && stable_after == stable_before,
          "an unrelated retained value invalidated a component cache entry");
    check(split_before != nullptr && split_after != nullptr && split_after != split_before,
          "a component reused output after one of its observed retained values changed");
    const runtime::Value* left_layout =
        split_after->children->at(0U)->properties.at("$layout").value();
    const runtime::Value* left_width =
        left_layout != nullptr ? left_layout->field("width") : nullptr;
    check(left_width != nullptr && left_width->field("weight") != nullptr &&
              left_width->field("weight")->number() != nullptr &&
              std::abs(*left_width->field("weight")->number() - 0.75) < 0.000'001,
          "the rebuilt component did not consume its changed retained value");

    static_cast<void>(application.host().adopt(bundle->host_snapshot(
        "component-cache-beta", 2U, data::parse_json(R"({"beta":{"value":"second"}})"))));
    ui::DescriptionBuildResult third = builder.build(runtime::LayerRole::overlay, "Main");
    const std::shared_ptr<const ui::DescriptionNode> alpha_after =
        find_description(find_description, third.root, "cache.host.alpha");
    const std::shared_ptr<const ui::DescriptionNode> beta_after =
        find_description(find_description, third.root, "cache.host.beta");
    check(alpha_before != nullptr && alpha_after == alpha_before,
          "an unrelated host snapshot generation invalidated a component cache entry");
    check(beta_before != nullptr && beta_after != nullptr && beta_after != beta_before &&
              *beta_after->properties.at("text").value()->string() == "second",
          "a component reused output after its owning host snapshot changed");

    static_cast<void>(builder.build(runtime::LayerRole::overlay, "Other"));
    ui::DescriptionBuildResult returned = builder.build(runtime::LayerRole::overlay, "Main");
    check(find_description(find_description, returned.root, "cache.host.alpha") == alpha_after &&
              find_description(find_description, returned.root, "cache.host.beta") == beta_after,
          "detaching one layer eagerly discarded reusable component descriptions");
}

void test_phased_input_dispatch_and_gesture_claim() {
    using namespace strata;
    const auto bundle = runtime::ApplicationBundle::create();
    runtime::ApplicationContext application("phased-input", bundle);
    const std::string source = R"(
overlay PhasedInput {
  root Panel(
    key: "phase.parent",
    behaviors: [{ id: "strata.focusable" }],
    layout: { kind: "PANEL", width: 220, height: 100, padding: 12 }
  ) {
    Button(key: "phase.child", label: "Child", layout: { width: 120, height: 40 })
  }
}
)";
    const auto no_imports = [](const std::string_view,
                               const std::string_view path) -> compiler::ModuleSource {
        throw compiler::ModuleLoadError("unexpected import '" + std::string(path) + "'");
    };
    check(application
              .compile_and_activate(compiler::ModuleSource{"phased-input.strata", source},
                                    no_imports, 0U)
              .activated(),
          "phased input fixture did not activate");

    const auto phase_name = [](const ui::InputEventPhase phase) -> std::string_view {
        switch (phase) {
        case ui::InputEventPhase::capture:
            return "capture";
        case ui::InputEventPhase::target:
            return "target";
        case ui::InputEventPhase::bubble:
            return "bubble";
        case ui::InputEventPhase::advance:
            return "advance";
        case ui::InputEventPhase::after_layout:
            return "after-layout";
        }
        return "target";
    };
    std::vector<std::string> pointer_press_route;
    std::vector<std::string> pointer_cancel_route;
    std::vector<ui::KeyInputEvent> routed_keys;
    std::vector<ui::TextInputEvent> routed_text;
    std::vector<ui::ImePreeditInputEvent> routed_ime;
    bool release_saw_claim = false;
    bool cancel_saw_state = false;

    ui::WidgetRegistry widgets;
    ui::WidgetInputPhase panel = widgets.find("Panel")->input;
    panel.event = [&](ui::WidgetInputScope& scope) {
        if (scope.kind() == ui::InputEventKind::pointer_press) {
            pointer_press_route.push_back("panel:" + std::string(phase_name(scope.phase())));
        } else if (scope.kind() == ui::InputEventKind::pointer_cancel) {
            pointer_cancel_route.push_back("panel:" + std::string(phase_name(scope.phase())));
        }
        return false;
    };
    widgets.register_input_phase("Panel", std::move(panel));
    ui::WidgetInputPhase button = widgets.find("Button")->input;
    button.event = [&](ui::WidgetInputScope& scope) {
        if (scope.kind() == ui::InputEventKind::pointer_press) {
            pointer_press_route.push_back("button:" + std::string(phase_name(scope.phase())));
        } else if (scope.kind() == ui::InputEventKind::pointer_drag &&
                   scope.phase() == ui::InputEventPhase::target) {
            check(scope.claim_gesture(), "target widget could not claim its active press");
        } else if (scope.kind() == ui::InputEventKind::pointer_release &&
                   scope.phase() == ui::InputEventPhase::target) {
            release_saw_claim = scope.gesture_claim_state() == ui::GestureClaimState::claimed;
        } else if (scope.kind() == ui::InputEventKind::pointer_cancel) {
            if (scope.phase() == ui::InputEventPhase::target) {
                cancel_saw_state = scope.gesture_claim_state() == ui::GestureClaimState::cancelled;
            }
            pointer_cancel_route.push_back("button:" + std::string(phase_name(scope.phase())));
        } else if (scope.kind() == ui::InputEventKind::key &&
                   scope.phase() == ui::InputEventPhase::target && scope.dispatch() != nullptr &&
                   scope.dispatch()->key() != nullptr) {
            routed_keys.push_back(*scope.dispatch()->key());
        } else if (scope.kind() == ui::InputEventKind::text &&
                   scope.phase() == ui::InputEventPhase::target && scope.dispatch() != nullptr &&
                   scope.dispatch()->text() != nullptr) {
            routed_text.push_back(*scope.dispatch()->text());
        } else if (scope.kind() == ui::InputEventKind::ime_preedit &&
                   scope.phase() == ui::InputEventPhase::target && scope.dispatch() != nullptr &&
                   scope.dispatch()->ime_preedit() != nullptr) {
            routed_ime.push_back(*scope.dispatch()->ime_preedit());
        }
        return false;
    };
    widgets.register_input_phase("Button", std::move(button));

    ui::BehaviorRegistry behaviors;
    ui::BehaviorInputPhase focusable = behaviors.find("strata.focusable")->input;
    focusable.event = [&](ui::BehaviorInputScope& scope) {
        const ui::InputDispatchContext* dispatch = scope.dispatch();
        if (dispatch != nullptr && dispatch->kind() == ui::InputEventKind::pointer_press) {
            pointer_press_route.push_back("behavior:" + std::string(phase_name(dispatch->phase())));
        } else if (dispatch != nullptr && dispatch->kind() == ui::InputEventKind::pointer_cancel) {
            pointer_cancel_route.push_back("behavior:" +
                                           std::string(phase_name(dispatch->phase())));
        }
        return false;
    };
    behaviors.register_input_phase("strata.focusable", std::move(focusable));

    ui::SurfaceEnvironment environment;
    environment.framebuffer_width = 320;
    environment.framebuffer_height = 180;
    environment.logical_width = 320.0;
    environment.logical_height = 180.0;
    environment.input = ui::SurfaceInputCapabilities{
        true, ui::PointerPrecision::fine, true, false, true, true, false,
    };
    ui::Surface surface("phased-input", application, runtime::LayerRole::overlay, "PhasedInput",
                        environment, {}, std::move(widgets), std::move(behaviors));
    static_cast<void>(surface.frame(1'000'000'000));
    const ui::RetainedNode* child = surface.tree().find_key("phase.child");
    const ui::LayoutRecord* child_layout =
        child != nullptr ? surface.layout().find(child->identity()) : nullptr;
    check(child_layout != nullptr, "phased input child was not arranged");
    const ui::Point origin{
        child_layout->bounds.x + child_layout->bounds.width * 0.5,
        child_layout->bounds.y + child_layout->bounds.height * 0.5,
    };
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        origin,
        ui::PointerEventType::press,
        17,
        0,
        {},
        {},
        1'010'000'000,
    }));
    static_cast<void>(surface.frame(1'016'666'667));
    check(pointer_press_route ==
              std::vector<std::string>{
                  "behavior:capture",
                  "panel:capture",
                  "button:target",
                  "behavior:bubble",
                  "panel:bubble",
              },
          "pointer press did not traverse one behavior/widget capture-target-bubble route");

    const ui::Point dragged{origin.x + 12.0, origin.y};
    static_cast<void>(surface.input().enqueue(std::vector<ui::SurfaceInputEvent>{
        ui::PointerInputEvent{
            dragged,
            ui::PointerEventType::move,
            17,
            0,
            {},
            {12.0, 0.0},
            1'020'000'000,
        },
        ui::PointerInputEvent{
            origin,
            ui::PointerEventType::move,
            17,
            0,
            {},
            {-12.0, 0.0},
            1'030'000'000,
        },
        ui::PointerInputEvent{
            origin,
            ui::PointerEventType::release,
            17,
            0,
            {},
            {},
            1'040'000'000,
        },
    }));
    const ui::SurfaceFrame release = surface.frame(1'050'000'000);
    check(release.operations.input_coalesced_moves == 1U,
          "captured pointer moves were not sampled once at the frame boundary");
    check(release_saw_claim, "gesture claim did not survive a drag returning to its origin");
    check(std::ranges::none_of(release.lifecycle_input.events,
                               [](const data::JsonValue& event) {
                                   const data::JsonValue* type = event.find("type");
                                   return type != nullptr && type->string() != nullptr &&
                                          *type->string() == "activated";
                               }),
          "claimed drag returning to its origin was incorrectly activated");

    static_cast<void>(surface.input().enqueue(std::vector<ui::SurfaceInputEvent>{
        ui::KeyInputEvent{
            "EnTeR",
            ui::KeyModifiers{true, false, true, false},
            ui::KeyEventType::release,
            1'051'000'000,
        },
    }));
    const ui::SurfaceFrame key_release = surface.frame(1'052'000'000);
    check(routed_keys.size() == 1U && routed_keys.back().key == "EnTeR" &&
              routed_keys.back().type == ui::KeyEventType::release &&
              routed_keys.back().timestamp_nanos == 1'051'000'000 &&
              routed_keys.back().modifiers.shift && routed_keys.back().modifiers.alt,
          "queued key release lost its raw generic-dispatch envelope");
    check(std::ranges::none_of(key_release.lifecycle_input.events,
                               [](const data::JsonValue& event) {
                                   const data::JsonValue* type = event.find("type");
                                   return type != nullptr && type->string() != nullptr &&
                                          *type->string() == "activated";
                               }),
          "key release invoked press-only activation policy");

    static_cast<void>(surface.input().enqueue(std::vector<ui::SurfaceInputEvent>{
        ui::KeyInputEvent{
            "EnTeR",
            {},
            ui::KeyEventType::repeat,
            1'053'000'000,
        },
    }));
    const ui::SurfaceFrame key_repeat = surface.frame(1'054'000'000);
    check(routed_keys.size() == 2U && routed_keys.back().type == ui::KeyEventType::repeat &&
              routed_keys.back().timestamp_nanos == 1'053'000'000,
          "queued key repeat was reconstructed as a press");
    check(std::ranges::any_of(key_repeat.lifecycle_input.events,
                              [](const data::JsonValue& event) {
                                  const data::JsonValue* type = event.find("type");
                                  return type != nullptr && type->string() != nullptr &&
                                         *type->string() == "activated";
                              }),
          "key repeat stopped following the existing press policy");

    static_cast<void>(surface.input().enqueue(std::vector<ui::SurfaceInputEvent>{
        ui::NavigationInputEvent{
            "activate",
            ui::KeyEventType::release,
            {},
            1'055'000'000,
        },
        ui::TextInputEvent{"raw", 0},
        ui::ImePreeditInputEvent{"preedit", 1U, 3U, 0},
    }));
    const ui::SurfaceFrame navigation_release = surface.frame(1'056'000'000);
    check(routed_keys.size() == 3U && routed_keys.back().key == "enter" &&
              routed_keys.back().type == ui::KeyEventType::release &&
              routed_keys.back().timestamp_nanos == 1'055'000'000,
          "controller navigation lost its original key action or timestamp");
    check(std::ranges::none_of(navigation_release.lifecycle_input.events,
                               [](const data::JsonValue& event) {
                                   const data::JsonValue* type = event.find("type");
                                   return type != nullptr && type->string() != nullptr &&
                                          *type->string() == "activated";
                               }),
          "controller release invoked press-only activation policy");
    check(routed_text.size() == 1U && routed_text.front().text == "raw" &&
              routed_text.front().timestamp_nanos == 0 && routed_ime.size() == 1U &&
              routed_ime.front().text == "preedit" && routed_ime.front().selection_start == 1U &&
              routed_ime.front().selection_end == 3U && routed_ime.front().timestamp_nanos == 0,
          "queued text or IME generic dispatch reconstructed the raw envelope");

    pointer_cancel_route.clear();
    static_cast<void>(surface.input().enqueue_pointer(ui::PointerInputEvent{
        origin,
        ui::PointerEventType::press,
        23,
        0,
        {},
        {},
        1'060'000'000,
    }));
    static_cast<void>(surface.frame(1'066'666'667));
    surface.cancel_interactions();
    check(cancel_saw_state, "pointer cancellation was not visible in the shared dispatch context");
    check(pointer_cancel_route ==
              std::vector<std::string>{
                  "behavior:capture",
                  "panel:capture",
                  "button:target",
                  "behavior:bubble",
                  "panel:bubble",
              },
          "surface cancellation bypassed generic capture-target-bubble delivery");
}

} // namespace

int main(const int argument_count, const char* const* const arguments) {
    try {
        if (argument_count == 3 && std::string_view(arguments[1]) == "--collections") {
            test_native_collection_interaction_and_templates(arguments[2]);
            std::cout << "strata_ui_tests: OK\n";
            return 0;
        }
        test_collection_models();
        test_keyed_reconciliation_and_detach_cleanup();
        test_exit_retention_and_prune_ownership();
        test_lazy_materialization_and_noop_reconcile();
        test_retained_virtual_realization_window();
        test_lazy_range_convergence_state_machine();
        test_materialization_publication_identity_and_eviction();
        test_variable_virtual_extents_and_stable_anchor();
        test_layered_layout_uses_independent_axes();
        test_anchored_portal_is_out_of_flow_and_flips();
        test_virtualization_cache_queries_only_observed_keys();
        test_retained_layout_cache_scale_and_invalidation();
        test_retained_layout_cache_translates_unchanged_subtrees();
        test_explicit_container_size_constrains_descendants();
        test_scroll_content_box_constrains_fill_child();
        test_exiting_child_retains_placement_without_affecting_flow();
        test_nested_scroll_pin_uses_nearest_scroll_offset();
        test_wrapped_linear_and_intrinsic_grid_layout();
        test_content_size_motion_interrupts_and_settles();
        test_scroll_virtual_range_clipping_and_safe_insets();
        test_active_work_scheduler_and_detach_cleanup();
        test_render_packet_batches_every_portable_command();
        test_authored_material_scope_is_fill_local();
        test_shadow_submission_extends_beyond_the_source_shape();
        test_gradient_paint_authoring_and_tessellation();
        test_vector_shape_tessellation();
        test_svg_image_projection_and_compound_fill();
        if (argument_count >= 3 && std::string_view(arguments[2]).size() != 0U) {
            test_bundled_font_metrics(arguments[1]);
            test_bundled_texture_descriptor(arguments[1]);
            test_render_submission_cache(arguments[1]);
            test_render_submission_translation_reuse(arguments[1]);
            test_native_nine_patch_geometry(arguments[1]);
            test_native_custom_mesh_geometry(arguments[1]);
            test_motion_timing_and_indeterminate_progress();
            test_component_slot_projection();
            test_component_cache_tracks_exact_retained_dependencies();
            test_phased_input_dispatch_and_gesture_claim();
            test_portable_description_and_declaration_state(arguments[1], arguments[2]);
            test_repeater_description_is_data_backed();
            test_surface_contextual_environment(arguments[1]);
            test_async_collection_key_isolation_and_lazy_tree_publication(arguments[1]);
            test_native_collection_interaction_and_templates(arguments[1]);
        }
        std::cout << "strata_ui_tests: OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "strata_ui_tests: " << error.what() << '\n';
        return 1;
    }
}
