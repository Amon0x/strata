# Images

Strata exposes PNG and static SVG resources and host-published runtime images through one
author-facing image model. A Surface maps a logical id to resource-adapter bytes, the Runtime maps
runtime image ids to pixels, and `.strata` uses either id with `Image`, a `Draw` image shape or any
widget icon property:

```strata
Image(
  image: "example:brand/logo",
  tint: #70aaff,
  layout: { width: 96, height: 96 }
)
```

The host declares the resource once. The C++ facade accepts both formats through `ImageResource`:

```cpp
strata::SurfaceOptions surface;
surface.images = {
    {"example:brand/logo", "assets/example/logo.svg"},
    {"example:photo", "assets/example/photo.png", strata::ImageSampling::linear},
};
```

The reusable desktop host has the equivalent `strata::desktop::ApplicationConfig::images` field.
Headless and desktop launch documents use the same shape:

```json
"images": [
  {"id": "example:brand/logo", "resource": "assets/example/logo.svg"},
  {"id": "example:photo", "resource": "assets/example/photo.png", "sampling": "linear"}
]
```

At the C boundary, populate `strata_surface_image_resource` and
`strata_surface_config.images/image_count`. Resource paths remain private to the host adapter;
authoring and render packets carry only logical or Surface-scoped ids.

## Rendering model

PNG bytes are inspected at Surface creation, reach the host through packet v12 the first time a
frame samples them, are decoded by the host, and are sampled as ordinary textures. SVG documents are parsed at Surface creation into immutable
display lists. During widget presentation, Strata projects their curves, fills, strokes,
transforms, `viewBox`, and `preserveAspectRatio` into ordinary clipped path commands. Submission
then tessellates those paths at the current logical size and device scale.

SVG is therefore resolution-independent and needs no SVG feature in D3D11, the CPU reference
renderer, or a custom packet-v12 backend. Both desktop and headless rendering consume the same
vertices, indices, materials, and scissors. PNG/SVG image content is immutable for a Surface
lifetime; edited assets take effect after rebuilding the owning artifact and recreating the session.

`Image.tint` modulates literal SVG colors. SVG `currentColor` resolves directly to the tint, which
makes monochrome icon assets naturally themeable. The default white tint preserves literal colors
and renders `currentColor` as white. Image opacity, source regions, root clipping, fill rules, and
source paint order are retained. Raster sampling modes do not apply to SVG geometry.

## Runtime images

A host publishes pictures it only has while running (downloads, generated previews, captures) as
straight-alpha RGBA8 pixels, and replaces or releases them at any time:

```cpp
runtime.publish_image("app:skin/steve", 64, 64, rgba, strata::ImageSampling::nearest);
runtime.release_image("app:skin/steve");
```

At the C boundary these are `strata_runtime_publish_image` and `strata_runtime_release_image`.
Documents reference the id like any static image; until it is published, or after it is released,
the draw is skipped. Each Surface uploads an image the first time one of its frames samples it
(packet v12 encoding 1, raw RGBA8), uploads it again only when it is replaced, and releases it from
the host when it is released. A settled Surface sends nothing for its resident images. Publishing
or releasing an image replans every Surface once, because the set of drawable images changed;
replacing an image with one of the same size only re-uploads its pixels. A Surface's static image
shadows a runtime image with the same id. The Runtime owns images on its thread, like host
snapshots.

## Supported static SVG subset

The built-in parser supports:

- an SVG root and nested `g` containers;
- `path`, `line`, `polyline`, `polygon`, `rect`, `circle`, and `ellipse`;
- all path-data commands, including shorthand curves and elliptical arcs;
- `matrix`, `translate`, `scale`, `rotate`, `skewX`, and `skewY` transforms;
- solid hex/basic-named paints, `none`, and `currentColor`;
- inherited fill/stroke properties, fill/stroke opacity, single-paint element opacity,
  nonzero/even-odd fill rules, stroke width, caps, joins, and miter limits;
- root `viewBox` and `preserveAspectRatio`.

This is intentionally not a browser SVG implementation. Scripts, CSS and `style`/`class`, event
attributes, DTDs/entities, external references, `use`, embedded images, text/fonts, animation,
gradients, patterns, filters, masks, clip paths, `foreignObject`, group opacity, and fractional
element opacity on shapes that combine fill and stroke are rejected. Those opacity cases require an
isolated compositing layer. Unsupported elements and attributes fail loading instead of silently
changing the artwork. Input bytes, elements, attributes, nesting, path segments, flattened points,
intersections, and sweep work are bounded. Candidate resources run their worst-case UI tessellation
before adoption, so over-budget geometry fails creation/reload rather than a later frame.

## Standalone parser and rasterizer

The installed `Strata::svg` CMake target exposes `<strata/svg.hpp>` without requiring the runtime or
a render backend:

```cpp
const strata::svg::Document document = strata::svg::parse(svg_source);
const strata::svg::Image image = strata::svg::rasterize(
    document,
    strata::svg::RasterOptions{256, 256}
);
```

`strata_svg_render INPUT.svg OUTPUT.pam [WIDTH HEIGHT]` is the corresponding command-line utility.
The deterministic CPU rasterizer and PAM encoder are useful for parser tests, fixture generation,
and tooling; UI rendering uses the vector display list directly rather than rasterizing it first.
