#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace strata::host {

struct RasterPoint final {
    double x = 0.0;
    double y = 0.0;
};

/**
 * A raster job: a picture the backend rasterizes into a region of an atlas, from outlines rather
 * than texels. The core flattens and strokes every outline on the CPU and sends its edges; the
 * backend accumulates their exact area into cells and resolves the cells into coverage, on the GPU
 * where it has one. Every step after quantization is integer arithmetic, so a GPU and the
 * reference (rasterize_job) produce identical coverage.
 *
 * Coordinates are fixed point, raster_one units a pixel, relative to the job's content rectangle.
 * Every edge lies within the content (the encoder clips: edges above or below are dropped, the
 * part of an edge right of the content too, and the part left of it is kept as a vertical edge on
 * the content's left side, which is all a pixel to its right sees of it). No edge spans more than
 * raster_piece units along either axis, which keeps every product in 32 bits.
 *
 * A layer may also be a stroke: its centreline as pieces (RasterStrokePiece) and a stroke style,
 * which the backend expands itself, a piece a thread: the piece's band, its join with the next
 * piece and the caps of a run's ends, each a convex polygon wound one way, so under the nonzero
 * rule their union is the stroke. Expansion is floating point, so a GPU's stroke may differ from
 * the reference's by a level along an edge.
 *
 * A layer may instead be art: an opt-in GPU program (HLSL) whose `artPoint(index, count)` gives
 * each point of an outline in the canvas's 0-1 space, which the backend strokes or fills itself,
 * so a picture that moves every frame costs the CPU nothing. Art is decorative and GPU-only: the
 * reference rasterizer leaves an art layer empty, and backends keep art jobs live, running them
 * again each frame they render with the time, `artTime()`, in seconds. A program may weight each
 * point (`artSetWeight`), scaling the coverage of the stroke piece it starts.
 *
 * A job for an R8 atlas has one coverage layer. A job for an RGBA8 atlas has painted layers,
 * front first, composited by area: a layer takes what the layers in front leave uncovered at its
 * full strength, and what they cover through their transparency. The result is straight RGBA.
 * Texels of the operation's region outside the content are cleared.
 */
inline constexpr std::int32_t raster_one = 256;
inline constexpr std::int32_t raster_piece = 16 * raster_one;
inline constexpr std::uint32_t raster_lut_size = 256U;
inline constexpr std::uint32_t maximum_raster_layers = 64U;
/** The largest content edge in pixels: an atlas page. */
inline constexpr std::uint32_t maximum_raster_extent = 4096U;

enum class RasterFill : std::uint32_t { nonzero = 0U, even_odd = 1U };
/**
 * What a layer paints: coverage alone (an R8 job's one layer), one colour, or a gradient whose
 * parameter is an affine function of the pixel centre (linear) or the length of a two-row affine
 * map of it (radial), looked up in a raster_lut_size colour table after its extend.
 */
enum class RasterPaint : std::uint32_t { coverage = 0U, solid = 1U, linear = 2U, radial = 3U };
enum class RasterExtend : std::uint32_t { clamp = 0U, repeat = 1U, mirror = 2U };
enum class RasterJoin : std::uint32_t { miter = 0U, round = 1U, bevel = 2U };
enum class RasterCap : std::uint32_t { butt = 0U, round = 1U, square = 2U };

/** A stroke piece joins the next piece at its end. */
inline constexpr std::uint32_t raster_piece_join = 1U;
/** A stroke piece starts a run, which its cap closes. */
inline constexpr std::uint32_t raster_piece_start_cap = 2U;
/** A stroke piece ends a run, which its cap closes. */
inline constexpr std::uint32_t raster_piece_end_cap = 4U;

struct RasterSegment final {
    std::int32_t x0 = 0;
    std::int32_t y0 = 0;
    std::int32_t x1 = 0;
    std::int32_t y1 = 0;
};

/**
 * One segment of a stroke's centreline in content pixels, from (x0, y0) to (x1, y1), with the
 * point after it (x2, y2) when it joins the next segment.
 */
struct RasterStrokePiece final {
    float x0 = 0.0F;
    float y0 = 0.0F;
    float x1 = 0.0F;
    float y1 = 0.0F;
    float x2 = 0.0F;
    float y2 = 0.0F;
    std::uint32_t flags = 0U;
    std::uint32_t reserved = 0U;
};

/** A stroke's style, in content pixels. */
struct RasterStroke final {
    float half_width = 0.0F;
    RasterJoin join = RasterJoin::miter;
    RasterCap cap = RasterCap::butt;
    float miter_limit = 4.0F;
    /** How far a round join's or cap's chords may stray from the arc. */
    float tolerance = 0.03F;
};

