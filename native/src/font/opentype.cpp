#include "font/opentype.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/utf8.hpp"
#include "font/truetype_outline.hpp"

namespace strata::font {
namespace {

class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    void require(const std::size_t offset, const std::size_t count) const {
        if (offset > bytes_.size() || count > bytes_.size() - offset) {
            throw FontError("OpenType table range exceeds the font bytes");
        }
    }

    [[nodiscard]] std::uint16_t u16(const std::size_t offset) const {
        require(offset, 2U);
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes_[offset]) << 8U |
            static_cast<std::uint16_t>(bytes_[offset + 1U])
        );
    }

    [[nodiscard]] std::int16_t i16(const std::size_t offset) const {
        return std::bit_cast<std::int16_t>(u16(offset));
    }

    [[nodiscard]] std::uint32_t u32(const std::size_t offset) const {
        require(offset, 4U);
        return static_cast<std::uint32_t>(
            static_cast<std::uint32_t>(bytes_[offset]) << 24U |
            static_cast<std::uint32_t>(bytes_[offset + 1U]) << 16U |
            static_cast<std::uint32_t>(bytes_[offset + 2U]) << 8U |
            static_cast<std::uint32_t>(bytes_[offset + 3U])
        );
    }

    [[nodiscard]] std::string tag(const std::size_t offset) const {
        require(offset, 4U);
        return std::string{
            static_cast<char>(bytes_[offset]),
            static_cast<char>(bytes_[offset + 1U]),
            static_cast<char>(bytes_[offset + 2U]),
            static_cast<char>(bytes_[offset + 3U]),
        };
    }

    [[nodiscard]] std::string raw_string(
        const std::size_t offset,
        const std::size_t length
    ) const {
        require(offset, length);
        return std::string(
            reinterpret_cast<const char*>(bytes_.data() + offset), length
        );
    }

    [[nodiscard]] std::span<const std::uint8_t> slice(
        const std::size_t offset,
        const std::size_t length
    ) const {
        require(offset, length);
        return bytes_.subspan(offset, length);
    }

private:
    std::span<const std::uint8_t> bytes_;
};

struct Table final {
    std::size_t offset = 0U;
    std::size_t length = 0U;
};

[[nodiscard]] std::uint32_t pair_key(
    const std::uint16_t left,
    const std::uint16_t right
) noexcept {
    return static_cast<std::uint32_t>(left) << 16U | static_cast<std::uint32_t>(right);
}

[[nodiscard]] std::size_t checked_add(
    const std::size_t base,
    const std::uint32_t relative,
    const Reader& reader
) {
    const std::size_t converted = static_cast<std::size_t>(relative);
    if (converted > reader.size() - std::min(base, reader.size())) {
        throw FontError("OpenType relative offset exceeds the font bytes");
    }
    return base + converted;
}

struct Cmap12Group final {
    std::uint32_t first = 0U;
    std::uint32_t last = 0U;
    std::uint32_t start_glyph = 0U;
};

struct Cmap4 final {
    std::size_t table_end = 0U;
    std::vector<std::uint8_t> bytes;
    std::vector<std::uint16_t> ends;
    std::vector<std::uint16_t> starts;
    std::vector<std::int16_t> deltas;
    std::vector<std::uint16_t> range_offsets;
    std::vector<std::size_t> range_word_offsets;
};

struct ValueAdjustment final {
    std::int16_t x_placement = 0;
    std::int16_t y_placement = 0;
    std::int16_t x_advance = 0;
    std::int16_t y_advance = 0;

    [[nodiscard]] bool zero() const noexcept {
        return x_placement == 0 && y_placement == 0 &&
               x_advance == 0 && y_advance == 0;
    }
};

struct PairAdjustment final {
    ValueAdjustment first;
    ValueAdjustment second;

    [[nodiscard]] bool zero() const noexcept { return first.zero() && second.zero(); }
};

struct GlyphRange final {
    std::uint16_t first = 0U;
    std::uint16_t last = 0U;
    std::uint16_t value = 0U;
};

[[nodiscard]] const GlyphRange* containing_range(
    const std::vector<GlyphRange>& ranges,
    const std::uint16_t glyph
) noexcept {
    const auto upper = std::ranges::upper_bound(ranges, glyph, {}, &GlyphRange::first);
    if (upper == ranges.begin()) return nullptr;
    const GlyphRange& candidate = *std::prev(upper);
    return glyph <= candidate.last ? &candidate : nullptr;
}

struct ClassDefinition final {
    std::vector<GlyphRange> ranges;

    [[nodiscard]] std::uint16_t value(const std::uint16_t glyph) const noexcept {
        const GlyphRange* const range = containing_range(ranges, glyph);
        return range != nullptr ? range->value : 0U;
    }
};

struct ClassPairRule final {
    std::vector<GlyphRange> coverage;
    ClassDefinition first_classes;
    ClassDefinition second_classes;
    std::uint16_t first_class_count = 0U;
    std::uint16_t second_class_count = 0U;
    std::map<std::uint32_t, PairAdjustment> adjustments;

    [[nodiscard]] const PairAdjustment* find(
        const std::uint16_t left,
        const std::uint16_t right
    ) const noexcept {
        if (containing_range(coverage, left) == nullptr) return nullptr;
        const std::uint16_t first_class = first_classes.value(left);
        const std::uint16_t second_class = second_classes.value(right);
        if (first_class >= first_class_count || second_class >= second_class_count) {
            return nullptr;
        }
        const auto found = adjustments.find(pair_key(first_class, second_class));
        if (found != adjustments.end()) return &found->second;
        static constexpr PairAdjustment zero{};
        return &zero;
    }
};

struct Anchor final {
    std::int16_t x = 0;
    std::int16_t y = 0;
};

struct MarkRecord final {
    std::uint16_t glyph = 0U;
    std::uint16_t mark_class = 0U;
    Anchor anchor;
};

enum class MarkAttachmentKind : std::uint8_t {
    base,
    ligature,
    mark,
};

struct MarkAttachmentRule final {
    MarkAttachmentKind kind = MarkAttachmentKind::base;
    std::map<std::uint16_t, MarkRecord> marks;
    /** Base/mark2 anchors indexed by mark class. */
    std::map<std::uint16_t, std::vector<std::optional<Anchor>>> anchors;
    /** Ligature anchors indexed first by component and then by mark class. */
    std::map<
        std::uint16_t,
        std::vector<std::vector<std::optional<Anchor>>>
    > ligature_anchors;
};

struct ParsedPairSubtable final {
    std::map<std::uint32_t, PairAdjustment> pairs;
    std::optional<ClassPairRule> class_rule;

    [[nodiscard]] const PairAdjustment* find(
        const std::uint16_t left,
        const std::uint16_t right
    ) const noexcept {
        const auto explicit_pair = pairs.find(pair_key(left, right));
        if (explicit_pair != pairs.end()) return &explicit_pair->second;
        return class_rule.has_value() ? class_rule->find(left, right) : nullptr;
    }
};

struct PositioningSubtable final {
    std::optional<ParsedPairSubtable> pair;
    std::optional<MarkAttachmentRule> mark;
};

struct LookupFilter final {
    std::uint16_t flags = 0U;
    std::optional<std::uint16_t> mark_filtering_set;
};

struct PositioningLookup final {
    LookupFilter filter;
    std::vector<PositioningSubtable> subtables;
};

struct LigatureSubstitutionRule final {
    std::uint16_t replacement = 0U;
    std::vector<std::uint16_t> components;
};

struct LigatureSubstitutionLookup final {
    LookupFilter filter;
    std::map<std::uint16_t, std::vector<LigatureSubstitutionRule>> rules;
};

struct GlyphRunState final {
    std::uint16_t glyph_id = 0U;
    std::size_t source_start = 0U;
    std::size_t source_end = 0U;
    std::vector<std::size_t> component_clusters;
    std::uint16_t ligature_component_count = 1U;
};

[[nodiscard]] double saturated_design_add(
    const double left,
    const double right
) noexcept {
    return std::clamp(
        left + right,
        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
        static_cast<double>(std::numeric_limits<std::int32_t>::max())
    );
}

[[nodiscard]] std::vector<GlyphRange> parse_coverage(
    const Reader& reader,
    const std::size_t offset
) {
    const std::uint16_t format = reader.u16(offset);
    const std::uint16_t count = reader.u16(offset + 2U);
    std::vector<GlyphRange> ranges;
    ranges.reserve(count);
    if (format == 1U) {
        reader.require(offset + 4U, static_cast<std::size_t>(count) * 2U);
        std::optional<std::uint16_t> previous;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::uint16_t glyph = reader.u16(offset + 4U + index * 2U);
            if (previous.has_value() && glyph <= *previous) {
                throw FontError("OpenType coverage glyphs are not strictly ordered");
            }
            ranges.push_back(GlyphRange{glyph, glyph, 0U});
            previous = glyph;
        }
        return ranges;
    }
    if (format == 2U) {
        reader.require(offset + 4U, static_cast<std::size_t>(count) * 6U);
        std::optional<std::uint16_t> previous_last;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = offset + 4U + index * 6U;
            const std::uint16_t first = reader.u16(record);
            const std::uint16_t last = reader.u16(record + 2U);
            if (first > last ||
                (previous_last.has_value() && first <= *previous_last)) {
                throw FontError("OpenType coverage ranges are invalid or unordered");
            }
            ranges.push_back(GlyphRange{first, last, 0U});
            previous_last = last;
        }
        return ranges;
    }
    throw FontError("unsupported OpenType coverage format");
}

[[nodiscard]] std::vector<std::uint16_t> expand_coverage(
    const std::vector<GlyphRange>& ranges
) {
    std::vector<std::uint16_t> glyphs;
    for (const GlyphRange& range : ranges) {
        const std::size_t count = static_cast<std::size_t>(range.last) - range.first + 1U;
        if (glyphs.size() > 65'536U - count) {
            throw FontError("OpenType coverage expands beyond the glyph id space");
        }
        for (std::uint32_t glyph = range.first; glyph <= range.last; ++glyph) {
            glyphs.push_back(static_cast<std::uint16_t>(glyph));
        }
    }
    return glyphs;
}

