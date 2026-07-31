#include "font/truetype_outline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace strata::font {
namespace {

constexpr std::size_t glyph_header_size = 10U;
constexpr std::uint8_t flag_on_curve = 0x01U;
constexpr std::uint8_t flag_x_short = 0x02U;
constexpr std::uint8_t flag_y_short = 0x04U;
constexpr std::uint8_t flag_repeat = 0x08U;
constexpr std::uint8_t flag_x_same_or_positive = 0x10U;
constexpr std::uint8_t flag_y_same_or_positive = 0x20U;

constexpr std::uint16_t args_are_words = 0x0001U;
constexpr std::uint16_t args_are_xy = 0x0002U;
constexpr std::uint16_t has_scale = 0x0008U;
constexpr std::uint16_t more_components = 0x0020U;
constexpr std::uint16_t has_xy_scale = 0x0040U;
constexpr std::uint16_t has_matrix = 0x0080U;
constexpr std::uint16_t has_instructions = 0x0100U;
constexpr std::uint16_t scaled_component_offset = 0x0800U;
constexpr std::uint16_t unscaled_component_offset = 0x1000U;
constexpr std::size_t maximum_component_depth = 32U;
constexpr std::size_t maximum_components_per_glyph = 1'024U;
constexpr std::size_t maximum_points_per_glyph = 1'000'000U;

class Reader final {
public:
    explicit Reader(const std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

    void require(const std::size_t offset, const std::size_t count) const {
        if (offset > bytes_.size() || count > bytes_.size() - offset) {
            throw FontError("TrueType outline range exceeds the font bytes");
        }
    }

    [[nodiscard]] std::uint8_t u8(const std::size_t offset) const {
        require(offset, 1U);
        return bytes_[offset];
    }

    [[nodiscard]] std::int8_t i8(const std::size_t offset) const {
        return std::bit_cast<std::int8_t>(u8(offset));
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

    [[nodiscard]] double f2dot14(const std::size_t offset) const {
        return static_cast<double>(i16(offset)) / 16'384.0;
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

private:
    std::span<const std::uint8_t> bytes_;
};

struct Table final {
    std::size_t offset = 0U;
    std::size_t length = 0U;
};

struct Transform final {
    double xx = 1.0;
    double xy = 0.0;
    double yx = 0.0;
    double yy = 1.0;
    double dx = 0.0;
    double dy = 0.0;

    [[nodiscard]] GlyphPoint apply(const GlyphPoint& point) const noexcept {
        return GlyphPoint{
            point.x * xx + point.y * xy + dx,
            point.x * yx + point.y * yy + dy,
            point.on_curve,
        };
    }

    [[nodiscard]] std::pair<double, double> apply_vector(
        const double x,
        const double y
    ) const noexcept {
        return {x * xx + y * xy, x * yx + y * yy};
    }
};

[[nodiscard]] GlyphPoint midpoint(const GlyphPoint& left, const GlyphPoint& right) noexcept {
    return GlyphPoint{(left.x + right.x) * 0.5, (left.y + right.y) * 0.5, true};
}

[[nodiscard]] std::vector<GlyphSegment> build_segments(const std::vector<GlyphPoint>& raw) {
    if (raw.empty()) return {};
    const auto first_on_curve = std::find_if(raw.begin(), raw.end(), [](const GlyphPoint& point) {
        return point.on_curve;
    });
    GlyphPoint start;
    std::vector<GlyphPoint> traversal;
    traversal.reserve(raw.size());
    if (first_on_curve != raw.end()) {
        start = *first_on_curve;
        const std::size_t start_index = static_cast<std::size_t>(first_on_curve - raw.begin());
        for (std::size_t offset = 1U; offset < raw.size(); ++offset) {
            traversal.push_back(raw[(start_index + offset) % raw.size()]);
        }
    } else {
        start = midpoint(raw.back(), raw.front());
        traversal = raw;
    }

    std::vector<GlyphSegment> result;
    result.reserve(raw.size() + 1U);
    GlyphPoint current = start;
    std::optional<GlyphPoint> control;
    for (const GlyphPoint& point : traversal) {
        if (point.on_curve) {
            if (control.has_value()) {
                result.push_back(GlyphSegment{GlyphSegmentKind::quadratic, current, *control, point});
                control.reset();
            } else {
                result.push_back(GlyphSegment{GlyphSegmentKind::line, current, current, point});
            }
            current = point;
        } else {
            if (control.has_value()) {
                const GlyphPoint implied = midpoint(*control, point);
                result.push_back(GlyphSegment{GlyphSegmentKind::quadratic, current, *control, implied});
                current = implied;
            }
            control = point;
        }
    }
    if (control.has_value()) {
        result.push_back(GlyphSegment{GlyphSegmentKind::quadratic, current, *control, start});
    } else if (!(current == start)) {
        result.push_back(GlyphSegment{GlyphSegmentKind::line, current, current, start});
    }
    return result;
}

[[nodiscard]] GlyphContour transformed(const GlyphContour& contour, const Transform& transform) {
    GlyphContour result;
    result.points.reserve(contour.points.size());
    result.segments.reserve(contour.segments.size());
    for (const GlyphPoint& point : contour.points) result.points.push_back(transform.apply(point));
    for (const GlyphSegment& segment : contour.segments) {
        result.segments.push_back(GlyphSegment{
            segment.kind,
            transform.apply(segment.from),
            transform.apply(segment.control),
            transform.apply(segment.to),
        });
    }
    return result;
}

[[nodiscard]] std::optional<GlyphBounds> bounds_from_points(
    const std::vector<GlyphContour>& contours
) {
    std::optional<GlyphBounds> result;
    for (const GlyphContour& contour : contours) {
        for (const GlyphPoint& point : contour.points) {
            if (!result.has_value()) {
                result = GlyphBounds{point.x, point.y, point.x, point.y};
            } else {
                result->left = std::min(result->left, point.x);
                result->bottom = std::min(result->bottom, point.y);
                result->right = std::max(result->right, point.x);
                result->top = std::max(result->top, point.y);
            }
        }
    }
    return result;
}

} // namespace

struct TrueTypeOutlineSource::Impl final {
    Impl(const std::span<const std::uint8_t> input, const std::uint16_t count)
        : reader(input), glyph_count(count), cache(count) {
        parse_tables();
        parse_locations();
    }

    Reader reader;
    std::uint16_t glyph_count = 0U;
    Table glyf;
    std::vector<std::size_t> locations;
    mutable std::vector<std::shared_ptr<const GlyphOutline>> cache;
    mutable std::mutex mutex;

    void parse_tables() {
        reader.require(0U, 12U);
        const std::uint16_t count = reader.u16(4U);
        reader.require(12U, static_cast<std::size_t>(count) * 16U);
        std::map<std::string, Table, std::less<>> tables;
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t record = 12U + index * 16U;
            const Table table{
                static_cast<std::size_t>(reader.u32(record + 8U)),
                static_cast<std::size_t>(reader.u32(record + 12U)),
            };
            if (!tables.emplace(reader.tag(record), table).second) {
                throw FontError("duplicate OpenType table while decoding outlines");
            }
        }
        const auto head = tables.find("head");
        const auto loca = tables.find("loca");
        const auto glyph_data = tables.find("glyf");
        if (head == tables.end() || loca == tables.end() || glyph_data == tables.end()) {
            throw FontError("TrueType outlines require head, loca, and glyf tables");
        }
        reader.require(head->second.offset, head->second.length);
        reader.require(loca->second.offset, loca->second.length);
        reader.require(glyph_data->second.offset, glyph_data->second.length);
        reader.require(head->second.offset, 54U);
        index_to_loc_format = reader.i16(head->second.offset + 50U);
        if (index_to_loc_format != 0 && index_to_loc_format != 1) {
            throw FontError("unsupported TrueType indexToLocFormat");
        }
        loca_table = loca->second;
        glyf = glyph_data->second;
    }

    void parse_locations() {
        const std::size_t count = static_cast<std::size_t>(glyph_count) + 1U;
        const std::size_t word_size = index_to_loc_format == 0 ? 2U : 4U;
        if (count > std::numeric_limits<std::size_t>::max() / word_size) {
            throw FontError("TrueType loca entry count overflows the host size");
        }
        reader.require(loca_table.offset, count * word_size);
        locations.reserve(count);
        for (std::size_t index = 0U; index < count; ++index) {
            const std::size_t relative = index_to_loc_format == 0
                ? static_cast<std::size_t>(reader.u16(loca_table.offset + index * 2U)) * 2U
                : static_cast<std::size_t>(reader.u32(loca_table.offset + index * 4U));
            if (relative > glyf.length || (!locations.empty() && relative < locations.back())) {
                throw FontError("TrueType loca offsets are outside or not monotonic within glyf");
            }
            locations.push_back(relative);
        }
    }

    [[nodiscard]] std::shared_ptr<const GlyphOutline> outline(const std::uint16_t glyph) const {
        if (glyph >= glyph_count) throw std::out_of_range("glyph id exceeds the TrueType glyph count");
        std::scoped_lock lock(mutex);
        std::set<std::uint16_t> stack;
        return resolve(glyph, stack, 0U);
    }

    [[nodiscard]] std::shared_ptr<const GlyphOutline> resolve(
        const std::uint16_t glyph,
        std::set<std::uint16_t>& stack,
        const std::size_t depth
    ) const {
        if (cache[glyph]) return cache[glyph];
        if (depth >= maximum_component_depth || !stack.insert(glyph).second) {
            throw FontError("TrueType compound glyph nesting is cyclic or too deep");
        }
        std::shared_ptr<const GlyphOutline> value;
        try {
            value = parse_glyph(glyph, stack, depth);
        } catch (...) {
            stack.erase(glyph);
            throw;
        }
        stack.erase(glyph);
        cache[glyph] = value;
        return value;
    }

    [[nodiscard]] std::shared_ptr<const GlyphOutline> parse_glyph(
        const std::uint16_t glyph,
        std::set<std::uint16_t>& stack,
        const std::size_t depth
    ) const {
        const std::size_t relative_start = locations[glyph];
        const std::size_t relative_end = locations[static_cast<std::size_t>(glyph) + 1U];
        if (relative_start == relative_end) return std::make_shared<const GlyphOutline>();
        if (relative_end - relative_start < glyph_header_size) {
            throw FontError("TrueType glyph is shorter than its header");
        }
        const std::size_t start = glyf.offset + relative_start;
        const std::size_t end = glyf.offset + relative_end;
        reader.require(start, relative_end - relative_start);
        const std::int16_t contour_count = reader.i16(start);
        const GlyphBounds header_bounds{
            static_cast<double>(reader.i16(start + 2U)),
            static_cast<double>(reader.i16(start + 4U)),
            static_cast<double>(reader.i16(start + 6U)),
            static_cast<double>(reader.i16(start + 8U)),
        };
        return contour_count >= 0
            ? parse_simple(start, end, static_cast<std::uint16_t>(contour_count), header_bounds)
            : parse_compound(start, end, header_bounds, stack, depth);
    }

    [[nodiscard]] std::shared_ptr<const GlyphOutline> parse_simple(
        const std::size_t start,
        const std::size_t end,
        const std::uint16_t contour_count,
        const GlyphBounds bounds
    ) const {
        const std::size_t contour_bytes = static_cast<std::size_t>(contour_count) * 2U;
        require_glyph_range(start, end, glyph_header_size, contour_bytes + 2U);
        std::vector<std::uint16_t> contour_ends;
        contour_ends.reserve(contour_count);
        std::int32_t previous = -1;
        for (std::size_t index = 0U; index < contour_count; ++index) {
            const std::uint16_t value = reader.u16(start + glyph_header_size + index * 2U);
            if (static_cast<std::int32_t>(value) <= previous) {
                throw FontError("TrueType simple glyph contour endpoints are not increasing");
            }
            contour_ends.push_back(value);
            previous = value;
        }
        const std::size_t point_count = contour_ends.empty()
            ? 0U
            : static_cast<std::size_t>(contour_ends.back()) + 1U;
        if (point_count > maximum_points_per_glyph) throw FontError("TrueType glyph point limit exceeded");
        std::size_t cursor = start + glyph_header_size + contour_bytes;
        const std::uint16_t instruction_length = reader.u16(cursor);
        cursor += 2U;
        require_absolute(end, cursor, instruction_length);
        cursor += instruction_length;
        if (point_count == 0U) return std::make_shared<const GlyphOutline>();

        std::vector<std::uint8_t> flags;
        flags.reserve(point_count);
        while (flags.size() < point_count) {
            require_absolute(end, cursor, 1U);
            const std::uint8_t flag = reader.u8(cursor++);
            flags.push_back(flag);
            if ((flag & flag_repeat) != 0U) {
                require_absolute(end, cursor, 1U);
                const std::uint8_t repeats = reader.u8(cursor++);
                if (static_cast<std::size_t>(repeats) > point_count - flags.size()) {
                    throw FontError("TrueType simple glyph flag repeat exceeds its point count");
                }
                flags.insert(flags.end(), repeats, flag);
            }
        }

        std::vector<double> xs(point_count);
        std::vector<double> ys(point_count);
        std::int32_t x = 0;
        for (std::size_t index = 0U; index < point_count; ++index) {
            const std::uint8_t flag = flags[index];
            if ((flag & flag_x_short) != 0U) {
                require_absolute(end, cursor, 1U);
                const std::int32_t magnitude = reader.u8(cursor++);
                x += (flag & flag_x_same_or_positive) != 0U ? magnitude : -magnitude;
            } else if ((flag & flag_x_same_or_positive) == 0U) {
                require_absolute(end, cursor, 2U);
                x += reader.i16(cursor);
                cursor += 2U;
            }
            xs[index] = static_cast<double>(x);
        }
        std::int32_t y = 0;
        for (std::size_t index = 0U; index < point_count; ++index) {
            const std::uint8_t flag = flags[index];
            if ((flag & flag_y_short) != 0U) {
                require_absolute(end, cursor, 1U);
                const std::int32_t magnitude = reader.u8(cursor++);
                y += (flag & flag_y_same_or_positive) != 0U ? magnitude : -magnitude;
            } else if ((flag & flag_y_same_or_positive) == 0U) {
                require_absolute(end, cursor, 2U);
                y += reader.i16(cursor);
                cursor += 2U;
            }
            ys[index] = static_cast<double>(y);
        }

        GlyphOutline result;
        result.bounds = bounds;
        result.contours.reserve(contour_count);
        std::size_t contour_start = 0U;
        for (const std::uint16_t contour_end : contour_ends) {
            GlyphContour contour;
            const std::size_t exclusive_end = static_cast<std::size_t>(contour_end) + 1U;
            contour.points.reserve(exclusive_end - contour_start);
            for (std::size_t index = contour_start; index < exclusive_end; ++index) {
                contour.points.push_back(GlyphPoint{
                    xs[index], ys[index], (flags[index] & flag_on_curve) != 0U,
                });
            }
            contour.segments = build_segments(contour.points);
            result.contours.push_back(std::move(contour));
            contour_start = exclusive_end;
        }
        return std::make_shared<const GlyphOutline>(std::move(result));
    }

    [[nodiscard]] std::shared_ptr<const GlyphOutline> parse_compound(
        const std::size_t start,
        const std::size_t end,
        const GlyphBounds header_bounds,
        std::set<std::uint16_t>& stack,
        const std::size_t depth
    ) const {
        GlyphOutline result;
        result.bounds = header_bounds;
        result.composite = true;
        std::vector<GlyphPoint> constructed_points;
        std::size_t cursor = start + glyph_header_size;
        std::uint16_t flags = 0U;
        std::size_t component_count = 0U;
        do {
            if (++component_count > maximum_components_per_glyph) {
                throw FontError("TrueType compound glyph component limit exceeded");
            }
            require_absolute(end, cursor, 4U);
            flags = reader.u16(cursor);
            const std::uint16_t component_glyph = reader.u16(cursor + 2U);
            cursor += 4U;
            if (component_glyph >= glyph_count) {
                throw FontError("TrueType compound glyph references an invalid glyph id");
            }
            const bool words = (flags & args_are_words) != 0U;
            const bool xy_arguments = (flags & args_are_xy) != 0U;
            const std::size_t argument_bytes = words ? 4U : 2U;
            require_absolute(end, cursor, argument_bytes);
            const std::int32_t argument1 = words
                ? (xy_arguments ? reader.i16(cursor) : reader.u16(cursor))
                : (xy_arguments ? reader.i8(cursor) : reader.u8(cursor));
            const std::int32_t argument2 = words
                ? (xy_arguments ? reader.i16(cursor + 2U) : reader.u16(cursor + 2U))
                : (xy_arguments ? reader.i8(cursor + 1U) : reader.u8(cursor + 1U));
            cursor += argument_bytes;

            Transform transform;
            if ((flags & has_scale) != 0U) {
                require_absolute(end, cursor, 2U);
                transform.xx = transform.yy = reader.f2dot14(cursor);
                cursor += 2U;
            } else if ((flags & has_xy_scale) != 0U) {
                require_absolute(end, cursor, 4U);
                transform.xx = reader.f2dot14(cursor);
                transform.yy = reader.f2dot14(cursor + 2U);
                cursor += 4U;
            } else if ((flags & has_matrix) != 0U) {
                require_absolute(end, cursor, 8U);
                transform.xx = reader.f2dot14(cursor);
                transform.xy = reader.f2dot14(cursor + 2U);
                transform.yx = reader.f2dot14(cursor + 4U);
                transform.yy = reader.f2dot14(cursor + 6U);
                cursor += 8U;
            }

            const std::shared_ptr<const GlyphOutline> component =
                resolve(component_glyph, stack, depth + 1U);
            if (xy_arguments) {
                const double offset_x = static_cast<double>(argument1);
                const double offset_y = static_cast<double>(argument2);
                if ((flags & scaled_component_offset) != 0U &&
                    (flags & unscaled_component_offset) == 0U) {
                    const auto scaled = transform.apply_vector(offset_x, offset_y);
                    transform.dx = scaled.first;
                    transform.dy = scaled.second;
                } else {
                    transform.dx = offset_x;
                    transform.dy = offset_y;
                }
            } else {
                const std::size_t parent_index = static_cast<std::size_t>(argument1);
                const std::size_t child_index = static_cast<std::size_t>(argument2);
                std::vector<GlyphPoint> child_points;
                for (const GlyphContour& contour : component->contours) {
                    for (const GlyphPoint& point : contour.points) child_points.push_back(transform.apply(point));
                }
                if (parent_index >= constructed_points.size() || child_index >= child_points.size()) {
                    throw FontError("TrueType compound glyph point-matched anchor is invalid");
                }
                transform.dx = constructed_points[parent_index].x - child_points[child_index].x;
                transform.dy = constructed_points[parent_index].y - child_points[child_index].y;
            }

            for (const GlyphContour& contour : component->contours) {
                GlyphContour next = transformed(contour, transform);
                constructed_points.insert(
                    constructed_points.end(), next.points.begin(), next.points.end()
                );
                if (constructed_points.size() > maximum_points_per_glyph) {
                    throw FontError("TrueType compound glyph expanded point limit exceeded");
                }
                result.contours.push_back(std::move(next));
            }
        } while ((flags & more_components) != 0U);

        if ((flags & has_instructions) != 0U) {
            require_absolute(end, cursor, 2U);
            const std::uint16_t instruction_length = reader.u16(cursor);
            cursor += 2U;
            require_absolute(end, cursor, instruction_length);
        }
        if (!result.bounds.has_value()) result.bounds = bounds_from_points(result.contours);
        return std::make_shared<const GlyphOutline>(std::move(result));
    }

private:
    Table loca_table;
    std::int16_t index_to_loc_format = 0;

    static void require_absolute(
        const std::size_t end,
        const std::size_t cursor,
        const std::size_t count
    ) {
        if (cursor > end || count > end - cursor) {
            throw FontError("TrueType glyph payload is truncated");
        }
    }

    static void require_glyph_range(
        const std::size_t start,
        const std::size_t end,
        const std::size_t relative,
        const std::size_t count
    ) {
        if (relative > end - start || count > end - start - relative) {
            throw FontError("TrueType glyph payload is truncated");
        }
    }
};

TrueTypeOutlineSource::TrueTypeOutlineSource(
    const std::span<const std::uint8_t> bytes,
    const std::uint16_t glyph_count
) : implementation_(std::make_unique<Impl>(bytes, glyph_count)) {}

TrueTypeOutlineSource::~TrueTypeOutlineSource() = default;
TrueTypeOutlineSource::TrueTypeOutlineSource(TrueTypeOutlineSource&&) noexcept = default;
TrueTypeOutlineSource& TrueTypeOutlineSource::operator=(TrueTypeOutlineSource&&) noexcept = default;

std::shared_ptr<const GlyphOutline> TrueTypeOutlineSource::outline(const std::uint16_t glyph) const {
    return implementation_->outline(glyph);
}

} // namespace strata::font
