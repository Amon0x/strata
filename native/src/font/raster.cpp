#include "font/raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

namespace strata::font {
namespace {

[[noreturn]] void freetype_error(const char* operation, const FT_Error error) {
    throw FontError(std::string("FreeType ") + operation + " failed with error " +
                    std::to_string(static_cast<int>(error)));
}

class FreeTypeLibrary final {
  public:
    FreeTypeLibrary() {
        const FT_Error error = FT_Init_FreeType(&library_);
        if (error != FT_Err_Ok)
            freetype_error("initialization", error);
    }

    ~FreeTypeLibrary() {
        if (library_ != nullptr)
            static_cast<void>(FT_Done_FreeType(library_));
    }

    FreeTypeLibrary(const FreeTypeLibrary&) = delete;
    FreeTypeLibrary& operator=(const FreeTypeLibrary&) = delete;

    [[nodiscard]] FT_Library get() const noexcept {
        return library_;
    }
    [[nodiscard]] std::mutex& lifecycle_mutex() noexcept {
        return lifecycle_mutex_;
    }

  private:
    FT_Library library_ = nullptr;
    std::mutex lifecycle_mutex_;
};

[[nodiscard]] FreeTypeLibrary& freetype_library() {
    static FreeTypeLibrary library;
    return library;
}

class FreeTypeFace final {
  public:
    FreeTypeFace(std::shared_ptr<const void> owner, const std::span<const std::uint8_t> bytes)
        : owner_(std::move(owner)) {
        if (bytes.empty() ||
            bytes.size() > static_cast<std::size_t>(std::numeric_limits<FT_Long>::max())) {
            throw FontError("FreeType font memory size cannot be represented");
        }
        FreeTypeLibrary& library = freetype_library();
        const std::scoped_lock lock(library.lifecycle_mutex());
        const FT_Error error =
            FT_New_Memory_Face(library.get(), reinterpret_cast<const FT_Byte*>(bytes.data()),
                               static_cast<FT_Long>(bytes.size()), 0, &face_);
        if (error != FT_Err_Ok)
            freetype_error("memory-face creation", error);
    }

    ~FreeTypeFace() {
        if (face_ == nullptr)
            return;
        FreeTypeLibrary& library = freetype_library();
        const std::scoped_lock lock(library.lifecycle_mutex());
        static_cast<void>(FT_Done_Face(face_));
    }

    FreeTypeFace(const FreeTypeFace&) = delete;
    FreeTypeFace& operator=(const FreeTypeFace&) = delete;

    [[nodiscard]] FT_Face get() const noexcept {
        return face_;
    }
    [[nodiscard]] std::mutex& mutex() noexcept {
        return mutex_;
    }

    void select_size(const FT_F26Dot6 size) {
        if (selected_size_ == size) return;
        const FT_Error error = FT_Set_Char_Size(face_, 0, size, 72U, 72U);
        if (error != FT_Err_Ok) freetype_error("character-size selection", error);
        selected_size_ = size;
    }

  private:
    std::shared_ptr<const void> owner_;
    FT_Face face_ = nullptr;
    FT_F26Dot6 selected_size_ = 0;
    std::mutex mutex_;
};

class FreeTypeFaceCache final {
  public:
    [[nodiscard]] std::shared_ptr<FreeTypeFace> get(const OpenTypeFont& font) {
        std::shared_ptr<const void> owner = font.cache_identity();
        const Key key{
            reinterpret_cast<std::uintptr_t>(owner.get()),
            std::this_thread::get_id(),
        };
        const std::scoped_lock lock(mutex_);
        if (const auto found = faces_.find(key); found != faces_.end()) {
            found->second.last_use = ++clock_;
            return found->second.face;
        }
        auto face = std::make_shared<FreeTypeFace>(std::move(owner), font.source_bytes());
        faces_.emplace(key, Entry{face, ++clock_});
        trim();
        return face;
    }

  private:
    using Key = std::pair<std::uintptr_t, std::thread::id>;

    struct Entry final {
        std::shared_ptr<FreeTypeFace> face;
        std::uint64_t last_use = 0U;
    };

    void trim() {
        constexpr std::size_t maximum_faces = 128U;
        while (faces_.size() > maximum_faces) {
            const auto oldest = std::ranges::min_element(
                faces_, {}, [](const auto& entry) { return entry.second.last_use; });
            faces_.erase(oldest);
        }
    }

