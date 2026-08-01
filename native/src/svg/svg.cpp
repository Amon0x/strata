#include "svg/svg.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <ranges>
#include <string>
#include <unordered_set>
#include <utility>

namespace strata::svg {
namespace {

struct Attribute final {
    std::string name;
    std::string value;
    std::size_t offset = 0U;
};

struct XmlNode final {
    std::string name;
    std::size_t offset = 0U;
    std::vector<Attribute> attributes;
    std::vector<XmlNode> children;
};

[[nodiscard]] bool ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\r' || value == '\n';
}

[[nodiscard]] bool name_start(const char value) noexcept {
    return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || value == '_' ||
           value == ':';
}

[[nodiscard]] bool name_character(const char value) noexcept {
    return name_start(value) || (value >= '0' && value <= '9') || value == '-' || value == '.';
}

[[nodiscard]] std::string lowercase(std::string_view value) {
    std::string result(value);
    std::ranges::transform(result, result.begin(), [](const char character) {
        if (character >= 'A' && character <= 'Z') {
            return static_cast<char>(character - 'A' + 'a');
        }
        return character;
    });
    return result;
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept {
    while (!value.empty() && ascii_space(value.front()))
        value.remove_prefix(1U);
    while (!value.empty() && ascii_space(value.back()))
        value.remove_suffix(1U);
    return value;
}

void append_utf8(std::string& destination, const std::uint32_t codepoint) {
    if (codepoint <= 0x7FU) {
        destination.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FFU) {
        destination.push_back(static_cast<char>(0xC0U | (codepoint >> 6U)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else if (codepoint <= 0xFFFFU) {
        destination.push_back(static_cast<char>(0xE0U | (codepoint >> 12U)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    } else {
        destination.push_back(static_cast<char>(0xF0U | (codepoint >> 18U)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3FU)));
        destination.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3FU)));
        destination.push_back(static_cast<char>(0x80U | (codepoint & 0x3FU)));
    }
}

class XmlParser final {
  public:
    XmlParser(const std::string_view source, const ParseLimits& limits)
        : source_(source), limits_(limits) {}

    [[nodiscard]] XmlNode document() {
        if (source_.size() > limits_.maximum_input_bytes) {
            fail(0U, "SVG exceeds the input byte limit");
        }
        if (source_.starts_with("\xEF\xBB\xBF"))
            position_ = 3U;
        skip_space();
        if (starts_with("<?xml") && position_ + 5U < source_.size() &&
            (ascii_space(source_[position_ + 5U]) || source_[position_ + 5U] == '?')) {
            const std::size_t end = source_.find("?>", position_ + 5U);
            if (end == std::string_view::npos)
                fail(position_, "unterminated XML declaration");
            position_ = end + 2U;
        }
        skip_misc();
        if (position_ >= source_.size())
            fail(position_, "SVG document is empty");
        XmlNode root = element(1U);
        skip_misc();
        if (position_ != source_.size())
            fail(position_, "content follows the root SVG element");
        return root;
    }

  private:
    [[noreturn]] void fail(const std::size_t offset, const std::string_view message) const {
        throw ParseError(offset, std::string(message));
    }

    [[nodiscard]] bool starts_with(const std::string_view value) const noexcept {
        return source_.substr(position_).starts_with(value);
    }

    void skip_space() noexcept {
        while (position_ < source_.size() && ascii_space(source_[position_]))
            ++position_;
    }

    void skip_comment() {
        const std::size_t end = source_.find("-->", position_ + 4U);
        if (end == std::string_view::npos)
            fail(position_, "unterminated XML comment");
        position_ = end + 3U;
    }

    void skip_misc() {
        while (true) {
            skip_space();
            if (starts_with("<!--")) {
                skip_comment();
                continue;
            }
            break;
        }
    }

    [[nodiscard]] std::string parse_name() {
        if (position_ >= source_.size() || !name_start(source_[position_])) {
            fail(position_, "expected an XML name");
        }
        const std::size_t begin = position_++;
        while (position_ < source_.size() && name_character(source_[position_]))
            ++position_;
        return std::string(source_.substr(begin, position_ - begin));
    }

    [[nodiscard]] std::uint32_t numeric_entity(const std::string_view digits, const int base,
                                               const std::size_t offset) const {
        if (digits.empty())
            fail(offset, "empty numeric character reference");
        std::uint32_t value = 0U;
        const auto result =
            std::from_chars(digits.data(), digits.data() + digits.size(), value, base);
        if (result.ec != std::errc{} || result.ptr != digits.data() + digits.size() ||
            value > 0x10FFFFU || (value >= 0xD800U && value <= 0xDFFFU) || value == 0U) {
            fail(offset, "invalid numeric character reference");
        }
        return value;
    }

    [[nodiscard]] std::string decode_attribute(const std::string_view encoded,
                                               const std::size_t source_offset) const {
        std::string decoded;
        decoded.reserve(encoded.size());
        std::size_t cursor = 0U;
        while (cursor < encoded.size()) {
            if (encoded[cursor] != '&') {
                decoded.push_back(encoded[cursor++]);
                continue;
            }
            const std::size_t semicolon = encoded.find(';', cursor + 1U);
            if (semicolon == std::string_view::npos) {
                fail(source_offset + cursor, "unterminated character reference");
            }
            const std::string_view entity = encoded.substr(cursor + 1U, semicolon - cursor - 1U);
            if (entity == "amp") {
                decoded.push_back('&');
            } else if (entity == "lt") {
                decoded.push_back('<');
            } else if (entity == "gt") {
                decoded.push_back('>');
            } else if (entity == "quot") {
                decoded.push_back('"');
            } else if (entity == "apos") {
                decoded.push_back('\'');
            } else if (entity.starts_with("#x") || entity.starts_with("#X")) {
                append_utf8(decoded, numeric_entity(entity.substr(2U), 16, source_offset + cursor));
            } else if (entity.starts_with('#')) {
                append_utf8(decoded, numeric_entity(entity.substr(1U), 10, source_offset + cursor));
            } else {
                fail(source_offset + cursor, "named XML entities are not supported");
            }
            cursor = semicolon + 1U;
        }
        return decoded;
    }

    [[nodiscard]] Attribute attribute() {
        Attribute result;
        result.offset = position_;
        result.name = parse_name();
        skip_space();
        if (position_ >= source_.size() || source_[position_] != '=') {
            fail(position_, "expected '=' after an SVG attribute name");
        }
        ++position_;
        skip_space();
        if (position_ >= source_.size() ||
            (source_[position_] != '"' && source_[position_] != '\'')) {
            fail(position_, "SVG attribute values must be quoted");
        }
        const char quote = source_[position_++];
        const std::size_t value_begin = position_;
        while (position_ < source_.size() && source_[position_] != quote) {
            if (source_[position_] == '<')
                fail(position_, "'<' is invalid in an attribute value");
            ++position_;
        }
        if (position_ >= source_.size())
            fail(value_begin, "unterminated SVG attribute value");
        result.value =
            decode_attribute(source_.substr(value_begin, position_ - value_begin), value_begin);
        ++position_;
        return result;
    }

    [[nodiscard]] XmlNode element(const std::size_t depth) {
        if (depth > limits_.maximum_nesting_depth) {
            fail(position_, "SVG exceeds the element nesting limit");
        }
        if (++element_count_ > limits_.maximum_elements) {
            fail(position_, "SVG exceeds the element count limit");
        }
        if (position_ >= source_.size() || source_[position_] != '<') {
            fail(position_, "expected an SVG element");
        }
        if (starts_with("<!")) {
            fail(position_, "DOCTYPE, ENTITY, CDATA, and other declarations are forbidden");
        }
        if (starts_with("<?"))
            fail(position_, "processing instructions are forbidden");
        XmlNode node;
        node.offset = position_++;
        node.name = parse_name();

        std::unordered_set<std::string> names;
        while (true) {
            skip_space();
            if (position_ >= source_.size())
                fail(node.offset, "unterminated SVG element");
            if (starts_with("/>")) {
                position_ += 2U;
                return node;
            }
            if (source_[position_] == '>') {
                ++position_;
                break;
            }
            if (node.attributes.size() >= limits_.maximum_attributes_per_element) {
                fail(position_, "SVG element exceeds the attribute count limit");
            }
            Attribute parsed = attribute();
            if (!names.insert(parsed.name).second) {
                fail(parsed.offset, "duplicate SVG attribute");
            }
            node.attributes.push_back(std::move(parsed));
        }

        while (true) {
            if (position_ >= source_.size())
                fail(node.offset, "unterminated SVG element");
            if (starts_with("</")) {
                position_ += 2U;
                const std::size_t close_offset = position_;
                const std::string closing_name = parse_name();
                if (closing_name != node.name)
                    fail(close_offset, "mismatched SVG closing tag");
                skip_space();
                if (position_ >= source_.size() || source_[position_] != '>') {
                    fail(position_, "expected '>' after an SVG closing tag");
                }
                ++position_;
                return node;
            }
            if (starts_with("<!--")) {
                skip_comment();
                continue;
            }
            if (source_[position_] == '<') {
                node.children.push_back(element(depth + 1U));
                continue;
            }
            const std::size_t text_begin = position_;
            while (position_ < source_.size() && source_[position_] != '<')
                ++position_;
            const std::string_view text = source_.substr(text_begin, position_ - text_begin);
            if (std::ranges::any_of(text, [](const char value) { return !ascii_space(value); })) {
                const std::string local = local_name(node.name, node.offset);
                if (local != "title" && local != "desc" && local != "metadata") {
                    fail(text_begin, "text is not allowed in this static SVG element");
                }
            }
        }
    }

    [[nodiscard]] static std::string local_name(const std::string_view qualified,
                                                const std::size_t offset) {
        const std::size_t colon = qualified.find(':');
        if (colon == std::string_view::npos)
            return std::string(qualified);
        if (qualified.substr(0U, colon) != "svg" ||
            qualified.find(':', colon + 1U) != std::string_view::npos) {
            throw ParseError(offset, "only the optional 'svg:' element namespace is supported");
        }
        return std::string(qualified.substr(colon + 1U));
    }

    std::string_view source_;
    const ParseLimits& limits_;
    std::size_t position_ = 0U;
    std::size_t element_count_ = 0U;
};

[[nodiscard]] std::string local_name(const std::string_view qualified, const std::size_t offset) {
    const std::size_t colon = qualified.find(':');
    if (colon == std::string_view::npos)
        return std::string(qualified);
    if (qualified.substr(0U, colon) != "svg" ||
        qualified.find(':', colon + 1U) != std::string_view::npos) {
        throw ParseError(offset, "only the optional 'svg:' element namespace is supported");
    }
    return std::string(qualified.substr(colon + 1U));
}

[[nodiscard]] const Attribute* find_attribute(const XmlNode& node,
                                              const std::string_view name) noexcept {
    const auto found = std::ranges::find(node.attributes, name, &Attribute::name);
    return found == node.attributes.end() ? nullptr : &*found;
}

[[noreturn]] void fail(const XmlNode& node, const std::string_view message) {
    throw ParseError(node.offset, std::string(message));
}

[[noreturn]] void fail(const Attribute& attribute, const std::string_view message) {
    throw ParseError(attribute.offset, std::string(message));
}

[[nodiscard]] double parse_number(const Attribute& attribute, std::string_view value) {
    value = trim(value);
    double parsed = 0.0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (value.empty() || result.ec != std::errc{} || result.ptr != value.data() + value.size() ||
        !std::isfinite(parsed)) {
        fail(attribute, "invalid finite SVG number");
    }
    return parsed;
}

[[nodiscard]] double parse_length(const Attribute& attribute) {
    std::string_view value = trim(attribute.value);
    if (value.ends_with("px"))
        value.remove_suffix(2U);
    return parse_number(attribute, value);
}

class NumberList final {
  public:
    NumberList(const std::string_view value, const std::size_t offset)
        : value_(value), source_offset_(offset) {}

    [[nodiscard]] bool empty() {
        skip_separator();
        return position_ == value_.size();
    }

    [[nodiscard]] double number() {
        skip_separator();
        if (position_ >= value_.size())
            error("expected an SVG number");
        const char* const begin = value_.data() + position_;
        double result = 0.0;
        const auto parsed = std::from_chars(begin, value_.data() + value_.size(), result);
        if (parsed.ec != std::errc{} || parsed.ptr == begin || !std::isfinite(result)) {
            error("invalid finite SVG number");
        }
        position_ = static_cast<std::size_t>(parsed.ptr - value_.data());
        return result;
    }

    void finish() {
        skip_separator();
        if (position_ != value_.size())
            error("unexpected data after SVG numbers");
    }

    [[nodiscard]] std::size_t position() const noexcept {
        return position_;
    }

  private:
    [[noreturn]] void error(const std::string_view message) const {
        throw ParseError(source_offset_ + position_, std::string(message));
    }

    void skip_separator() noexcept {
        while (position_ < value_.size() &&
               (ascii_space(value_[position_]) || value_[position_] == ',')) {
            ++position_;
        }
    }

    std::string_view value_;
    std::size_t source_offset_ = 0U;
    std::size_t position_ = 0U;
};

[[nodiscard]] AffineTransform multiply(const AffineTransform& left,
                                       const AffineTransform& right) noexcept {
    return AffineTransform{
        left.a * right.a + left.c * right.b,          left.b * right.a + left.d * right.b,
        left.a * right.c + left.c * right.d,          left.b * right.c + left.d * right.d,
        left.a * right.e + left.c * right.f + left.e, left.b * right.e + left.d * right.f + left.f,
    };
}

[[nodiscard]] AffineTransform translation(const double x, const double y) noexcept {
    return AffineTransform{1.0, 0.0, 0.0, 1.0, x, y};
}

[[nodiscard]] AffineTransform parse_transform(const Attribute& attribute) {
    const std::string_view source = attribute.value;
    std::size_t position = 0U;
    AffineTransform result;
    const auto transform_error = [&](const std::string_view message) -> void {
        throw ParseError(attribute.offset + position, std::string(message));
    };
    while (true) {
        while (position < source.size() &&
               (ascii_space(source[position]) || source[position] == ',')) {
            ++position;
        }
        if (position == source.size())
            return result;
        const std::size_t name_begin = position;
        while (position < source.size() && ((source[position] >= 'A' && source[position] <= 'Z') ||
                                            (source[position] >= 'a' && source[position] <= 'z'))) {
            ++position;
        }
        if (name_begin == position)
            transform_error("expected an SVG transform name");
        const std::string name = lowercase(source.substr(name_begin, position - name_begin));
        while (position < source.size() && ascii_space(source[position]))
            ++position;
        if (position >= source.size() || source[position] != '(') {
            transform_error("expected '(' after an SVG transform name");
        }
        const std::size_t arguments_begin = ++position;
        const std::size_t close = source.find(')', position);
        if (close == std::string_view::npos)
            transform_error("unterminated SVG transform");
        NumberList arguments(source.substr(arguments_begin, close - arguments_begin),
                             attribute.offset + arguments_begin);
        std::vector<double> values;
        while (!arguments.empty())
            values.push_back(arguments.number());
        arguments.finish();
        position = close + 1U;

        AffineTransform operation;
        if (name == "matrix" && values.size() == 6U) {
            operation =
                AffineTransform{values[0], values[1], values[2], values[3], values[4], values[5]};
        } else if (name == "translate" && (values.size() == 1U || values.size() == 2U)) {
            operation = translation(values[0], values.size() == 2U ? values[1] : 0.0);
        } else if (name == "scale" && (values.size() == 1U || values.size() == 2U)) {
            operation.a = values[0];
            operation.d = values.size() == 2U ? values[1] : values[0];
        } else if (name == "rotate" && (values.size() == 1U || values.size() == 3U)) {
            const double radians = values[0] * std::numbers::pi / 180.0;
            const double cosine = std::cos(radians);
            const double sine = std::sin(radians);
            const AffineTransform rotation{cosine, sine, -sine, cosine, 0.0, 0.0};
            if (values.size() == 1U) {
                operation = rotation;
            } else {
                operation = multiply(multiply(translation(values[1], values[2]), rotation),
                                     translation(-values[1], -values[2]));
            }
        } else if ((name == "skewx" || name == "skewy") && values.size() == 1U) {
            const double tangent = std::tan(values[0] * std::numbers::pi / 180.0);
            if (!std::isfinite(tangent))
                transform_error("invalid SVG skew transform");
            if (name == "skewx") {
                operation.c = tangent;
            } else {
                operation.b = tangent;
            }
        } else {
            transform_error("unsupported transform or invalid transform argument count");
        }
        result = multiply(result, operation);
    }
}

[[nodiscard]] std::uint8_t hex_nibble(const char value, const Attribute& attribute) {
    if (value >= '0' && value <= '9')
        return static_cast<std::uint8_t>(value - '0');
    if (value >= 'a' && value <= 'f')
        return static_cast<std::uint8_t>(value - 'a' + 10);
    if (value >= 'A' && value <= 'F')
        return static_cast<std::uint8_t>(value - 'A' + 10);
    fail(attribute, "invalid hexadecimal SVG color");
}

[[nodiscard]] std::uint8_t hex_byte(const char high, const char low, const Attribute& attribute) {
    return static_cast<std::uint8_t>((hex_nibble(high, attribute) << 4U) |
                                     hex_nibble(low, attribute));
}

[[nodiscard]] Color parse_color(const Attribute& attribute, const Color current_color) {
    const std::string_view raw = trim(attribute.value);
    const std::string value = lowercase(raw);
    if (value == "currentcolor")
        return current_color;
    if (value.starts_with('#')) {
        if (value.size() == 4U || value.size() == 5U) {
            const std::uint8_t red = hex_nibble(value[1], attribute);
            const std::uint8_t green = hex_nibble(value[2], attribute);
            const std::uint8_t blue = hex_nibble(value[3], attribute);
            const std::uint8_t alpha = value.size() == 5U ? hex_nibble(value[4], attribute) : 15U;
            return Color{
                static_cast<std::uint8_t>(red * 17U),
                static_cast<std::uint8_t>(green * 17U),
                static_cast<std::uint8_t>(blue * 17U),
                static_cast<std::uint8_t>(alpha * 17U),
            };
        }
        if (value.size() == 7U || value.size() == 9U) {
            return Color{
                hex_byte(value[1], value[2], attribute),
                hex_byte(value[3], value[4], attribute),
                hex_byte(value[5], value[6], attribute),
                value.size() == 9U ? hex_byte(value[7], value[8], attribute)
                                   : static_cast<std::uint8_t>(255U),
            };
        }
        fail(attribute, "SVG hex colors must use 3, 4, 6, or 8 digits");
    }
    static constexpr std::array<std::pair<std::string_view, Color>, 17U> named{{
        {"black", {0U, 0U, 0U, 255U}},
        {"silver", {192U, 192U, 192U, 255U}},
        {"gray", {128U, 128U, 128U, 255U}},
        {"white", {255U, 255U, 255U, 255U}},
        {"maroon", {128U, 0U, 0U, 255U}},
        {"red", {255U, 0U, 0U, 255U}},
        {"purple", {128U, 0U, 128U, 255U}},
        {"fuchsia", {255U, 0U, 255U, 255U}},
        {"green", {0U, 128U, 0U, 255U}},
        {"lime", {0U, 255U, 0U, 255U}},
        {"olive", {128U, 128U, 0U, 255U}},
        {"yellow", {255U, 255U, 0U, 255U}},
        {"navy", {0U, 0U, 128U, 255U}},
        {"blue", {0U, 0U, 255U, 255U}},
        {"teal", {0U, 128U, 128U, 255U}},
        {"aqua", {0U, 255U, 255U, 255U}},
        {"transparent", {0U, 0U, 0U, 0U}},
    }};
    const auto found = std::ranges::find(named, value, &decltype(named)::value_type::first);
    if (found != named.end())
        return found->second;
    fail(attribute, "unsupported SVG color; use a static hex, basic named, or currentColor paint");
}

[[nodiscard]] double opacity(const Attribute& attribute) {
    const double value = parse_number(attribute, attribute.value);
    if (value < 0.0 || value > 1.0)
        fail(attribute, "SVG opacity must be between zero and one");
    return value;
}

[[nodiscard]] bool namespace_attribute(const std::string_view name) noexcept {
    return name == "xmlns" || name.starts_with("xmlns:");
}

[[nodiscard]] bool metadata_attribute(const std::string_view name) noexcept {
    return name == "id" || name == "role" || name == "aria-hidden" || name == "focusable";
}

[[nodiscard]] bool presentation_attribute(const std::string_view name) noexcept {
    return name == "fill" || name == "stroke" || name == "fill-opacity" ||
           name == "stroke-opacity" || name == "opacity" || name == "fill-rule" ||
           name == "stroke-width" || name == "stroke-linecap" || name == "stroke-linejoin" ||
           name == "stroke-miterlimit" || name == "transform";
}

void reject_intrinsically_unsafe_attribute(const Attribute& attribute) {
    const std::string name = lowercase(attribute.name);
    if (name == "style" || name == "class" || name == "href" || name.ends_with(":href") ||
        name.starts_with("on")) {
        fail(attribute, "CSS, references, and event attributes are forbidden in static SVG");
    }
}

void validate_attributes(const XmlNode& node, const std::span<const std::string_view> geometry,
                         const bool root) {
    for (const Attribute& attribute : node.attributes) {
        reject_intrinsically_unsafe_attribute(attribute);
        if (namespace_attribute(attribute.name) || metadata_attribute(attribute.name) ||
            presentation_attribute(attribute.name) ||
            std::ranges::find(geometry, attribute.name) != geometry.end() ||
            (root && attribute.name == "version")) {
            continue;
        }
        fail(attribute, "unsupported SVG attribute");
    }
}

[[nodiscard]] PaintStyle parse_paint(const XmlNode& node, PaintStyle style,
                                     const Color current_color, const bool container) {
    if (const Attribute* const value = find_attribute(node, "fill")) {
        if (lowercase(trim(value->value)) == "none") {
            style.has_fill = false;
        } else {
            style.has_fill = true;
            style.fill = parse_color(*value, current_color);
        }
    }
    if (const Attribute* const value = find_attribute(node, "stroke")) {
        if (lowercase(trim(value->value)) == "none") {
            style.has_stroke = false;
        } else {
            style.has_stroke = true;
            style.stroke = parse_color(*value, current_color);
        }
    }
    if (const Attribute* const value = find_attribute(node, "fill-opacity")) {
        style.fill_opacity = opacity(*value);
    }
    if (const Attribute* const value = find_attribute(node, "stroke-opacity")) {
        style.stroke_opacity = opacity(*value);
    }
    if (const Attribute* const value = find_attribute(node, "opacity")) {
        if (container) {
            fail(*value,
                 "group opacity requires isolated compositing and is not in the static subset");
        }
        style.opacity = opacity(*value);
    } else {
        style.opacity = 1.0;
    }
    if (const Attribute* const value = find_attribute(node, "stroke-width")) {
        style.stroke_width = parse_length(*value);
        if (style.stroke_width < 0.0)
            fail(*value, "SVG stroke width cannot be negative");
    }
    if (const Attribute* const value = find_attribute(node, "stroke-miterlimit")) {
        style.miter_limit = parse_number(*value, value->value);
        if (style.miter_limit < 1.0)
            fail(*value, "SVG miter limit must be at least one");
    }
    if (const Attribute* const value = find_attribute(node, "fill-rule")) {
        const std::string rule = lowercase(trim(value->value));
        if (rule == "nonzero")
            style.fill_rule = FillRule::nonzero;
        else if (rule == "evenodd")
            style.fill_rule = FillRule::evenodd;
        else
            fail(*value, "SVG fill rule must be nonzero or evenodd");
    }
    if (const Attribute* const value = find_attribute(node, "stroke-linecap")) {
        const std::string cap = lowercase(trim(value->value));
        if (cap == "butt")
            style.line_cap = LineCap::butt;
        else if (cap == "round")
            style.line_cap = LineCap::round;
        else if (cap == "square")
            style.line_cap = LineCap::square;
        else
            fail(*value, "unsupported SVG line cap");
    }
    if (const Attribute* const value = find_attribute(node, "stroke-linejoin")) {
        const std::string join = lowercase(trim(value->value));
        if (join == "miter")
            style.line_join = LineJoin::miter;
        else if (join == "round")
            style.line_join = LineJoin::round;
        else if (join == "bevel")
            style.line_join = LineJoin::bevel;
        else
            fail(*value, "unsupported SVG line join");
    }
    return style;
}

class PathBuilder final {
  public:
    explicit PathBuilder(const std::size_t maximum_segments)
        : maximum_segments_(maximum_segments) {}

    void move(const Point point) {
        add(PathSegment{PathVerb::move, {}, {}, point});
    }
    void line(const Point point) {
        add(PathSegment{PathVerb::line, {}, {}, point});
    }
    void cubic(const Point first, const Point second, const Point point) {
        add(PathSegment{PathVerb::cubic, first, second, point});
    }
    void close(const Point point) {
        add(PathSegment{PathVerb::close, {}, {}, point});
    }
    [[nodiscard]] Path finish() && {
        return Path{std::move(segments_)};
    }

  private:
    void add(PathSegment segment) {
        if (segments_.size() >= maximum_segments_) {
            throw ParseError(0U, "SVG exceeds the path segment limit");
        }
        segments_.push_back(segment);
    }

    std::size_t maximum_segments_ = 0U;
    std::vector<PathSegment> segments_;
};

[[nodiscard]] Point add(const Point left, const Point right) noexcept {
    return Point{left.x + right.x, left.y + right.y};
}

[[nodiscard]] Point subtract(const Point left, const Point right) noexcept {
    return Point{left.x - right.x, left.y - right.y};
}

[[nodiscard]] Point scale(const Point point, const double factor) noexcept {
    return Point{point.x * factor, point.y * factor};
}

void append_quadratic(PathBuilder& builder, const Point from, const Point control, const Point to) {
    builder.cubic(add(from, scale(subtract(control, from), 2.0 / 3.0)),
                  add(to, scale(subtract(control, to), 2.0 / 3.0)), to);
}

[[nodiscard]] double vector_angle(const Point vector) noexcept {
    return std::atan2(vector.y, vector.x);
}

void append_arc(PathBuilder& builder, const Point from, Point radii, const double rotation_degrees,
                const bool large_arc, const bool sweep, const Point to) {
    if (from == to)
        return;
    radii.x = std::abs(radii.x);
    radii.y = std::abs(radii.y);
    if (radii.x == 0.0 || radii.y == 0.0) {
        builder.line(to);
        return;
    }
    const double phi = std::fmod(rotation_degrees, 360.0) * std::numbers::pi / 180.0;
    const double cosine = std::cos(phi);
    const double sine = std::sin(phi);
    const Point half_delta{(from.x - to.x) * 0.5, (from.y - to.y) * 0.5};
    const Point prime{
        cosine * half_delta.x + sine * half_delta.y,
        -sine * half_delta.x + cosine * half_delta.y,
    };
    double radius_x_squared = radii.x * radii.x;
    double radius_y_squared = radii.y * radii.y;
    const double prime_x_squared = prime.x * prime.x;
    const double prime_y_squared = prime.y * prime.y;
    const double correction =
        prime_x_squared / radius_x_squared + prime_y_squared / radius_y_squared;
    if (correction > 1.0) {
        const double factor = std::sqrt(correction);
        radii = scale(radii, factor);
        radius_x_squared = radii.x * radii.x;
        radius_y_squared = radii.y * radii.y;
    }
    const double denominator =
        radius_x_squared * prime_y_squared + radius_y_squared * prime_x_squared;
    const double numerator =
        std::max(0.0, radius_x_squared * radius_y_squared - radius_x_squared * prime_y_squared -
                          radius_y_squared * prime_x_squared);
    const double sign = large_arc == sweep ? -1.0 : 1.0;
    const double coefficient = denominator == 0.0 ? 0.0 : sign * std::sqrt(numerator / denominator);
    const Point center_prime{
        coefficient * radii.x * prime.y / radii.y,
        coefficient * -radii.y * prime.x / radii.x,
    };
    const Point midpoint{(from.x + to.x) * 0.5, (from.y + to.y) * 0.5};
    const Point center{
        cosine * center_prime.x - sine * center_prime.y + midpoint.x,
        sine * center_prime.x + cosine * center_prime.y + midpoint.y,
    };
    const Point start_vector{
        (prime.x - center_prime.x) / radii.x,
        (prime.y - center_prime.y) / radii.y,
    };
    const Point end_vector{
        (-prime.x - center_prime.x) / radii.x,
        (-prime.y - center_prime.y) / radii.y,
    };
    double start_angle = vector_angle(start_vector);
    double delta = vector_angle(end_vector) - start_angle;
    if (!sweep && delta > 0.0)
        delta -= 2.0 * std::numbers::pi;
    if (sweep && delta < 0.0)
        delta += 2.0 * std::numbers::pi;
    const std::size_t pieces =
        static_cast<std::size_t>(std::ceil(std::abs(delta) / (std::numbers::pi * 0.5)));
    const double piece_delta = delta / static_cast<double>(std::max<std::size_t>(pieces, 1U));

    const auto point_at = [&](const double angle) -> Point {
        return Point{
            center.x + cosine * radii.x * std::cos(angle) - sine * radii.y * std::sin(angle),
            center.y + sine * radii.x * std::cos(angle) + cosine * radii.y * std::sin(angle),
        };
    };
    const auto derivative_at = [&](const double angle) -> Point {
        return Point{
            -cosine * radii.x * std::sin(angle) - sine * radii.y * std::cos(angle),
            -sine * radii.x * std::sin(angle) + cosine * radii.y * std::cos(angle),
        };
    };
    for (std::size_t piece = 0U; piece < pieces; ++piece) {
        const double end_angle = start_angle + piece_delta;
        const double alpha = 4.0 / 3.0 * std::tan(piece_delta * 0.25);
        const Point begin = point_at(start_angle);
        const Point end = piece + 1U == pieces ? to : point_at(end_angle);
        builder.cubic(add(begin, scale(derivative_at(start_angle), alpha)),
                      subtract(end, scale(derivative_at(end_angle), alpha)), end);
        start_angle = end_angle;
    }
}

class PathDataParser final {
  public:
    PathDataParser(const Attribute& attribute, const std::size_t maximum_segments)
        : attribute_(attribute), source_(attribute.value), builder_(maximum_segments) {}

    [[nodiscard]] Path parse() {
        char command = '\0';
        while (true) {
            skip_separators();
            if (position_ == source_.size())
                break;
            if (command_character(source_[position_])) {
                command = source_[position_++];
            } else if (command == '\0' || command == 'Z' || command == 'z') {
                error("expected an SVG path command");
            }
            const char upper =
                static_cast<char>(command >= 'a' && command <= 'z' ? command - 'a' + 'A' : command);
            if (builder_empty_ && upper != 'M') {
                error("SVG path data must begin with a move command");
            }
            execute(command);
        }
        if (builder_empty_)
            error("SVG path data is empty");
        return std::move(builder_).finish();
    }

  private:
    [[noreturn]] void error(const std::string_view message) const {
        throw ParseError(attribute_.offset + position_, std::string(message));
    }

    [[nodiscard]] static bool command_character(const char value) noexcept {
        constexpr std::string_view commands = "MmZzLlHhVvCcSsQqTtAa";
        return commands.find(value) != std::string_view::npos;
    }

    void skip_separators() noexcept {
        while (position_ < source_.size() &&
               (ascii_space(source_[position_]) || source_[position_] == ',')) {
            ++position_;
        }
    }

    [[nodiscard]] bool number_follows() {
        std::size_t cursor = position_;
        while (cursor < source_.size() &&
               (ascii_space(source_[cursor]) || source_[cursor] == ',')) {
            ++cursor;
        }
        if (cursor == source_.size())
            return false;
        const char value = source_[cursor];
        return value == '+' || value == '-' || value == '.' || (value >= '0' && value <= '9');
    }

    [[nodiscard]] double number() {
        skip_separators();
        if (position_ == source_.size())
            error("expected an SVG path number");
        const char* const begin = source_.data() + position_;
        double value = 0.0;
        const auto parsed = std::from_chars(begin, source_.data() + source_.size(), value);
        if (parsed.ec != std::errc{} || parsed.ptr == begin || !std::isfinite(value)) {
            error("invalid finite SVG path number");
        }
        position_ = static_cast<std::size_t>(parsed.ptr - source_.data());
        return value;
    }

    [[nodiscard]] bool flag() {
        const double value = number();
        if (value != 0.0 && value != 1.0)
            error("SVG arc flags must be zero or one");
        return value == 1.0;
    }

    [[nodiscard]] Point point(const bool relative) {
        Point value{number(), number()};
        return relative ? add(current_, value) : value;
    }

    void mark(const char command) noexcept {
        previous_command_ = command;
        builder_empty_ = false;
    }

    void execute(const char command) {
        const bool relative = command >= 'a' && command <= 'z';
        const char upper = static_cast<char>(relative ? command - 'a' + 'A' : command);
        if (upper == 'Z') {
            if (!has_subpath_)
                error("close command has no current SVG subpath");
            builder_.close(subpath_start_);
            current_ = subpath_start_;
            mark(command);
            return;
        }
        if (!number_follows())
            error("SVG path command is missing arguments");

        bool first = true;
        do {
            const Point before = current_;
            if (upper == 'M') {
                const Point destination = point(relative);
                if (first) {
                    builder_.move(destination);
                    subpath_start_ = destination;
                    has_subpath_ = true;
                } else {
                    builder_.line(destination);
                }
                current_ = destination;
            } else if (upper == 'L') {
                const Point destination = point(relative);
                builder_.line(destination);
                current_ = destination;
            } else if (upper == 'H') {
                const double x = number();
                current_.x = relative ? current_.x + x : x;
                builder_.line(current_);
            } else if (upper == 'V') {
                const double y = number();
                current_.y = relative ? current_.y + y : y;
                builder_.line(current_);
            } else if (upper == 'C') {
                const Point first_control = point(relative);
                const Point second_control = point(relative);
                const Point destination = point(relative);
                builder_.cubic(first_control, second_control, destination);
                current_ = destination;
                last_cubic_control_ = second_control;
            } else if (upper == 'S') {
                Point first_control = before;
                const char previous_upper =
                    static_cast<char>(previous_command_ >= 'a' && previous_command_ <= 'z'
                                          ? previous_command_ - 'a' + 'A'
                                          : previous_command_);
                if (previous_upper == 'C' || previous_upper == 'S') {
                    first_control = add(before, subtract(before, last_cubic_control_));
                }
                const Point second_control = point(relative);
                const Point destination = point(relative);
                builder_.cubic(first_control, second_control, destination);
                current_ = destination;
                last_cubic_control_ = second_control;
            } else if (upper == 'Q') {
                const Point control = point(relative);
                const Point destination = point(relative);
                append_quadratic(builder_, before, control, destination);
                current_ = destination;
                last_quadratic_control_ = control;
            } else if (upper == 'T') {
                Point control = before;
                const char previous_upper =
                    static_cast<char>(previous_command_ >= 'a' && previous_command_ <= 'z'
                                          ? previous_command_ - 'a' + 'A'
                                          : previous_command_);
                if (previous_upper == 'Q' || previous_upper == 'T') {
                    control = add(before, subtract(before, last_quadratic_control_));
                }
                const Point destination = point(relative);
                append_quadratic(builder_, before, control, destination);
                current_ = destination;
                last_quadratic_control_ = control;
            } else if (upper == 'A') {
                const Point radii{number(), number()};
                const double rotation = number();
                const bool large_arc = flag();
                const bool sweep = flag();
                const Point destination = point(relative);
                append_arc(builder_, before, radii, rotation, large_arc, sweep, destination);
                current_ = destination;
            } else {
                error("unsupported SVG path command");
            }
            mark(command);
            first = false;
        } while (number_follows());
    }

    const Attribute& attribute_;
    std::string_view source_;
    PathBuilder builder_;
    std::size_t position_ = 0U;
    Point current_;
    Point subpath_start_;
    Point last_cubic_control_;
    Point last_quadratic_control_;
    char previous_command_ = '\0';
    bool has_subpath_ = false;
    bool builder_empty_ = true;
};

[[nodiscard]] double optional_number(const XmlNode& node, const std::string_view name,
                                     const double fallback) {
    const Attribute* const attribute = find_attribute(node, name);
    return attribute == nullptr ? fallback : parse_length(*attribute);
}

[[nodiscard]] Path ellipse_path(const Point center, const double radius_x, const double radius_y,
                                const std::size_t maximum_segments) {
    constexpr double kappa = 0.5522847498307936;
    PathBuilder builder(maximum_segments);
    builder.move(Point{center.x + radius_x, center.y});
    builder.cubic(Point{center.x + radius_x, center.y + kappa * radius_y},
                  Point{center.x + kappa * radius_x, center.y + radius_y},
                  Point{center.x, center.y + radius_y});
    builder.cubic(Point{center.x - kappa * radius_x, center.y + radius_y},
                  Point{center.x - radius_x, center.y + kappa * radius_y},
                  Point{center.x - radius_x, center.y});
    builder.cubic(Point{center.x - radius_x, center.y - kappa * radius_y},
                  Point{center.x - kappa * radius_x, center.y - radius_y},
                  Point{center.x, center.y - radius_y});
    builder.cubic(Point{center.x + kappa * radius_x, center.y - radius_y},
                  Point{center.x + radius_x, center.y - kappa * radius_y},
                  Point{center.x + radius_x, center.y});
    builder.close(Point{center.x + radius_x, center.y});
    return std::move(builder).finish();
}

[[nodiscard]] Path shape_path(const XmlNode& node, const std::string_view name,
                              const std::size_t maximum_segments) {
    if (name == "path") {
        const Attribute* const data = find_attribute(node, "d");
        if (data == nullptr)
            fail(node, "SVG path requires a 'd' attribute");
        return PathDataParser(*data, maximum_segments).parse();
    }
    if (name == "line") {
        PathBuilder builder(maximum_segments);
        builder.move(Point{optional_number(node, "x1", 0.0), optional_number(node, "y1", 0.0)});
        builder.line(Point{optional_number(node, "x2", 0.0), optional_number(node, "y2", 0.0)});
        return std::move(builder).finish();
    }
    if (name == "polyline" || name == "polygon") {
        const Attribute* const points = find_attribute(node, "points");
        if (points == nullptr)
            fail(node, "SVG polyline/polygon requires a points attribute");
        NumberList values(points->value, points->offset);
        PathBuilder builder(maximum_segments);
        bool first = true;
        Point start;
        while (!values.empty()) {
            const Point point{values.number(), values.number()};
            if (first) {
                builder.move(point);
                start = point;
                first = false;
            } else {
                builder.line(point);
            }
        }
        values.finish();
        if (first)
            fail(*points, "SVG points list is empty");
        if (name == "polygon")
            builder.close(start);
        return std::move(builder).finish();
    }
    if (name == "circle" || name == "ellipse") {
        const Point center{optional_number(node, "cx", 0.0), optional_number(node, "cy", 0.0)};
        const double radius_x = optional_number(node, name == "circle" ? "r" : "rx", 0.0);
        const double radius_y = optional_number(node, name == "circle" ? "r" : "ry", 0.0);
        if (radius_x < 0.0 || radius_y < 0.0)
            fail(node, "SVG radii cannot be negative");
        if (radius_x == 0.0 || radius_y == 0.0)
            return {};
        return ellipse_path(center, radius_x, radius_y, maximum_segments);
    }
    if (name == "rect") {
        const double x = optional_number(node, "x", 0.0);
        const double y = optional_number(node, "y", 0.0);
        const double width = optional_number(node, "width", 0.0);
        const double height = optional_number(node, "height", 0.0);
        if (width < 0.0 || height < 0.0)
            fail(node, "SVG rectangle size cannot be negative");
        if (width == 0.0 || height == 0.0)
            return {};
        const Attribute* const rx_attribute = find_attribute(node, "rx");
        const Attribute* const ry_attribute = find_attribute(node, "ry");
        double radius_x = rx_attribute == nullptr
                              ? (ry_attribute == nullptr ? 0.0 : parse_length(*ry_attribute))
                              : parse_length(*rx_attribute);
        double radius_y = ry_attribute == nullptr ? radius_x : parse_length(*ry_attribute);
        if (radius_x < 0.0 || radius_y < 0.0)
            fail(node, "SVG rectangle radii cannot be negative");
        radius_x = std::min(radius_x, width * 0.5);
        radius_y = std::min(radius_y, height * 0.5);
        PathBuilder builder(maximum_segments);
        builder.move(Point{x + radius_x, y});
        builder.line(Point{x + width - radius_x, y});
        if (radius_x > 0.0 && radius_y > 0.0) {
            append_arc(builder, Point{x + width - radius_x, y}, Point{radius_x, radius_y}, 0.0,
                       false, true, Point{x + width, y + radius_y});
        }
        builder.line(Point{x + width, y + height - radius_y});
        if (radius_x > 0.0 && radius_y > 0.0) {
            append_arc(builder, Point{x + width, y + height - radius_y}, Point{radius_x, radius_y},
                       0.0, false, true, Point{x + width - radius_x, y + height});
        }
        builder.line(Point{x + radius_x, y + height});
        if (radius_x > 0.0 && radius_y > 0.0) {
            append_arc(builder, Point{x + radius_x, y + height}, Point{radius_x, radius_y}, 0.0,
                       false, true, Point{x, y + height - radius_y});
        }
        builder.line(Point{x, y + radius_y});
        if (radius_x > 0.0 && radius_y > 0.0) {
            append_arc(builder, Point{x, y + radius_y}, Point{radius_x, radius_y}, 0.0, false, true,
                       Point{x + radius_x, y});
        }
        builder.close(Point{x + radius_x, y});
        return std::move(builder).finish();
    }
    fail(node, "unsupported SVG geometry element");
}

[[nodiscard]] AffineTransform node_transform(const XmlNode& node) {
    const Attribute* const transform = find_attribute(node, "transform");
    return transform == nullptr ? AffineTransform{} : parse_transform(*transform);
}

[[nodiscard]] std::span<const std::string_view> geometry_attributes(const std::string_view name) {
    static constexpr std::array path{std::string_view{"d"}};
    static constexpr std::array line{std::string_view{"x1"}, std::string_view{"y1"},
                                     std::string_view{"x2"}, std::string_view{"y2"}};
    static constexpr std::array points{std::string_view{"points"}};
    static constexpr std::array rect{std::string_view{"x"},     std::string_view{"y"},
                                     std::string_view{"width"}, std::string_view{"height"},
                                     std::string_view{"rx"},    std::string_view{"ry"}};
    static constexpr std::array circle{std::string_view{"cx"}, std::string_view{"cy"},
                                       std::string_view{"r"}};
    static constexpr std::array ellipse{std::string_view{"cx"}, std::string_view{"cy"},
                                        std::string_view{"rx"}, std::string_view{"ry"}};
    static constexpr std::array none{std::string_view{""}};
    if (name == "path")
        return path;
    if (name == "line")
        return line;
    if (name == "polyline" || name == "polygon")
        return points;
    if (name == "rect")
        return rect;
    if (name == "circle")
        return circle;
    if (name == "ellipse")
        return ellipse;
    return std::span<const std::string_view>(none).first(0U);
}

[[nodiscard]] bool shape_name(const std::string_view name) noexcept {
    return name == "path" || name == "line" || name == "polyline" || name == "polygon" ||
           name == "rect" || name == "circle" || name == "ellipse";
}

void visit(const XmlNode& node, const AffineTransform& parent_transform,
           const PaintStyle& parent_paint, const ParseOptions& options, Document& document) {
    const std::string name = local_name(node.name, node.offset);
    if (name == "title" || name == "desc") {
        if (!node.children.empty())
            fail(node, "SVG title and desc cannot contain elements");
        return;
    }
    if (name == "g") {
        validate_attributes(node, {}, false);
        const PaintStyle paint = parse_paint(node, parent_paint, options.current_color, true);
        const AffineTransform transform = multiply(parent_transform, node_transform(node));
        for (const XmlNode& child : node.children) {
            visit(child, transform, paint, options, document);
        }
        return;
    }
    if (!shape_name(name)) {
        fail(node, "unsupported or active SVG element in the static subset");
    }
    if (!node.children.empty())
        fail(node, "SVG geometry elements cannot contain child elements");
    validate_attributes(node, geometry_attributes(name), false);
    Path path = shape_path(node, name, options.limits.maximum_path_segments);
    if (path.empty())
        return;
    DrawCommand command;
    command.path = std::move(path);
    command.transform = multiply(parent_transform, node_transform(node));
    command.paint = parse_paint(node, parent_paint, options.current_color, false);
    document.commands.push_back(std::move(command));
}

[[nodiscard]] PreserveAspectRatio parse_aspect_ratio(const XmlNode& root) {
    const Attribute* const attribute = find_attribute(root, "preserveAspectRatio");
    if (attribute == nullptr)
        return {};
    std::string value = std::string(trim(attribute->value));
    if (value.starts_with("defer "))
        value = std::string(trim(std::string_view(value).substr(6U)));
    if (lowercase(value) == "none") {
        return PreserveAspectRatio{AspectAlign::none, AspectAlign::none, false};
    }
    const std::size_t separator = value.find_first_of(" \t\r\n");
    const std::string alignment = value.substr(0U, separator);
    const std::string mode = separator == std::string::npos
                                 ? "meet"
                                 : lowercase(trim(std::string_view(value).substr(separator + 1U)));
    if (alignment.size() != 8U || alignment[0] != 'x' || alignment[4] != 'Y' ||
        (mode != "meet" && mode != "slice")) {
        fail(*attribute, "invalid preserveAspectRatio value");
    }
    const auto align = [&](const std::string_view part) -> AspectAlign {
        if (part == "Min")
            return AspectAlign::minimum;
        if (part == "Mid")
            return AspectAlign::middle;
        if (part == "Max")
            return AspectAlign::maximum;
        fail(*attribute, "invalid preserveAspectRatio alignment");
    };
    return PreserveAspectRatio{
        align(std::string_view(alignment).substr(1U, 3U)),
        align(std::string_view(alignment).substr(5U, 3U)),
        mode == "slice",
    };
}

} // namespace

Point AffineTransform::apply(const Point point) const noexcept {
    return Point{a * point.x + c * point.y + e, b * point.x + d * point.y + f};
}

ParseError::ParseError(const std::size_t byte_offset, std::string message)
    : std::invalid_argument(std::move(message) + " at byte " + std::to_string(byte_offset)),
      byte_offset_(byte_offset) {}

Document parse(const std::string_view source, const ParseOptions& options) {
    if (options.limits.maximum_input_bytes == 0U || options.limits.maximum_elements == 0U ||
        options.limits.maximum_attributes_per_element == 0U ||
        options.limits.maximum_nesting_depth == 0U || options.limits.maximum_path_segments == 0U) {
        throw std::invalid_argument("SVG parse limits must be nonzero");
    }
    const XmlNode root = XmlParser(source, options.limits).document();
    if (local_name(root.name, root.offset) != "svg") {
        fail(root, "root element must be svg");
    }
    static constexpr std::array root_attributes{
        std::string_view{"width"}, std::string_view{"height"}, std::string_view{"viewBox"},
        std::string_view{"preserveAspectRatio"}};
    validate_attributes(root, root_attributes, true);

    Document document;
    const Attribute* const view_box = find_attribute(root, "viewBox");
    if (view_box != nullptr) {
        NumberList values(view_box->value, view_box->offset);
        document.view_box =
            ViewBox{values.number(), values.number(), values.number(), values.number()};
        values.finish();
        if (document.view_box.width <= 0.0 || document.view_box.height <= 0.0) {
            fail(*view_box, "SVG viewBox width and height must be positive");
        }
    }
    const Attribute* const width = find_attribute(root, "width");
    const Attribute* const height = find_attribute(root, "height");
    document.width = width == nullptr ? document.view_box.width : parse_length(*width);
    document.height = height == nullptr ? document.view_box.height : parse_length(*height);
    if (document.width <= 0.0 || document.height <= 0.0) {
        fail(root, "SVG requires positive width/height or a positive viewBox");
    }
    if (view_box == nullptr) {
        document.view_box = ViewBox{0.0, 0.0, document.width, document.height};
    }
    document.preserve_aspect_ratio = parse_aspect_ratio(root);
    const PaintStyle root_paint = parse_paint(root, PaintStyle{}, options.current_color, true);
    const AffineTransform root_transform = node_transform(root);
    for (const XmlNode& child : root.children) {
        visit(child, root_transform, root_paint, options, document);
    }
    return document;
}

} // namespace strata::svg
