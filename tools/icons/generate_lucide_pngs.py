#!/usr/bin/env python3
import json
from io import BytesIO
from pathlib import Path

import cairosvg
from PIL import Image


ROOT = Path.cwd()
MANIFEST = ROOT / "tools" / "icons" / "manifest.json"


def main():
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    source = ROOT / manifest["source"]
    output = ROOT / manifest["output"]
    size = int(manifest["size"])
    stroke_width = int(manifest["stroke_width"])
    output.mkdir(parents=True, exist_ok=True)

    for name in manifest["icons"]:
        svg_path = source / f"{name}.svg"
        png_path = output / f"{name}.png"
        png_path.write_bytes(render_lucide_icon(svg_path, size, stroke_width))
        print(f"{png_path.relative_to(ROOT)}")


def render_lucide_icon(svg_path, size, stroke_width):
    svg = svg_path.read_text(encoding="utf-8")
    svg = svg.replace('stroke="currentColor"', 'stroke="#ffffff"')
    svg = svg.replace('stroke-width="2"', f'stroke-width="{stroke_width}"')
    rendered = cairosvg.svg2png(
        bytestring=svg.encode("utf-8"),
        output_width=size,
        output_height=size,
    )
    image = Image.open(BytesIO(rendered)).convert("RGBA")
    if image.size != (size, size):
        image = image.resize((size, size), Image.Resampling.LANCZOS)

    output = BytesIO()
    image.save(output, format="PNG")
    return output.getvalue()


if __name__ == "__main__":
    main()