[[nodiscard]] ClassDefinition parse_class_definition(
    const Reader& reader,
    const std::size_t offset
) {
    ClassDefinition result;
    const std::uint16_t format = reader.u16(offset);
    if (format == 1U) {
        const std::uint16_t first = reader.u16(offset + 2U);
        const std::uint16_t count = reader.u16(offset + 4U);
        reader.require(offset + 6U, static_cast<std::size_t>(count) * 2U);
        result.ranges.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const std::uint32_t glyph = static_cast<std::uint32_t>(first) +
                                        static_cast<std::uint32_t>(index);
            if (glyph > std::numeric_limits<std::uint16_t>::max()) {
                throw FontError("OpenType class definition glyph exceeds uint16");
            }
            result.ranges.push_back(GlyphRange{
                static_cast<std::uint16_t>(glyph),
                static_cast<std::uint16_t>(glyph),
                reader.u16(offset + 6U + index * 2U),
            });
        }
        return result;
    }
    if (format == 2U) {
        const std::uint16_t count = reader.u16(offset + 2U);
        reader.require(offset + 4U, static_cast<std::size_t>(count) * 6U);
        result.ranges.reserve(count);
        std::optional<std::uint16_t> previous_last;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = offset + 4U + index * 6U;
            const std::uint16_t first = reader.u16(record);
            const std::uint16_t last = reader.u16(record + 2U);
            if (first > last ||
                (previous_last.has_value() && first <= *previous_last)) {
                throw FontError("OpenType class ranges are invalid or unordered");
            }
            result.ranges.push_back(GlyphRange{first, last, reader.u16(record + 4U)});
            previous_last = last;
        }
        return result;
    }
    throw FontError("unsupported OpenType class definition format");
}

[[nodiscard]] std::size_t value_record_size(const std::uint16_t format) noexcept {
    return static_cast<std::size_t>(std::popcount(static_cast<unsigned int>(format & 0x00FFU))) * 2U;
}

[[nodiscard]] ValueAdjustment parse_value_record(
    const Reader& reader,
    const std::size_t offset,
    const std::uint16_t format
) {
    reader.require(offset, value_record_size(format));
    ValueAdjustment result;
    std::size_t cursor = offset;
    if ((format & 0x0001U) != 0U) { result.x_placement = reader.i16(cursor); cursor += 2U; }
    if ((format & 0x0002U) != 0U) { result.y_placement = reader.i16(cursor); cursor += 2U; }
    if ((format & 0x0004U) != 0U) { result.x_advance = reader.i16(cursor); cursor += 2U; }
    if ((format & 0x0008U) != 0U) { result.y_advance = reader.i16(cursor); }
    return result;
}

[[nodiscard]] std::uint32_t next_utf8(
    const std::string_view text,
    std::size_t& offset
) noexcept {
    const auto byte = [&text](const std::size_t index) {
        return static_cast<std::uint8_t>(text[index]);
    };
    const std::uint8_t first = byte(offset++);
    if (first < 0x80U) return first;
    if (first < 0xE0U) {
        const std::uint32_t result = static_cast<std::uint32_t>(first & 0x1FU) << 6U |
                                     static_cast<std::uint32_t>(byte(offset) & 0x3FU);
        ++offset;
        return result;
    }
    if (first < 0xF0U) {
        const std::uint32_t result = static_cast<std::uint32_t>(first & 0x0FU) << 12U |
                                     static_cast<std::uint32_t>(byte(offset) & 0x3FU) << 6U |
                                     static_cast<std::uint32_t>(byte(offset + 1U) & 0x3FU);
        offset += 2U;
        return result;
    }
    const std::uint32_t result = static_cast<std::uint32_t>(first & 0x07U) << 18U |
                                 static_cast<std::uint32_t>(byte(offset) & 0x3FU) << 12U |
                                 static_cast<std::uint32_t>(byte(offset + 1U) & 0x3FU) << 6U |
                                 static_cast<std::uint32_t>(byte(offset + 2U) & 0x3FU);
    offset += 3U;
    return result;
}

void append_utf8(std::string& output, const std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | code_point >> 6U));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | code_point >> 12U));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | code_point >> 18U));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

} // namespace

