# Static SVG module

`strata::svg` is a portable parser and deterministic CPU rasterizer. It owns its document, path,
paint, transform, and image types and has no dependency on Strata's UI tree, runtime, resource
loader, render packets, or host implementations.

## Supported static subset

- SVG roots and nested `g` containers
- `path`, `line`, `polyline`, `polygon`, `rect`, `circle`, and `ellipse`
- all SVG path-data commands, including shorthand curves and elliptical arcs
- affine `matrix`, `translate`, `scale`, `rotate`, `skewX`, and `skewY` transforms
- solid `fill` and `stroke` paints using hex, the basic named colors, `none`, or `currentColor`
- fill rules, fill/stroke opacity, stroke width, line caps, line joins, and miter limits
- root `viewBox` and `preserveAspectRatio`

The parser resolves inherited presentation attributes into a flat, renderer-neutral draw list.
Quadratic curves and arcs become cubic curves; source order remains paint order. The rasterizer uses
bounded adaptive curve flattening and deterministic supersampling and returns straight-alpha RGBA8.

## Deliberate exclusions and limits

This is not a browser and never tries to interpret browser SVG. It rejects scripts, stylesheets and
`style`/`class` attributes, events, DTDs and entities, external references, `use`, images, fonts,
animation, gradients, patterns, filters, masks, clip paths, and `foreignObject`. Unsupported elements
or attributes fail parsing instead of silently changing the image. Group opacity is also rejected
because correct group opacity requires an isolated compositing layer.

Default limits cap source bytes, element/attribute counts, nesting, and path segments. Raster output,
supersampling, temporary memory, flattened curve points, and total point-in-geometry work are
separately bounded. `ParseOptions::current_color` provides the only context-sensitive paint value
and does not require a CSS engine.

`strata_svg_render INPUT.svg OUTPUT.pam [WIDTH HEIGHT]` is the standalone end-to-end utility. PAM was
chosen because it preserves RGBA without adding a PNG or platform-image dependency.
