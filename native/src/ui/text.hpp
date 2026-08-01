#pragma once

#include <compare>
#include <cstdint>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "font/opentype.hpp"
#include "resource/resource.hpp"
#include "runtime/diagnostic.hpp"
#include "runtime/value.hpp"
#include "ui/layout.hpp"
#include "ui/typography.hpp"

namespace strata::ui {

/** Canonical authoring fields that participate in text measurement/layout cache identity. */
[[nodiscard]] bool text_layout_field(std::string_view name) noexcept;
/** Compares only text-layout-affecting fields of two style/object values. */
[[nodiscard]] bool text_layout_projection_equal(
    const runtime::Value* current,
    const runtime::Value* next
) noexcept;

struct TextOperationCounters final {
    std::size_t requests = 0U;
    std::size_t cache_hits = 0U;
    std::size_t cache_misses = 0U;
    std::uint64_t cache_lookup_nanos = 0U;
    std::uint64_t cache_restore_nanos = 0U;
    std::uint64_t cache_store_nanos = 0U;
    std::uint64_t shaping_nanos = 0U;
    std::uint64_t font_resolution_nanos = 0U;
    std::uint64_t opentype_nanos = 0U;
    std::uint64_t line_assembly_nanos = 0U;
};

struct TextLayoutLine final {
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
    /** UTF-16 source gap trimmed from this line at a soft word-wrap boundary. */
    std::optional<std::size_t> soft_wrap_gap_start_offset;
    std::optional<std::size_t> soft_wrap_gap_end_offset;
    double width = 0.0;
    double x = 0.0;
    double y = 0.0;
    double height = 0.0;
    double baseline = 0.0;
};

/** A contiguous immutable draw run produced by resolved style and per-glyph font fallback. */
struct TextResolvedRun final {
    std::size_t text_start_offset = 0U;
    std::size_t text_end_offset = 0U;
    std::size_t glyph_start_index = 0U;
    std::size_t glyph_end_index = 0U;
    std::size_t line_index = 0U;
    std::size_t style_identity = 0U;
    std::optional<std::size_t> authored_span_index;
    std::string font_id;
    double pixel_size = 0.0;
    double letter_spacing = 0.0;
    std::optional<runtime::ColorValue> color;
    bool interactive = false;
    std::uint32_t font_style_flags = 0U;
    FontRasterization font_rasterization = FontRasterization::grayscale;
};

struct TextLayoutData final {
    font::ShapedText shaped;
    /** Parallel to shaped.glyphs; each glyph retains the face selected by fallback resolution. */
    std::vector<std::string> glyph_font_ids;
    /** Parallel to shaped.glyphs; rich runs may select independent effective pixel sizes. */
    std::vector<double> glyph_pixel_sizes;
    std::vector<TextResolvedRun> resolved_runs;
    std::vector<TextLayoutLine> lines;
    std::optional<double> wrap_width;
    bool clipped = false;
    bool truncated = false;
};

/**
 * Cheap immutable handle shared by measurement, paint, hit testing, selection, and editor
 * geometry. Copies retain one cache-produced geometry object rather than cloning its vectors.
 */
class TextLayout final {
private:
    std::shared_ptr<const TextLayoutData> storage_;
    explicit TextLayout(std::shared_ptr<const TextLayoutData> storage) noexcept;
    friend class TextEngine;

public:
    const font::ShapedText& shaped;
    const std::vector<std::string>& glyph_font_ids;
    const std::vector<double>& glyph_pixel_sizes;
    const std::vector<TextResolvedRun>& resolved_runs;
    const std::vector<TextLayoutLine>& lines;
    const std::optional<double>& wrap_width;
    const bool& clipped;
    const bool& truncated;

    TextLayout();
    TextLayout(const TextLayout& other) noexcept;
    TextLayout(TextLayout&& other) noexcept;
    TextLayout& operator=(const TextLayout& other) noexcept;
    TextLayout& operator=(TextLayout&& other) noexcept;
    [[nodiscard]] bool shares_storage_with(const TextLayout& other) const noexcept;
};

/**
 * Explicit overrides for transient text that has no authored Text/RichText layout node of its own.
 * Typography still inherits from the owner node; only the geometry/overflow contract is replaced.
 */
struct TextLayoutOptions final {
    std::optional<double> wrap_width;
    std::optional<std::string> wrap_mode;
    std::optional<std::string> overflow;
    std::optional<std::size_t> max_lines;
    std::optional<std::string> alignment;

    [[nodiscard]] friend auto operator<=>(const TextLayoutOptions&, const TextLayoutOptions&) = default;
};

/** Surface-owned portable text metrics service; shaping/render caches build on this same font. */
class TextEngine final {
public:
    using FontRegistry = std::map<std::string, font::OpenTypeFont, std::less<>>;

    explicit TextEngine(font::OpenTypeFont control_font);
    TextEngine(font::OpenTypeFont control_font, font::OpenTypeFont regular_font);
    explicit TextEngine(FontRegistry fonts);