struct OpenTypeFont::Impl final {
    explicit Impl(resource::ResourceBytes input)
        : font_bytes(std::move(input)), font_reader(font_bytes) {
        parse_directory();
        parse_metrics();
        parse_cmap();
        try {
            parse_gdef();
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "GDEF positioning metadata ignored: " + std::string(error.what())
            );
        }
        try {
            parse_gsub();
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "GSUB directory ignored after previously valid lookups: " +
                std::string(error.what())
            );
        }
        try {
            parse_gpos();
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "GPOS directory ignored after previously valid lookups: " +
                std::string(error.what())
            );
        }
        try {
            parse_kern();
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "kern directory ignored after previously valid subtables: " +
                std::string(error.what())
            );
        }
        try {
            parse_os2_metadata();
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "OS/2 metadata ignored: " + std::string(error.what())
            );
        }
        try {
            parse_name_metadata();
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "name metadata directory ignored after previously valid records: " +
                std::string(error.what())
            );
        }
        if (tables.contains("glyf") || tables.contains("loca")) {
            outlines = std::make_unique<TrueTypeOutlineSource>(font_bytes, glyphs);
        }
    }

    resource::ResourceBytes font_bytes;
    Reader font_reader;
    std::map<std::string, Table, std::less<>> tables;
    std::uint16_t units = 0U;
    std::int16_t ascent = 0;
    std::int16_t descent = 0;
    std::int16_t gap = 0;
    std::uint16_t glyphs = 0U;
    std::vector<std::uint16_t> advances;
    std::vector<Cmap12Group> cmap12;
    std::optional<Cmap4> cmap4;
    std::vector<PositioningLookup> positioning_lookups;
    std::vector<LigatureSubstitutionLookup> substitution_lookups;
    ClassDefinition glyph_classes;
    ClassDefinition mark_attachment_classes;
    std::vector<std::vector<GlyphRange>> mark_filtering_sets;
    std::map<std::uint32_t, std::int16_t> kern_pairs;
    FontMetadata metadata;
    std::vector<std::string> optional_diagnostics;
    std::unique_ptr<TrueTypeOutlineSource> outlines;

    [[nodiscard]] const Table& table(const std::string_view name) const {
        const auto found = tables.find(name);
        if (found == tables.end()) throw FontError("required OpenType table is missing: " + std::string(name));
        font_reader.require(found->second.offset, found->second.length);
        return found->second;
    }

    [[nodiscard]] Reader required_table_reader(const std::string_view name) const {
        const Table& required = table(name);
        return Reader(std::span<const std::uint8_t>(font_bytes).subspan(
            required.offset, required.length
        ));
    }

    [[nodiscard]] std::optional<Reader> optional_table_reader(
        const std::string_view name
    ) const {
        const auto found = tables.find(name);
        if (found == tables.end()) return std::nullopt;
        font_reader.require(found->second.offset, found->second.length);
        return Reader(std::span<const std::uint8_t>(font_bytes).subspan(
            found->second.offset, found->second.length
        ));
    }

    void parse_directory() {
        font_reader.require(0U, 12U);
        const std::uint32_t version = font_reader.u32(0U);
        if (version != 0x00010000U && version != 0x4F54544FU) {
            throw FontError("unsupported sfnt scaler type");
        }
        const std::uint16_t count = font_reader.u16(4U);
        font_reader.require(12U, static_cast<std::size_t>(count) * 16U);
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = 12U + index * 16U;
            const std::string name = font_reader.tag(record);
            const std::size_t offset = static_cast<std::size_t>(font_reader.u32(record + 8U));
            const std::size_t length = static_cast<std::size_t>(font_reader.u32(record + 12U));
            if (!tables.emplace(name, Table{offset, length}).second) {
                throw FontError("duplicate OpenType table: " + name);
            }
        }
    }

    void parse_metrics() {
        const Reader head = required_table_reader("head");
        head.require(0U, 54U);
        units = head.u16(18U);
        if (units < 16U || units > 16'384U) throw FontError("invalid OpenType unitsPerEm");

        const Reader hhea = required_table_reader("hhea");
        hhea.require(0U, 36U);
        ascent = hhea.i16(4U);
        descent = hhea.i16(6U);
        gap = hhea.i16(8U);
        const std::uint16_t metric_count = hhea.u16(34U);

        try {
            if (const std::optional<Reader> os2 = optional_table_reader("OS/2");
                os2.has_value()) {
                if (os2->size() < 74U) {
                    throw FontError("table is shorter than 74 bytes");
                }
                if ((os2->u16(62U) & 0x0080U) != 0U) {
                    ascent = os2->i16(68U);
                    descent = os2->i16(70U);
                    gap = os2->i16(72U);
                }
            }
        } catch (const std::exception& error) {
            optional_diagnostics.push_back(
                "OS/2 typographic metrics ignored: " + std::string(error.what())
            );
        }

        const Reader maxp = required_table_reader("maxp");
        maxp.require(0U, 6U);
        glyphs = maxp.u16(4U);
        if (glyphs == 0U || metric_count == 0U || metric_count > glyphs) {
            throw FontError("invalid OpenType horizontal metric counts");
        }
        const Reader hmtx = required_table_reader("hmtx");
        const std::size_t required = static_cast<std::size_t>(metric_count) * 4U +
                                     static_cast<std::size_t>(glyphs - metric_count) * 2U;
        hmtx.require(0U, required);
        advances.resize(glyphs);
        for (std::size_t index = 0U; index < metric_count; ++index) {
            advances[index] = hmtx.u16(index * 4U);
        }
        std::fill(advances.begin() + metric_count, advances.end(), advances[metric_count - 1U]);
    }

    void parse_cmap() {
        const Reader cmap_table = required_table_reader("cmap");
        cmap_table.require(0U, 4U);
        const std::uint16_t count = cmap_table.u16(2U);
        cmap_table.require(4U, static_cast<std::size_t>(count) * 8U);
        std::optional<std::size_t> selected12;
        std::optional<std::size_t> selected4;
        int rank12 = -1;
        int rank4 = -1;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = 4U + index * 8U;
            const std::uint16_t platform = cmap_table.u16(record);
            const std::uint16_t encoding = cmap_table.u16(record + 2U);
            const std::size_t subtable = checked_add(
                0U, cmap_table.u32(record + 4U), cmap_table
            );
            const std::uint16_t format = cmap_table.u16(subtable);
            const int rank = platform == 3U && encoding == 10U ? 3 :
                             platform == 0U ? 2 :
                             platform == 3U && encoding == 1U ? 1 : 0;
            if (format == 12U && rank > rank12) { selected12 = subtable; rank12 = rank; }
            if (format == 4U && rank > rank4) { selected4 = subtable; rank4 = rank; }
        }
        if (selected12.has_value()) {
            cmap_table.require(*selected12, 8U);
            const std::size_t length = static_cast<std::size_t>(
                cmap_table.u32(*selected12 + 4U)
            );
            if (length < 16U) throw FontError("cmap format 12 is shorter than its header");
            const Reader format12_table(cmap_table.slice(*selected12, length));
            const std::uint32_t count32 = format12_table.u32(12U);
            if (count32 > (length - 16U) / 12U) throw FontError("invalid cmap format 12 group count");
            cmap12.reserve(static_cast<std::size_t>(count32));
            for (std::size_t index = 0U; index < count32; ++index) {
                const std::size_t group = 16U + index * 12U;
                const Cmap12Group value{
                    format12_table.u32(group),
                    format12_table.u32(group + 4U),
                    format12_table.u32(group + 8U),
                };
                if (value.first > value.last || value.last > 0x10FFFFU) {
                    throw FontError("invalid cmap format 12 group");
                }
                cmap12.push_back(value);
            }
        }
        if (selected4.has_value()) {
            const std::size_t offset = *selected4;
            cmap_table.require(offset, 4U);
            const std::size_t length = cmap_table.u16(offset + 2U);
            if (length < 16U) throw FontError("cmap format 4 is shorter than its header");
            const Reader format4_table(cmap_table.slice(offset, length));
            const std::uint16_t seg_count_x2 = format4_table.u16(6U);
            if (seg_count_x2 == 0U || (seg_count_x2 & 1U) != 0U) {
                throw FontError("invalid cmap format 4 segment count");
            }
            const std::size_t segments = seg_count_x2 / 2U;
            const std::size_t ends = 14U;
            const std::size_t starts = ends + segments * 2U + 2U;
            const std::size_t deltas = starts + segments * 2U;
            const std::size_t offsets = deltas + segments * 2U;
            format4_table.require(offsets, segments * 2U);
            Cmap4 value;
            value.table_end = length;
            const std::span<const std::uint8_t> format4_bytes =
                format4_table.slice(0U, length);
            value.bytes.assign(format4_bytes.begin(), format4_bytes.end());
            value.ends.reserve(segments);
            value.starts.reserve(segments);
            value.deltas.reserve(segments);
            value.range_offsets.reserve(segments);
            value.range_word_offsets.reserve(segments);
            for (std::size_t index = 0U; index < segments; ++index) {
                value.ends.push_back(format4_table.u16(ends + index * 2U));
                value.starts.push_back(format4_table.u16(starts + index * 2U));
                value.deltas.push_back(format4_table.i16(deltas + index * 2U));
                value.range_offsets.push_back(format4_table.u16(offsets + index * 2U));
                value.range_word_offsets.push_back(offsets + index * 2U);
            }
            cmap4 = std::move(value);
        }
        if (cmap12.empty() && !cmap4.has_value()) throw FontError("font has no supported Unicode cmap");
    }

    void parse_gdef() {
        const std::optional<Reader> table_reader = optional_table_reader("GDEF");
        if (!table_reader.has_value()) return;
        const Reader& reader = *table_reader;
        reader.require(0U, 12U);
        const std::uint16_t major = reader.u16(0U);
        const std::uint16_t minor = reader.u16(2U);
        if (major != 1U) return;
        const std::uint16_t glyph_class_offset = reader.u16(4U);
        const std::uint16_t mark_attach_offset = reader.u16(10U);
        if (glyph_class_offset != 0U) {
            glyph_classes = parse_class_definition(reader, glyph_class_offset);
        }
        if (mark_attach_offset != 0U) {
            mark_attachment_classes = parse_class_definition(reader, mark_attach_offset);
        }
        if (minor < 2U) return;
        reader.require(0U, 14U);
        const std::uint16_t sets_offset = reader.u16(12U);
        if (sets_offset == 0U) return;
        reader.require(sets_offset, 4U);
        if (reader.u16(sets_offset) != 1U) {
            throw FontError("unsupported GDEF MarkGlyphSets format");
        }
        const std::uint16_t count = reader.u16(sets_offset + 2U);
        reader.require(sets_offset + 4U, static_cast<std::size_t>(count) * 4U);
        mark_filtering_sets.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t coverage = checked_add(
                sets_offset,
                reader.u32(sets_offset + 4U + index * 4U),
                reader
            );
            mark_filtering_sets.push_back(parse_coverage(reader, coverage));
        }
    }

    [[nodiscard]] std::vector<std::uint16_t> lang_features(
        const Reader& reader,
        const std::size_t offset
    ) const {
        reader.require(offset, 6U);
        std::vector<std::uint16_t> result;
        const auto append = [&result](const std::uint16_t value) {
            if (!std::ranges::contains(result, value)) result.push_back(value);
        };
        const std::uint16_t required_index = reader.u16(offset + 2U);
        if (required_index != 0xFFFFU) append(required_index);
        const std::uint16_t count = reader.u16(offset + 4U);
        reader.require(offset + 6U, static_cast<std::size_t>(count) * 2U);
        for (std::size_t index = 0U; index < count; ++index) {
            append(reader.u16(offset + 6U + index * 2U));
        }
        return result;
    }

    void append_script_features(
        const Reader& reader,
        const std::size_t script,
        std::vector<std::uint16_t>& feature_indices
    ) const {
        const auto append = [&feature_indices](const std::vector<std::uint16_t>& selected) {
            for (const std::uint16_t value : selected) {
                if (!std::ranges::contains(feature_indices, value)) {
                    feature_indices.push_back(value);
                }
            }
        };
        reader.require(script, 4U);
        const std::uint16_t default_offset = reader.u16(script);
        if (default_offset != 0U) append(lang_features(reader, script + default_offset));
        const std::uint16_t count = reader.u16(script + 2U);
        reader.require(script + 4U, static_cast<std::size_t>(count) * 6U);
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = script + 4U + index * 6U;
            const std::string name = reader.tag(record);
            if (name != "dflt" && name != "ENG ") continue;
            append(lang_features(reader, script + reader.u16(record + 4U)));
        }
    }

    [[nodiscard]] Anchor parse_anchor(
        const Reader& reader,
        const std::size_t offset
    ) const {
        reader.require(offset, 6U);
        const std::uint16_t format = reader.u16(offset);
        if (format < 1U || format > 3U) throw FontError("unsupported GPOS anchor format");
        return Anchor{reader.i16(offset + 2U), reader.i16(offset + 4U)};
    }

    [[nodiscard]] std::map<std::uint16_t, MarkRecord> parse_mark_array(
        const Reader& reader,
        const std::size_t offset,
        const std::vector<std::uint16_t>& coverage,
        const std::uint16_t class_count
    ) const {
        const std::uint16_t count = reader.u16(offset);
        reader.require(offset + 2U, static_cast<std::size_t>(count) * 4U);
        if (count > coverage.size()) throw FontError("GPOS MarkArray exceeds its coverage");
        std::map<std::uint16_t, MarkRecord> result;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = offset + 2U + index * 4U;
            const std::uint16_t mark_class = reader.u16(record);
            if (mark_class >= class_count) throw FontError("GPOS mark class exceeds class count");
            const std::uint16_t anchor_offset = reader.u16(record + 2U);
            if (anchor_offset == 0U) throw FontError("GPOS mark record has no anchor");
            const std::uint16_t glyph = coverage[index];
            result.insert_or_assign(glyph, MarkRecord{
                glyph,
                mark_class,
                parse_anchor(reader, offset + anchor_offset),
            });
        }
        return result;
    }

    [[nodiscard]] std::vector<std::optional<Anchor>> parse_anchor_record(
        const Reader& reader,
        const std::size_t record,
        const std::size_t relative_base,
        const std::uint16_t class_count
    ) const {
        reader.require(record, static_cast<std::size_t>(class_count) * 2U);
        std::vector<std::optional<Anchor>> result(class_count);
        for (std::size_t mark_class = 0U; mark_class < class_count; ++mark_class) {
            const std::uint16_t anchor_offset = reader.u16(record + mark_class * 2U);
            if (anchor_offset != 0U) {
                result[mark_class] = parse_anchor(reader, relative_base + anchor_offset);
            }
        }
        return result;
    }

    [[nodiscard]] ParsedPairSubtable parse_pair_subtable(
        const Reader& reader,
        const std::size_t offset
    ) const {
        reader.require(offset, 10U);
        ParsedPairSubtable result;
        const std::uint16_t format = reader.u16(offset);
        const std::uint16_t first_format = reader.u16(offset + 4U);
        const std::uint16_t second_format = reader.u16(offset + 6U);
        if (format == 1U) {
            const auto coverage = expand_coverage(parse_coverage(reader, offset + reader.u16(offset + 2U)));
            const std::uint16_t count = reader.u16(offset + 8U);
            reader.require(offset + 10U, static_cast<std::size_t>(count) * 2U);
            for (std::size_t index = 0U; index < count; ++index) {
                if (index >= coverage.size()) break;
                const std::size_t pair_set = offset + reader.u16(offset + 10U + index * 2U);
                const std::uint16_t pair_count = reader.u16(pair_set);
                std::size_t cursor = pair_set + 2U;
                for (std::size_t pair_index = 0U; pair_index < pair_count; ++pair_index) {
                    const std::uint16_t right = reader.u16(cursor);
                    cursor += 2U;
                    const ValueAdjustment first = parse_value_record(reader, cursor, first_format);
                    cursor += value_record_size(first_format);
                    const ValueAdjustment second = parse_value_record(reader, cursor, second_format);
                    cursor += value_record_size(second_format);
                    const PairAdjustment adjustment{first, second};
                    result.pairs.insert_or_assign(pair_key(coverage[index], right), adjustment);
                }
            }
            return result;
        }
        if (format == 2U) {
            reader.require(offset, 16U);
            ClassPairRule rule;
            rule.coverage = parse_coverage(reader, offset + reader.u16(offset + 2U));
            rule.first_classes = parse_class_definition(reader, offset + reader.u16(offset + 8U));
            rule.second_classes = parse_class_definition(reader, offset + reader.u16(offset + 10U));
            const std::uint16_t first_count = reader.u16(offset + 12U);
            const std::uint16_t second_count = reader.u16(offset + 14U);
            rule.first_class_count = first_count;
            rule.second_class_count = second_count;
            std::size_t cursor = offset + 16U;
            for (std::size_t first_class = 0U; first_class < first_count; ++first_class) {
                for (std::size_t second_class = 0U; second_class < second_count; ++second_class) {
                    const ValueAdjustment first = parse_value_record(reader, cursor, first_format);
                    cursor += value_record_size(first_format);
                    const ValueAdjustment second = parse_value_record(reader, cursor, second_format);
                    cursor += value_record_size(second_format);
                    const PairAdjustment adjustment{first, second};
                    if (!adjustment.zero()) {
                        if (first_class > 0xFFFFU || second_class > 0xFFFFU) {
                            throw FontError("OpenType class id exceeds uint16");
                        }
                        rule.adjustments.emplace(
                            pair_key(
                                static_cast<std::uint16_t>(first_class),
                                static_cast<std::uint16_t>(second_class)
                            ),
                            adjustment
                        );
                    }
                }
            }
            result.class_rule = std::move(rule);
            return result;
        }
        throw FontError("unsupported GPOS PairPos format");
    }

    [[nodiscard]] MarkAttachmentRule parse_mark_subtable(
        const Reader& reader,
        const std::size_t offset,
        const MarkAttachmentKind kind
    ) const {
        reader.require(offset, 12U);
        if (reader.u16(offset) != 1U) throw FontError("unsupported GPOS mark format");
        const auto mark_coverage = expand_coverage(parse_coverage(
            reader, offset + reader.u16(offset + 2U)
        ));
        const auto base_coverage = expand_coverage(parse_coverage(
            reader, offset + reader.u16(offset + 4U)
        ));
        const std::uint16_t class_count = reader.u16(offset + 6U);
        if (class_count == 0U) throw FontError("GPOS mark lookup has no classes");
        const std::size_t mark_array = offset + reader.u16(offset + 8U);
        const std::size_t base_array = offset + reader.u16(offset + 10U);
        MarkAttachmentRule result;
        result.kind = kind;
        result.marks = parse_mark_array(reader, mark_array, mark_coverage, class_count);

        const std::uint16_t base_count = reader.u16(base_array);
        if (base_count > base_coverage.size()) {
            throw FontError("GPOS attachment array exceeds its coverage");
        }
        if (kind != MarkAttachmentKind::ligature) {
            const std::size_t record_size = static_cast<std::size_t>(class_count) * 2U;
            reader.require(base_array + 2U, static_cast<std::size_t>(base_count) * record_size);
            for (std::size_t index = 0U; index < base_count; ++index) {
                result.anchors.insert_or_assign(
                    base_coverage[index],
                    parse_anchor_record(
                        reader, base_array + 2U + index * record_size, base_array, class_count
                    )
                );
            }
            return result;
        }

        reader.require(base_array + 2U, static_cast<std::size_t>(base_count) * 2U);
        for (std::size_t index = 0U; index < base_count; ++index) {
            const std::uint16_t attach_offset = reader.u16(base_array + 2U + index * 2U);
            if (attach_offset == 0U) continue;
            const std::size_t attach = base_array + attach_offset;
            const std::uint16_t component_count = reader.u16(attach);
            const std::size_t component_size = static_cast<std::size_t>(class_count) * 2U;
            reader.require(attach + 2U, static_cast<std::size_t>(component_count) * component_size);
            std::vector<std::vector<std::optional<Anchor>>> components;
            components.reserve(component_count);
            for (std::size_t component = 0U; component < component_count; ++component) {
                components.push_back(parse_anchor_record(
                    reader, attach + 2U + component * component_size, attach, class_count
                ));
            }
            result.ligature_anchors.insert_or_assign(
                base_coverage[index], std::move(components)
            );
        }
        return result;
    }

    [[nodiscard]] std::optional<PositioningSubtable> parse_positioning_subtable(
        const Reader& reader,
        const std::uint16_t type,
        const std::size_t subtable
    ) const {
        PositioningSubtable result;
        if (type == 2U) {
            result.pair = parse_pair_subtable(reader, subtable);
        } else if (type == 4U) {
            result.mark = parse_mark_subtable(reader, subtable, MarkAttachmentKind::base);
        } else if (type == 5U) {
            result.mark = parse_mark_subtable(reader, subtable, MarkAttachmentKind::ligature);
        } else if (type == 6U) {
            result.mark = parse_mark_subtable(reader, subtable, MarkAttachmentKind::mark);
        } else {
            return std::nullopt;
        }
        return result;
    }

    [[nodiscard]] std::optional<PositioningLookup> parse_lookup(
        const Reader& reader,
        const std::size_t lookup,
        const std::uint16_t lookup_index
    ) {
        reader.require(lookup, 6U);
        const std::uint16_t type = reader.u16(lookup);
        PositioningLookup result;
        result.filter.flags = reader.u16(lookup + 2U);
        const std::uint16_t count = reader.u16(lookup + 4U);
        reader.require(lookup + 6U, static_cast<std::size_t>(count) * 2U);
        if ((result.filter.flags & 0x0010U) != 0U) {
            result.filter.mark_filtering_set = reader.u16(
                lookup + 6U + static_cast<std::size_t>(count) * 2U
            );
            if (*result.filter.mark_filtering_set >= mark_filtering_sets.size()) {
                throw FontError("GPOS lookup mark filtering set is absent from GDEF");
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t subtable = lookup + reader.u16(lookup + 6U + index * 2U);
            try {
                std::uint16_t resolved_type = type;
                std::size_t resolved_subtable = subtable;
                if (type == 9U) {
                    reader.require(subtable, 8U);
                    if (reader.u16(subtable) != 1U) {
                        throw FontError("unsupported GPOS extension format");
                    }
                    resolved_type = reader.u16(subtable + 2U);
                    resolved_subtable = checked_add(
                        subtable, reader.u32(subtable + 4U), reader
                    );
                }
                if (auto parsed = parse_positioning_subtable(
                        reader, resolved_type, resolved_subtable
                    ); parsed.has_value()) {
                    result.subtables.push_back(std::move(*parsed));
                }
            } catch (const std::exception& error) {
                optional_diagnostics.push_back(
                    "GPOS lookup " + std::to_string(lookup_index) + " subtable " +
                    std::to_string(index) + " ignored: " + error.what()
                );
            }
        }
        return result.subtables.empty()
            ? std::nullopt
            : std::optional<PositioningLookup>(std::move(result));
    }

    void parse_gpos() {
        const std::optional<Reader> table_reader = optional_table_reader("GPOS");
        if (!table_reader.has_value()) return;
        const Reader& reader = *table_reader;
        constexpr std::size_t base = 0U;
        reader.require(base, 10U);
        if (reader.u16(base) != 1U) return;
        const std::size_t scripts = base + reader.u16(base + 4U);
        const std::size_t features = base + reader.u16(base + 6U);
        const std::size_t lookups = base + reader.u16(base + 8U);

        std::vector<std::uint16_t> feature_indices;
        const std::uint16_t script_count = reader.u16(scripts);
        reader.require(scripts + 2U, static_cast<std::size_t>(script_count) * 6U);
        for (std::size_t index = 0U; index < script_count; ++index) {
            const std::size_t record = scripts + 2U + index * 6U;
            append_script_features(
                reader, scripts + reader.u16(record + 4U), feature_indices
            );
        }

        struct Feature final { std::string tag; std::vector<std::uint16_t> lookups; };
        const std::uint16_t feature_count = reader.u16(features);
        reader.require(features + 2U, static_cast<std::size_t>(feature_count) * 6U);
        std::vector<Feature> feature_records;
        feature_records.reserve(feature_count);
        for (std::size_t index = 0U; index < feature_count; ++index) {
            const std::size_t record = features + 2U + index * 6U;
            const std::size_t feature = features + reader.u16(record + 4U);
            reader.require(feature, 4U);
            const std::uint16_t count = reader.u16(feature + 2U);
            reader.require(feature + 4U, static_cast<std::size_t>(count) * 2U);
            Feature value{reader.tag(record), {}};
            value.lookups.reserve(count);
            for (std::size_t lookup_index = 0U; lookup_index < count; ++lookup_index) {
                value.lookups.push_back(reader.u16(feature + 4U + lookup_index * 2U));
            }
            feature_records.push_back(std::move(value));
        }

        const std::uint16_t lookup_count = reader.u16(lookups);
        reader.require(lookups + 2U, static_cast<std::size_t>(lookup_count) * 2U);
        std::vector<bool> active_lookups(lookup_count, false);
        for (const std::uint16_t feature_index : feature_indices) {
            if (feature_index >= feature_records.size()) continue;
            const Feature& feature = feature_records[feature_index];
            if (feature.tag != "kern" && feature.tag != "mark" && feature.tag != "mkmk") {
                continue;
            }
            for (const std::uint16_t lookup_index : feature.lookups) {
                if (lookup_index < lookup_count) active_lookups[lookup_index] = true;
            }
        }
        for (std::size_t lookup_index = 0U; lookup_index < lookup_count; ++lookup_index) {
            if (!active_lookups[lookup_index]) continue;
            try {
                if (auto lookup = parse_lookup(
                    reader,
                    lookups + reader.u16(lookups + 2U + lookup_index * 2U),
                    static_cast<std::uint16_t>(lookup_index)
                ); lookup.has_value()) {
                    positioning_lookups.push_back(std::move(*lookup));
                }
            } catch (const std::exception& error) {
                optional_diagnostics.push_back(
                    "GPOS lookup " + std::to_string(lookup_index) +
                    " ignored: " + error.what()
                );
            }
        }
    }

    [[nodiscard]] std::map<std::uint16_t, std::vector<LigatureSubstitutionRule>>
    parse_ligature_subtable(
        const Reader& reader,
        const std::size_t offset
    ) const {
        reader.require(offset, 6U);
        if (reader.u16(offset) != 1U) {
            throw FontError("unsupported GSUB LigatureSubst format");
        }
        const std::vector<std::uint16_t> coverage = expand_coverage(parse_coverage(
            reader, offset + reader.u16(offset + 2U)
        ));
        const std::uint16_t set_count = reader.u16(offset + 4U);
        reader.require(offset + 6U, static_cast<std::size_t>(set_count) * 2U);
        if (set_count > coverage.size()) {
            throw FontError("GSUB LigatureSet count exceeds coverage");
        }
        std::map<std::uint16_t, std::vector<LigatureSubstitutionRule>> result;
        for (std::size_t set_index = 0U; set_index < set_count; ++set_index) {
            const std::size_t set = offset + reader.u16(offset + 6U + set_index * 2U);
            const std::uint16_t ligature_count = reader.u16(set);
            reader.require(set + 2U, static_cast<std::size_t>(ligature_count) * 2U);
            std::vector<LigatureSubstitutionRule>& rules = result[coverage[set_index]];
            rules.reserve(rules.size() + ligature_count);
            for (std::size_t index = 0U; index < ligature_count; ++index) {
                const std::size_t ligature = set + reader.u16(set + 2U + index * 2U);
                reader.require(ligature, 4U);
                const std::uint16_t component_count = reader.u16(ligature + 2U);
                if (component_count < 2U) {
                    throw FontError("GSUB ligature has fewer than two components");
                }
                reader.require(
                    ligature + 4U,
                    static_cast<std::size_t>(component_count - 1U) * 2U
                );
                LigatureSubstitutionRule rule;
                rule.replacement = reader.u16(ligature);
                rule.components.reserve(component_count - 1U);
                for (std::size_t component = 1U; component < component_count; ++component) {
                    rule.components.push_back(reader.u16(
                        ligature + 4U + (component - 1U) * 2U
                    ));
                }
                rules.push_back(std::move(rule));
            }
        }
        return result;
    }

    [[nodiscard]] std::optional<LigatureSubstitutionLookup> parse_gsub_lookup(
        const Reader& reader,
        const std::size_t lookup,
        const std::uint16_t lookup_index
    ) {
        reader.require(lookup, 6U);
        const std::uint16_t type = reader.u16(lookup);
        LigatureSubstitutionLookup result;
        result.filter.flags = reader.u16(lookup + 2U);
        const std::uint16_t count = reader.u16(lookup + 4U);
        reader.require(lookup + 6U, static_cast<std::size_t>(count) * 2U);
        if ((result.filter.flags & 0x0010U) != 0U) {
            result.filter.mark_filtering_set = reader.u16(
                lookup + 6U + static_cast<std::size_t>(count) * 2U
            );
            if (*result.filter.mark_filtering_set >= mark_filtering_sets.size()) {
                throw FontError("GSUB lookup mark filtering set is absent from GDEF");
            }
        }
        for (std::size_t index = 0U; index < count; ++index) {
            try {
                std::uint16_t resolved_type = type;
                std::size_t subtable = lookup + reader.u16(
                    lookup + 6U + index * 2U
                );
                if (type == 7U) {
                    reader.require(subtable, 8U);
                    if (reader.u16(subtable) != 1U) {
                        throw FontError("unsupported GSUB extension format");
                    }
                    resolved_type = reader.u16(subtable + 2U);
                    subtable = checked_add(subtable, reader.u32(subtable + 4U), reader);
                }
                if (resolved_type != 4U) continue;
                auto parsed = parse_ligature_subtable(reader, subtable);
                for (auto& [first, rules] : parsed) {
                    std::vector<LigatureSubstitutionRule>& target = result.rules[first];
                    target.insert(
                        target.end(),
                        std::make_move_iterator(rules.begin()),
                        std::make_move_iterator(rules.end())
                    );
                }
            } catch (const std::exception& error) {
                optional_diagnostics.push_back(
                    "GSUB lookup " + std::to_string(lookup_index) + " subtable " +
                    std::to_string(index) + " ignored: " + error.what()
                );
            }
        }
        return result.rules.empty()
            ? std::nullopt
            : std::optional<LigatureSubstitutionLookup>(std::move(result));
    }

    void parse_gsub() {
        const std::optional<Reader> table_reader = optional_table_reader("GSUB");
        if (!table_reader.has_value()) return;
        const Reader& reader = *table_reader;
        reader.require(0U, 10U);
        if (reader.u16(0U) != 1U) return;
        const std::size_t scripts = reader.u16(4U);
        const std::size_t features = reader.u16(6U);
        const std::size_t lookups = reader.u16(8U);

        std::vector<std::uint16_t> feature_indices;
        const std::uint16_t script_count = reader.u16(scripts);
        reader.require(scripts + 2U, static_cast<std::size_t>(script_count) * 6U);
        for (std::size_t index = 0U; index < script_count; ++index) {
            const std::size_t record = scripts + 2U + index * 6U;
            append_script_features(
                reader, scripts + reader.u16(record + 4U), feature_indices
            );
        }

        struct Feature final { std::string tag; std::vector<std::uint16_t> lookups; };
        const std::uint16_t feature_count = reader.u16(features);
        reader.require(features + 2U, static_cast<std::size_t>(feature_count) * 6U);
        std::vector<Feature> feature_records;
        feature_records.reserve(feature_count);
        for (std::size_t index = 0U; index < feature_count; ++index) {
            const std::size_t record = features + 2U + index * 6U;
            const std::size_t feature = features + reader.u16(record + 4U);
            reader.require(feature, 4U);
            const std::uint16_t count = reader.u16(feature + 2U);
            reader.require(feature + 4U, static_cast<std::size_t>(count) * 2U);
            Feature value{reader.tag(record), {}};
            value.lookups.reserve(count);
            for (std::size_t index_in_feature = 0U; index_in_feature < count;
                 ++index_in_feature) {
                value.lookups.push_back(reader.u16(
                    feature + 4U + index_in_feature * 2U
                ));
            }
            feature_records.push_back(std::move(value));
        }

        const std::uint16_t lookup_count = reader.u16(lookups);
        reader.require(lookups + 2U, static_cast<std::size_t>(lookup_count) * 2U);
        std::vector<bool> active_lookups(lookup_count, false);
        for (const std::uint16_t feature_index : feature_indices) {
            if (feature_index >= feature_records.size()) continue;
            const Feature& feature = feature_records[feature_index];
            if (feature.tag != "rlig" && feature.tag != "liga" && feature.tag != "clig") {
                continue;
            }
            for (const std::uint16_t lookup_index : feature.lookups) {
                if (lookup_index < lookup_count) active_lookups[lookup_index] = true;
            }
        }
        for (std::size_t lookup_index = 0U; lookup_index < lookup_count; ++lookup_index) {
            if (!active_lookups[lookup_index]) continue;
            try {
                if (auto lookup = parse_gsub_lookup(
                    reader,
                    lookups + reader.u16(lookups + 2U + lookup_index * 2U),
                    static_cast<std::uint16_t>(lookup_index)
                ); lookup.has_value()) {
                    substitution_lookups.push_back(std::move(*lookup));
                }
            } catch (const std::exception& error) {
                optional_diagnostics.push_back(
                    "GSUB lookup " + std::to_string(lookup_index) +
                    " ignored: " + error.what()
                );
            }
        }
    }

    void apply_kern_pair(
        const std::uint32_t key,
        const std::int16_t value,
        const bool override
    ) {
        const auto found = kern_pairs.find(key);
        if (override || found == kern_pairs.end()) {
            kern_pairs.insert_or_assign(key, value);
            return;
        }
        const std::int32_t accumulated = static_cast<std::int32_t>(found->second) + value;
        found->second = static_cast<std::int16_t>(std::clamp<std::int32_t>(
            accumulated,
            std::numeric_limits<std::int16_t>::min(),
            std::numeric_limits<std::int16_t>::max()
        ));
    }

    void parse_kern_format_zero(
        const Reader& reader,
        const std::size_t body,
        const std::size_t end,
        const bool override
    ) {
        if (body > end || end - body < 8U) throw FontError("truncated kern format 0 header");
        const std::uint16_t pair_count = reader.u16(body);
        const std::size_t records = body + 8U;
        if (static_cast<std::size_t>(pair_count) > (end - records) / 6U) {
            throw FontError("kern format 0 pair count exceeds its subtable");
        }
        for (std::size_t pair = 0U; pair < pair_count; ++pair) {
            const std::size_t record = records + pair * 6U;
            apply_kern_pair(
                pair_key(reader.u16(record), reader.u16(record + 2U)),
                reader.i16(record + 4U),
                override
            );
        }
    }

    void parse_kern() {
        const std::optional<Reader> table_reader = optional_table_reader("kern");
        if (!table_reader.has_value() || table_reader->size() < 4U) return;
        const Reader& reader = *table_reader;
        constexpr std::size_t base = 0U;
        const std::size_t end = reader.size();
        if (reader.u16(base) == 0U) {
            const std::uint16_t count = reader.u16(base + 2U);
            std::size_t subtable = base + 4U;
            for (std::size_t index = 0U; index < count; ++index) {
                if (subtable > end || end - subtable < 6U) {
                    optional_diagnostics.push_back(
                        "kern subtable " + std::to_string(index) +
                        " ignored: truncated header"
                    );
                    break;
                }
                const std::uint16_t length = reader.u16(subtable + 2U);
                const std::uint16_t coverage = reader.u16(subtable + 4U);
                if (length < 6U || static_cast<std::size_t>(length) > end - subtable) {
                    optional_diagnostics.push_back(
                        "kern subtable " + std::to_string(index) +
                        " ignored: invalid length"
                    );
                    break;
                }
                const std::uint16_t format = static_cast<std::uint16_t>(coverage >> 8U);
                const bool horizontal = (coverage & 0x0001U) != 0U;
                const bool cross_stream = (coverage & 0x0004U) != 0U;
                if (format == 0U && horizontal && !cross_stream) {
                    const auto before = kern_pairs;
                    try {
                        parse_kern_format_zero(
                            reader, subtable + 6U, subtable + length,
                            (coverage & 0x0008U) != 0U
                        );
                    } catch (const std::exception& error) {
                        kern_pairs = before;
                        optional_diagnostics.push_back(
                            "kern subtable " + std::to_string(index) +
                            " ignored: " + error.what()
                        );
                    }
                }
                subtable += length;
            }
            return;
        }
        if (reader.u32(base) != 0x00010000U || reader.size() < 8U) return;
        const std::uint32_t count = reader.u32(base + 4U);
        std::size_t subtable = base + 8U;
        for (std::uint32_t index = 0U; index < count; ++index) {
            if (subtable > end || end - subtable < 8U) {
                optional_diagnostics.push_back(
                    "kern v1 subtable " + std::to_string(index) +
                    " ignored: truncated header"
                );
                break;
            }
            const std::uint32_t length = reader.u32(subtable);
            const std::uint16_t coverage = reader.u16(subtable + 4U);
            if (length < 8U || static_cast<std::size_t>(length) > end - subtable) {
                optional_diagnostics.push_back(
                    "kern v1 subtable " + std::to_string(index) +
                    " ignored: invalid length"
                );
                break;
            }
            const std::uint16_t format = static_cast<std::uint16_t>(coverage >> 8U);
            const bool vertical = (coverage & 0x0001U) != 0U;
            const bool cross_stream = (coverage & 0x0004U) != 0U;
            const bool variation = (coverage & 0x0008U) != 0U;
            if (format == 0U && !vertical && !cross_stream && !variation) {
                const auto before = kern_pairs;
                try {
                    parse_kern_format_zero(
                        reader, subtable + 8U, subtable + length, false
                    );
                } catch (const std::exception& error) {
                    kern_pairs = before;
                    optional_diagnostics.push_back(
                        "kern v1 subtable " + std::to_string(index) +
                        " ignored: " + error.what()
                    );
                }
            }
            subtable += length;
        }
    }

    [[nodiscard]] std::string name_string(
        const Reader& reader,
        const std::uint16_t platform,
        const std::size_t offset,
        const std::size_t length
    ) const {
        if (offset > reader.size() || length > reader.size() - offset) {
            throw FontError("name record exceeds its table");
        }
        reader.require(offset, length);
        if (platform != 0U && platform != 3U) {
            return reader.raw_string(offset, length);
        }
        if ((length & 1U) != 0U) throw FontError("Unicode name record has odd byte length");
        std::string result;
        for (std::size_t cursor = 0U; cursor < length; cursor += 2U) {
            std::uint32_t code_point = reader.u16(offset + cursor);
            if (code_point >= 0xD800U && code_point <= 0xDBFFU && cursor + 3U < length) {
                const std::uint32_t low = reader.u16(offset + cursor + 2U);
                if (low >= 0xDC00U && low <= 0xDFFFU) {
                    code_point = 0x10000U + ((code_point - 0xD800U) << 10U) +
                                 (low - 0xDC00U);
                    cursor += 2U;
                }
            }
            append_utf8(result, code_point);
        }
        return result;
    }

    void parse_os2_metadata() {
        const std::optional<Reader> table_reader = optional_table_reader("OS/2");
        if (!table_reader.has_value()) return;
        const Reader& reader = *table_reader;
        if (reader.size() < 8U) throw FontError("table is shorter than 8 bytes");
        const std::uint16_t weight = reader.u16(4U);
        const std::uint16_t width = reader.u16(6U);
        if (weight == 0U || weight > 1000U || width == 0U || width > 9U) {
            throw FontError("weight or width class is outside the OpenType domain");
        }
        metadata.weight_class = weight;
        metadata.width_class = width;
        // The frozen Kotlin parser only exposes OS/2 metadata after the complete v0 body.
        if (reader.size() >= 78U) {
            const std::uint16_t selection = reader.u16(62U);
            if ((selection & 0x0020U) != 0U) metadata.style_flags |= font_style_bold;
            if ((selection & 0x0001U) != 0U) metadata.style_flags |= font_style_italic;
        }
    }

    void parse_name_metadata() {
        const std::optional<Reader> table_reader = optional_table_reader("name");
        if (!table_reader.has_value()) return;
        const Reader& reader = *table_reader;
        reader.require(0U, 6U);
        const std::uint16_t count = reader.u16(2U);
        const std::uint16_t strings = reader.u16(4U);
        if (6U + static_cast<std::size_t>(count) * 12U > reader.size()) {
            throw FontError("name record directory exceeds its table");
        }
        struct Candidate final { int score = -1; std::string value; };
        Candidate family;
        Candidate subfamily;
        Candidate full_name;
        Candidate postscript;
        for (std::size_t index = 0U; index < count; ++index) {
            try {
                const std::size_t record = 6U + index * 12U;
                const std::uint16_t platform = reader.u16(record);
                const std::uint16_t language = reader.u16(record + 4U);
                const std::uint16_t name_id = reader.u16(record + 6U);
                Candidate* target = name_id == 1U || name_id == 16U ? &family
                    : name_id == 2U || name_id == 17U ? &subfamily
                    : name_id == 4U ? &full_name
                    : name_id == 6U ? &postscript : nullptr;
                if (target == nullptr) continue;
                const int typographic = name_id == 16U || name_id == 17U ? 10 : 0;
                const int score = typographic +
                    (platform == 3U ? 4 : platform == 0U ? 3 : 1) +
                    (language == 0x0409U || language == 0U ? 1 : 0);
                std::string value = name_string(
                    reader,
                    platform,
                    static_cast<std::size_t>(strings) + reader.u16(record + 10U),
                    reader.u16(record + 8U)
                );
                if (value.empty() || score <= target->score) continue;
                target->value = std::move(value);
                target->score = score;
            } catch (const std::exception& error) {
                optional_diagnostics.push_back(
                    "name record " + std::to_string(index) + " ignored: " + error.what()
                );
            }
        }
        metadata.family = std::move(family.value);
        metadata.subfamily = std::move(subfamily.value);
        metadata.full_name = std::move(full_name.value);
        metadata.postscript_name = std::move(postscript.value);
    }

    [[nodiscard]] std::uint16_t glyph_id(const std::uint32_t code_point) const noexcept {
        if (!cmap12.empty()) {
            const auto found = std::ranges::lower_bound(
                cmap12,
                code_point,
                {},
                &Cmap12Group::last
            );
            if (found != cmap12.end() && code_point >= found->first) {
                const std::uint32_t glyph = found->start_glyph + code_point - found->first;
                return glyph < glyphs ? static_cast<std::uint16_t>(glyph) : 0U;
            }
        }
        if (code_point > 0xFFFFU || !cmap4.has_value()) return 0U;
        const std::uint16_t character = static_cast<std::uint16_t>(code_point);
        for (std::size_t index = 0U; index < cmap4->ends.size(); ++index) {
            if (character > cmap4->ends[index]) continue;
            if (character < cmap4->starts[index]) return 0U;
            std::uint16_t glyph = 0U;
            if (cmap4->range_offsets[index] == 0U) {
                glyph = static_cast<std::uint16_t>(
                    static_cast<std::int32_t>(character) + cmap4->deltas[index]
                );
            } else {
                const Reader cmap_reader(std::span<const std::uint8_t>(cmap4->bytes));
                const std::size_t word = cmap4->range_word_offsets[index] +
                                         cmap4->range_offsets[index] +
                                         static_cast<std::size_t>(character - cmap4->starts[index]) * 2U;
                if (word > cmap4->table_end || cmap4->table_end - word < 2U) return 0U;
                glyph = cmap_reader.u16(word);
                if (glyph != 0U) {
                    glyph = static_cast<std::uint16_t>(
                        static_cast<std::int32_t>(glyph) + cmap4->deltas[index]
                    );
                }
            }
            return glyph < glyphs ? glyph : 0U;
        }
        return 0U;
    }

    [[nodiscard]] bool covered(
        const std::vector<GlyphRange>& coverage,
        const std::uint16_t glyph
    ) const noexcept {
        return containing_range(coverage, glyph) != nullptr;
    }

    [[nodiscard]] bool glyph_is_mark(const std::uint16_t glyph) const noexcept {
        if (glyph_classes.value(glyph) == 3U) return true;
        for (const PositioningLookup& lookup : positioning_lookups) {
            for (const PositioningSubtable& subtable : lookup.subtables) {
                if (subtable.mark.has_value() && subtable.mark->marks.contains(glyph)) {
                    return true;
                }
            }
        }
        return false;
    }

    template <typename Glyph>
    [[nodiscard]] bool ignored_by_lookup(
        const LookupFilter& filter,
        const Glyph& input
    ) const noexcept {
        std::uint16_t glyph_class = glyph_classes.value(input.glyph_id);
        if (glyph_class == 0U) {
            glyph_class = glyph_is_mark(input.glyph_id) ? 3U
                : input.ligature_component_count > 1U ? 2U : 1U;
        }
        if (glyph_class == 1U && (filter.flags & 0x0002U) != 0U) return true;
        if (glyph_class == 2U && (filter.flags & 0x0004U) != 0U) return true;
        if (glyph_class != 3U) return false;
        if ((filter.flags & 0x0008U) != 0U) return true;
        const std::uint16_t attachment_type = static_cast<std::uint16_t>(
            filter.flags >> 8U
        );
        if (attachment_type != 0U &&
            mark_attachment_classes.value(input.glyph_id) != attachment_type) {
            return true;
        }
        if (filter.mark_filtering_set.has_value() &&
            !covered(mark_filtering_sets[*filter.mark_filtering_set], input.glyph_id)) {
            return true;
        }
        return false;
    }

    template <typename Glyph>
    [[nodiscard]] std::optional<std::size_t> next_eligible(
        const LookupFilter& filter,
        const std::span<const Glyph> glyph_run,
        const std::size_t current
    ) const noexcept {
        for (std::size_t index = current + 1U; index < glyph_run.size(); ++index) {
            if (!ignored_by_lookup(filter, glyph_run[index])) return index;
        }
        return std::nullopt;
    }

    template <typename Glyph>
    [[nodiscard]] std::optional<std::size_t> previous_eligible(
        const LookupFilter& filter,
        const std::span<const Glyph> glyph_run,
        const std::size_t current
    ) const noexcept {
        for (std::size_t index = current; index-- > 0U;) {
            if (!ignored_by_lookup(filter, glyph_run[index])) return index;
        }
        return std::nullopt;
    }

    [[nodiscard]] static const PairAdjustment* pair_in_lookup(
        const PositioningLookup& lookup,
        const std::uint16_t left,
        const std::uint16_t right
    ) noexcept {
        for (const PositioningSubtable& subtable : lookup.subtables) {
            if (!subtable.pair.has_value()) continue;
            if (const PairAdjustment* adjustment = subtable.pair->find(left, right);
                adjustment != nullptr) {
                return adjustment;
            }
        }
        return nullptr;
    }

    [[nodiscard]] std::optional<PairAdjustment> gpos_adjustment(
        const std::uint16_t left,
        const std::uint16_t right
    ) const noexcept {
        PairAdjustment result;
        bool matched = false;
        const GlyphPositioningInput left_input{left};
        const GlyphPositioningInput right_input{right};
        for (const PositioningLookup& lookup : positioning_lookups) {
            if (ignored_by_lookup(lookup.filter, left_input) ||
                ignored_by_lookup(lookup.filter, right_input)) {
                continue;
            }
            const PairAdjustment* pair = pair_in_lookup(lookup, left, right);
            if (pair == nullptr) continue;
            matched = true;
            const auto accumulate = [](ValueAdjustment& target, const ValueAdjustment& value) {
                target.x_placement = static_cast<std::int16_t>(std::clamp<std::int32_t>(
                    static_cast<std::int32_t>(target.x_placement) + value.x_placement,
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max()
                ));
                target.y_placement = static_cast<std::int16_t>(std::clamp<std::int32_t>(
                    static_cast<std::int32_t>(target.y_placement) + value.y_placement,
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max()
                ));
                target.x_advance = static_cast<std::int16_t>(std::clamp<std::int32_t>(
                    static_cast<std::int32_t>(target.x_advance) + value.x_advance,
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max()
                ));
                target.y_advance = static_cast<std::int16_t>(std::clamp<std::int32_t>(
                    static_cast<std::int32_t>(target.y_advance) + value.y_advance,
                    std::numeric_limits<std::int16_t>::min(),
                    std::numeric_limits<std::int16_t>::max()
                ));
            };
            accumulate(result.first, pair->first);
            accumulate(result.second, pair->second);
        }
        return matched ? std::optional<PairAdjustment>(result) : std::nullopt;
    }

    [[nodiscard]] std::vector<GlyphRunState> substitute_glyphs(
        const std::span<const GlyphRunInput> source
    ) const {
        std::vector<GlyphRunState> result;
        result.reserve(source.size());
        for (const GlyphRunInput& glyph : source) {
            result.push_back(GlyphRunState{
                glyph.glyph_id,
                glyph.text_start_offset,
                glyph.text_end_offset,
                {glyph.text_start_offset},
                1U,
            });
        }
        for (const LigatureSubstitutionLookup& lookup : substitution_lookups) {
            for (std::size_t cursor = 0U; cursor < result.size();) {
                if (ignored_by_lookup(lookup.filter, result[cursor])) {
                    ++cursor;
                    continue;
                }
                const auto rules = lookup.rules.find(result[cursor].glyph_id);
                if (rules == lookup.rules.end()) {
                    ++cursor;
                    continue;
                }
                for (const LigatureSubstitutionRule& rule : rules->second) {
                    std::vector<std::size_t> matched{cursor};
                    std::size_t previous = cursor;
                    bool matches = true;
                    for (const std::uint16_t component : rule.components) {
                        const std::optional<std::size_t> next = next_eligible(
                            lookup.filter,
                            std::span<const GlyphRunState>(result),
                            previous
                        );
                        if (!next.has_value() || result[*next].glyph_id != component) {
                            matches = false;
                            break;
                        }
                        matched.push_back(*next);
                        previous = *next;
                    }
                    if (!matches) continue;

                    GlyphRunState ligature = result[cursor];
                    ligature.glyph_id = rule.replacement;
                    ligature.component_clusters.clear();
                    for (const std::size_t index : matched) {
                        ligature.source_end = std::max(
                            ligature.source_end, result[index].source_end
                        );
                        ligature.component_clusters.insert(
                            ligature.component_clusters.end(),
                            result[index].component_clusters.begin(),
                            result[index].component_clusters.end()
                        );
                    }
                    if (ligature.component_clusters.size() >
                        std::numeric_limits<std::uint16_t>::max()) {
                        throw FontError("GSUB ligature component provenance exceeds uint16");
                    }
                    ligature.ligature_component_count = static_cast<std::uint16_t>(
                        ligature.component_clusters.size()
                    );
                    result[cursor] = std::move(ligature);
                    for (auto index = matched.rbegin(); index != matched.rend(); ++index) {
                        if (*index != cursor) {
                            result.erase(result.begin() + static_cast<std::ptrdiff_t>(*index));
                        }
                    }
                    break;
                }
                ++cursor;
            }
        }
        return result;
    }

    [[nodiscard]] double synthetic_advance(
        const std::uint16_t glyph,
        const FontStyleGeometry style
    ) const {
        if (!style.bold() ||
            glyph >= advances.size() || advances[glyph] == 0U || outlines == nullptr) {
            return 0.0;
        }
        const std::shared_ptr<const GlyphOutline> outline = outlines->outline(glyph);
        return outline != nullptr && !outline->empty()
            ? style.bold_strength(units)
            : 0.0;
    }

    [[nodiscard]] std::vector<PositionedRunGlyph> shape_glyph_run(
        const std::span<const GlyphRunInput> source,
        const std::uint32_t font_style_flags
    ) const {
        std::vector<GlyphRunState> substituted = substitute_glyphs(source);
        std::vector<GlyphPositioningInput> positioning;
        positioning.reserve(substituted.size());
        for (const GlyphRunState& glyph : substituted) {
            positioning.push_back(GlyphPositioningInput{
                glyph.glyph_id,
                glyph.source_start,
                0U,
                glyph.ligature_component_count,
            });
        }
        for (std::size_t index = 0U; index < substituted.size(); ++index) {
            if (!glyph_is_mark(substituted[index].glyph_id)) continue;
            for (std::size_t candidate = index; candidate-- > 0U;) {
                if (glyph_is_mark(substituted[candidate].glyph_id)) continue;
                if (substituted[candidate].component_clusters.size() <= 1U) break;
                const auto component = std::ranges::upper_bound(
                    substituted[candidate].component_clusters,
                    substituted[index].source_start
                );
                const std::size_t component_index = component ==
                    substituted[candidate].component_clusters.begin()
                        ? 0U
                        : static_cast<std::size_t>(component -
                              substituted[candidate].component_clusters.begin() - 1);
                positioning[index].ligature_component = static_cast<std::uint16_t>(std::min(
                    component_index,
                    substituted[candidate].component_clusters.size() - 1U
                ));
                break;
            }
        }
        const std::vector<GlyphPositionAdjustment> positions = position_glyphs(
            std::span<const GlyphPositioningInput>(positioning), font_style_flags
        );
        std::vector<PositionedRunGlyph> result;
        result.reserve(substituted.size());
        for (std::size_t index = 0U; index < substituted.size(); ++index) {
            result.push_back(PositionedRunGlyph{
                substituted[index].glyph_id,
                substituted[index].source_start,
                substituted[index].source_end,
                std::move(substituted[index].component_clusters),
                positions[index],
            });
        }
        return result;
    }

    [[nodiscard]] std::vector<GlyphPositionAdjustment> position_glyphs(
        const std::span<const GlyphPositioningInput> glyph_run,
        const std::uint32_t font_style_flags
    ) const {
        const FontStyleGeometry style = resolve_font_style_geometry(
            font_style_flags, metadata.style_flags
        );
        std::vector<GlyphPositionAdjustment> result(glyph_run.size());
        if (style.bold()) {
            for (std::size_t index = 0U; index < glyph_run.size(); ++index) {
                result[index].x_advance = synthetic_advance(
                    glyph_run[index].glyph_id, style
                );
            }
        }
        const auto add = [](GlyphPositionAdjustment& target,
                            const ValueAdjustment& value) {
            target.x_placement = saturated_design_add(target.x_placement, value.x_placement);
            target.y_placement = saturated_design_add(target.y_placement, value.y_placement);
            target.x_advance = saturated_design_add(target.x_advance, value.x_advance);
            target.y_advance = saturated_design_add(target.y_advance, value.y_advance);
        };

        struct PlannedPairAdjustment final {
            std::size_t left = 0U;
            std::size_t right = 0U;
            const PairAdjustment* adjustment = nullptr;
        };
        std::vector<std::vector<PlannedPairAdjustment>> pair_plans(
            positioning_lookups.size()
        );
        std::vector<bool> pair_lookups(positioning_lookups.size(), false);
        std::vector<bool> adjacent_pair_positioned(
            glyph_run.empty() ? 0U : glyph_run.size() - 1U,
            false
        );
        for (std::size_t lookup_index = 0U;
             lookup_index < positioning_lookups.size(); ++lookup_index) {
            const PositioningLookup& lookup = positioning_lookups[lookup_index];
            pair_lookups[lookup_index] = std::ranges::any_of(
                lookup.subtables,
                [](const PositioningSubtable& subtable) {
                    return subtable.pair.has_value();
                }
            );
            if (!pair_lookups[lookup_index]) continue;
            std::vector<PlannedPairAdjustment>& plan = pair_plans[lookup_index];
            plan.reserve(glyph_run.size());
            for (std::size_t left = 0U; left < glyph_run.size(); ++left) {
                if (ignored_by_lookup(lookup.filter, glyph_run[left])) continue;
                const std::optional<std::size_t> right = next_eligible(
                    lookup.filter, glyph_run, left
                );
                if (!right.has_value()) continue;
                const PairAdjustment* pair = pair_in_lookup(
                    lookup, glyph_run[left].glyph_id, glyph_run[*right].glyph_id
                );
                if (pair == nullptr) continue;
                plan.push_back(PlannedPairAdjustment{left, *right, pair});
                if (*right == left + 1U) adjacent_pair_positioned[left] = true;
            }
        }

        // Legacy kern contributes to the pen before GPOS positioning and is suppressed only for
        // adjacent pairs covered by a selected PairPos lookup. Pair applications were planned
        // above so coverage discovery does not require a duplicate OpenType lookup scan.
        for (std::size_t index = 1U; index < glyph_run.size(); ++index) {
            if (adjacent_pair_positioned[index - 1U]) continue;
            const auto kern = kern_pairs.find(pair_key(
                glyph_run[index - 1U].glyph_id, glyph_run[index].glyph_id
            ));
            if (kern != kern_pairs.end()) {
                result[index - 1U].x_advance = saturated_design_add(
                    result[index - 1U].x_advance, kern->second
                );
            }
        }

        for (std::size_t lookup_index = 0U;
             lookup_index < positioning_lookups.size(); ++lookup_index) {
            const PositioningLookup& lookup = positioning_lookups[lookup_index];
            if (pair_lookups[lookup_index]) {
                for (const PlannedPairAdjustment& pair : pair_plans[lookup_index]) {
                    add(result[pair.left], pair.adjustment->first);
                    add(result[pair.right], pair.adjustment->second);
                }
                continue;
            }

            std::vector<double> pen_x(glyph_run.size());
            std::vector<double> pen_y(glyph_run.size());
            for (std::size_t index = 1U; index < glyph_run.size(); ++index) {
                const std::uint16_t previous = glyph_run[index - 1U].glyph_id;
                const std::uint16_t nominal = previous < advances.size()
                    ? advances[previous] : 0U;
                pen_x[index] = pen_x[index - 1U] +
                    static_cast<double>(nominal) + result[index - 1U].x_advance;
                pen_y[index] = pen_y[index - 1U] + result[index - 1U].y_advance;
            }
            for (std::size_t mark_index = 1U; mark_index < glyph_run.size(); ++mark_index) {
                if (ignored_by_lookup(lookup.filter, glyph_run[mark_index])) continue;
                const std::optional<std::size_t> candidate = previous_eligible(
                    lookup.filter, glyph_run, mark_index
                );
                if (!candidate.has_value()) continue;
                for (const PositioningSubtable& subtable : lookup.subtables) {
                    if (!subtable.mark.has_value()) continue;
                    const MarkAttachmentRule& rule = *subtable.mark;
                    const auto mark = rule.marks.find(glyph_run[mark_index].glyph_id);
                    if (mark == rule.marks.end()) continue;
                    std::optional<Anchor> base_anchor;
                    if (rule.kind == MarkAttachmentKind::ligature) {
                        const auto found = rule.ligature_anchors.find(
                            glyph_run[*candidate].glyph_id
                        );
                        if (found == rule.ligature_anchors.end() ||
                            found->second.empty()) {
                            continue;
                        }
                        const std::size_t component =
                            glyph_run[mark_index].ligature_component;
                        if (component >= found->second.size()) continue;
                        const auto& anchors = found->second[component];
                        if (mark->second.mark_class >= anchors.size() ||
                            !anchors[mark->second.mark_class].has_value()) {
                            continue;
                        }
                        base_anchor = anchors[mark->second.mark_class];
                    } else {
                        const auto found = rule.anchors.find(
                            glyph_run[*candidate].glyph_id
                        );
                        if (found == rule.anchors.end() ||
                            mark->second.mark_class >= found->second.size() ||
                            !found->second[mark->second.mark_class].has_value()) {
                            continue;
                        }
                        base_anchor = found->second[mark->second.mark_class];
                    }
                    if (!base_anchor.has_value()) continue;
                    const double target_x = pen_x[*candidate] +
                        result[*candidate].x_placement + base_anchor->x;
                    const double target_y = pen_y[*candidate] +
                        result[*candidate].y_placement + base_anchor->y;
                    result[mark_index].x_placement = std::clamp(
                        target_x - pen_x[mark_index] - mark->second.anchor.x,
                        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                        static_cast<double>(std::numeric_limits<std::int32_t>::max())
                    );
                    result[mark_index].y_placement = std::clamp(
                        target_y - pen_y[mark_index] - mark->second.anchor.y,
                        static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                        static_cast<double>(std::numeric_limits<std::int32_t>::max())
                    );
                    break;
                }
            }
        }
        if (style.italic()) {
            // Apply the same baseline-anchored linear transform used by rasterization to every
            // GPOS translation/advance vector. This carries PairPos y movement, mark/ligature
            // attachment, and vertical pens into the sheared coordinate space uniformly.
            for (GlyphPositionAdjustment& position : result) {
                position.x_placement = saturated_design_add(
                    position.x_placement,
                    style.transform_x(0.0, position.y_placement)
                );
                position.x_advance = saturated_design_add(
                    position.x_advance,
                    style.transform_x(0.0, position.y_advance)
                );
            }
        }
        return result;
    }
};

