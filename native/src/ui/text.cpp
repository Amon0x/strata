#include "ui/text.hpp"

#include <chrono>
#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <cctype>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/value.hpp"

namespace strata::ui {
namespace {

[[nodiscard]] const runtime::Value* property(
    const DescriptionNode& description,
    const std::string_view name
) noexcept {
    const auto found = description.properties.find(name);
    return found != description.properties.end() ? found->second.value() : nullptr;
}

[[nodiscard]] const runtime::Value* style_field(
    const DescriptionNode& description,
    const std::string_view name
) noexcept {
    for (const std::string_view container : {"textStyle", "$layout", "style"}) {
        if (const runtime::Value* style = property(description, container);
            style != nullptr && style->object() != nullptr) {
            if (const runtime::Value* value = style->field(name); value != nullptr) return value;
        }
    }
    return property(description, name);
}

[[nodiscard]] double number(
    const runtime::Value* value,
    const double fallback
) noexcept {
    if (value == nullptr || value->number() == nullptr ||
        !std::isfinite(*value->number())) return fallback;
    return *value->number();
}

[[nodiscard]] std::optional<double> positive_number(
    const runtime::Value* value
) noexcept {
    const double resolved = number(value, -1.0);
    return resolved > 0.0 ? std::optional<double>(resolved) : std::nullopt;
}

[[nodiscard]] std::optional<std::size_t> positive_count(
    const runtime::Value* value
) noexcept {
    const double resolved = number(value, -1.0);
    if (resolved <= 0.0) return std::nullopt;
    return static_cast<std::size_t>(std::min(
        std::floor(resolved),
        static_cast<double>(std::numeric_limits<std::size_t>::max())
    ));
}

[[nodiscard]] std::uint32_t unsigned_flags(
    const runtime::Value* value,
    const std::uint32_t fallback = 0U
) noexcept {
    const double resolved = number(value, static_cast<double>(fallback));
    if (resolved <= 0.0) return 0U;
    return static_cast<std::uint32_t>(std::min(
        std::floor(resolved),
        static_cast<double>(std::numeric_limits<std::uint32_t>::max())
    ));
}

[[nodiscard]] std::string text_option(
    const DescriptionNode& description,
    const std::string_view name,
    std::string fallback
) {
    const runtime::Value* value = style_field(description, name);
    return value != nullptr && value->string() != nullptr
               ? *value->string()
               : std::move(fallback);
}

[[nodiscard]] FontRasterization font_rasterization_value(
    const runtime::Value* value,
    const FontRasterization fallback = FontRasterization::grayscale
) noexcept {
    if (value == nullptr || value->string() == nullptr) return fallback;
    return *value->string() == "MSDF" ? FontRasterization::msdf
                                      : FontRasterization::grayscale;
}

[[nodiscard]] std::string hexadecimal(const std::uint32_t value) {
    char buffer[8]{};
    const auto converted = std::to_chars(std::begin(buffer), std::end(buffer), value, 16);
    std::string result(buffer, converted.ptr);
    std::ranges::transform(result, result.begin(), [](const unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    if (result.size() < 4U) result.insert(0U, 4U - result.size(), '0');
    return result;
}

[[nodiscard]] std::size_t utf16_length(const std::string_view text) noexcept {
    std::size_t result = 0U;
    for (std::size_t offset = 0U; offset < text.size();) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        const std::size_t width = (lead & 0x80U) == 0U ? 1U
            : (lead & 0xE0U) == 0xC0U ? 2U
            : (lead & 0xF0U) == 0xE0U ? 3U : 4U;
        result += width == 4U ? 2U : 1U;
        offset += std::min(width, text.size() - offset);
    }
    return result;
}

[[nodiscard]] const runtime::Value* object_field(
    const runtime::Value* object,
    const std::string_view name
) noexcept {
    return object != nullptr && object->object() != nullptr ? object->field(name) : nullptr;
}

[[nodiscard]] std::string font_value(
    const runtime::Value* value,
    std::string fallback
) {
    if (value != nullptr && value->string() != nullptr && !value->string()->empty()) {
        return *value->string();
    }
    if (value != nullptr && value->texture() != nullptr && !value->texture()->id.empty()) {
        return value->texture()->id;
    }
    return fallback;
}

[[nodiscard]] std::vector<std::string> fallback_font_values(
    const runtime::Value* value
) {
    std::vector<std::string> result;
    if (value == nullptr || value->list() == nullptr) return result;
    for (const runtime::Value& entry : value->list()->values) {
        std::string id;
        if (entry.string() != nullptr) id = *entry.string();
        else if (entry.texture() != nullptr) id = entry.texture()->id;
        if (!id.empty() && !std::ranges::contains(result, id)) result.push_back(std::move(id));
    }
    return result;
}

[[nodiscard]] std::optional<std::uint32_t> packed_color(
    const runtime::Value* value
) noexcept {
    if (value == nullptr || value->color() == nullptr) return std::nullopt;
    const runtime::ColorValue& color = *value->color();
    return static_cast<std::uint32_t>(color.red) << 24U |
           static_cast<std::uint32_t>(color.green) << 16U |
           static_cast<std::uint32_t>(color.blue) << 8U |
           static_cast<std::uint32_t>(color.alpha);
}

[[nodiscard]] runtime::ColorValue unpacked_color(const std::uint32_t value) noexcept {
    return runtime::ColorValue{
        static_cast<std::uint8_t>(value >> 24U),
        static_cast<std::uint8_t>(value >> 16U),
        static_cast<std::uint8_t>(value >> 8U),
        static_cast<std::uint8_t>(value),
    };
}

} // namespace

TextLayout::TextLayout(std::shared_ptr<const TextLayoutData> storage) noexcept
    : storage_(std::move(storage)),
      shaped(storage_->shaped),
      glyph_font_ids(storage_->glyph_font_ids),
      glyph_pixel_sizes(storage_->glyph_pixel_sizes),
      resolved_runs(storage_->resolved_runs),
      lines(storage_->lines),
      wrap_width(storage_->wrap_width),
      clipped(storage_->clipped),
      truncated(storage_->truncated) {}

TextLayout::TextLayout()
    : TextLayout(std::make_shared<const TextLayoutData>()) {}

TextLayout::TextLayout(const TextLayout& other) noexcept
    : TextLayout(other.storage_) {}

TextLayout::TextLayout(TextLayout&& other) noexcept
    : TextLayout(other.storage_) {}

TextLayout& TextLayout::operator=(const TextLayout& other) noexcept {
    if (this == &other) return *this;
    this->~TextLayout();
    ::new (this) TextLayout(other);
    return *this;
}

TextLayout& TextLayout::operator=(TextLayout&& other) noexcept {
    return operator=(other);
}

bool TextLayout::shares_storage_with(const TextLayout& other) const noexcept {
    return storage_ == other.storage_;
}

bool text_layout_field(const std::string_view name) noexcept {
    static constexpr std::array<std::string_view, 16U> fields{
        "text", "label", "spans", "font", "fallbackFonts", "fontSize", "pixelSize", "lineHeight",
        "lineHeightMultiplier", "letterSpacing", "fontStyleFlags", "fontRasterization", "wrapWidth",
        "wrapMode", "overflow", "maxLines",
    };
    return std::ranges::contains(fields, name) || name == "alignment";
}

bool text_layout_projection_equal(
    const runtime::Value* current,
    const runtime::Value* next
) noexcept {
    static constexpr std::array<std::string_view, 14U> fields{
        "font", "fallbackFonts", "fontSize", "pixelSize", "lineHeight", "lineHeightMultiplier",
        "letterSpacing", "fontStyleFlags", "fontRasterization", "wrapWidth", "wrapMode", "overflow",
        "maxLines", "alignment",
    };
    for (const std::string_view field : fields) {
        const runtime::Value* left = object_field(current, field);
        const runtime::Value* right = object_field(next, field);
        if ((left == nullptr) != (right == nullptr) ||
            (left != nullptr && *left != *right)) {
            return false;
        }
    }
    return true;
}

TextEngine::TextEngine(font::OpenTypeFont control_font)
    : TextEngine(FontRegistry{
          {"strata:fonts/default-medium", control_font},
          {"strata:fonts/default", std::move(control_font)},
      }) {}

TextEngine::TextEngine(font::OpenTypeFont control_font, font::OpenTypeFont regular_font)
    : TextEngine(FontRegistry{
          {"strata:fonts/default-medium", std::move(control_font)},
          {"strata:fonts/default", std::move(regular_font)},
      }) {}

TextEngine::TextEngine(FontRegistry fonts)
    : fonts_(std::move(fonts)) {
    if (!fonts_.contains("strata:fonts/default-medium") ||
        !fonts_.contains("strata:fonts/default")) {
        throw std::invalid_argument(
            "text font registry requires strata:fonts/default-medium and strata:fonts/default"
        );
    }
    for (const auto& [font_id_value, face] : fonts_) {
        for (const std::string& detail : face.optional_diagnostics()) {
            diagnostics_.push_back(runtime::RuntimeDiagnostic{
                "STRATA.TEXT.FONT_OPTIONAL_TABLE_IGNORED",
                "Font '" + font_id_value + "' remains usable, but an optional feature was "
                    "ignored: " + detail,
                {},
                std::nullopt,
                runtime::DiagnosticSeverity::warning,
            });
        }
    }
}

std::shared_ptr<const TextEngine> TextEngine::load_control_font(
    const std::filesystem::path& root,
    const resource::ResourceId& resource
) {
    return std::make_shared<const TextEngine>(font::OpenTypeFont::parse(
        resource::load_binary_resource(root, resource)
    ));
}

std::shared_ptr<const TextEngine> TextEngine::load_default_fonts(
    const std::filesystem::path& root
) {
    return std::make_shared<const TextEngine>(
        FontRegistry{
            {
                "strata:fonts/default-medium",
                font::OpenTypeFont::parse(resource::load_binary_resource(
                    root,
                    resource::ResourceId::parse("assets/strata/fonts/medium.ttf")
                )),
            },
            {
                "strata:fonts/default",
                font::OpenTypeFont::parse(resource::load_binary_resource(
                    root,
                    resource::ResourceId::parse("assets/strata/fonts/default.ttf")
                )),
            },
            {
                "strata:fonts/mono",
                font::OpenTypeFont::parse(resource::load_binary_resource(
                    root,
                    resource::ResourceId::parse("assets/strata/fonts/mono.ttf")
                )),
            },
        }
    );
}

Size TextEngine::measure(
    const RetainedNode& node,
    const Constraints& constraints
) const {
    const DescriptionNode& description = node.description();
    const runtime::Value* text_value = property(description, "text");
    if (text_value == nullptr) text_value = node.retained_value("$text");
    if (text_value == nullptr) text_value = property(description, "label");
    if (text_value == nullptr || text_value->string() == nullptr) return {};

    const LayoutStyle style = layout_style(description);
    const bool explicit_width = style.width.kind == LayoutSize::Kind::fixed ||
                                style.width.kind == LayoutSize::Kind::percent ||
                                style.width.kind == LayoutSize::Kind::fill ||
                                style.width.kind == LayoutSize::Kind::clamp ||
                                constraints.min_width == constraints.max_width;
    const std::optional<double> bounded_width =
        std::isfinite(constraints.max_width) && constraints.max_width > 0.0
            ? std::optional<double>(constraints.max_width)
            : std::nullopt;

    CacheKey selected = request_key(
        node,
        *text_value->string(),
        explicit_width ? bounded_width : std::nullopt
    );
    const TextLayout* layout_value = &cached_layout(selected, true);
    if (!explicit_width && bounded_width.has_value() &&
        layout_value->shaped.metrics.width > *bounded_width) {
        selected = request_key(node, *text_value->string(), bounded_width);
        layout_value = &cached_layout(selected, true);
    }
    measured_requests_.insert_or_assign(node.identity(), selected);

    const font::TextMetrics& metrics = layout_value->shaped.metrics;
    return Size{metrics.width, metrics.height};
}

const font::OpenTypeFont& TextEngine::control_font() const noexcept {
    return fonts_.find("strata:fonts/default-medium")->second;
}

const font::OpenTypeFont& TextEngine::font(const std::string_view id) const noexcept {
    const auto found = fonts_.find(id);
    return found != fonts_.end()
               ? found->second
               : fonts_.find("strata:fonts/default")->second;
}

std::string TextEngine::requested_font_id(const RetainedNode& node) const {
    const runtime::Value* value = style_field(node.description(), "font");
    if (value != nullptr) {
        if (value->string() != nullptr && !value->string()->empty()) return *value->string();
        if (value->texture() != nullptr && !value->texture()->id.empty()) return value->texture()->id;
    }
    return "strata:fonts/default";
}

std::string TextEngine::font_id(const RetainedNode& node) const {
    const std::string requested = requested_font_id(node);
    return fonts_.contains(requested) ? requested : "strata:fonts/default";
}

double TextEngine::pixel_size(const RetainedNode& node) const noexcept {
    return std::max(
        0.001,
        number(style_field(node.description(), "pixelSize"),
               number(style_field(node.description(), "fontSize"), 12.0))
    );
}

std::uint32_t TextEngine::font_style_flags(const RetainedNode& node) const noexcept {
    return unsigned_flags(style_field(node.description(), "fontStyleFlags"));
}

FontRasterization TextEngine::font_rasterization(const RetainedNode& node) const noexcept {
    return font_rasterization_value(style_field(node.description(), "fontRasterization"));
}

double TextEngine::line_height_multiplier(const RetainedNode& node) const noexcept {
    return std::max(0.001, number(style_field(node.description(), "lineHeightMultiplier"), 1.0));
}

double TextEngine::letter_spacing(const RetainedNode& node) const noexcept {
    return number(style_field(node.description(), "letterSpacing"), 0.0);
}

font::ShapedText TextEngine::shape(
    const RetainedNode& node,
    const std::string_view text
) const {
    CacheKey key = request_key(node, text, std::nullopt);
    if (const auto measured = measured_requests_.find(node.identity());
        measured != measured_requests_.end() && measured->second.text == text &&
        !key.wrap_width.has_value()) {
        key.wrap_width = measured->second.wrap_width;
    }
    return cached_layout(std::move(key), true).shaped;
}

TextLayout TextEngine::layout(
    const RetainedNode& node,
    const std::string_view text
) const {
    CacheKey key = request_key(node, text, std::nullopt);
    if (const auto measured = measured_requests_.find(node.identity());
        measured != measured_requests_.end() && measured->second.text == text &&
        !key.wrap_width.has_value()) {
        key.wrap_width = measured->second.wrap_width;
    }
    return cached_layout(std::move(key), true);
}

TextLayout TextEngine::layout(
    const RetainedNode& node,
    const std::string_view text,
    const TextLayoutOptions& options
) const {
    return cached_layout(request_key(node, text, std::nullopt, &options), true);
}

void TextEngine::adopt_generations(
    const std::uint64_t scale_context,
    const std::uint64_t style_resources,
    const std::uint64_t font_resources
) const {
    if (generations_initialized_ &&
        scale_context_generation_ == scale_context &&
        style_generation_ == style_resources &&
        font_generation_ == font_resources) {
        return;
    }
    if (generations_initialized_) {
        cache_.clear();
        measured_requests_.clear();
    }
    scale_context_generation_ = scale_context;
    style_generation_ = style_resources;
    font_generation_ = font_resources;
    generations_initialized_ = true;
}

TextEngine::CacheKey TextEngine::request_key(
    const RetainedNode& node,
    const std::string_view text,
    const std::optional<double> measured_wrap_width,
    const TextLayoutOptions* const overrides
) const {
    const DescriptionNode& description = node.description();
    const std::optional<double> authored_wrap = positive_number(
        style_field(description, "wrapWidth")
    );
    std::optional<double> resolved_wrap = authored_wrap.has_value()
        ? authored_wrap
        : measured_wrap_width;
    if (overrides != nullptr && overrides->wrap_width.has_value()) {
        const double requested = *overrides->wrap_width;
        resolved_wrap = std::isfinite(requested) && requested > 0.0
            ? std::optional<double>(requested)
            : std::nullopt;
    }
    CacheKey result{
        std::string(text),
        requested_font_id(node),
        fallback_font_values(style_field(description, "fallbackFonts")),
        pixel_size(node),
        positive_number(style_field(description, "lineHeight")),
        line_height_multiplier(node),
        letter_spacing(node),
        font_style_flags(node),
        font_rasterization(node),
        resolved_wrap,
        overrides != nullptr && overrides->wrap_mode.has_value()
            ? *overrides->wrap_mode
            : text_option(description, "wrapMode", "WORD"),
        overrides != nullptr && overrides->overflow.has_value()
            ? *overrides->overflow
            : text_option(description, "overflow", "CLIP"),
        overrides != nullptr && overrides->max_lines.has_value()
            ? std::optional<std::size_t>(std::max<std::size_t>(1U, *overrides->max_lines))
            : positive_count(style_field(description, "maxLines")),
        overrides != nullptr && overrides->alignment.has_value()
            ? *overrides->alignment
            : text_option(description, "alignment", "START"),
    };
    if (description.type != "RichText") return result;
    const runtime::Value* spans = property(description, "spans");
    if (spans == nullptr || spans->list() == nullptr) return result;
    std::size_t offset = 0U;
    result.style_runs.reserve(spans->list()->values.size());
    const auto span_expressions = description.properties.find("spans");
    for (std::size_t span_index = 0U; span_index < spans->list()->values.size(); ++span_index) {
        const runtime::Value& span = spans->list()->values[span_index];
        const runtime::Value* span_text = span.field("text");
        if (span_text == nullptr || span_text->string() == nullptr) continue;
        const std::size_t end = offset + utf16_length(*span_text->string());
        const runtime::Value* style = span.field("style");
        const auto run_field = [&span, style](const std::string_view name) {
            if (const runtime::Value* value = object_field(style, name); value != nullptr) {
                return value;
            }
            return span.field(name);
        };
        bool interactive = false;
        if (span_expressions != description.properties.end() &&
            span_expressions->second.list() != nullptr &&
            span_index < (**span_expressions->second.list()).values.size()) {
            const runtime::ExpressionValue& expression =
                (**span_expressions->second.list()).values[span_index];
            const runtime::ExpressionValue* action = expression.object() != nullptr
                ? (**expression.object()).field("action") : nullptr;
            interactive = action != nullptr && action->action() != nullptr &&
                          *action->action() != nullptr;
        }
        const runtime::Value* authored_fallbacks = run_field("fallbackFonts");
        result.style_runs.push_back(StyleRun{
            offset,
            end,
            span_index + 1U,
            span_index,
            font_value(run_field("font"), result.font_id),
            authored_fallbacks != nullptr
                ? fallback_font_values(authored_fallbacks)
                : result.fallback_fonts,
            std::max(0.001, number(
                run_field("pixelSize"),
                number(run_field("fontSize"), result.pixel_size)
            )),
            positive_number(run_field("lineHeight")).has_value()
                ? positive_number(run_field("lineHeight"))
                : result.line_height,
            std::max(0.001, number(
                run_field("lineHeightMultiplier"), result.line_height_multiplier
            )),
            number(run_field("letterSpacing"), result.letter_spacing),
            unsigned_flags(run_field("fontStyleFlags"), result.font_style_flags),
            font_rasterization_value(
                run_field("fontRasterization"), result.font_rasterization
            ),
            packed_color(run_field("color")),
            interactive,
        });
        offset = end;
    }
    return result;
}

const TextLayout& TextEngine::cached_layout(
    CacheKey key,
    const bool count_request
) const {
    if (count_request) ++operation_counters_.requests;
    const auto lookup_started = std::chrono::steady_clock::now();
    const auto found = cache_.find(key);
    operation_counters_.cache_lookup_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - lookup_started
        ).count()
    );
    if (found != cache_.end()) {
        const auto restore_started = std::chrono::steady_clock::now();
        if (count_request) ++operation_counters_.cache_hits;
        const TextLayout& restored = found->second;
        operation_counters_.cache_restore_nanos += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - restore_started
            ).count()
        );
        return restored;
    }
    if (!fonts_.contains(key.font_id)) {
        record_missing_font(key.font_id);
    }
    const auto shaping_started = std::chrono::steady_clock::now();
    TextLayout layout_value = build_layout(key);
    operation_counters_.shaping_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - shaping_started
        ).count()
    );
    record_missing_glyphs(key.font_id, layout_value.shaped);
    if (count_request) ++operation_counters_.cache_misses;
    const auto store_started = std::chrono::steady_clock::now();
    const TextLayout& stored = cache_.emplace(std::move(key), std::move(layout_value)).first->second;
    operation_counters_.cache_store_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - store_started
        ).count()
    );
    return stored;
}

