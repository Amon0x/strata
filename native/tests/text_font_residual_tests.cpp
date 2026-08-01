#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "font/opentype.hpp"
#include "font/atlas.hpp"
#include "font/raster.hpp"
#include "resource/resource.hpp"
#include "runtime/expression.hpp"
#include "runtime/value.hpp"
#include "ui/text.hpp"
#include "ui/text_geometry.hpp"
#include "ui/render.hpp"
#include "ui/tree.hpp"

namespace {

using Bytes = strata::resource::ResourceBytes;

void check(const bool condition, const std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void check_near(const double actual, const double expected, const std::string_view message) {
    if (std::abs(actual - expected) > 0.000'001) {
        throw std::runtime_error(std::string(message) + ": expected " +
                                 std::to_string(expected) + ", got " +
                                 std::to_string(actual));
    }
}

[[nodiscard]] std::uint16_t u16(const Bytes& bytes, const std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes.at(offset)) << 8U | bytes.at(offset + 1U)
    );
}

[[nodiscard]] std::uint32_t u32(const Bytes& bytes, const std::size_t offset) {
    return static_cast<std::uint32_t>(bytes.at(offset)) << 24U |
           static_cast<std::uint32_t>(bytes.at(offset + 1U)) << 16U |
           static_cast<std::uint32_t>(bytes.at(offset + 2U)) << 8U |
           bytes.at(offset + 3U);
}

void set_u16(Bytes& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value >> 8U);
    bytes.at(offset + 1U) = static_cast<std::uint8_t>(value);
}

void set_u32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    bytes.at(offset) = static_cast<std::uint8_t>(value >> 24U);
    bytes.at(offset + 1U) = static_cast<std::uint8_t>(value >> 16U);
    bytes.at(offset + 2U) = static_cast<std::uint8_t>(value >> 8U);
    bytes.at(offset + 3U) = static_cast<std::uint8_t>(value);
}

void append_u16(Bytes& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void append_i16(Bytes& bytes, const std::int16_t value) {
    append_u16(bytes, static_cast<std::uint16_t>(value));
}

void append_u32(Bytes& bytes, const std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16U));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

[[nodiscard]] std::size_t table_record(
    const Bytes& bytes,
    const std::string_view tag
) {
    const std::uint16_t count = u16(bytes, 4U);
    for (std::size_t index = 0U; index < count; ++index) {
        const std::size_t record = 12U + index * 16U;
        if (std::string_view(
                reinterpret_cast<const char*>(bytes.data() + record), 4U
            ) == tag) {
            return record;
        }
    }
    throw std::runtime_error("font fixture has no table " + std::string(tag));
}

void replace_table(
    Bytes& font,
    const std::string_view existing_tag,
    const std::string_view replacement_tag,
    const Bytes& table
) {
    while ((font.size() & 3U) != 0U) font.push_back(0U);
    const std::size_t offset = font.size();
    font.insert(font.end(), table.begin(), table.end());
    const std::size_t record = table_record(font, existing_tag);
    for (std::size_t index = 0U; index < 4U; ++index) {
        font.at(record + index) = static_cast<std::uint8_t>(replacement_tag[index]);
    }
    set_u32(font, record + 8U, static_cast<std::uint32_t>(offset));
    set_u32(font, record + 12U, static_cast<std::uint32_t>(table.size()));
}

[[nodiscard]] Bytes coverage(const std::uint16_t glyph) {
    Bytes result;
    append_u16(result, 1U);
    append_u16(result, 1U);
    append_u16(result, glyph);
    return result;
}

[[nodiscard]] Bytes anchor(const std::int16_t x, const std::int16_t y) {
    Bytes result;
    append_u16(result, 1U);
    append_i16(result, x);
    append_i16(result, y);
    return result;
}

[[nodiscard]] Bytes mark_subtable(
    const std::uint16_t mark,
    const std::uint16_t base,
    const std::int16_t base_x,
    const std::int16_t base_y,
    const bool ligature
) {
    Bytes result(12U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 6U, 1U);
    const auto append = [&result](const Bytes& value) {
        const std::size_t offset = result.size();
        result.insert(result.end(), value.begin(), value.end());
        return static_cast<std::uint16_t>(offset);
    };
    set_u16(result, 2U, append(coverage(mark)));
    set_u16(result, 4U, append(coverage(base)));
    const std::size_t mark_array = result.size();
    append_u16(result, 1U);
    append_u16(result, 0U);
    append_u16(result, 6U);
    const Bytes mark_anchor = anchor(10, 20);
    result.insert(result.end(), mark_anchor.begin(), mark_anchor.end());
    set_u16(result, 8U, static_cast<std::uint16_t>(mark_array));
    const std::size_t base_array = result.size();
    append_u16(result, 1U);
    if (!ligature) {
        append_u16(result, 4U);
        const Bytes base_anchor = anchor(base_x, base_y);
        result.insert(result.end(), base_anchor.begin(), base_anchor.end());
    } else {
        append_u16(result, 4U);
        append_u16(result, 1U);
        append_u16(result, 4U);
        const Bytes base_anchor = anchor(base_x, base_y);
        result.insert(result.end(), base_anchor.begin(), base_anchor.end());
    }
    set_u16(result, 10U, static_cast<std::uint16_t>(base_array));
    return result;
}

[[nodiscard]] Bytes pair_subtable(
    const std::uint16_t left,
    const std::uint16_t right
) {
    Bytes result(12U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 2U, 12U);
    set_u16(result, 4U, 0x000FU);
    set_u16(result, 8U, 1U);
    set_u16(result, 10U, 18U);
    const Bytes covered = coverage(left);
    result.insert(result.end(), covered.begin(), covered.end());
    append_u16(result, 1U);
    append_u16(result, right);
    append_i16(result, 11);
    append_i16(result, 12);
    append_i16(result, -20);
    append_i16(result, 30);
    return result;
}

[[nodiscard]] Bytes lookup(
    const std::uint16_t type,
    const std::vector<Bytes>& subtables,
    const std::uint16_t flags = 0U,
    const std::optional<std::uint16_t> mark_filtering_set = std::nullopt
) {
    Bytes result(
        6U + subtables.size() * 2U + (mark_filtering_set.has_value() ? 2U : 0U), 0U
    );
    set_u16(result, 0U, type);
    set_u16(result, 2U, flags);
    set_u16(result, 4U, static_cast<std::uint16_t>(subtables.size()));
    if (mark_filtering_set.has_value()) {
        set_u16(result, 6U + subtables.size() * 2U, *mark_filtering_set);
    }
    for (std::size_t index = 0U; index < subtables.size(); ++index) {
        set_u16(result, 6U + index * 2U, static_cast<std::uint16_t>(result.size()));
        result.insert(result.end(), subtables[index].begin(), subtables[index].end());
    }
    return result;
}

[[nodiscard]] Bytes two_component_mark_ligature_subtable(
    const std::uint16_t mark,
    const std::uint16_t ligature
) {
    Bytes result(12U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 6U, 1U);
    const auto append = [&result](const Bytes& value) {
        const std::size_t offset = result.size();
        result.insert(result.end(), value.begin(), value.end());
        return static_cast<std::uint16_t>(offset);
    };
    set_u16(result, 2U, append(coverage(mark)));
    set_u16(result, 4U, append(coverage(ligature)));
    const std::size_t mark_array = result.size();
    append_u16(result, 1U);
    append_u16(result, 0U);
    append_u16(result, 6U);
    const Bytes mark_anchor = anchor(10, 20);
    result.insert(result.end(), mark_anchor.begin(), mark_anchor.end());
    set_u16(result, 8U, static_cast<std::uint16_t>(mark_array));
    const std::size_t ligature_array = result.size();
    append_u16(result, 1U);
    append_u16(result, 4U);
    append_u16(result, 2U);
    append_u16(result, 6U);
    append_u16(result, 12U);
    const Bytes first_anchor = anchor(300, 600);
    const Bytes second_anchor = anchor(400, 800);
    result.insert(result.end(), first_anchor.begin(), first_anchor.end());
    result.insert(result.end(), second_anchor.begin(), second_anchor.end());
    set_u16(result, 10U, static_cast<std::uint16_t>(ligature_array));
    return result;
}

[[nodiscard]] Bytes class_definition(
    std::vector<std::pair<std::uint16_t, std::uint16_t>> glyph_classes
) {
    std::ranges::sort(glyph_classes);
    Bytes result;
    append_u16(result, 2U);
    append_u16(result, static_cast<std::uint16_t>(glyph_classes.size()));
    for (const auto [glyph, glyph_class] : glyph_classes) {
        append_u16(result, glyph);
        append_u16(result, glyph);
        append_u16(result, glyph_class);
    }
    return result;
}