    [[nodiscard]] static std::shared_ptr<const TextEngine> load_control_font(
        const std::filesystem::path& root,
        const resource::ResourceId& resource
    );
    [[nodiscard]] static std::shared_ptr<const TextEngine> load_default_fonts(
        const std::filesystem::path& root
    );

    [[nodiscard]] Size measure(
        const RetainedNode& node,
        const Constraints& constraints
    ) const;

    [[nodiscard]] const font::OpenTypeFont& control_font() const noexcept;
    [[nodiscard]] const font::OpenTypeFont& font(std::string_view id) const noexcept;
    [[nodiscard]] std::string font_id(const RetainedNode& node) const;
    [[nodiscard]] double pixel_size(const RetainedNode& node) const noexcept;
    [[nodiscard]] std::uint32_t font_style_flags(const RetainedNode& node) const noexcept;
    [[nodiscard]] FontRasterization font_rasterization(const RetainedNode& node) const noexcept;
    [[nodiscard]] double line_height_multiplier(const RetainedNode& node) const noexcept;
    [[nodiscard]] double letter_spacing(const RetainedNode& node) const noexcept;
    [[nodiscard]] font::ShapedText shape(const RetainedNode& node, std::string_view text) const;
    [[nodiscard]] TextLayout layout(const RetainedNode& node, std::string_view text) const;
    /** Uses the same immutable/cache-backed layout path as authored text with explicit overrides. */
    [[nodiscard]] TextLayout layout(
        const RetainedNode& node,
        std::string_view text,
        const TextLayoutOptions& options
    ) const;
    /** Atomically invalidates shape/layout entries when their external cache token changes. */
    void adopt_generations(
        std::uint64_t scale_context,
        std::uint64_t style_resources,
        std::uint64_t font_resources
    ) const;
    void begin_frame() const noexcept;
    [[nodiscard]] TextOperationCounters operation_counters() const noexcept;
    [[nodiscard]] std::vector<runtime::RuntimeDiagnostic> take_diagnostics() const;
    void clear_diagnostics() const noexcept;

private:
    struct StyleRun final {
        std::size_t start = 0U;
        std::size_t end = 0U;
        std::size_t identity = 0U;
        std::size_t authored_span_index = 0U;
        std::string font_id;
        std::vector<std::string> fallback_fonts;
        double pixel_size = 0.0;
        std::optional<double> line_height;
        double line_height_multiplier = 1.0;
        double letter_spacing = 0.0;
        std::uint32_t font_style_flags = 0U;
        FontRasterization font_rasterization = FontRasterization::grayscale;
        std::optional<std::uint32_t> color_rgba;
        bool interactive = false;
        [[nodiscard]] friend auto operator<=>(const StyleRun&, const StyleRun&) = default;
    };

    struct CacheKey final {
        std::string text;
        std::string font_id;
        std::vector<std::string> fallback_fonts;
        double pixel_size = 0.0;
        std::optional<double> line_height;
        double line_height_multiplier = 0.0;
        double letter_spacing = 0.0;
        std::uint32_t font_style_flags = 0U;
        FontRasterization font_rasterization = FontRasterization::grayscale;
        std::optional<double> wrap_width;
        std::string wrap_mode;
        std::string overflow;
        std::optional<std::size_t> max_lines;
        std::string alignment;
        std::vector<StyleRun> style_runs{};
        [[nodiscard]] friend auto operator<=>(const CacheKey&, const CacheKey&) = default;
    };

    [[nodiscard]] CacheKey request_key(
        const RetainedNode& node,
        std::string_view text,
        std::optional<double> measured_wrap_width,
        const TextLayoutOptions* overrides = nullptr
    ) const;
    [[nodiscard]] const TextLayout& cached_layout(
        CacheKey key,
        bool count_request
    ) const;
    [[nodiscard]] TextLayout build_layout(const CacheKey& key) const;
    [[nodiscard]] std::string requested_font_id(const RetainedNode& node) const;
    void record_missing_glyphs(
        std::string_view font_id,
        const font::ShapedText& shaped
    ) const;
    void record_missing_font(std::string_view font_id) const;

    FontRegistry fonts_;
    mutable std::map<CacheKey, TextLayout> cache_;
    mutable std::map<std::uint64_t, CacheKey> measured_requests_;
    mutable std::set<std::string, std::less<>> missing_fonts_;
    mutable std::set<std::string, std::less<>> missing_glyph_groups_;
    mutable std::vector<runtime::RuntimeDiagnostic> diagnostics_;
    mutable TextOperationCounters operation_counters_;
    mutable std::uint64_t scale_context_generation_ = 0U;
    mutable std::uint64_t style_generation_ = 0U;
    mutable std::uint64_t font_generation_ = 0U;
    mutable bool generations_initialized_ = false;
};

} // namespace strata::ui