    std::mutex mutex_;
    // FT_Face carries mutable size, transform, and glyph-slot state. Caching one face per calling
    // thread lets a bounded raster batch run concurrently without sharing that mutable state.
    std::map<Key, Entry> faces_;
    std::uint64_t clock_ = 0U;
};

[[nodiscard]] FreeTypeFaceCache& freetype_faces() {
    // The library must outlive cached FT_Face objects during process teardown.
    static_cast<void>(freetype_library());
    static FreeTypeFaceCache cache;
    return cache;
}

void validate(const CoverageRasterConfig& config) {
    if (config.maximum_dimension == 0U ||
        !std::isfinite(config.stem_darkening_pixels) ||
        config.stem_darkening_pixels < 0.0 ||
        !std::isfinite(config.stem_darkening_taper_start_physical_pixel_size) ||
        config.stem_darkening_taper_start_physical_pixel_size < 0.0 ||
        !std::isfinite(config.stem_darkening_taper_end_physical_pixel_size) ||
        config.stem_darkening_taper_end_physical_pixel_size <
            config.stem_darkening_taper_start_physical_pixel_size ||
        !std::isfinite(config.transfer_strength) || config.transfer_strength < 0.0 ||
        config.transfer_strength > 1.0) {
        throw FontError("coverage raster configuration is invalid");
    }
    switch (config.hinting) {
    case GlyphHinting::none:
    case GlyphHinting::light:
    case GlyphHinting::full:
        return;
    }
    throw FontError("coverage raster hinting mode is invalid");
}

[[nodiscard]] FT_Int32 load_flags(const GlyphHinting hinting) noexcept {
    switch (hinting) {
    case GlyphHinting::none:
        return FT_LOAD_DEFAULT | FT_LOAD_NO_HINTING | FT_LOAD_NO_AUTOHINT;
    case GlyphHinting::light:
        return FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
    case GlyphHinting::full:
        return FT_LOAD_DEFAULT | FT_LOAD_TARGET_NORMAL;
    }
    return FT_LOAD_DEFAULT;
}

[[nodiscard]] double optical_stem_darkening(
    const CoverageRasterConfig& config,
    const double physical_pixel_size
) noexcept {
    if (config.stem_darkening_pixels == 0.0 ||
        physical_pixel_size <= config.stem_darkening_taper_start_physical_pixel_size) {
        return config.stem_darkening_pixels;
    }
    if (physical_pixel_size >= config.stem_darkening_taper_end_physical_pixel_size ||
        config.stem_darkening_taper_end_physical_pixel_size <=
            config.stem_darkening_taper_start_physical_pixel_size) {
        return 0.0;
    }
    const double progress =
        (physical_pixel_size - config.stem_darkening_taper_start_physical_pixel_size) /
        (config.stem_darkening_taper_end_physical_pixel_size -
         config.stem_darkening_taper_start_physical_pixel_size);
    const double smooth_progress = progress * progress * (3.0 - 2.0 * progress);
    return config.stem_darkening_pixels * (1.0 - smooth_progress);
}

[[nodiscard]] std::uint8_t transferred_coverage(const std::uint8_t value,
                                                const double strength) noexcept {
    if (strength == 0.0 || value == 0U || value == 255U)
        return value;
    const double coverage = static_cast<double>(value) / 255.0;
    const double transferred =
        std::clamp(coverage + strength * coverage * (1.0 - coverage), 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(transferred * 255.0));
}

[[nodiscard]] std::uint8_t gray_value(const FT_Bitmap& bitmap, const std::uint8_t value,
                                      const double transfer_strength) noexcept {
    const std::uint8_t normalized =
        bitmap.num_grays <= 1U || bitmap.num_grays == 256U
            ? value
            : static_cast<std::uint8_t>(std::lround(static_cast<double>(value) * 255.0 /
                                                    static_cast<double>(bitmap.num_grays - 1U)));
    return transferred_coverage(normalized, transfer_strength);
}

} // namespace

double SubpixelPhase::offset() const {
    if (divisions == 0U || index >= divisions)
        throw FontError("subpixel phase is invalid");
    return static_cast<double>(index) / static_cast<double>(divisions);
}

SubpixelPhase SubpixelPhase::quantize(const double physical_position,
                                      const std::uint16_t divisions) {
    if (divisions == 0U)
        throw FontError("subpixel phase divisions must be positive");
    if (!std::isfinite(physical_position) || divisions == 1U)
        return {};
    const double rounded = std::round(physical_position * static_cast<double>(divisions));
    if (rounded < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
        rounded > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
        return {};
    }
    const std::int64_t scaled = static_cast<std::int64_t>(rounded);
    const std::int64_t divisor = divisions;
    const std::int64_t modulus = ((scaled % divisor) + divisor) % divisor;
    return SubpixelPhase{static_cast<std::uint16_t>(modulus), divisions};
}

std::optional<GlyphRasterBitmap>
rasterize_coverage(const OpenTypeFont& font, const std::uint16_t glyph, const double pixel_size,
                   const double display_scale, const SubpixelPhase phase,
                   const CoverageRasterConfig& config, const std::uint32_t font_style_flags) {
    validate(config);
    if (!std::isfinite(pixel_size) || pixel_size <= 0.0 || !std::isfinite(display_scale) ||
        display_scale <= 0.0) {
        throw FontError("coverage raster size and display scale must be finite and positive");
    }
    const double phase_offset = phase.offset();
    const double physical_pixel_size = pixel_size * display_scale;
    const double size_26_6_value = std::round(physical_pixel_size * 64.0);
    if (!std::isfinite(size_26_6_value) || size_26_6_value < 1.0 ||
        size_26_6_value > static_cast<double>(std::numeric_limits<FT_F26Dot6>::max())) {
        throw FontError("coverage raster physical size cannot be represented by FreeType");
    }

    const std::shared_ptr<FreeTypeFace> cached_face = freetype_faces().get(font);
    const std::scoped_lock face_lock(cached_face->mutex());
    FT_Face face = cached_face->get();
    cached_face->select_size(static_cast<FT_F26Dot6>(size_26_6_value));

    const FontStyleGeometry style =
        resolve_font_style_geometry(font_style_flags, font.metadata().style_flags);
    FT_Matrix matrix{
        1L << 16U,
        style.italic() ? static_cast<FT_Fixed>(std::lround(synthetic_italic_shear * 65'536.0)) : 0L,
        0L,
        1L << 16U,
    };
    FT_Vector translation{
        static_cast<FT_Pos>(std::lround(phase_offset * 64.0)),
        0L,
    };
    FT_Set_Transform(face, &matrix, &translation);

    const FT_Error load_error = FT_Load_Glyph(face, glyph, load_flags(config.hinting));
    if (load_error != FT_Err_Ok)
        freetype_error("glyph loading", load_error);
    if (face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        const double darkening = optical_stem_darkening(config, physical_pixel_size);
        const double synthetic_bold =
            style.bold() ? style.bold_strength(physical_pixel_size) : 0.0;
        const FT_Pos x_strength =
            static_cast<FT_Pos>(std::lround((darkening + synthetic_bold) * 64.0));
        const FT_Pos y_strength = static_cast<FT_Pos>(std::lround(darkening * 64.0));
        if (x_strength > 0L || y_strength > 0L) {
            const FT_Error embolden_error =
                FT_Outline_EmboldenXY(&face->glyph->outline, x_strength, y_strength);
            if (embolden_error != FT_Err_Ok)
                freetype_error("outline emboldening", embolden_error);
        }
    }
    const FT_Error render_error = FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL);
    if (render_error != FT_Err_Ok)
        freetype_error("grayscale glyph rendering", render_error);

    const FT_Bitmap& source = face->glyph->bitmap;
    if (source.width == 0U || source.rows == 0U)
        return std::nullopt;
    if (source.pixel_mode != FT_PIXEL_MODE_GRAY || source.buffer == nullptr) {
        throw FontError("FreeType grayscale glyph produced an unsupported bitmap format");
    }

    const std::uint64_t padding = config.padding_pixels;
    const std::uint64_t width64 = static_cast<std::uint64_t>(source.width) + padding * 2U;
    const std::uint64_t height64 = static_cast<std::uint64_t>(source.rows) + padding * 2U;
    if (width64 > config.maximum_dimension || height64 > config.maximum_dimension) {
        throw FontError("coverage raster exceeds its configured maximum dimension");
    }
    if (width64 > std::numeric_limits<std::size_t>::max() / height64) {
        throw FontError("coverage raster pixel count overflows the host size");
    }
    const auto width = static_cast<std::uint32_t>(width64);
    const auto height = static_cast<std::uint32_t>(height64);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(width) * height, 0U);
    const std::size_t source_pitch = static_cast<std::size_t>(
        source.pitch < 0 ? -static_cast<std::int64_t>(source.pitch) : source.pitch);
    for (std::uint32_t row = 0U; row < source.rows; ++row) {
        const std::uint32_t source_row = source.pitch < 0 ? source.rows - row - 1U : row;
        const std::uint8_t* source_bytes =
            source.buffer + static_cast<std::size_t>(source_row) * source_pitch;
        std::uint8_t* destination = bytes.data() +
                                    static_cast<std::size_t>(row + config.padding_pixels) * width +
                                    config.padding_pixels;
        for (std::uint32_t column = 0U; column < source.width; ++column) {
            destination[column] =
                gray_value(source, source_bytes[column], config.transfer_strength);
        }
    }

    const double physical_left =
        static_cast<double>(face->glyph->bitmap_left) - static_cast<double>(padding);
    const double physical_top =
        static_cast<double>(face->glyph->bitmap_top) + static_cast<double>(padding);
    const RasterPlaneBounds layout_bounds{
        (physical_left - phase_offset) / display_scale,
        (physical_top - static_cast<double>(height)) / display_scale,
        (physical_left + static_cast<double>(width) - phase_offset) / display_scale,
        physical_top / display_scale,
    };
    const double layout_scale = pixel_size / static_cast<double>(font.units_per_em());
    const double physical_scale = layout_scale * display_scale;
    return GlyphRasterBitmap{
        width,
        height,
        std::move(bytes),
        RasterPlaneBounds{
            layout_bounds.left / layout_scale,
            layout_bounds.bottom / layout_scale,
            layout_bounds.right / layout_scale,
            layout_bounds.top / layout_scale,
        },
        layout_bounds,
        1.0,
        1.0 / display_scale,
        layout_scale,
        physical_scale,
        GlyphRasterMode::coverage,
    };
}

} // namespace strata::font