[[nodiscard]] Bytes synthetic_gdef(
    const std::uint16_t left,
    const std::uint16_t right,
    const std::uint16_t ligature,
    const std::uint16_t included_mark,
    const std::uint16_t excluded_mark
) {
    constexpr std::uint16_t base_glyph_class = 1U;
    constexpr std::uint16_t ligature_glyph_class = 2U;
    constexpr std::uint16_t mark_glyph_class = 3U;
    constexpr std::uint16_t included_mark_set = 1U;
    constexpr std::uint16_t excluded_mark_set = 2U;
    Bytes result(14U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 2U, 2U);
    const auto append = [&result](const Bytes& value) {
        const std::size_t offset = result.size();
        result.insert(result.end(), value.begin(), value.end());
        return static_cast<std::uint16_t>(offset);
    };
    set_u16(result, 4U, append(class_definition({
        {left, base_glyph_class}, {right, base_glyph_class},
        {ligature, ligature_glyph_class},
        {included_mark, mark_glyph_class}, {excluded_mark, mark_glyph_class},
    })));
    set_u16(result, 10U, append(class_definition({
        {included_mark, included_mark_set}, {excluded_mark, excluded_mark_set},
    })));
    const std::size_t sets = result.size();
    append_u16(result, 1U);
    append_u16(result, 1U);
    append_u32(result, 8U);
    const Bytes included_coverage = coverage(included_mark);
    result.insert(result.end(), included_coverage.begin(), included_coverage.end());
    set_u16(result, 12U, static_cast<std::uint16_t>(sets));
    return result;
}

[[nodiscard]] Bytes gpos_with_lookups(
    const std::vector<Bytes>& lookups,
    std::vector<std::uint16_t> feature_lookup_indices = {}
) {
    if (feature_lookup_indices.empty()) {
        feature_lookup_indices.reserve(lookups.size());
        for (std::size_t index = 0U; index < lookups.size(); ++index) {
            feature_lookup_indices.push_back(static_cast<std::uint16_t>(index));
        }
    }
    Bytes scripts;
    append_u16(scripts, 1U);
    scripts.insert(scripts.end(), {'D', 'F', 'L', 'T'});
    append_u16(scripts, 8U);
    append_u16(scripts, 4U);
    append_u16(scripts, 0U);
    append_u16(scripts, 0U);
    append_u16(scripts, 0xFFFFU);
    append_u16(scripts, 1U);
    append_u16(scripts, 0U);

    Bytes features(8U, 0U);
    set_u16(features, 0U, 1U);
    features[2U] = 'k';
    features[3U] = 'e';
    features[4U] = 'r';
    features[5U] = 'n';
    set_u16(features, 6U, 8U);
    append_u16(features, 0U);
    append_u16(features, static_cast<std::uint16_t>(feature_lookup_indices.size()));
    for (const std::uint16_t index : feature_lookup_indices) {
        append_u16(features, index);
    }

    Bytes lookup_list(2U + lookups.size() * 2U, 0U);
    set_u16(lookup_list, 0U, static_cast<std::uint16_t>(lookups.size()));
    for (std::size_t index = 0U; index < lookups.size(); ++index) {
        set_u16(
            lookup_list,
            2U + index * 2U,
            static_cast<std::uint16_t>(lookup_list.size())
        );
        lookup_list.insert(lookup_list.end(), lookups[index].begin(), lookups[index].end());
    }

    Bytes result(10U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 4U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), scripts.begin(), scripts.end());
    set_u16(result, 6U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), features.begin(), features.end());
    set_u16(result, 8U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), lookup_list.begin(), lookup_list.end());
    return result;
}

[[nodiscard]] Bytes ligature_substitution(
    const std::uint16_t first,
    const std::uint16_t second,
    const std::uint16_t replacement
) {
    Bytes result(8U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 2U, 8U);
    const Bytes covered = coverage(first);
    result.insert(result.end(), covered.begin(), covered.end());
    set_u16(result, 4U, 1U);
    set_u16(result, 6U, static_cast<std::uint16_t>(result.size()));
    append_u16(result, 1U);
    append_u16(result, 4U);
    append_u16(result, replacement);
    append_u16(result, 2U);
    append_u16(result, second);
    return result;
}

[[nodiscard]] Bytes synthetic_gsub(
    const std::uint16_t first,
    const std::uint16_t second,
    const std::uint16_t replacement
) {
    const std::vector<Bytes> lookups{
        lookup(4U, {ligature_substitution(first, second, replacement)}),
    };

    Bytes scripts;
    append_u16(scripts, 1U);
    scripts.insert(scripts.end(), {'D', 'F', 'L', 'T'});
    append_u16(scripts, 8U);
    append_u16(scripts, 4U);
    append_u16(scripts, 0U);
    append_u16(scripts, 0U);
    append_u16(scripts, 0xFFFFU);
    append_u16(scripts, 1U);
    append_u16(scripts, 0U);

    Bytes features(8U, 0U);
    set_u16(features, 0U, 1U);
    features[2U] = 'l';
    features[3U] = 'i';
    features[4U] = 'g';
    features[5U] = 'a';
    set_u16(features, 6U, 8U);
    append_u16(features, 0U);
    append_u16(features, 1U);
    append_u16(features, 0U);

    Bytes lookup_list(4U, 0U);
    set_u16(lookup_list, 0U, 1U);
    set_u16(lookup_list, 2U, 4U);
    lookup_list.insert(lookup_list.end(), lookups[0].begin(), lookups[0].end());

    Bytes result(10U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 4U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), scripts.begin(), scripts.end());
    set_u16(result, 6U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), features.begin(), features.end());
    set_u16(result, 8U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), lookup_list.begin(), lookup_list.end());
    return result;
}

[[nodiscard]] Bytes synthetic_gpos(
    const std::uint16_t pair_left,
    const std::uint16_t pair_right,
    const std::uint16_t base,
    const std::uint16_t ligature,
    const std::uint16_t mark2,
    const std::uint16_t mark1
) {
    Bytes corrupt_pair(10U, 0U);
    set_u16(corrupt_pair, 0U, 99U);
    const std::vector<Bytes> lookups{
        lookup(2U, {pair_subtable(pair_left, pair_right), corrupt_pair}),
        lookup(4U, {mark_subtable(mark1, base, 200, 500, false)}),
        lookup(5U, {mark_subtable(mark1, ligature, 300, 600, true)}),
        lookup(6U, {mark_subtable(mark1, mark2, 400, 700, false)}),
    };

    Bytes scripts;
    append_u16(scripts, 1U);
    scripts.insert(scripts.end(), {'D', 'F', 'L', 'T'});
    append_u16(scripts, 8U);
    append_u16(scripts, 4U);
    append_u16(scripts, 0U);
    append_u16(scripts, 0U);
    append_u16(scripts, 0xFFFFU);
    append_u16(scripts, 3U);
    append_u16(scripts, 0U);
    append_u16(scripts, 1U);
    append_u16(scripts, 2U);

    Bytes features(20U, 0U);
    set_u16(features, 0U, 3U);
    const std::string tags = "kernmarkmkmk";
    for (std::size_t index = 0U; index < 3U; ++index) {
        for (std::size_t byte = 0U; byte < 4U; ++byte) {
            features[2U + index * 6U + byte] =
                static_cast<std::uint8_t>(tags[index * 4U + byte]);
        }
        set_u16(features, 2U + index * 6U + 4U,
                static_cast<std::uint16_t>(features.size()));
        append_u16(features, 0U);
        if (index == 1U) {
            append_u16(features, 2U);
            append_u16(features, 1U);
            append_u16(features, 2U);
        } else {
            append_u16(features, 1U);
            append_u16(features, index == 0U ? 0U : 3U);
        }
    }

    Bytes lookup_list(2U + lookups.size() * 2U, 0U);
    set_u16(lookup_list, 0U, static_cast<std::uint16_t>(lookups.size()));
    for (std::size_t index = 0U; index < lookups.size(); ++index) {
        set_u16(lookup_list, 2U + index * 2U,
                static_cast<std::uint16_t>(lookup_list.size()));
        lookup_list.insert(lookup_list.end(), lookups[index].begin(), lookups[index].end());
    }

    Bytes result(10U, 0U);
    set_u16(result, 0U, 1U);
    set_u16(result, 4U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), scripts.begin(), scripts.end());
    set_u16(result, 6U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), features.begin(), features.end());
    set_u16(result, 8U, static_cast<std::uint16_t>(result.size()));
    result.insert(result.end(), lookup_list.begin(), lookup_list.end());
    return result;
}

