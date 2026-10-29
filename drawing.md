# Drawing system

## Goal

Anything a person might draw — an exotic chart, a gauge, a progress ring, an icon, an SVG, a
diagram — renders with exact coverage at any size and transform, matches the shape that was asked
for to a tenth of a pixel, and costs nothing to animate beyond what actually changed. Simple shapes
are evaluated analytically on the GPU; arbitrary paths are rasterized once into coverage masks with
the same quality as text; paint is applied per pixel, so colour, opacity and gradients never touch
geometry. Native widgets, extensions and authored `Draw` shapes all use one painter.

Where it stands (measured October 2026): fills grow by about 0.4 px and strokes by about 1 px
because anti-aliasing ramps sit outside the geometry; non-convex fills have notched corners and
overlapping ramps; adjacent shapes bleed into each other; multi-stop gradients on paths skip stops;
a colour change, a size change or a non-translation transform re-tessellates on the CPU; translucent
strokes go through an O(E²) union sweep that throws past a few thousand edges; shape vertices carry
88 bytes each with nothing shared; and no shape property can be animated except by rebuilding it.

Ground rules:

- D3D11, Vulkan and the software renderer stay in step: one coverage model, the software renderer
  computing the same function per pixel.
- Each item lands with a test: a geometry or coverage measurement against an exact reference, or an
  interaction pin.
- The packet format may change (render packet version bump) where the new model needs it.
- The full `windows-x64` and Vulkan CTest gates pass at the end of each phase.

## Phase 1: analytic primitives

- [x] One coverage model: coverage is the box-filtered share of the pixel the shape covers,
      `clamp(0.5 - d / footprint)` from a Euclidean distance in the shape's own space and the
      pixel's footprint across it (`include/strata/render_shapes.hpp`), so edges sit exactly on the
      geometry under any affine transform, presentation scale included.
- [x] Analytic draw modes in the shared shader and the software renderer: ellipse (fill and
      stroke), ring sector (inner and outer radius, start, sweep, butt or round ends: pies,
      donuts, gauges, arcs), and line segment (butt, round or square caps). Their quads are outset by
      a pixel so no edge is cut.
- [x] Rounded rectangles, borders and shadows use the same model: device-pixel softness, outset
      quads, no clipped outer ramp.
- [x] Shapes keep what they are: circles, ellipses, arcs, sectors and lines carry an analytic
      description next to their path (`ellipse_shape`, `sector_shape`, `arc_shape` and
      `segment_shape` build them in code), and the encoder draws them analytically whenever their
      paint and stroke allow.
- [x] `Draw`'s `rect` shapes draw as rounded rects analytically (circular corners on screen; a
      centred stroke is the rounded-rect border over the stroke's outer edge).
- [x] `Draw` gains `sector` (a ring or pie slice with an inner radius) and `ring`.
- [x] Coverage tests (`strata.render.drawing`): every analytic shape, rounded rect and border
      covers its true area within 0.04% (a diagonal half-pixel hairline within 0.32%), at scales
      1, 1.25 and 2, rotated and unrotated, on every backend: an edge biased by a tenth of a pixel
      would show as several times that.

## Phase 2: coverage masks for arbitrary paths

- [x] A path rasterizer with exact area coverage for fills (nonzero and even-odd), strokes (caps,
      joins, miter limit, dashes) and their combination: strokes expand to a uniformly wound
      outline at device resolution and FreeType's smooth rasterizer takes the nonzero union
      (`font::rasterize_polygons`).
- [x] A shape mask atlas with the glyph atlas's packing, eviction and uploads (its own pages, so
      shapes never evict glyphs), keyed by geometry, device size and subpixel phase. Shapes larger
      than a page draw in 256-pixel tiles with a one-pixel apron, so seams sample their neighbours;
      empty tiles take nothing and full ones one shared texel.
- [x] Paint per pixel: mask × paint, where paint is a colour or a gradient evaluated in the shader
      band by band (one quad per interval between stops, each lerping across its interval), so
      multi-stop linear and radial gradients with repeat and mirror are exact on every outline. A
      gradient-painted analytic shape takes the mask path; rect fills keep their stop-split
      tessellation, which is exact for linear gradients.
- [x] Translucent strokes, self-overlapping paths and SVG icons render with uniform alpha and no
      seams. SVG import no longer tessellates to validate, so its sweep limits no longer reject
      icons; the sweep remains only as the fallback for a shape no mask can take (past 1,024
      tiles or 65,535 outline points), which is left out rather than failing the frame.
- [ ] Adjacent shapes that share an edge (pie slices, tiles) meet without a halo or a gap.

## Phase 3: change and animation cost

- [x] Colour, opacity and gradient changes re-plan only the changed draw and reuse its geometry:
      masks are keyed by outline, size, density and sub-pixel phase, never by paint, and an
      unrelated change re-plans nothing else. (A `Draw` that changes size rasterizes anew, since
      its shapes are normalized to its bounds.)
- [x] Analytic primitives stay exact under any presentation transform (their coverage comes from
      the pixel's own derivatives). Settled motion is baked back into commands, so masks are exact
      at rest; a group that stays a group (a toned node, or a scale held while other motion runs)
      rasterizes its masks at its own density, and a moving zoom at the nearest quarter octave.
- [~] Dropped for now, by measurement: shared vertices and a per-draw record. Mesh encoding is
      0.23 ms of a 1.5 ms frame for 500 animated arcs and 0.01 ms of the 10,000-point line's
      9.4 ms; the cost is in describe, parse and rasterize, not in the 88-byte vertex.
- [x] `Draw` shapes animate: when shapes change with the same structure, their numbers, points,
      angles, stroke widths, dash offsets and colours interpolate with a motion policy
      (`animateChanges: { properties: ["shapes"] }`); a structural change snaps.
- [x] Charts animate their data by default: bars grow, lines morph, slices sweep, and series fade
      (slices sweep closed) on legend toggle. Entry growth from zero is not done.
- [x] `strata_drawing_benchmark` measures drawing through the full pipeline with budgets: a live
      10,000-point line (9.4 ms of 12), a 200-slice pie recoloured (0.7 of 2), 500 arcs animating
      (1.5 of 3) and a 2,000-segment outline moving (0.5 of 1). Getting there replaced FreeType's
      65,535-point polygon rasterizer with an exact-area accumulator that has no point limit,
      rasterizes a tile row once, and raised the path (262,144 segments), flattening and `Draw`
      shape-count (16,384) limits.

## Phase 4: one painter

- [x] A painter on `WidgetRenderScope` (`circle`, `ellipse`, `sector`, `arc`, `line`, `polyline`,
      `polygon`, `path`, with `rounded_rect`, clips and transforms beside them) in logical pixels,
      with colour and gradient paints, each shape choosing analytic or masked drawing itself; and
      the same shapes for extensions through `Present`, carried by the widget table's
      `render_shape` (`STRATA_WIDGET_API_VERSION_2`, colours only).
- [x] Every built-in draws through the one shape path: `Chart` through the painter, icons
      (checkmarks, chevrons, the calendar glyph) and SVG images as `PathShape`s through
      `WidgetRenderScope::shape`, which the painter itself feeds; the calendar's cells are rounded
      rects, which the same coverage model draws.
- [x] `docs/native-widgets.md` (Vector drawing) and `docs/strata-language.md` (Vector drawing,
      Charts) document drawing end to end.
