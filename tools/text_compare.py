"""Draws the same text in Chrome and in Strata, and measures the difference.

Same fonts, sizes and colours, as DirectWrite on a real Windows machine draws them. Ink is how much
colour a line lays down, which is what text weighs at viewing distance. A ratio far from 1 is text
that reads heavier or lighter than Chrome's; `err` is the mean pixel difference out of 255 once the
line is aligned, under about 0.3 being the same pixels. A large `err` with `ink` near 1 usually means
glyphs drifted along the line (shaping) or sit lower than the search reaches (line layout).

Needs a built strata_headless, Chrome, and Python with numpy and Pillow:

    python tools/text_compare.py
    python tools/text_compare.py --raster portable --sizes 16,20,24,28 --line 44
"""

import argparse
import json
import pathlib
import shutil
import subprocess

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
HEADLESS = ROOT / "build/cmake/windows-x64/native/RelWithDebInfo/strata_headless.exe"
CHROME = pathlib.Path(r"C:\Program Files\Google\Chrome\Application\chrome.exe")
OUT = ROOT / "build/text-compare"
TEXT = "The quick brown fox jumps over a lazy dog 0123456789 const value = items.map(x => x + 1); {[()]} illicit minimum"
# Name, Strata font, CSS family.
FONTS = [
    ("default", "strata:fonts/default", "strata-default"),
    ("mono", "strata:fonts/mono", "strata-mono"),
    ("segoe", "system:segoeui.ttf", "Segoe UI"),
    ("consolas", "system:consola.ttf", "Consolas"),
    ("cascadia", "system:CascadiaMono.ttf", "Cascadia Mono"),
    ("verdana", "system:verdana.ttf", "Verdana"),
    ("georgia", "system:georgia.ttf", "Georgia"),
    ("comic", "system:comic.ttf", "Comic Sans MS"),
    ("bahnschrift", "system:bahnschrift.ttf", "Bahnschrift"),
]
SCHEMES = {"dark": ("#16181C", "#D4D4D4"), "light": ("#FFFFFF", "#1F2328")}


def chrome(path, scheme, sizes, line, width, height):
    background, ink = SCHEMES[scheme]
    fonts = (ROOT / "src/main/resources/assets/strata/fonts").as_posix()
    escaped = TEXT.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    rows = "".join(
        f"<div style=\"top:{index * line}px;font-family:'{family}';font-size:{size}px\">{escaped}</div>"
        for index, (_, _, family, size) in enumerate(slots(sizes))
    )
    page = path.with_suffix(".html")
    page.write_text(
        f"""<!doctype html><meta charset=utf-8><style>
@font-face{{font-family:strata-default;src:url("file:///{fonts}/default.ttf")}}
@font-face{{font-family:strata-mono;src:url("file:///{fonts}/mono.ttf")}}
html,body{{margin:0;background:{background};color:{ink}}}
div{{position:absolute;left:10px;height:{line}px;line-height:{line}px;white-space:nowrap}}
</style>{rows}""",
        encoding="utf-8",
    )
    subprocess.run(
        [CHROME, "--headless=new", "--force-device-scale-factor=1", "--hide-scrollbars",
         "--allow-file-access-from-files", f"--window-size={width},{height}",
         f"--user-data-dir={OUT / 'chrome-profile'}", f"--screenshot={path}", page.as_uri()],
        check=True, capture_output=True, timeout=120,
    )