[[nodiscard]] Bytes synthetic_kern(
    const std::uint16_t left,
    const std::uint16_t right
) {
    Bytes result;
    append_u16(result, 0U);
    append_u16(result, 2U);
    append_u16(result, 0U);
    append_u16(result, 20U);
    append_u16(result, 1U);
    append_u16(result, 1U);
    append_u16(result, 6U);
    append_u16(result, 0U);
    append_u16(result, 0U);
    append_u16(result, left);
    append_u16(result, right);
    append_i16(result, -123);
    append_u16(result, 0U);
    append_u16(result, 14U);
    append_u16(result, 1U);
    append_u16(result, 1U);
    append_u16(result, 6U);
    append_u16(result, 0U);
    append_u16(result, 0U);
    return result;
}

[[nodiscard]] strata::runtime::Value object(
    std::initializer_list<std::pair<std::string, strata::runtime::Value>> fields
) {
    return strata::runtime::Value(
        std::vector<std::pair<std::string, strata::runtime::Value>>(fields)
    );
}

[[nodiscard]] std::shared_ptr<const strata::ui::DescriptionNode> text_node(
    std::string type,
    strata::runtime::Value style,
    std::optional<strata::runtime::Value> spans = std::nullopt
) {
    strata::ui::DescriptionNode::Properties properties{
        {"$layout", strata::runtime::ExpressionValue(std::move(style))},
    };
    if (spans.has_value()) {
        properties.emplace("spans", strata::runtime::ExpressionValue(std::move(*spans)));
    }
    return strata::ui::DescriptionNode::create(
        std::move(type),
        std::optional<std::string>("text.fixture"),
        "/text-fixture",
        "screen TextFixture",
        std::move(properties),
        std::make_shared<const strata::ui::EagerDescriptionChildren>(
            std::vector<std::shared_ptr<const strata::ui::DescriptionNode>>{}
        )
    );
}

[[nodiscard]] const strata::ui::RetainedNode& retain(
    strata::ui::RetainedTree& tree,
    std::shared_ptr<const strata::ui::DescriptionNode> description
) {
    static_cast<void>(tree.reconcile(std::move(description)));
    return *tree.root();
}