OpenTypeFont::OpenTypeFont(std::shared_ptr<const Impl> implementation)
    : implementation_(std::move(implementation)) {}

OpenTypeFont OpenTypeFont::parse(resource::ResourceBytes bytes) {
    if (bytes.empty()) throw FontError("OpenType font bytes must not be empty");
    const auto content_hash = [](const std::span<const std::uint8_t> content) noexcept {
        std::uint64_t hash = 1469598103934665603ULL;
        for (const std::uint8_t byte : content) {
            hash ^= byte;
            hash *= 1099511628211ULL;
        }
        return hash;
    };
    struct CachedFont final {
        std::uint64_t hash = 0U;
        std::weak_ptr<const Impl> implementation;
    };
    static std::mutex cache_mutex;
    static std::vector<CachedFont> cache;

    const std::uint64_t hash = content_hash(bytes);
    {
        const std::scoped_lock lock(cache_mutex);
        for (auto iterator = cache.begin(); iterator != cache.end();) {
            const std::shared_ptr<const Impl> existing = iterator->implementation.lock();
            if (existing == nullptr) {
                iterator = cache.erase(iterator);
                continue;
            }
            if (iterator->hash == hash && existing->font_bytes == bytes) {
                return OpenTypeFont(existing);
            }
            ++iterator;
        }
    }

    auto parsed = std::make_shared<const Impl>(std::move(bytes));
    {
        const std::scoped_lock lock(cache_mutex);
        // Parsing happens outside the lock. Resolve a concurrent identical parse deterministically
        // before publishing the candidate.
        for (auto iterator = cache.begin(); iterator != cache.end();) {
            const std::shared_ptr<const Impl> existing = iterator->implementation.lock();
            if (existing == nullptr) {
                iterator = cache.erase(iterator);
                continue;
            }
            if (iterator->hash == hash && existing->font_bytes == parsed->font_bytes) {
                return OpenTypeFont(existing);
            }
            ++iterator;
        }
        cache.push_back(CachedFont{hash, parsed});
    }
    return OpenTypeFont(std::move(parsed));
}