def strata(path, scheme, sizes, line, width, height, args):
    background, ink = SCHEMES[scheme]
    resources = OUT / "resources"
    (resources / "ui").mkdir(parents=True, exist_ok=True)
    if not (resources / "assets").exists():
        shutil.copytree(ROOT / "src/main/resources/assets/strata/fonts", resources / "assets/strata/fonts")
    rows = "".join(
        f'    Panel(style: {{ background: #00000000; border: null; padding: 0 }}, layout: {{ width: {width}, height: {line}, position: "ABSOLUTE", inset: {{ left: 10, top: {index * line} }} }}) {{\n'
        f'      Text(text: "{TEXT}", style: {{ font: "{font}"; pixelSize: {size}; color: {ink}FF }}, layout: {{ alignSelf: "CENTER" }})\n    }}\n'
        for index, (_, font, _, size) in enumerate(slots(sizes))
    )
    (resources / "ui/compare.strata").write_text(
        f"overlay Compare {{\n  root Panel(style: {{ background: {background}FF; border: null; radius: 0; padding: 0 }}, layout: {{ width: {width}, height: {height} }}) {{\n{rows}  }}\n}}\n",
        encoding="utf-8",
    )
    scenario = {
        "version": 1,
        "application": {"id": "compare", "module": "ui/compare.strata", "root": "Compare", "actions": []},
        "surface": {
            "id": "compare", "role": "overlay", "backend": "d3d11", "width": width, "height": height, "scale": 1,
            "clearColor": background + "ff", "textAntialiasing": args.antialiasing, "textRasterizer": args.raster,
            "fonts": [
                {"id": "strata:fonts/default-medium", "resource": "assets/strata/fonts/medium.ttf"},
                {"id": "strata:fonts/default", "resource": "assets/strata/fonts/default.ttf"},
                {"id": "strata:fonts/mono", "resource": "assets/strata/fonts/mono.ttf"},
            ],
            "images": [],
        },
        "snapshots": [],
        "steps": [
            {"advance": {"milliseconds": 16, "frames": 2}},
            {"settle": {"milliseconds": 3000}},
            {"advance": {"milliseconds": 16, "frames": 2}},
            {"capture": "text"},
        ],
    }
    capture = path.with_suffix("")
    shutil.rmtree(capture, ignore_errors=True)
    capture.mkdir(parents=True)
    scenario_path = capture / "scenario.json"
    scenario_path.write_text(json.dumps(scenario), encoding="utf-8")
    subprocess.run([HEADLESS, "--resources", resources, "--scenario", scenario_path, "--output", capture], check=True)
    shutil.copy(next(capture.rglob("*.png")), path)


def slots(sizes):
    return [(name, font, family, size) for name, font, family in FONTS for size in sizes]


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float64)


def colour(text):
    return np.array([int(text[index : index + 2], 16) for index in (1, 3, 5)], dtype=np.float64)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--raster", default="system", choices=["system", "portable"])
    parser.add_argument("--antialiasing", default="subpixel", choices=["subpixel", "grayscale"])
    parser.add_argument("--sizes", default="11,12,13,14,15")
    parser.add_argument("--line", type=int, default=22)
    parser.add_argument("--width", type=int, default=1100)
    args = parser.parse_args()
    sizes = [int(size) for size in args.sizes.split(",")]
    height = args.line * len(FONTS) * len(sizes)
    OUT.mkdir(parents=True, exist_ok=True)
    weights = np.array([0.2126, 0.7152, 0.0722])
    for scheme, (background, ink) in SCHEMES.items():
        chrome_png = OUT / f"chrome-{scheme}.png"
        strata_png = OUT / f"strata-{scheme}.png"
        chrome(chrome_png, scheme, sizes, args.line, args.width, height)
        strata(strata_png, scheme, sizes, args.line, args.width, height, args)
        theirs, ours = load(chrome_png), load(strata_png)
        back, front = colour(background) @ weights, colour(ink) @ weights
        ratios = []
        print(f"--- {scheme}: font, size, ink strata/chrome, err at the best alignment, moved by (rows, columns)")
        zoomed = []
        for index, (name, _, _, size) in enumerate(slots(sizes)):
            top = index * args.line
            a = theirs[top : top + args.line, : args.width]
            b = ours[top : top + args.line, : args.width]
            ratio = ((b @ weights - back) / (front - back)).sum() / ((a @ weights - back) / (front - back)).sum()
            err, rows, columns = min(
                (np.abs(np.roll(np.roll(b, r, 0), c, 1) - a).mean(), r, c)
                for r in range(-3, 4)
                for c in (-1, 0, 1)
            )
            ratios.append(ratio)
            print(f"{name:12s}{size:3d}  {ratio:.3f}  {err:6.2f}  {rows:+d} {columns:+d}")
            for image in (a, b):
                piece = Image.fromarray(image[:, :560].astype(np.uint8))
                zoomed.append(piece.resize((1680, args.line * 3), Image.NEAREST))
        sheet = Image.new("RGB", (1680, sum(piece.height for piece in zoomed)))
        for row, piece in enumerate(zoomed):
            sheet.paste(piece, (0, row * piece.height))
        sheet.save(OUT / f"compare-{scheme}.png")
        print(f"mean ink {np.mean(ratios):.3f}, mean distance from 1 {np.mean(np.abs(np.array(ratios) - 1.0)):.3f}")
    print(f"Pairs of lines, Chrome above Strata, three times as large: {OUT}")


main()