void test_optional_tables_and_generic_positioning(const std::filesystem::path& resources) {
    const auto load = [&] {
        return strata::resource::load_binary_resource(
            resources,
            strata::resource::ResourceId::parse("assets/strata/fonts/default.ttf")
        );
    };
    const strata::font::OpenTypeFont original = strata::font::OpenTypeFont::parse(load());
    const std::uint16_t pair_left = original.glyph_id('E');
    const std::uint16_t pair_right = original.glyph_id('F');
    const std::uint16_t base = original.glyph_id('A');
    const std::uint16_t ligature = original.glyph_id('C');
    const std::uint16_t mark2 = original.glyph_id('D');
    const std::uint16_t mark1 = original.glyph_id('B');
    check(pair_left != 0U && pair_right != 0U && base != 0U && ligature != 0U &&
              mark2 != 0U && mark1 != 0U,
          "bundled font lost synthetic GPOS glyph fixtures");

    Bytes positioned_bytes = load();
    replace_table(
        positioned_bytes,
        "GPOS",
        "GPOS",
        synthetic_gpos(pair_left, pair_right, base, ligature, mark2, mark1)
    );
    const strata::font::OpenTypeFont positioned =
        strata::font::OpenTypeFont::parse(std::move(positioned_bytes));
    const std::array<std::uint16_t, 2U> pair_glyphs{pair_left, pair_right};
    const auto pair = positioned.position_glyphs(pair_glyphs);
    check(pair[0].x_placement == 11 && pair[0].y_placement == 12 &&
              pair[0].x_advance == -20 && pair[0].y_advance == 30,
          "generic PairPos lost placement or x/y advance values");
    const strata::font::ShapedText pair_shaped = positioned.shape_utf8("EF", 20.0);
    const double pair_scale = 20.0 / static_cast<double>(positioned.units_per_em());
    check(pair_shaped.glyphs.size() == 2U,
          "synthetic PairPos shaping did not retain both glyphs");
    check_near(pair_shaped.glyphs.front().y_advance, 30.0 * pair_scale,
               "PairPos y-advance did not reach shaped glyph data");
    check_near(pair_shaped.glyphs.back().y_placement, 30.0 * pair_scale,
               "PairPos y-advance did not move the following shaped glyph pen");
    strata::ui::TextEngine positioned_engine(strata::ui::TextEngine::FontRegistry{
        {"positioned", positioned},
        {"strata:fonts/default", original},
        {"strata:fonts/default-medium", original},
    });
    strata::ui::RetainedTree positioned_tree;
    const strata::ui::RetainedNode& positioned_node = retain(
        positioned_tree,
        text_node("Text", object({
            {"font", strata::runtime::Value("positioned")},
            {"pixelSize", strata::runtime::Value(20.0)},
        }))
    );
    const strata::ui::TextLayout positioned_layout =
        positioned_engine.layout(positioned_node, "EF");
    check_near(positioned_layout.shaped.glyphs.front().y_advance, 30.0 * pair_scale,
               "generic y-advance did not survive into shared TextLayout glyphs");
    check_near(positioned_layout.shaped.glyphs.back().y_placement, 30.0 * pair_scale,
               "shared TextLayout did not carry the advanced vertical pen");
    const std::array<std::uint16_t, 2U> mark_base_glyphs{base, mark1};
    const auto mark_base = positioned.position_glyphs(mark_base_glyphs);
    check(mark_base[1].x_placement ==
              200 - static_cast<std::int32_t>(positioned.horizontal_advance(base)) - 10 &&
              mark_base[1].y_placement == 480,
          "MarkToBase anchors were not resolved against glyph pen positions");

    Bytes kern_mark_bytes = load();
    replace_table(
        kern_mark_bytes,
        "GPOS",
        "GPOS",
        synthetic_gpos(pair_left, pair_right, base, ligature, mark2, mark1)
    );
    replace_table(kern_mark_bytes, "prep", "kern", synthetic_kern(base, mark1));
    const strata::font::OpenTypeFont kern_mark =
        strata::font::OpenTypeFont::parse(std::move(kern_mark_bytes));
    const auto kern_mark_positioning = kern_mark.position_glyphs(mark_base_glyphs);
    check(kern_mark_positioning[0].x_advance == -123 &&
              kern_mark_positioning[1].x_placement == mark_base[1].x_placement + 123,
          "MarkToBase positioning did not include the preceding legacy kern advance");

    const strata::font::ShapedText mark_shaped = positioned.shape_utf8("AB", 20.0);
    check_near(mark_shaped.glyphs.back().y_placement, 480.0 * pair_scale,
               "MarkToBase anchor positioning did not reach shaped glyph data");

    const strata::font::FontStyleGeometry bold_style =
        strata::font::resolve_font_style_geometry(
            strata::font::font_style_bold, positioned.metadata().style_flags
        );
    const double bold_design_advance = bold_style.bold_strength(positioned.units_per_em());
    const auto bold_mark_base = positioned.position_glyphs(
        mark_base_glyphs, strata::font::font_style_bold
    );
    check_near(
        bold_mark_base[0].x_advance,
        bold_design_advance,
        "synthetic bold advance was not produced by styled font positioning"
    );
    check_near(
        bold_mark_base[1].x_placement,
        mark_base[1].x_placement - bold_design_advance,
        "MarkToBase placement did not see synthetic bold in the preceding pen"
    );

    const strata::font::FontStyleGeometry italic_style =
        strata::font::resolve_font_style_geometry(
            strata::font::font_style_italic, positioned.metadata().style_flags
        );
    const auto italic_pair = positioned.position_glyphs(
        pair_glyphs, strata::font::font_style_italic
    );
    check_near(
        italic_pair[0].x_placement,
        italic_style.transform_x(11.0, 12.0),
        "italic shaping did not shear PairPos placement by its y placement"
    );
    check_near(
        italic_pair[0].x_advance,
        italic_style.transform_x(-20.0, 30.0),
        "italic shaping did not shear PairPos advance by its y advance"
    );
    const auto italic_mark_base = positioned.position_glyphs(
        mark_base_glyphs, strata::font::font_style_italic
    );
    check_near(
        italic_mark_base[1].x_placement,
        italic_style.transform_x(mark_base[1].x_placement, mark_base[1].y_placement),
        "italic MarkToBase placement did not transform the nonzero-y anchor vector"
    );

    const auto styled_attachment_layout = [&](const std::uint32_t style_flags) {
        strata::ui::RetainedTree tree;
        const strata::ui::RetainedNode& node = retain(
            tree,
            text_node("Text", object({
                {"font", strata::runtime::Value("positioned")},
                {"fontStyleFlags", strata::runtime::Value(
                    static_cast<double>(style_flags)
                )},
                {"pixelSize", strata::runtime::Value(20.0)},
            }))
        );
        return positioned_engine.layout(node, "AB");
    };
    const strata::ui::TextLayout bold_attachment = styled_attachment_layout(
        strata::font::font_style_bold
    );
    const strata::ui::TextLayout italic_attachment = styled_attachment_layout(
        strata::font::font_style_italic
    );
    const auto check_styled_attachment = [&](
        const strata::ui::TextLayout& layout,
        const std::uint32_t style_flags,
        const std::string_view label
    ) {
        check(layout.shaped.glyphs.size() == 2U,
              std::string(label) + " attachment layout lost its two live glyphs");
        const strata::font::FontStyleGeometry style =
            strata::font::resolve_font_style_geometry(
                style_flags, positioned.metadata().style_flags
            );
        const strata::font::ShapedGlyph& base_glyph = layout.shaped.glyphs[0];
        const strata::font::ShapedGlyph& mark_glyph = layout.shaped.glyphs[1];
        const double base_anchor_x = base_glyph.x + base_glyph.x_placement +
            style.transform_x(200.0, 500.0) * pair_scale;
        const double mark_anchor_x = mark_glyph.x + mark_glyph.x_placement +
            style.transform_x(10.0, 20.0) * pair_scale;
        const double base_anchor_y = base_glyph.y_placement + 500.0 * pair_scale;
        const double mark_anchor_y = mark_glyph.y_placement + 20.0 * pair_scale;
        check_near(mark_anchor_x, base_anchor_x,
                   std::string(label) + " logical x placement missed its styled GPOS anchor");
        check_near(mark_anchor_y, base_anchor_y,
                   std::string(label) + " logical y placement missed its GPOS anchor");

        const std::string owner = "styled-attachment-" + std::string(label);
        strata::font::GlyphAtlas coverage_atlas(owner + "-coverage");
        strata::font::GlyphAtlas msdf_atlas(owner + "-msdf");
        const auto coverage_base = coverage_atlas.request_coverage(
            "positioned", positioned, base, 20.0, {}, style_flags
        );
        const auto coverage_mark = coverage_atlas.request_coverage(
            "positioned", positioned, mark1, 20.0, {}, style_flags
        );
        const auto msdf_base = msdf_atlas.request(
            "positioned", positioned, base, 20.0, {}, style_flags,
            strata::font::GlyphRasterMode::msdf
        );
        const auto msdf_mark = msdf_atlas.request(
            "positioned", positioned, mark1, 20.0, {}, style_flags,
            strata::font::GlyphRasterMode::msdf
        );
        check(coverage_base.has_value() && coverage_mark.has_value() &&
                  msdf_base.has_value() && msdf_mark.has_value() &&
                  coverage_base->mode == strata::font::GlyphRasterMode::coverage &&
                  msdf_base->mode == strata::font::GlyphRasterMode::msdf,
              std::string(label) + " attachment did not reach both styled raster modes");
        const auto same_plane = [](const strata::font::RasterPlaneBounds& left,
                                   const strata::font::RasterPlaneBounds& right) {
            return left.left == right.left && left.bottom == right.bottom &&
                left.right == right.right && left.top == right.top;
        };
        const auto coverage_bitmap = strata::font::rasterize_coverage(
            positioned, base, 20.0, 1.0, {}, {}, style_flags
        );
        const auto coverage_mark_bitmap = strata::font::rasterize_coverage(
            positioned, mark1, 20.0, 1.0, {}, {}, style_flags
        );
        const auto plain_coverage_bitmap = strata::font::rasterize_coverage(
            positioned, base, 20.0, 1.0
        );
        const auto msdf_bitmap = strata::font::rasterize_msdf(
            positioned, base, 20.0, {}, style_flags
        );
        const auto msdf_mark_bitmap = strata::font::rasterize_msdf(
            positioned, mark1, 20.0, {}, style_flags
        );
        const auto plain_msdf_bitmap = strata::font::rasterize_msdf(
            positioned, base, 20.0
        );
        check(coverage_bitmap.has_value() && coverage_mark_bitmap.has_value() &&
                  plain_coverage_bitmap.has_value() && msdf_bitmap.has_value() &&
                  msdf_mark_bitmap.has_value() && plain_msdf_bitmap.has_value() &&
                  coverage_bitmap->bytes != plain_coverage_bitmap->bytes &&
                  msdf_bitmap->bytes != plain_msdf_bitmap->bytes &&
                  same_plane(
                      coverage_base->plane_bounds_layout_pixels,
                      coverage_bitmap->plane_bounds_layout_pixels
                  ) &&
                  same_plane(
                      msdf_base->plane_bounds_layout_pixels,
                      msdf_bitmap->plane_bounds_layout_pixels
                  ) &&
                  same_plane(
                      coverage_mark->plane_bounds_layout_pixels,
                      coverage_mark_bitmap->plane_bounds_layout_pixels
                  ) &&
                  same_plane(
                      msdf_mark->plane_bounds_layout_pixels,
                      msdf_mark_bitmap->plane_bounds_layout_pixels
                  ),
              std::string(label) +
                  " shaping transform diverged from coverage/MSDF atlas bitmap geometry");
    };
    check_styled_attachment(
        bold_attachment, strata::font::font_style_bold, "bold"
    );
    check_styled_attachment(
        italic_attachment, strata::font::font_style_italic, "italic"
    );
    const std::array<std::uint16_t, 2U> mark_ligature_glyphs{ligature, mark1};
    const auto mark_ligature = positioned.position_glyphs(mark_ligature_glyphs);
    check(mark_ligature[1].y_placement == 580,
          "MarkToLigature lookup dispatch lost its component anchor");
    const std::array<std::uint16_t, 2U> mark_mark_glyphs{mark2, mark1};
    const auto mark_mark = positioned.position_glyphs(mark_mark_glyphs);
    check(mark_mark[1].y_placement == 680,
          "MarkToMark lookup dispatch lost its mark2 anchor");
    check(std::ranges::any_of(
              positioned.optional_diagnostics(),
              [](const std::string& value) { return value.contains("GPOS lookup 0 subtable 1"); }
          ),
          "malformed later GPOS subtable did not produce a recoverable diagnostic");

    Bytes ordered_bytes = load();
    replace_table(
        ordered_bytes,
        "GDEF",
        "GDEF",
        synthetic_gdef(pair_left, pair_right, ligature, mark1, mark2)
    );
    constexpr std::uint16_t mark_filtering_set = 0U;
    replace_table(
        ordered_bytes,
        "GPOS",
        "GPOS",
        gpos_with_lookups({
            lookup(2U, {pair_subtable(pair_left, pair_right)}, 0x0008U),
            lookup(5U, {two_component_mark_ligature_subtable(mark1, ligature)}),
            lookup(
                4U,
                {mark_subtable(mark1, base, 200, 500, false)},
                0x0010U,
                mark_filtering_set
            ),
            lookup(
                4U,
                {mark_subtable(mark2, base, 250, 550, false)},
                0x0100U
            ),
            lookup(
                4U,
                {mark_subtable(mark2, base, 275, 575, false)},
                0x0010U,
                mark_filtering_set
            ),
        })
    );
    const strata::font::OpenTypeFont ordered =
        strata::font::OpenTypeFont::parse(std::move(ordered_bytes));
    const std::array<strata::font::GlyphPositioningInput, 3U> skipped_mark_pair{
        strata::font::GlyphPositioningInput{pair_left, 0U, 0U, 1U},
        strata::font::GlyphPositioningInput{mark1, 1U, 0U, 1U},
        strata::font::GlyphPositioningInput{pair_right, 2U, 0U, 1U},
    };
    const auto skipped_mark_positioning = ordered.position_glyphs(skipped_mark_pair);
    check(skipped_mark_positioning[0].x_placement == 11 &&
              skipped_mark_positioning[0].x_advance == -20,
          "PairPos did not apply sequentially across an IgnoreMarks-filtered glyph");
    const std::array<strata::font::GlyphPositioningInput, 2U> second_component{
        strata::font::GlyphPositioningInput{ligature, 0U, 0U, 2U},
        strata::font::GlyphPositioningInput{mark1, 1U, 1U, 1U},
    };
    const auto component_positioning = ordered.position_glyphs(second_component);
    check(component_positioning[1].y_placement == 780,
          "MarkToLigature ignored the retained ligature component association");
    const std::array<strata::font::GlyphPositioningInput, 2U> included_mark{
        strata::font::GlyphPositioningInput{base, 0U, 0U, 1U},
        strata::font::GlyphPositioningInput{mark1, 1U, 0U, 1U},
    };
    check(ordered.position_glyphs(included_mark)[1].y_placement == 480,
          "a mark admitted by the GDEF filtering set was not positioned");
    const std::array<strata::font::GlyphPositioningInput, 2U> excluded_mark{
        strata::font::GlyphPositioningInput{base, 0U, 0U, 1U},
        strata::font::GlyphPositioningInput{mark2, 1U, 0U, 1U},
    };
    const auto excluded_positioning = ordered.position_glyphs(excluded_mark);
    check(excluded_positioning[1].x_placement == 0 &&
              excluded_positioning[1].y_placement == 0,
          "mark attachment type/filtering-set flags admitted an excluded mark");

    std::vector<Bytes> numeric_lookups(6U, lookup(1U, {}));
    numeric_lookups[2U] = lookup(2U, {pair_subtable(base, mark1)});
    numeric_lookups[5U] = lookup(
        4U, {mark_subtable(mark1, base, 200, 500, false)}
    );
    Bytes numeric_bytes = load();
    replace_table(
        numeric_bytes,
        "GDEF",
        "GDEF",
        synthetic_gdef(pair_left, pair_right, ligature, mark1, mark2)
    );
    replace_table(
        numeric_bytes,
        "GPOS",
        "GPOS",
        gpos_with_lookups(numeric_lookups, {5U, 2U})
    );
    const strata::font::OpenTypeFont numeric =
        strata::font::OpenTypeFont::parse(std::move(numeric_bytes));
    const std::array<std::uint16_t, 2U> numeric_pair{base, mark1};
    const auto numeric_positioning = numeric.position_glyphs(numeric_pair);
    const std::int32_t expected_numeric_mark_x = 221 -
        static_cast<std::int32_t>(numeric.horizontal_advance(base));
    check(numeric_positioning[1].x_placement == expected_numeric_mark_x,
          "active GPOS lookups followed reversed feature indices instead of numeric LookupList order");
    const std::array<std::uint16_t, 3U> blocked_attachment{
        base, pair_left, mark1,
    };
    const auto blocked_positioning = numeric.position_glyphs(blocked_attachment);
    check(blocked_positioning[2].x_placement == 0 &&
              blocked_positioning[2].y_placement == 0,
          "an uncovered eligible predecessor did not block attachment to an earlier base");

    Bytes live_bytes = load();
    replace_table(
        live_bytes,
        "GDEF",
        "GDEF",
        synthetic_gdef(pair_left, pair_right, ligature, mark1, mark2)
    );
    replace_table(
        live_bytes,
        "GSUB",
        "GSUB",
        synthetic_gsub(pair_left, pair_right, ligature)
    );
    replace_table(
        live_bytes,
        "GPOS",
        "GPOS",
        gpos_with_lookups({
            lookup(5U, {two_component_mark_ligature_subtable(mark1, ligature)}),
        })
    );
    const strata::font::OpenTypeFont live =
        strata::font::OpenTypeFont::parse(std::move(live_bytes));
    strata::ui::TextEngine live_engine(strata::ui::TextEngine::FontRegistry{
        {"live", live},
        {"strata:fonts/default", original},
        {"strata:fonts/default-medium", original},
    });
    strata::ui::RetainedTree live_tree;
    const strata::ui::RetainedNode& live_node = retain(
        live_tree,
        text_node("Text", object({
            {"font", strata::runtime::Value("live")},
            {"pixelSize", strata::runtime::Value(20.0)},
        }))
    );
    const strata::ui::TextLayout live_layout = live_engine.layout(live_node, "EFB");
    const double live_scale = 20.0 / static_cast<double>(live.units_per_em());
    check(live_layout.shaped.glyphs.size() == 2U &&
              live_layout.shaped.glyphs[0].glyph_id == ligature &&
              live_layout.shaped.glyphs[0].text_start_offset == 0U &&
              live_layout.shaped.glyphs[0].text_end_offset == 2U &&
              live_layout.shaped.glyphs[1].text_start_offset == 2U &&
              live_layout.shaped.glyphs[1].text_end_offset == 3U,
          "live TextEngine shaping lost GSUB ligature source clusters");
    check_near(
        live_layout.shaped.glyphs[1].y_placement,
        780.0 * live_scale,
        "live GSUB provenance did not select the second MarkToLigature component"
    );
    check(live_layout.shaped.clusters.size() == 2U &&
              live_layout.shaped.clusters[0].text_start_offset == 0U &&
              live_layout.shaped.clusters[0].text_end_offset == 2U &&
              live_layout.resolved_runs.size() == 1U &&
              live_layout.resolved_runs[0].text_end_offset == 3U,
          "live ligature shaping damaged cluster or resolved-run output");

    strata::ui::RetainedTree styled_live_tree;
    const strata::ui::RetainedNode& styled_live_node = retain(
        styled_live_tree,
        text_node("Text", object({
            {"font", strata::runtime::Value("live")},
            {"fontStyleFlags", strata::runtime::Value(3.0)},
            {"pixelSize", strata::runtime::Value(20.0)},
        }))
    );
    const strata::ui::TextLayout styled_live = live_engine.layout(styled_live_node, "EFB");
    const strata::font::FontStyleGeometry bold_italic =
        strata::font::resolve_font_style_geometry(
            strata::font::font_style_bold | strata::font::font_style_italic,
            live.metadata().style_flags
        );
    check(styled_live.shaped.glyphs.size() == 2U &&
              styled_live.shaped.glyphs[0].glyph_id == ligature &&
              styled_live.shaped.glyphs[0].text_end_offset == 2U &&
              styled_live.shaped.glyphs[1].text_start_offset == 2U,
          "styled live shaping damaged GSUB ligature provenance");
    const strata::font::ShapedGlyph& styled_ligature = styled_live.shaped.glyphs[0];
    const strata::font::ShapedGlyph& styled_mark = styled_live.shaped.glyphs[1];
    const double styled_ligature_anchor = styled_ligature.x +
        styled_ligature.x_placement + bold_italic.transform_x(400.0, 800.0) * live_scale;
    const double styled_mark_anchor = styled_mark.x + styled_mark.x_placement +
        bold_italic.transform_x(10.0, 20.0) * live_scale;
    check_near(
        styled_mark_anchor,
        styled_ligature_anchor,
        "bold/italic live MarkToLigature anchor diverged after GSUB shaping"
    );
    check_near(
        styled_mark.y_placement + 20.0 * live_scale,
        styled_ligature.y_placement + 800.0 * live_scale,
        "styled live MarkToLigature y anchor diverged"
    );

    std::uint16_t kern_left = 0U;
    std::uint16_t kern_right = 0U;
    for (std::uint32_t left = 33U; left < 127U && kern_left == 0U; ++left) {
        for (std::uint32_t right = 33U; right < 127U; ++right) {
            const std::uint16_t left_glyph = original.glyph_id(left);
            const std::uint16_t right_glyph = original.glyph_id(right);
            if (left_glyph == 0U || right_glyph == 0U) continue;
            const strata::font::PairPositionAdjustment adjustment =
                original.pair_position_adjustment(left_glyph, right_glyph);
            if (adjustment.first.x_placement == 0 && adjustment.first.y_placement == 0 &&
                adjustment.first.x_advance == 0 && adjustment.first.y_advance == 0 &&
                adjustment.second.x_placement == 0 && adjustment.second.y_placement == 0 &&
                adjustment.second.x_advance == 0 && adjustment.second.y_advance == 0) {
                kern_left = left_glyph;
                kern_right = right_glyph;
                break;
            }
        }
    }
    check(kern_left != 0U, "bundled font has no neutral pair for the kern fixture");
    Bytes kern_bytes = load();
    replace_table(kern_bytes, "prep", "kern", synthetic_kern(kern_left, kern_right));
    const strata::font::OpenTypeFont kern = strata::font::OpenTypeFont::parse(std::move(kern_bytes));
    check(kern.pair_advance_adjustment(kern_left, kern_right) == -123,
          "corrupt later kern subtable erased a valid earlier pair");
    check(std::ranges::any_of(
              kern.optional_diagnostics(),
              [](const std::string& value) { return value.contains("kern subtable 1"); }
          ),
          "corrupt later kern subtable was not diagnosed");

    Bytes name_bytes = load();
    const std::size_t name_record = table_record(name_bytes, "name");
    const std::size_t name_offset = u32(name_bytes, name_record + 8U);
    const std::uint16_t name_count = u16(name_bytes, name_offset + 2U);
    check(name_count > 1U, "bundled font has no later name record to corrupt");
    const std::size_t corrupt_record = name_offset + 6U +
                                       static_cast<std::size_t>(name_count - 1U) * 12U;
    set_u16(name_bytes, corrupt_record + 6U, 1U);
    set_u16(name_bytes, corrupt_record + 8U, 2U);
    set_u16(name_bytes, corrupt_record + 10U, 0xFFFFU);
    const strata::font::OpenTypeFont named = strata::font::OpenTypeFont::parse(std::move(name_bytes));
    check(!named.metadata().family.empty(),
          "corrupt later name record erased valid prior family metadata");
    check(std::ranges::any_of(
              named.optional_diagnostics(),
              [](const std::string& value) { return value.contains("name record"); }
          ),
          "corrupt name record was not isolated or diagnosed");

    Bytes os2_bytes = load();
    const std::size_t os2_record = table_record(os2_bytes, "OS/2");
    set_u32(os2_bytes, os2_record + 12U, 4U);
    const strata::font::OpenTypeFont malformed_os2 =
        strata::font::OpenTypeFont::parse(std::move(os2_bytes));
    check(malformed_os2.glyph_id('A') != 0U &&
              std::ranges::any_of(
                  malformed_os2.optional_diagnostics(),
                  [](const std::string& value) { return value.contains("OS/2"); }
              ),
          "malformed optional OS/2 table rejected or silently damaged a usable face");

    Bytes escaped_name = load();
    const std::size_t escaped_name_record = table_record(escaped_name, "name");
    set_u32(
        escaped_name,
        escaped_name_record + 8U,
        static_cast<std::uint32_t>(escaped_name.size() + 64U)
    );
    set_u32(escaped_name, escaped_name_record + 12U, 8U);
    const strata::font::OpenTypeFont name_outside_file =
        strata::font::OpenTypeFont::parse(std::move(escaped_name));
    check(name_outside_file.glyph_id('A') != 0U &&
              std::ranges::any_of(
                  name_outside_file.optional_diagnostics(),
                  [](const std::string& value) { return value.contains("name metadata"); }
              ),
          "an out-of-file optional directory record rejected a valid mandatory face");

    Bytes escaped_gpos = load();
    Bytes short_gpos(10U, 0U);
    set_u16(short_gpos, 0U, 1U);
    set_u16(short_gpos, 4U, 12U);
    replace_table(escaped_gpos, "GPOS", "GPOS", short_gpos);
    escaped_gpos.resize(escaped_gpos.size() + 32U, 0U);
    const strata::font::OpenTypeFont bounded_gpos =
        strata::font::OpenTypeFont::parse(std::move(escaped_gpos));
    check(bounded_gpos.glyph_id('A') != 0U &&
              std::ranges::any_of(
                  bounded_gpos.optional_diagnostics(),
                  [](const std::string& value) { return value.contains("GPOS"); }
              ),
          "an in-file offset escaped the declared GPOS table slice");

    for (const auto& [tag, declared_length] :
         std::vector<std::pair<std::string_view, std::uint32_t>>{
             {"head", 18U},
             {"hhea", 34U},
             {"maxp", 4U},
             {"hmtx", 2U},
             {"cmap", 4U},
         }) {
        Bytes short_required = load();
        const std::size_t record = table_record(short_required, tag);
        const std::size_t offset = u32(short_required, record + 8U);
        check(offset + declared_length < short_required.size(),
              "mandatory-table fixture has no following unrelated bytes");
        set_u32(short_required, record + 12U, declared_length);
        bool rejected = false;
        try {
            static_cast<void>(strata::font::OpenTypeFont::parse(
                std::move(short_required)
            ));
        } catch (const strata::font::FontError&) {
            rejected = true;
        }
        check(rejected,
              "short declared mandatory table consumed bytes from the following table");
    }
}