std::uint16_t OpenTypeFont::units_per_em() const noexcept { return implementation_->units; }
std::int16_t OpenTypeFont::ascender() const noexcept { return implementation_->ascent; }
std::int16_t OpenTypeFont::descender() const noexcept { return implementation_->descent; }
std::int16_t OpenTypeFont::line_gap() const noexcept { return implementation_->gap; }
std::uint16_t OpenTypeFont::glyph_count() const noexcept { return implementation_->glyphs; }
const FontMetadata& OpenTypeFont::metadata() const noexcept {
    return implementation_->metadata;
}
std::shared_ptr<const void> OpenTypeFont::cache_identity() const noexcept {
    return implementation_;
}
std::span<const std::uint8_t> OpenTypeFont::source_bytes() const noexcept {
    return implementation_->font_bytes;
}
const std::vector<std::string>& OpenTypeFont::optional_diagnostics() const noexcept {
    return implementation_->optional_diagnostics;
}

std::uint16_t OpenTypeFont::glyph_id(const std::uint32_t code_point) const noexcept {
    return implementation_->glyph_id(code_point);
}

std::uint16_t OpenTypeFont::horizontal_advance(const std::uint16_t glyph) const noexcept {
    return glyph < implementation_->advances.size() ? implementation_->advances[glyph] : 0U;
}

