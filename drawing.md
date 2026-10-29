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
- [ ] `Draw`'s `rect` shapes draw as rounded rects analytically.
- [x] `Draw` gains `sector` (a ring or pie slice with an inner radius) and `ring`.
- [x] Coverage tests (`strata.render.drawing`): every analytic shape, rounded rect and border
      covers its true area within 0.04% (a diagonal half-pixel hairline within 0.32%), at scales
      1, 1.25 and 2, rotated and unrotated, on every backend: an edge biased by a tenth of a pixel
      would show as several times that.

## Phase 2: coverage masks for arbitrary paths

- [ ] A path rasterizer with exact area coverage for fills (nonzero and even-odd), strokes (caps,
      joins, miter limit, dashes) and their combination, through FreeType's smooth rasterizer and
      stroker, which are already linked.
- [ ] A shape mask atlas sharing the glyph atlas's pages, eviction and uploads, keyed by geometry,
      device size and subpixel phase; very large shapes are tiled.
- [ ] Paint per pixel: mask × paint, where paint is a colour or a gradient evaluated in the shader
      from a stop ramp, so multi-stop linear and radial gradients with repeat and mirror are exact on
      every shape, analytic or masked.
- [ ] Translucent strokes, self-overlapping paths and SVG icons render with uniform alpha and no
      seams; the O(E²) union sweep and its throwing limits are gone.
- [ ] Adjacent shapes that share an edge (pie slices, tiles) meet without a halo or a gap.

## Phase 3: change and animation cost

- [ ] Colour, opacity and gradient changes rewrite draw data only; geometry is cached by shape,
      size and scale, so a window resize or an unrelated change re-rasterizes nothing.
- [ ] Analytic primitives under any presentation transform, and masks once a scale settles, stay
      exact; nothing blurs when a presentation group scales up.
- [ ] Shape vertices stop repeating draw data: shared vertices and a per-draw record instead of 64
      bytes of draw data on every vertex.
- [ ] `Draw` shapes animate: when shapes change with the same structure, their numbers, points,
      angles, stroke widths, dash offsets and colours interpolate with a motion policy
      (`animateChanges: { properties: ["shapes"] }`).
- [ ] Charts animate their data: bars grow, lines morph, slices sweep, series fade on legend toggle.
- [ ] `strata_core_benchmark` measures drawing: a 10,000-point line, a 200-slice pie, 500 animated
      arcs, a complex SVG; each has a budget.

## Phase 4: one painter

- [ ] A painter on `WidgetRenderScope` and the extension API: fill and stroke paths, circles,
      ellipses, arcs, sectors, lines, polylines and rounded rectangles with paints, transforms and
      clips, choosing analytic or masked drawing itself.
- [ ] Chart, icons, the calendar and every built-in presenter draw through it.
- [ ] `docs/native-widgets.md` and `docs/strata-language.md` document drawing end to end.
