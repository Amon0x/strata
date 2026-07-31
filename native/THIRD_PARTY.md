# Native third-party dependencies

## FreeType 2.13.3

Strata links FreeType statically as the replaceable size-specific grayscale glyph raster backend.
The native core continues to own font bytes, OpenType shaping/layout, raster cache identity, atlas
placement, and resource lifetime; FreeType faces borrow immutable bytes while retaining their
`OpenTypeFont` owner and are serialized per face because `FT_Face` is not concurrently mutable.

CMake fetches the exact 2.13.3 release archive with a pinned SHA-256. Optional compression, PNG,
HarfBuzz, and Brotli integrations are disabled because Strata currently loads ordinary TrueType
outlines and owns shaping separately. Version updates require native text conformance, atlas
lifetime, full Windows build, and F7 performance verification.

FreeType is distributed under the FreeType Project License. The unmodified license is packaged at
`META-INF/licenses/FreeType-FTL.txt` and installed with the native package. Portions of this software
are copyright © 2023 The FreeType Project (www.freetype.org). All rights reserved.

## Roboto 3.016

Strata bundles the upstream hinted static Roboto Regular and Medium TrueType faces as its default UI
font resources. They come from the `v3.016` release of
[`googlefonts/roboto-3-classic`](https://github.com/googlefonts/roboto-3-classic):

- `Roboto-Regular.ttf`: SHA-256 `9f300202f482ad59f8b13bc3131a295744d14fd530fa3765a0a11ec87264d203`
- `Roboto-Medium.ttf`: SHA-256 `80ce163ec5ecd91a883aa20e80d31dc74971b55cf658dd7c5e8d21f4e9fcb417`

Roboto is distributed under the SIL Open Font License 1.1. The upstream license is packaged at
`META-INF/licenses/Roboto-OFL.txt` and installed with the native package.

## JetBrains Mono

Strata bundles JetBrains Mono for its monospaced UI style. The font is distributed under the SIL
Open Font License 1.1; the upstream license is stored beside the font as
`assets/strata/fonts/LICENSE-JetBrainsMono.txt`.

## Lucide icons

The bundled chevron textures are generated from Lucide SVG sources by
`tools/icons/generate_lucide_pngs.py`. Lucide is distributed under the ISC license, with Feather-
derived icons under MIT. The combined upstream notice is packaged as
`META-INF/licenses/Lucide-ISC.txt`.

## Tracy (optional)

When `STRATA_ENABLE_TRACY=ON`, CMake fetches Tracy revision
`05cceee0df3b8d7c6fa87e9638af311dbabc63cb`. Tracy is not enabled in ordinary builds and its source
or profiler executables are not vendored in this repository. Tracy is distributed under the
3-clause BSD license.