std::shared_ptr<const GlyphOutline> OpenTypeFont::glyph_outline(const std::uint16_t glyph) const {
    if (glyph >= implementation_->glyphs) throw std::out_of_range("glyph id exceeds font glyph count");
    if (implementation_->outlines == nullptr) return std::make_shared<const GlyphOutline>();
    return implementation_->outlines->outline(glyph);
}

std::int16_t OpenTypeFont::pair_advance_adjustment(
    const std::uint16_t left,
    const std::uint16_t right
) const noexcept {
    if (const std::optional<PairAdjustment> adjustment =
            implementation_->gpos_adjustment(left, right);
        adjustment.has_value()) {
        return adjustment->first.x_advance;
    }
    const auto found = implementation_->kern_pairs.find(pair_key(left, right));
    return found != implementation_->kern_pairs.end() ? found->second : 0;
}

PairPositionAdjustment OpenTypeFont::pair_position_adjustment(
    const std::uint16_t left,
    const std::uint16_t right
) const noexcept {
    if (const std::optional<PairAdjustment> adjustment =
            implementation_->gpos_adjustment(left, right);
        adjustment.has_value()) {
        const auto convert = [](const ValueAdjustment& value) {
            return PairPositionValue{
                value.x_placement,
                value.y_placement,
                value.x_advance,
                value.y_advance,
            };
        };
        return PairPositionAdjustment{convert(adjustment->first), convert(adjustment->second)};
    }
    PairPositionAdjustment result;
    const auto found = implementation_->kern_pairs.find(pair_key(left, right));
    if (found != implementation_->kern_pairs.end()) result.first.x_advance = found->second;
    return result;
}