void test_layout_runs_fallback_and_soft_wrap(const std::filesystem::path& resources) {
    using namespace strata;
    const Bytes regular_bytes = resource::load_binary_resource(
        resources, resource::ResourceId::parse("assets/strata/fonts/default.ttf")
    );
    const font::OpenTypeFont regular = font::OpenTypeFont::parse(regular_bytes);
    Bytes intrinsic_bold_bytes = regular_bytes;
    const std::size_t os2 = u32(
        intrinsic_bold_bytes,
        table_record(intrinsic_bold_bytes, "OS/2") + 8U
    );
    set_u16(
        intrinsic_bold_bytes,
        os2 + 62U,
        static_cast<std::uint16_t>((u16(intrinsic_bold_bytes, os2 + 62U) & ~0x0021U) | 0x0020U)
    );
    const font::OpenTypeFont intrinsic_bold = font::OpenTypeFont::parse(
        std::move(intrinsic_bold_bytes)
    );
    check(intrinsic_bold.metadata().style_flags == font::font_style_bold,
          "native OS/2 style metadata diverged from frozen FontStyleFlags bits");
    const font::OpenTypeFont medium = font::OpenTypeFont::parse(
        resource::load_binary_resource(
            resources, resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
        )
    );
    const font::OpenTypeFont mono = font::OpenTypeFont::parse(
        resource::load_binary_resource(
            resources, resource::ResourceId::parse("assets/strata/fonts/mono.ttf")
        )
    );
    ui::TextEngine engine(ui::TextEngine::FontRegistry{
        {"explicit", medium},
        {"family-bold", intrinsic_bold},
        {"family-regular", regular},
        {"primary", mono},
        {"strata:fonts/default", regular},
        {"strata:fonts/default-medium", medium},
    });

    ui::RetainedTree default_tree;
    const ui::RetainedNode& default_node = retain(
        default_tree,
        text_node("Text", object({{"pixelSize", runtime::Value(12.0)}}))
    );
    check(engine.layout(default_node, "A").glyph_font_ids ==
              std::vector<std::string>{"strata:fonts/default"},
          "unstyled text did not resolve to the Regular default face");

    ui::RetainedTree fallback_tree;
    const ui::RetainedNode& fallback_node = retain(fallback_tree, text_node("Text", object({
        {"fallbackFonts", runtime::Value(std::vector<runtime::Value>{runtime::Value("explicit")})},
        {"font", runtime::Value("missing-primary")},
        {"pixelSize", runtime::Value(12.0)},
    })));
    const ui::TextLayout fallback = engine.layout(fallback_node, "A");
    check(fallback.glyph_font_ids == std::vector<std::string>{"explicit"} &&
              fallback.resolved_runs.size() == 1U &&
              fallback.resolved_runs.front().font_rasterization ==
                  ui::FontRasterization::grayscale,
          "default text did not retain fallback resolution and grayscale rasterization");

    ui::RetainedTree primary_tree;
    const ui::RetainedNode& primary_node = retain(primary_tree, text_node("Text", object({
        {"fallbackFonts", runtime::Value(std::vector<runtime::Value>{runtime::Value("explicit")})},
        {"font", runtime::Value("primary")},
        {"pixelSize", runtime::Value(12.0)},
    })));
    check(engine.layout(primary_node, "A").glyph_font_ids ==
              std::vector<std::string>{"primary"},
          "fallback resolution did not preserve primary-first ordering");
    check(mono.glyph_id(0x0132U) == 0U && medium.glyph_id(0x0132U) != 0U &&
              mono.glyph_id(0x2026U) != 0U && medium.glyph_id(0x2026U) != 0U,
          "bundled fonts lost the fallback ellipsis fixtures");
    ui::TextLayoutOptions fallback_ellipsis_options;
    fallback_ellipsis_options.wrap_width = 1.0;
    fallback_ellipsis_options.wrap_mode = "NONE";
    fallback_ellipsis_options.overflow = "ELLIPSIS";
    const ui::TextLayout fallback_ellipsis = engine.layout(
        primary_node,
        "\xC4\xB2",
        fallback_ellipsis_options
    );
    check(!fallback_ellipsis.glyph_font_ids.empty() &&
              fallback_ellipsis.glyph_font_ids.back() == "explicit",
          "ellipsis did not retain the resolved fallback face of the truncated text");

    const runtime::Value spans(std::vector<runtime::Value>{
        object({
            {"style", object({
                {"font", runtime::Value("primary")},
                {"letterSpacing", runtime::Value(0.0)},
                {"pixelSize", runtime::Value(10.0)},
            })},
            {"text", runtime::Value("\xF0\x9F\x98\x80" "A")},
        }),
        object({
            {"style", object({
                {"fallbackFonts", runtime::Value(std::vector<runtime::Value>{
                    runtime::Value("explicit"), runtime::Value("explicit"),
                })},
                {"font", runtime::Value("missing-span-primary")},
                {"fontRasterization", runtime::Value("MSDF")},
                {"letterSpacing", runtime::Value(4.0)},
                {"pixelSize", runtime::Value(20.0)},
            })},
            {"text", runtime::Value("V")},
        }),
    });
    ui::RetainedTree rich_tree;
    const ui::RetainedNode& rich_node = retain(
        rich_tree, text_node("RichText", object({}), spans)
    );
    const ui::TextLayout rich = engine.layout(rich_node, "\xF0\x9F\x98\x80" "AV");
    const auto latin_a = std::ranges::find(rich.shaped.glyphs, static_cast<std::uint32_t>('A'),
                                           &font::ShapedGlyph::code_point);
    const auto latin_v = std::ranges::find(rich.shaped.glyphs, static_cast<std::uint32_t>('V'),
                                           &font::ShapedGlyph::code_point);
    check(latin_a != rich.shaped.glyphs.end() && latin_v != rich.shaped.glyphs.end() &&
              latin_a->text_start_offset == 2U && latin_v->text_start_offset == 3U,
          "rich run boundaries lost canonical UTF-16 offsets around a non-BMP code point");
    check_near(
        latin_v->x - latin_a->x,
        static_cast<double>(mono.horizontal_advance(mono.glyph_id('A'))) * 10.0 /
            static_cast<double>(mono.units_per_em()),
        "pair shaping or letter spacing crossed a full authored style boundary"
    );
    check(rich.resolved_runs.size() >= 2U &&
              rich.resolved_runs[rich.resolved_runs.size() - 2U].style_identity !=
                  rich.resolved_runs.back().style_identity &&
              rich.resolved_runs[rich.resolved_runs.size() - 2U].font_rasterization ==
                  ui::FontRasterization::grayscale &&
              rich.resolved_runs.back().font_id == "explicit" &&
              rich.resolved_runs.back().pixel_size == 20.0 &&
              rich.resolved_runs.back().font_rasterization == ui::FontRasterization::msdf,
          "resolved rich runs lost style identity, fallback face, mixed metrics, or raster mode");

    const runtime::Value flag_spans(std::vector<runtime::Value>{
        object({
            {"style", object({{"fontStyleFlags", runtime::Value(1.0)}})},
            {"text", runtime::Value("A")},
        }),
        object({
            {"style", object({{"fontStyleFlags", runtime::Value(2.0)}})},
            {"text", runtime::Value("A")},
        }),
    });
    ui::RetainedTree flag_tree;
    const ui::RetainedNode& flag_node = retain(
        flag_tree, text_node("RichText", object({}), flag_spans)
    );
    const ui::TextLayout flagged = engine.layout(flag_node, "AA");
    check(flagged.resolved_runs.size() == 2U &&
              flagged.resolved_runs[0].font_style_flags == 1U &&
              flagged.resolved_runs[1].font_style_flags == 2U &&
              flagged.shaped.glyphs[0].font_style_flags == 1U &&
              flagged.shaped.glyphs[1].font_style_flags == 2U,
          "bold/italic-only rich spans collapsed into one resolved run");

    ui::RetainedTree family_style_tree;
    const ui::RetainedNode& family_style_node = retain(
        family_style_tree,
        text_node("Text", object({
            {"fallbackFonts", runtime::Value(std::vector<runtime::Value>{
                runtime::Value("family-bold"),
            })},
            {"font", runtime::Value("family-regular")},
            {"fontStyleFlags", runtime::Value(1.0)},
            {"pixelSize", runtime::Value(24.0)},
        }))
    );
    const ui::TextLayout family_styled = engine.layout(family_style_node, "A");
    check(family_styled.glyph_font_ids == std::vector<std::string>{"family-bold"} &&
              family_styled.resolved_runs.size() == 1U &&
              family_styled.resolved_runs.front().font_id == "family-bold" &&
              family_styled.resolved_runs.front().font_style_flags == font::font_style_bold,
          "authored family fallback did not resolve the canonical intrinsic bold face");
    const double intrinsic_bold_advance =
        static_cast<double>(intrinsic_bold.horizontal_advance(intrinsic_bold.glyph_id('A'))) *
        24.0 / static_cast<double>(intrinsic_bold.units_per_em());
    check_near(
        family_styled.shaped.metrics.width,
        intrinsic_bold_advance,
        "intrinsic styled-face layout incorrectly retained synthetic bold advance"
    );

    ui::RetainedTree synthetic_plain_tree;
    const ui::RetainedNode& synthetic_plain_node = retain(
        synthetic_plain_tree,
        text_node("Text", object({
            {"font", runtime::Value("primary")},
            {"pixelSize", runtime::Value(24.0)},
        }))
    );
    ui::RetainedTree synthetic_bold_tree;
    const ui::RetainedNode& synthetic_bold_node = retain(
        synthetic_bold_tree,
        text_node("Text", object({
            {"font", runtime::Value("primary")},
            {"fontStyleFlags", runtime::Value(1.0)},
            {"pixelSize", runtime::Value(24.0)},
        }))
    );
    ui::RetainedTree synthetic_italic_tree;
    const ui::RetainedNode& synthetic_italic_node = retain(
        synthetic_italic_tree,
        text_node("Text", object({
            {"font", runtime::Value("primary")},
            {"fontStyleFlags", runtime::Value(2.0)},
            {"pixelSize", runtime::Value(24.0)},
        }))
    );
    const double synthetic_plain_width = engine.layout(synthetic_plain_node, "A").shaped.metrics.width;
    const double synthetic_bold_width = engine.layout(synthetic_bold_node, "A").shaped.metrics.width;
    const double synthetic_italic_width = engine.layout(synthetic_italic_node, "A").shaped.metrics.width;
    check_near(
        synthetic_bold_width - synthetic_plain_width,
        24.0 * font::synthetic_bold_em_fraction,
        "synthetic bold raster expansion and layout advance use different geometry"
    );
    check_near(
        synthetic_italic_width,
        synthetic_plain_width,
        "baseline-anchored synthetic italic unexpectedly changed pen advance"
    );

    const runtime::Value newline_spans(std::vector<runtime::Value>{
        object({
            {"style", object({
                {"fontStyleFlags", runtime::Value(1.0)},
                {"lineHeight", runtime::Value(17.0)},
                {"pixelSize", runtime::Value(10.0)},
            })},
            {"text", runtime::Value("\n")},
        }),
        object({
            {"style", object({
                {"fontStyleFlags", runtime::Value(2.0)},
                {"lineHeight", runtime::Value(31.0)},
                {"pixelSize", runtime::Value(20.0)},
            })},
            {"text", runtime::Value("\n")},
        }),
    });
    ui::RetainedTree newline_tree;
    const ui::RetainedNode& newline_node = retain(
        newline_tree, text_node("RichText", object({}), newline_spans)
    );
    const ui::TextLayout blank_lines = engine.layout(newline_node, "\n\n");
    check(blank_lines.lines.size() == 3U,
          "consecutive rich-text newlines did not retain all blank lines");
    check_near(blank_lines.lines[0].height, 17.0,
               "first styled newline used node-default line metrics");
    check_near(blank_lines.lines[1].height, 31.0,
               "second styled newline used node-default line metrics");

    ui::RetainedTree cache_tree_one;
    const ui::RetainedNode& cache_node_one = retain(
        cache_tree_one,
        text_node("Text", object({{"fontStyleFlags", runtime::Value(64.0)}}))
    );
    ui::RetainedTree cache_tree_two;
    const ui::RetainedNode& cache_node_two = retain(
        cache_tree_two,
        text_node("Text", object({{"fontStyleFlags", runtime::Value(128.0)}}))
    );
    const std::size_t misses_before = engine.operation_counters().cache_misses;
    static_cast<void>(engine.layout(cache_node_one, "A"));
    static_cast<void>(engine.layout(cache_node_two, "A"));
    check(engine.operation_counters().cache_misses == misses_before + 2U,
          "fontStyleFlags did not participate in text layout cache identity");

    font::CoverageRasterConfig style_coverage;
    style_coverage.hinting = font::GlyphHinting::none;
    const std::optional<font::GlyphRasterBitmap> plain_coverage = font::rasterize_coverage(
        regular, regular.glyph_id('A'), 48.0, 1.0, {}, style_coverage
    );
    const std::optional<font::GlyphRasterBitmap> bold_coverage = font::rasterize_coverage(
        regular, regular.glyph_id('A'), 48.0, 1.0, {}, style_coverage, font::font_style_bold
    );
    const std::optional<font::GlyphRasterBitmap> italic_coverage = font::rasterize_coverage(
        regular, regular.glyph_id('A'), 48.0, 1.0, {}, style_coverage, font::font_style_italic
    );
    check(plain_coverage.has_value() && bold_coverage.has_value() && italic_coverage.has_value() &&
              bold_coverage->bytes != plain_coverage->bytes &&
              italic_coverage->bytes != plain_coverage->bytes &&
              bold_coverage->plane_bounds_layout_pixels.bottom ==
                  plain_coverage->plane_bounds_layout_pixels.bottom &&
              bold_coverage->plane_bounds_layout_pixels.top ==
                  plain_coverage->plane_bounds_layout_pixels.top &&
              bold_coverage->plane_bounds_layout_pixels.right -
                      bold_coverage->plane_bounds_layout_pixels.left >
                  plain_coverage->plane_bounds_layout_pixels.right -
                      plain_coverage->plane_bounds_layout_pixels.left,
          "coverage style flags did not produce distinct bold/italic bitmap geometry");
    const std::optional<font::GlyphRasterBitmap> plain_msdf = font::rasterize_msdf(
        regular, regular.glyph_id('A'), 48.0
    );
    const std::optional<font::GlyphRasterBitmap> bold_msdf = font::rasterize_msdf(
        regular, regular.glyph_id('A'), 48.0, {}, font::font_style_bold
    );
    const std::optional<font::GlyphRasterBitmap> italic_msdf = font::rasterize_msdf(
        regular, regular.glyph_id('A'), 48.0, {}, font::font_style_italic
    );
    check(plain_msdf.has_value() && bold_msdf.has_value() && italic_msdf.has_value() &&
              bold_msdf->bytes != plain_msdf->bytes && italic_msdf->bytes != plain_msdf->bytes &&
              bold_msdf->plane_bounds_layout_pixels.bottom ==
                  plain_msdf->plane_bounds_layout_pixels.bottom &&
              bold_msdf->plane_bounds_layout_pixels.top ==
                  plain_msdf->plane_bounds_layout_pixels.top,
          "MSDF style flags did not produce distinct bold/italic distance geometry");

    font::GlyphAtlas atlas("text-font-style-flags");
    const std::uint16_t atlas_glyph = regular.glyph_id('A');
    const std::optional<font::GlyphAtlasEntry> atlas_plain = atlas.request_coverage(
        "strata:fonts/default", regular, atlas_glyph, 48.0, {}
    );
    const std::optional<font::GlyphAtlasEntry> atlas_bold = atlas.request_coverage(
        "strata:fonts/default", regular, atlas_glyph, 48.0, {}, 1U
    );
    const std::optional<font::GlyphAtlasEntry> atlas_italic = atlas.request_coverage(
        "strata:fonts/default", regular, atlas_glyph, 48.0, {}, 2U
    );
    check(atlas_plain.has_value() && atlas_bold.has_value() && atlas_italic.has_value() &&
              atlas.request_coverage(
                  "strata:fonts/default", regular, atlas_glyph, 48.0, {}, 1U
              ).has_value() &&
              atlas.cached_glyph_count() == 3U,
          "realized bold/italic glyphs collapsed or missed their stable atlas identity");
    check(
        atlas_bold->plane_bounds_layout_pixels.right -
                atlas_bold->plane_bounds_layout_pixels.left >
            atlas_plain->plane_bounds_layout_pixels.right -
                atlas_plain->plane_bounds_layout_pixels.left,
        "atlas style identity changed without carrying styled bitmap geometry"
    );

    font::GlyphAtlas intrinsic_atlas("text-font-intrinsic-style");
    const std::uint16_t intrinsic_glyph = intrinsic_bold.glyph_id('A');
    check(intrinsic_atlas.request_coverage(
              "family-bold", intrinsic_bold, intrinsic_glyph, 48.0, {}, 0U
          ).has_value() &&
              intrinsic_atlas.request_coverage(
                  "family-bold", intrinsic_bold, intrinsic_glyph, 48.0, {},
                  font::font_style_bold
              ).has_value() &&
              intrinsic_atlas.cached_glyph_count() == 1U,
          "intrinsic bold face duplicated an already-realized atlas entry");

    ui::RetainedTree measure_tree;
    const ui::RetainedNode& measure_node = retain(measure_tree, text_node("Text", object({
        {"font", runtime::Value("primary")}, {"pixelSize", runtime::Value(12.0)},
    })));
    const double word_width = engine.layout(measure_node, "AA").shaped.metrics.width;
    const double space_width = engine.layout(measure_node, " ").shaped.metrics.width;
    const double wrap_width = word_width + space_width * 2.0;
    for (const std::string& alignment : {std::string("CENTER"), std::string("END")}) {
        ui::RetainedTree wrap_tree;
        const ui::RetainedNode& wrap_node = retain(wrap_tree, text_node("Text", object({
            {"alignment", runtime::Value(alignment)},
            {"font", runtime::Value("primary")},
            {"pixelSize", runtime::Value(12.0)},
            {"wrapMode", runtime::Value("WORD")},
            {"wrapWidth", runtime::Value(wrap_width)},
        })));
        const std::string source = "AA   AA";
        const ui::TextLayout first = engine.layout(wrap_node, source);
        const ui::TextLayout second = engine.layout(wrap_node, source);
        check(first.shares_storage_with(second),
              "warm text layout cloned geometry instead of sharing its immutable cache result");
        check(first.lines.size() == 2U &&
                  first.lines.front().text_end_offset == 2U &&
                  first.lines.front().soft_wrap_gap_start_offset ==
                      std::optional<std::size_t>(2U) &&
                  first.lines.front().soft_wrap_gap_end_offset ==
                      std::optional<std::size_t>(5U),
              "soft word wrap did not retain the trimmed source-offset gap centrally");
        check_near(first.lines.front().width, word_width,
                   "soft-wrap whitespace still corrupted visual line width");
        const double expected_x = alignment == "CENTER"
            ? (wrap_width - word_width) * 0.5 : wrap_width - word_width;
        check_near(first.lines.front().x, expected_x,
                   "trimmed break whitespace still corrupted center/end alignment");
        check(ui::text_layout_hit_offset(
                  first, ui::Point{wrap_width + 10.0, first.lines.front().height * 0.5}
              ) == 2U,
              "soft-wrap hit testing lost the trimmed whitespace source gap");
        const ui::Rect caret = ui::text_layout_caret_rect(
            first, ui::Point{}, source, 3U
        );
        check_near(caret.x, expected_x + word_width,
                   "caret geometry assigned soft-wrap whitespace visual width");
        const std::vector<ui::Rect> selection = ui::text_layout_selection_rects(
            first, ui::Point{}, 2U, 5U
        );
        check(selection.size() == 1U && selection.front().width == 0.0,
              "selection geometry lost the zero-width soft-wrap source gap");
    }
}