/** An art layer's program and its canvas (see the job description above). */
struct RasterArt final {
    /** HLSL defining `float2 artPoint(uint index, uint count)`. */
    std::string source;
    std::uint32_t count = 0U;
    /** Whether the last point joins the first. */
    bool closed = false;
    /** Whether points pair off into lines of their own, each capped (stroked only). */
    bool segments = false;
    /** Stroked with the layer's stroke, or filled by the layer's fill rule. */
    bool stroked = false;
    /** The canvas in content pixels: a point (u, v) lies at (x + u width, y + v height). */
    float x = 0.0F;
    float y = 0.0F;
    float width = 0.0F;
    float height = 0.0F;
    /** Read by `artParameter(index)`. */
    std::array<float, 16U> parameters{};
};
inline constexpr std::uint32_t maximum_art_points = 1U << 20U;
inline constexpr std::size_t maximum_art_source = 64U * 1024U;

/** A run of a stroke's centreline: its points, and whether its last joins its first. */
struct RasterStrokeRun final {
    std::vector<RasterPoint> points;
    bool closed = false;
};

struct RasterLayer final {
    RasterFill fill = RasterFill::nonzero;
    RasterPaint paint = RasterPaint::coverage;
    /** Straight RGBA8, red in the low byte. */
    std::uint32_t color = 0xFFFFFFFFU;
    RasterExtend extend = RasterExtend::clamp;
    /**
     * The gradient parameter at content pixel centre (x, y): linear p0 x + p1 y + p2; radial the
     * length of (p0 x + p1 y + p2, p3 x + p4 y + p5).
     */
    std::array<float, 6U> parameters{};
    std::uint32_t first_segment = 0U;
    std::uint32_t segment_count = 0U;
    /** A stroke layer's style and its centreline pieces. */
    RasterStroke stroke;
    std::uint32_t first_piece = 0U;
    std::uint32_t piece_count = 0U;
    /** An art layer's program, instead of segments or pieces. */
    std::optional<RasterArt> art;
    /** raster_lut_size packed colours for a gradient, empty otherwise. */
    std::vector<std::uint32_t> lut;
};

struct RasterJob final {
    /** The content rectangle within the operation's region. */
    std::uint32_t x = 0U;
    std::uint32_t y = 0U;
    std::uint32_t width = 0U;
    std::uint32_t height = 0U;
    std::vector<RasterLayer> layers;
    std::vector<RasterSegment> segments;
    std::vector<RasterStrokePiece> pieces;
};

/** Builds a job's bytes from closed polygons in pixels. */
class RasterJobBuilder final {
  public:
    /** A job whose content is `width` x `height` pixels. */
    RasterJobBuilder(std::uint32_t width, std::uint32_t height);

    /**
     * Adds a layer behind the ones added before it. Each polygon is closed (its last point joins
     * its first); `origin` is subtracted from every point to place it on the content.
     */
    void add_layer(RasterFill fill, const RasterLayer& paint,
                   std::span<const std::vector<RasterPoint>> polygons, RasterPoint origin);
    /**
     * Adds a stroke layer behind the ones added before it: the backend expands the runs by
     * `stroke`. Pieces too far from the content to reach it are left out.
     */
    void add_stroke_layer(const RasterLayer& paint, const RasterStroke& stroke,
                          std::span<const RasterStrokeRun> runs, RasterPoint origin);
    /**
     * Adds an art layer behind the ones added before it: `art` strokes with `stroke` when it is
     * stroked, and fills by `fill` otherwise.
     */
    void add_art_layer(RasterFill fill, const RasterLayer& paint, const RasterStroke& stroke,
                       RasterArt art);
    /** The job's content size. */
    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    /** The job, its content at (`x`, `y`) of the operation's region. */
    [[nodiscard]] std::vector<std::uint8_t> encode(std::uint32_t x, std::uint32_t y) const;

  private:
    void add_edge(double ax, double ay, double bx, double by);
    void add_piece(std::int64_t x0, std::int64_t y0, std::int64_t x1, std::int64_t y1);

    std::uint32_t width_ = 0U;
    std::uint32_t height_ = 0U;
    std::vector<RasterLayer> layers_;
    std::vector<RasterSegment> segments_;
    std::vector<RasterStrokePiece> pieces_;
};

/** Whether a job draws art, which a backend keeps live and runs again each frame. */
[[nodiscard]] bool raster_job_has_art(const RasterJob& job) noexcept;

/** How far a stroke reaches from its centreline: its half width, a mitre's or a square's more. */
[[nodiscard]] double raster_stroke_reach(const RasterStroke& stroke) noexcept;

/**
 * Reads and validates a job for a `region_width` x `region_height` region of an atlas of
 * `format` (a texture_format_* of render_packet.hpp). Throws std::invalid_argument when the bytes
 * are not a valid job for it.
 */
[[nodiscard]] RasterJob decode_raster_job(std::span<const std::uint8_t> bytes,
                                          std::uint32_t region_width,
                                          std::uint32_t region_height, std::uint32_t format);

/**
 * The reference rasterizer: writes the job's region into `pixels`, rows `row_bytes` apart, one
 * byte a texel for an R8 format and four for RGBA8. GPU backends reproduce it exactly.
 */
void rasterize_job(const RasterJob& job, std::uint32_t format, std::uint32_t region_width,
                   std::uint32_t region_height, std::span<std::uint8_t> pixels,
                   std::size_t row_bytes);

} // namespace strata::host