std::vector<GlyphPositionAdjustment> OpenTypeFont::position_glyphs(
    const std::span<const std::uint16_t> glyphs,
    const std::uint32_t font_style_flags
) const {
    std::vector<GlyphPositioningInput> associated;
    associated.reserve(glyphs.size());
    for (std::size_t index = 0U; index < glyphs.size(); ++index) {
        associated.push_back(GlyphPositioningInput{glyphs[index], index, 0U, 1U});
    }
    return implementation_->position_glyphs(
        std::span<const GlyphPositioningInput>(associated), font_style_flags
    );
}

std::vector<GlyphPositionAdjustment> OpenTypeFont::position_glyphs(
    const std::span<const GlyphPositioningInput> glyphs,
    const std::uint32_t font_style_flags
) const {
    return implementation_->position_glyphs(glyphs, font_style_flags);
}

std::vector<PositionedRunGlyph> OpenTypeFont::shape_glyph_run(
    const std::span<const GlyphRunInput> glyphs,
    const std::uint32_t font_style_flags
) const {
    return implementation_->shape_glyph_run(glyphs, font_style_flags);
}

TextMetrics OpenTypeFont::measure_utf8(
    const std::string_view text,
    const double pixel_size,
    const double line_height_multiplier
) const {
    return shape_utf8(text, pixel_size, line_height_multiplier).metrics;
}