void test_logical_glyph_json_default_absence() {
    using namespace strata;
    ui::LogicalGlyph default_glyph;
    default_glyph.font_id = "fixture:regular";
    default_glyph.glyph_id = 7U;
    const data::JsonValue default_json = ui::render_command_json(ui::TextRunRenderCommand{
        {}, {}, 12.0, {default_glyph},
    });
    const data::JsonValue* default_glyphs = default_json.find("glyphs");
    check(default_glyphs != nullptr && default_glyphs->array() != nullptr &&
              default_glyphs->array()->size() == 1U &&
              default_json.find("fontRasterization") != nullptr &&
              default_json.find("fontRasterization")->string() != nullptr &&
              *default_json.find("fontRasterization")->string() == "GRAYSCALE",
          "logical text JSON lost its glyph array or default grayscale mode");
    const data::JsonValue& default_value = default_glyphs->array()->front();
    check(default_value.find("fontStyleFlags") == nullptr &&
              default_value.find("yAdvance") == nullptr,
          "frozen logical glyph JSON emitted zero-valued v1 extension fields");
    check(default_value.find("xPlacement") != nullptr &&
              default_value.find("xPlacement")->number() != nullptr &&
              *default_value.find("xPlacement")->number() == 0.0 &&
              default_value.find("yPlacement") != nullptr &&
              default_value.find("yPlacement")->number() != nullptr &&
              *default_value.find("yPlacement")->number() == 0.0,
          "frozen-present zero placement fields were accidentally omitted");

    ui::LogicalGlyph extended_glyph = default_glyph;
    extended_glyph.font_style_flags = 0x8000'0003U;
    extended_glyph.y_advance = -2.25;
    const data::JsonValue extended_json = ui::render_command_json(ui::TextRunRenderCommand{
        {}, {}, 12.0, {extended_glyph}, ui::FontRasterization::msdf,
    });
    const data::JsonValue& extended_value = extended_json.find("glyphs")->array()->front();
    check(extended_value.find("fontStyleFlags") != nullptr &&
              extended_value.find("fontStyleFlags")->integer() != nullptr &&
              *extended_value.find("fontStyleFlags")->integer() ==
                  static_cast<std::int64_t>(extended_glyph.font_style_flags) &&
              extended_value.find("yAdvance") != nullptr &&
              extended_value.find("yAdvance")->number() != nullptr &&
              *extended_value.find("yAdvance")->number() == -2.25 &&
              extended_json.find("fontRasterization") != nullptr &&
              *extended_json.find("fontRasterization")->string() == "MSDF",
          "logical glyph JSON did not retain exact extension values or explicit MSDF mode");
}

} // namespace

int main(const int argument_count, const char* const* arguments) {
    try {
        if (argument_count != 2) throw std::runtime_error("expected registry resource path");
        const std::filesystem::path resources =
            std::filesystem::path(arguments[1]).parent_path().parent_path();
        test_logical_glyph_json_default_absence();
        test_optional_tables_and_generic_positioning(resources);
        test_layout_runs_fallback_and_soft_wrap(resources);
        std::cout << "text/font residual tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