TextLayout TextEngine::build_layout(const CacheKey& key) const {
    const auto font_resolution_started = std::chrono::steady_clock::now();
    using FontCandidate = FontRegistry::value_type;
    using FontOrder = std::vector<const FontCandidate*>;
    struct Unit final {
        std::uint32_t code_point = 0U;
        std::size_t start = 0U;
        std::size_t end = 0U;
        std::size_t style_identity = 0U;
        std::optional<std::size_t> authored_span_index;
        const std::string* font_id = nullptr;
        const font::OpenTypeFont* face = nullptr;
        const std::vector<std::string>* fallback_fonts = nullptr;
        std::uint16_t glyph_id = 0U;
        double leading = 0.0;
        double advance = 0.0;
        double x_placement = 0.0;
        double y_placement = 0.0;
        double y_advance = 0.0;
        double y_pen = 0.0;
        double ascent = 0.0;
        double natural_height = 0.0;
        double pixel_size = 0.0;
        std::optional<double> line_height;
        double line_height_multiplier = 1.0;
        double letter_spacing = 0.0;
        std::uint32_t font_style_flags = 0U;
        FontRasterization font_rasterization = FontRasterization::grayscale;
        std::optional<std::uint32_t> color_rgba;
        bool interactive = false;
        bool whitespace = false;
        bool newline = false;
    };
    struct DraftLine final {
        std::vector<std::size_t> units;
        std::vector<std::size_t> soft_wrap_gap_units;
        std::size_t start = 0U;
        std::size_t end = 0U;
        std::optional<std::size_t> hard_break_unit;
    };

    const auto upper = [](std::string value) {
        std::ranges::transform(value, value.begin(), [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
        return value;
    };
    const auto decode = [](const std::string_view value, std::size_t& offset) {
        const auto lead = static_cast<unsigned char>(value[offset++]);
        if ((lead & 0x80U) == 0U) return static_cast<std::uint32_t>(lead);
        std::size_t count = 0U;
        std::uint32_t result = 0U;
        if ((lead & 0xE0U) == 0xC0U) count = 1U, result = lead & 0x1FU;
        else if ((lead & 0xF0U) == 0xE0U) count = 2U, result = lead & 0x0FU;
        else count = 3U, result = lead & 0x07U;
        for (std::size_t index = 0U; index < count && offset < value.size(); ++index) {
            result = (result << 6U) | (static_cast<unsigned char>(value[offset++]) & 0x3FU);
        }
        return result;
    };

    const auto fallback_order = [this](
        const std::string& requested,
        const std::vector<std::string>& authored,
        const std::uint32_t requested_style_flags
    ) {
        FontOrder base;
        const auto append = [&base, this](const std::string& id) {
            const auto found = fonts_.find(id);
            if (found == fonts_.end()) {
                record_missing_font(id);
                return;
            }
            if (std::ranges::find(base, &*found) == base.end()) base.push_back(&*found);
        };
        append(requested);
        for (const std::string& id : authored) append(id);
        append("strata:fonts/default");

        const std::uint32_t requested_known =
            requested_style_flags & font::known_font_style_flags;
        if (requested_known == 0U) return base;

        // A fallback list is face-ordered, but matching faces from the same authored family form
        // one style-selection group. Prefer the face that realizes the most requested bits before
        // falling back to synthetic geometry, without crossing into an earlier/later family.
        FontOrder result;
        result.reserve(base.size());
        std::vector<bool> emitted(base.size(), false);
        for (std::size_t anchor_index = 0U; anchor_index < base.size(); ++anchor_index) {
            if (emitted[anchor_index]) continue;
            const font::FontMetadata& anchor = base[anchor_index]->second.metadata();
            std::vector<std::size_t> family;
            for (std::size_t candidate_index = anchor_index;
                 candidate_index < base.size(); ++candidate_index) {
                if (emitted[candidate_index]) continue;
                const font::FontMetadata& candidate =
                    base[candidate_index]->second.metadata();
                const bool same_family = !anchor.family.empty() && candidate.family == anchor.family;
                const bool same_width = anchor.width_class == 0U || candidate.width_class == 0U ||
                    candidate.width_class == anchor.width_class;
                if (candidate_index == anchor_index || (same_family && same_width)) {
                    family.push_back(candidate_index);
                }
            }
            std::ranges::stable_sort(family, [&base, requested_known](
                const std::size_t left,
                const std::size_t right
            ) {
                const std::uint32_t left_style =
                    base[left]->second.metadata().style_flags & font::known_font_style_flags;
                const std::uint32_t right_style =
                    base[right]->second.metadata().style_flags & font::known_font_style_flags;
                const bool left_exact = left_style == requested_known;
                const bool right_exact = right_style == requested_known;
                if (left_exact != right_exact) return left_exact;
                const int left_match = std::popcount(left_style & requested_known);
                const int right_match = std::popcount(right_style & requested_known);
                if (left_match != right_match) return left_match > right_match;
                const int left_extra = std::popcount(left_style & ~requested_known);
                const int right_extra = std::popcount(right_style & ~requested_known);
                return left_extra < right_extra;
            });
            for (const std::size_t index : family) {
                emitted[index] = true;
                result.push_back(base[index]);
            }
        }
        return result;
    };

    const FontOrder default_font_order = fallback_order(
        key.font_id, key.fallback_fonts, key.font_style_flags
    );
    std::vector<FontOrder> style_font_orders;
    style_font_orders.reserve(key.style_runs.size());
    for (const StyleRun& style : key.style_runs) {
        style_font_orders.push_back(fallback_order(
            style.font_id, style.fallback_fonts, style.font_style_flags
        ));
    }

    std::vector<Unit> units;
    units.reserve(key.text.size());
    std::size_t byte_offset = 0U;
    std::size_t utf16_offset = 0U;
    std::size_t style_run_index = 0U;
    while (byte_offset < key.text.size()) {
        const std::uint32_t code_point = decode(key.text, byte_offset);
        const std::size_t start = utf16_offset;
        utf16_offset += code_point > 0xFFFFU ? 2U : 1U;
        Unit unit;
        unit.code_point = code_point;
        unit.start = start;
        unit.end = utf16_offset;
        unit.newline = code_point == 0x0AU;
        unit.whitespace = code_point == 0x20U || code_point == 0x09U || code_point == 0x0DU;
        while (style_run_index + 1U < key.style_runs.size() &&
               start >= key.style_runs[style_run_index].end) {
            ++style_run_index;
        }
        const StyleRun* style = !key.style_runs.empty() &&
            start >= key.style_runs[style_run_index].start &&
            start < key.style_runs[style_run_index].end
                ? &key.style_runs[style_run_index]
                : nullptr;
        unit.style_identity = style != nullptr ? style->identity : 0U;
        if (style != nullptr) unit.authored_span_index = style->authored_span_index;
        unit.pixel_size = style != nullptr ? style->pixel_size : key.pixel_size;
        unit.line_height = style != nullptr ? style->line_height : key.line_height;
        unit.line_height_multiplier = style != nullptr
            ? style->line_height_multiplier : key.line_height_multiplier;
        unit.letter_spacing = style != nullptr ? style->letter_spacing : key.letter_spacing;
        unit.font_style_flags = style != nullptr
            ? style->font_style_flags : key.font_style_flags;
        unit.font_rasterization = style != nullptr
            ? style->font_rasterization : key.font_rasterization;
        unit.color_rgba = style != nullptr ? style->color_rgba : std::nullopt;
        unit.interactive = style != nullptr && style->interactive;
        const FontOrder& ordered_fonts = style != nullptr
            ? style_font_orders[style_run_index]
            : default_font_order;
        unit.fallback_fonts = style != nullptr
            ? &style->fallback_fonts
            : &key.fallback_fonts;
        if (!ordered_fonts.empty()) {
            unit.font_id = &ordered_fonts.front()->first;
            unit.face = &ordered_fonts.front()->second;
        }
        if (!unit.newline) {
            for (const FontCandidate* candidate : ordered_fonts) {
                const std::uint16_t glyph = candidate->second.glyph_id(code_point);
                if (glyph == 0U) continue;
                unit.font_id = &candidate->first;
                unit.face = &candidate->second;
                unit.glyph_id = glyph;
                break;
            }
        }
        if (unit.face == nullptr) {
            units.push_back(std::move(unit));
            continue;
        }

        const font::OpenTypeFont& face = *unit.face;
        const double scale = unit.pixel_size / static_cast<double>(face.units_per_em());
        unit.ascent = std::max(0.0, static_cast<double>(face.ascender()) * scale);
        const double descent = std::max(0.0, -static_cast<double>(face.descender()) * scale);
        const double gap = std::max(0.0, static_cast<double>(face.line_gap()) * scale);
        unit.natural_height = std::max(unit.pixel_size, unit.ascent + descent + gap);
        if (unit.glyph_id != 0U) {
            unit.advance = static_cast<double>(face.horizontal_advance(unit.glyph_id)) * scale;
        }
        units.push_back(std::move(unit));
    }
    operation_counters_.font_resolution_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - font_resolution_started
        ).count()
    );

    const auto opentype_started = std::chrono::steady_clock::now();
    std::vector<Unit> shaped_units;
    shaped_units.reserve(units.size());
    for (std::size_t begin = 0U; begin < units.size();) {
        if (units[begin].newline || units[begin].glyph_id == 0U) {
            shaped_units.push_back(std::move(units[begin]));
            ++begin;
            continue;
        }
        std::size_t end = begin + 1U;
        while (end < units.size() && !units[end].newline && units[end].glyph_id != 0U &&
               units[end].font_id == units[begin].font_id &&
               units[end].style_identity == units[begin].style_identity) {
            ++end;
        }
        std::vector<font::GlyphRunInput> glyphs;
        glyphs.reserve(end - begin);
        for (std::size_t index = begin; index < end; ++index) {
            glyphs.push_back(font::GlyphRunInput{
                units[index].glyph_id,
                units[index].start,
                units[index].end,
            });
        }
        const font::OpenTypeFont& face = *units[begin].face;
        const std::vector<font::PositionedRunGlyph> shaped = face.shape_glyph_run(
            std::span<const font::GlyphRunInput>(glyphs),
            units[begin].font_style_flags
        );
        double y_pen = 0.0;
        const std::span<const Unit> source_run =
            std::span<const Unit>(units).subspan(begin, end - begin);
        std::size_t source_index = 0U;
        for (std::size_t shaped_index = 0U; shaped_index < shaped.size(); ++shaped_index) {
            const font::PositionedRunGlyph& glyph = shaped[shaped_index];
            while (source_index < source_run.size() &&
                   source_run[source_index].start < glyph.text_start_offset) {
                ++source_index;
            }
            if (source_index >= source_run.size() ||
                source_run[source_index].start != glyph.text_start_offset) {
                throw std::logic_error("font shaping lost its source cluster");
            }
            Unit unit = source_run[source_index];
            unit.end = glyph.text_end_offset;
            unit.glyph_id = glyph.glyph_id;
            if (glyph.ligature_component_clusters.size() > 1U) unit.whitespace = false;
            const font::GlyphPositionAdjustment& position = glyph.position;
            const double scale = unit.pixel_size / static_cast<double>(face.units_per_em());
            unit.leading = shaped_index == 0U ? 0.0 : unit.letter_spacing;
            unit.advance = static_cast<double>(face.horizontal_advance(unit.glyph_id)) * scale +
                position.x_advance * scale;
            unit.x_placement = position.x_placement * scale;
            unit.y_placement = position.y_placement * scale;
            unit.y_advance = position.y_advance * scale;
            unit.y_pen = y_pen;
            y_pen += unit.y_advance;
            shaped_units.push_back(std::move(unit));
        }
        begin = end;
    }
    units = std::move(shaped_units);
    operation_counters_.opentype_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - opentype_started
        ).count()
    );

    const auto line_assembly_started = std::chrono::steady_clock::now();
    const std::string wrap_mode = upper(key.wrap_mode);
    const bool wraps = key.wrap_width.has_value() && wrap_mode != "NONE";
    const double wrap_width = key.wrap_width.value_or(std::numeric_limits<double>::infinity());
    const auto unit_width = [&units](const std::size_t index, const bool first) {
        const Unit& unit = units[index];
        return (first ? 0.0 : unit.leading) + unit.advance;
    };
    std::vector<DraftLine> drafts;
    const auto layout_segment = [&](const std::vector<std::size_t>& segment,
                                    const std::size_t segment_start,
                                    const std::size_t segment_end,
                                    const std::optional<std::size_t> hard_break_unit) {
        if (segment.empty()) {
            drafts.push_back(DraftLine{{}, {}, segment_start, segment_end, hard_break_unit});
            return;
        }
        if (!wraps) {
            drafts.push_back(DraftLine{segment, {}, segment.front() < units.size()
                ? units[segment.front()].start : segment_start, segment_end, hard_break_unit});
            return;
        }
        const bool character_wrap = wrap_mode == "CHARACTER" || wrap_mode == "CHAR";
        std::vector<std::vector<std::size_t>> tokens;
        if (character_wrap) {
            for (const std::size_t index : segment) tokens.push_back({index});
        } else {
            for (const std::size_t index : segment) {
                if (tokens.empty() || units[tokens.back().front()].whitespace != units[index].whitespace) {
                    tokens.push_back({});
                }
                tokens.back().push_back(index);
            }
        }
        DraftLine current;
        current.start = segment_start;
        double current_width = 0.0;
        const auto flush = [&](const bool trim_break_whitespace) {
            if (current.units.empty()) return;
            if (trim_break_whitespace &&
                std::ranges::any_of(current.units, [&units](const std::size_t index) {
                    return !units[index].whitespace;
                })) {
                const auto trailing = std::ranges::find_if(
                    current.units | std::views::reverse,
                    [&units](const std::size_t index) { return !units[index].whitespace; }
                ).base();
                current.soft_wrap_gap_units.assign(trailing, current.units.end());
                current.units.erase(trailing, current.units.end());
            }
            const std::vector<std::size_t>& source = !current.units.empty()
                ? current.units : current.soft_wrap_gap_units;
            current.start = source.empty() ? segment_start : units[source.front()].start;
            current.end = current.units.empty()
                ? source.empty() ? segment_end : units[source.back()].end
                : units[current.units.back()].end;
            drafts.push_back(std::move(current));
            current = DraftLine{};
            current_width = 0.0;
        };
        for (const std::vector<std::size_t>& token : tokens) {
            double standalone_width = 0.0;
            double appended_width = 0.0;
            for (std::size_t offset = 0U; offset < token.size(); ++offset) {
                standalone_width += unit_width(token[offset], offset == 0U);
                appended_width += unit_width(token[offset], false);
            }
            const bool whitespace_token = !token.empty() && units[token.front()].whitespace;
            if (!current.units.empty() && !whitespace_token &&
                current_width + appended_width > wrap_width) {
                flush(!character_wrap);
            }
            if (standalone_width > wrap_width && token.size() > 1U) {
                for (const std::size_t index : token) {
                    const double width = unit_width(index, current.units.empty());
                    if (!current.units.empty() && current_width + width > wrap_width) {
                        flush(false);
                    }
                    current.units.push_back(index);
                    current_width += width;
                }
            } else {
                const double accepted_width = current.units.empty()
                    ? standalone_width : appended_width;
                current.units.insert(current.units.end(), token.begin(), token.end());
                current_width += accepted_width;
            }
        }
        flush(false);
        if (hard_break_unit.has_value() && !drafts.empty()) {
            drafts.back().hard_break_unit = hard_break_unit;
        }
    };

    std::vector<std::size_t> segment;
    std::size_t segment_start = 0U;
    for (std::size_t index = 0U; index < units.size(); ++index) {
        if (!units[index].newline) {
            segment.push_back(index);
            continue;
        }
        layout_segment(segment, segment_start, units[index].start, index);
        segment.clear();
        segment_start = units[index].end;
    }
    layout_segment(segment, segment_start, utf16_offset, std::nullopt);
    if (drafts.empty()) drafts.push_back(DraftLine{{}, {}, 0U, 0U, std::nullopt});

    TextLayoutData result;
    result.wrap_width = key.wrap_width;
    bool line_count_truncated = false;
    if (key.max_lines.has_value() && drafts.size() > *key.max_lines) {
        drafts.resize(*key.max_lines);
        result.truncated = true;
        line_count_truncated = true;
    }
    std::vector<std::size_t> ellipsis_lines;
    if (!wraps && key.wrap_width.has_value()) {
        for (std::size_t line = 0U; line < drafts.size(); ++line) {
            const DraftLine& draft = drafts[line];
            double unwrapped_width = 0.0;
            for (std::size_t position = 0U; position < draft.units.size(); ++position) {
                unwrapped_width += unit_width(draft.units[position], position == 0U);
            }
            if (unwrapped_width > *key.wrap_width) {
                if (upper(key.overflow) == "ELLIPSIS") {
                    result.truncated = true;
                    ellipsis_lines.push_back(line);
                } else {
                    result.clipped = true;
                }
            }
        }
    }

    if (line_count_truncated && upper(key.overflow) == "ELLIPSIS" &&
        key.wrap_width.has_value() && !drafts.empty() &&
        !std::ranges::contains(ellipsis_lines, drafts.size() - 1U)) {
        ellipsis_lines.push_back(drafts.size() - 1U);
    }
    for (const std::size_t ellipsis_line : ellipsis_lines) {
        DraftLine& target_line = drafts[ellipsis_line];
        Unit marker;
        marker.code_point = 0x2026U;
        marker.start = target_line.soft_wrap_gap_units.empty()
            ? target_line.end
            : units[target_line.soft_wrap_gap_units.back()].end;
        marker.end = marker.start;
        const Unit* marker_style = !target_line.units.empty()
            ? &units[target_line.units.back()]
            : !target_line.soft_wrap_gap_units.empty()
                ? &units[target_line.soft_wrap_gap_units.back()]
                : target_line.hard_break_unit.has_value()
                    ? &units[*target_line.hard_break_unit]
                    : nullptr;
        if (marker_style != nullptr) {
            const Unit& style = *marker_style;
            marker.font_id = style.font_id;
            marker.face = style.face;
            marker.fallback_fonts = style.fallback_fonts;
            marker.pixel_size = style.pixel_size;
            marker.line_height = style.line_height;
            marker.line_height_multiplier = style.line_height_multiplier;
            marker.letter_spacing = style.letter_spacing;
            marker.font_style_flags = style.font_style_flags;
            marker.font_rasterization = style.font_rasterization;
            marker.style_identity = style.style_identity;
            marker.authored_span_index = style.authored_span_index;
            marker.color_rgba = style.color_rgba;
            marker.interactive = style.interactive;
        } else {
            marker.fallback_fonts = &key.fallback_fonts;
            marker.pixel_size = key.pixel_size;
            marker.line_height = key.line_height;
            marker.line_height_multiplier = key.line_height_multiplier;
            marker.letter_spacing = key.letter_spacing;
            marker.font_style_flags = key.font_style_flags;
            marker.font_rasterization = key.font_rasterization;
        }
        const std::string& marker_request = marker.font_id != nullptr
            ? *marker.font_id
            : key.font_id;
        const FontOrder marker_fonts = fallback_order(
            marker_request,
            marker.fallback_fonts != nullptr ? *marker.fallback_fonts : key.fallback_fonts,
            marker.font_style_flags
        );
        for (const FontCandidate* candidate : marker_fonts) {
            const std::uint16_t glyph = candidate->second.glyph_id(marker.code_point);
            if (glyph == 0U) continue;
            marker.font_id = &candidate->first;
            marker.face = &candidate->second;
            marker.glyph_id = glyph;
            const font::OpenTypeFont& face = candidate->second;
            const double scale = marker.pixel_size / static_cast<double>(face.units_per_em());
            const std::array<std::uint16_t, 1U> marker_glyph{glyph};
            const font::GlyphPositionAdjustment style = face.position_glyphs(
                marker_glyph, marker.font_style_flags
            ).front();
            marker.advance = (
                static_cast<double>(face.horizontal_advance(glyph)) + style.x_advance
            ) * scale;
            marker.ascent = std::max(0.0, static_cast<double>(face.ascender()) * scale);
            const double descent = std::max(0.0, -static_cast<double>(face.descender()) * scale);
            const double gap = std::max(0.0, static_cast<double>(face.line_gap()) * scale);
            marker.natural_height = std::max(
                marker.pixel_size, marker.ascent + descent + gap
            );
            break;
        }
        if (marker.glyph_id != 0U) {
            double width = 0.0;
            for (std::size_t position = 0U; position < target_line.units.size(); ++position) {
                width += unit_width(target_line.units[position], position == 0U);
            }
            while (!target_line.units.empty() && width + marker.advance > *key.wrap_width) {
                const std::size_t position = target_line.units.size() - 1U;
                width -= unit_width(target_line.units.back(), position == 0U);
                target_line.units.pop_back();
            }
            units.push_back(std::move(marker));
            target_line.units.push_back(units.size() - 1U);
        }
    }

    const std::string alignment = upper(key.alignment);
    result.shaped.metrics.natural_line_height = 0.0;
    result.shaped.metrics.line_count = drafts.size();
    result.lines.reserve(drafts.size());
    double line_y = 0.0;
    for (std::size_t line_index = 0U; line_index < drafts.size(); ++line_index) {
        const DraftLine& draft = drafts[line_index];
        double width = 0.0;
        for (std::size_t position = 0U; position < draft.units.size(); ++position) {
            width += unit_width(draft.units[position], position == 0U);
        }
        double line_x = 0.0;
        if (key.wrap_width.has_value() && alignment == "CENTER") {
            line_x = std::max(0.0, (*key.wrap_width - width) * 0.5);
        } else if (key.wrap_width.has_value() && (alignment == "END" || alignment == "RIGHT")) {
            line_x = std::max(0.0, *key.wrap_width - width);
        }
        double line_ascent = 0.0;
        const bool has_resolved_metrics = !draft.units.empty() ||
            draft.hard_break_unit.has_value();
        double natural_height = has_resolved_metrics ? 0.0 : key.pixel_size;
        double line_height = has_resolved_metrics
            ? 0.0
            : key.line_height.value_or(key.pixel_size * key.line_height_multiplier);
        for (const std::size_t unit_index : draft.units) {
            const Unit& unit = units[unit_index];
            line_ascent = std::max(line_ascent, unit.ascent);
            natural_height = std::max(natural_height, unit.natural_height);
            line_height = std::max(
                line_height,
                unit.line_height.value_or(
                    unit.natural_height * unit.line_height_multiplier
                )
            );
        }
        if (draft.hard_break_unit.has_value()) {
            const Unit& unit = units[*draft.hard_break_unit];
            line_ascent = std::max(line_ascent, unit.ascent);
            natural_height = std::max(natural_height, unit.natural_height);
            line_height = std::max(
                line_height,
                unit.line_height.value_or(
                    unit.natural_height * unit.line_height_multiplier
                )
            );
        }
        result.shaped.metrics.natural_line_height = std::max(
            result.shaped.metrics.natural_line_height, natural_height
        );
        const double baseline = line_y +
            std::max(0.0, line_height - natural_height) * 0.5 + line_ascent;
        double cursor = 0.0;
        for (std::size_t position = 0U; position < draft.units.size(); ++position) {
            const Unit& unit = units[draft.units[position]];
            const double leading = position == 0U ? 0.0 : unit.leading;
            const double cluster_advance = leading + unit.advance;
            result.shaped.clusters.push_back(font::ShapedCluster{
                unit.code_point,
                unit.start,
                unit.end,
                line_x + cursor,
                cluster_advance,
                line_index,
            });
            if (unit.glyph_id == 0U) {
                result.shaped.missing_code_points.push_back(unit.code_point);
            } else {
                ++result.shaped.metrics.glyph_count;
                if (!unit.whitespace) {
                    result.shaped.glyphs.push_back(font::ShapedGlyph{
                        unit.glyph_id,
                        unit.code_point,
                        unit.start,
                        unit.end,
                        line_x + cursor + leading,
                        baseline,
                        unit.advance,
                        unit.x_placement,
                        unit.y_pen + unit.y_placement,
                        unit.y_advance,
                        line_index,
                        unit.font_style_flags,
                    });
                    result.glyph_font_ids.push_back(*unit.font_id);
                    result.glyph_pixel_sizes.push_back(unit.pixel_size);
                    const std::size_t glyph_index = result.shaped.glyphs.size() - 1U;
                    const bool continues_run = !result.resolved_runs.empty() &&
                        result.resolved_runs.back().glyph_end_index == glyph_index &&
                        result.resolved_runs.back().line_index == line_index &&
                        result.resolved_runs.back().style_identity == unit.style_identity &&
                        result.resolved_runs.back().font_id == *unit.font_id &&
                        result.resolved_runs.back().pixel_size == unit.pixel_size &&
                        result.resolved_runs.back().font_style_flags == unit.font_style_flags &&
                        result.resolved_runs.back().font_rasterization == unit.font_rasterization;
                    if (continues_run) {
                        TextResolvedRun& run = result.resolved_runs.back();
                        run.text_end_offset = unit.end;
                        run.glyph_end_index = glyph_index + 1U;
                    } else {
                        result.resolved_runs.push_back(TextResolvedRun{
                            unit.start,
                            unit.end,
                            glyph_index,
                            glyph_index + 1U,
                            line_index,
                            unit.style_identity,
                            unit.authored_span_index,
                            *unit.font_id,
                            unit.pixel_size,
                            unit.letter_spacing,
                            unit.color_rgba.has_value()
                                ? std::optional<runtime::ColorValue>(
                                      unpacked_color(*unit.color_rgba)
                                  )
                                : std::nullopt,
                            unit.interactive,
                            unit.font_style_flags,
                            unit.font_rasterization,
                        });
                    }
                }
            }
            cursor += cluster_advance;
        }
        for (const std::size_t unit_index : draft.soft_wrap_gap_units) {
            const Unit& unit = units[unit_index];
            result.shaped.clusters.push_back(font::ShapedCluster{
                unit.code_point,
                unit.start,
                unit.end,
                line_x + cursor,
                0.0,
                line_index,
                true,
            });
            if (unit.glyph_id == 0U) {
                result.shaped.missing_code_points.push_back(unit.code_point);
            } else {
                ++result.shaped.metrics.glyph_count;
            }
        }
        const std::optional<std::size_t> gap_start =
            draft.soft_wrap_gap_units.empty()
                ? std::nullopt
                : std::optional<std::size_t>(
                      units[draft.soft_wrap_gap_units.front()].start
                  );
        const std::optional<std::size_t> gap_end =
            draft.soft_wrap_gap_units.empty()
                ? std::nullopt
                : std::optional<std::size_t>(
                      units[draft.soft_wrap_gap_units.back()].end
                  );
        result.lines.push_back(TextLayoutLine{
            draft.start,
            draft.end,
            gap_start,
            gap_end,
            width,
            line_x,
            line_y,
            line_height,
            baseline,
        });
        result.shaped.metrics.width = std::max(result.shaped.metrics.width, width);
        line_y += line_height;
    }
    if (key.wrap_width.has_value() && (result.clipped || result.truncated)) {
        result.shaped.metrics.width = std::min(result.shaped.metrics.width, *key.wrap_width);
    }
    result.shaped.metrics.height = line_y;
    operation_counters_.line_assembly_nanos += static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - line_assembly_started
        ).count()
    );
    return TextLayout(std::make_shared<const TextLayoutData>(std::move(result)));
}