ShapedText OpenTypeFont::shape_utf8(
    const std::string_view text,
    const double pixel_size,
    const double line_height_multiplier,
    const double letter_spacing
) const {
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0 ||
        !std::isfinite(line_height_multiplier) || line_height_multiplier <= 0.0 ||
        !std::isfinite(letter_spacing)) {
        throw std::invalid_argument("text shaping parameters must be finite and text sizes positive");
    }
    if (!core::valid_utf8(text)) throw FontError("text to shape is not valid UTF-8");

    const double scale = pixel_size / static_cast<double>(implementation_->units);
    const double ascent_value = std::max(0.0, static_cast<double>(implementation_->ascent) * scale);
    const double descent_value = std::max(0.0, -static_cast<double>(implementation_->descent) * scale);
    const double gap_value = std::max(0.0, static_cast<double>(implementation_->gap) * scale);
    const double natural_height = std::max(pixel_size, ascent_value + descent_value + gap_value);
    const double line_height = natural_height * line_height_multiplier;

    ShapedText shaped;
    shaped.metrics.natural_line_height = natural_height;
    shaped.metrics.line_count = 1U;
    struct Item final {
        std::uint32_t code_point = 0U;
        std::size_t start = 0U;
        std::size_t end = 0U;
        std::uint16_t glyph = 0U;
    };
    double x = 0.0;
    std::size_t line_index = 0U;
    std::size_t utf16_offset = 0U;
    std::vector<Item> items;
    const auto flush = [&] {
        if (items.empty()) return;
        std::vector<std::uint16_t> glyphs;
        glyphs.reserve(items.size());
        for (const Item& item : items) glyphs.push_back(item.glyph);
        const std::vector<GlyphPositionAdjustment> positions = position_glyphs(
            std::span<const std::uint16_t>(glyphs)
        );
        double y_pen = 0.0;
        for (std::size_t index = 0U; index < items.size(); ++index) {
            const Item& item = items[index];
            const GlyphPositionAdjustment& position = positions[index];
            if (index != 0U) x += letter_spacing;
            const double nominal = static_cast<double>(horizontal_advance(item.glyph)) * scale;
            const double x_advance = nominal +
                static_cast<double>(position.x_advance) * scale;
            shaped.clusters.push_back(ShapedCluster{
                item.code_point, item.start, item.end, x, x_advance, line_index,
            });
            const bool drawable = item.code_point != 0x20U &&
                                  item.code_point != 0x09U &&
                                  item.code_point != 0x0DU;
            if (drawable) {
                shaped.glyphs.push_back(ShapedGlyph{
                    item.glyph,
                    item.code_point,
                    item.start,
                    item.end,
                    x,
                    ascent_value + static_cast<double>(line_index) * line_height,
                    x_advance,
                    static_cast<double>(position.x_placement) * scale,
                    y_pen + static_cast<double>(position.y_placement) * scale,
                    static_cast<double>(position.y_advance) * scale,
                    line_index,
                });
            }
            x += x_advance;
            y_pen += static_cast<double>(position.y_advance) * scale;
            ++shaped.metrics.glyph_count;
        }
        items.clear();
    };
    std::size_t byte_offset = 0U;
    while (byte_offset < text.size()) {
        const std::uint32_t code_point = next_utf8(text, byte_offset);
        const std::size_t start = utf16_offset;
        utf16_offset += code_point > 0xFFFFU ? 2U : 1U;
        if (code_point == 0x0AU) {
            flush();
            shaped.metrics.width = std::max(shaped.metrics.width, x);
            x = 0.0;
            ++line_index;
            ++shaped.metrics.line_count;
            continue;
        }
        const std::uint16_t glyph = implementation_->glyph_id(code_point);
        if (glyph == 0U) {
            flush();
            shaped.missing_code_points.push_back(code_point);
            shaped.clusters.push_back(ShapedCluster{
                code_point, start, utf16_offset, x, 0.0, line_index,
            });
            continue;
        }
        items.push_back(Item{code_point, start, utf16_offset, glyph});
    }
    flush();
    shaped.metrics.width = std::max(shaped.metrics.width, x);
    shaped.metrics.height = line_height * static_cast<double>(shaped.metrics.line_count);
    return shaped;
}

} // namespace strata::font