void TextEngine::begin_frame() const noexcept {
    operation_counters_ = {};
}

TextOperationCounters TextEngine::operation_counters() const noexcept {
    return operation_counters_;
}

void TextEngine::record_missing_font(const std::string_view font_id_value) const {
    if (!missing_fonts_.insert(std::string(font_id_value)).second) return;
    diagnostics_.push_back(runtime::RuntimeDiagnostic{
        "STRATA.TEXT.FONT_NOT_REGISTERED",
        "Text layout requested font '" + std::string(font_id_value) +
            "', but it is not registered.",
        {},
        std::nullopt,
        runtime::DiagnosticSeverity::warning,
    });
}

void TextEngine::record_missing_glyphs(
    const std::string_view font_id_value,
    const font::ShapedText& shaped
) const {
    constexpr std::uint32_t group_mask = UINT32_C(255);
    for (const std::uint32_t code_point : shaped.missing_code_points) {
        const std::uint32_t start = code_point & ~group_mask;
        const std::string key = std::string(font_id_value) + ":" + std::to_string(start);
        if (!missing_glyph_groups_.insert(key).second) continue;
        diagnostics_.push_back(runtime::RuntimeDiagnostic{
            "STRATA.TEXT.MISSING_GLYPH",
            "No font in the fallback chain for '" + std::string(font_id_value) +
                "' contains glyphs for code point group U+" + hexadecimal(start) + "-U+" +
                hexadecimal(start + group_mask) + ".",
            {},
            std::nullopt,
            runtime::DiagnosticSeverity::warning,
        });
    }
}

std::vector<runtime::RuntimeDiagnostic> TextEngine::take_diagnostics() const {
    std::vector<runtime::RuntimeDiagnostic> result;
    result.swap(diagnostics_);
    return result;
}

void TextEngine::clear_diagnostics() const noexcept {
    diagnostics_.clear();
    missing_fonts_.clear();
    missing_glyph_groups_.clear();
}

} // namespace strata::ui
