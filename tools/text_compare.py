"""Draws the same text in Chromium and in Strata, and measures the difference.

Same fonts, sizes and colours, as this machine's own text stack draws them. Ink is how much colour a
line lays down, which is what text weighs at viewing distance. A ratio far from 1 is text that reads
heavier or lighter than Chromium's; `err` is the mean pixel difference out of 255 once the line is
aligned, under about 0.3 being the same pixels. A large `err` with `ink` near 1 usually means glyphs
drifted along the line (shaping) or sit lower than the search reaches (line layout).

Needs a built strata_headless, Chromium, and Python with numpy and Pillow. On Linux the Windows fonts
under /mnt/c are used when they are there, since their hinting is what tells rasterizers apart:

    python tools/text_compare.py
    python tools/text_compare.py --raster portable --sizes 16,20,24,28 --line 44
    python3 tools/text_compare.py --headless ~/build/native/strata_headless --chrome ~/chromium/chrome
"""

import argparse
import glob
import json
import os
import pathlib
import shutil
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = pathlib.Path(__file__).resolve().parent.parent
WINDOWS = sys.platform == "win32"
OUT = pathlib.Path(os.path.expanduser("~/.cache/strata-text-compare")) if not WINDOWS else ROOT / "build/text-compare"
TEXT = "The quick brown fox jumps over a lazy dog 0123456789 const value = items.map(x => x + 1); {[()]} illicit minimum"
BUNDLED = ROOT / "src/main/resources/assets/strata/fonts"
SYSTEM_FONTS = pathlib.Path("C:/Windows/Fonts") if WINDOWS else pathlib.Path("/mnt/c/Windows/Fonts")
# Name and file. Linux's own fonts join them where they are installed.
FONTS = [
    ("default", BUNDLED / "default.ttf"),
    ("mono", BUNDLED / "mono.ttf"),
    ("segoe", SYSTEM_FONTS / "segoeui.ttf"),
    ("consolas", SYSTEM_FONTS / "consola.ttf"),
    ("cascadia", SYSTEM_FONTS / "CascadiaMono.ttf"),
    ("verdana", SYSTEM_FONTS / "verdana.ttf"),
    ("georgia", SYSTEM_FONTS / "georgia.ttf"),
    ("comic", SYSTEM_FONTS / "comic.ttf"),
    ("bahnschrift", SYSTEM_FONTS / "bahnschrift.ttf"),
    ("dejavu", pathlib.Path("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf")),
    ("liberation", pathlib.Path("/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf")),
]
FONTS = [font for font in FONTS if font[1].exists()]
SCHEMES = {"dark": ("#16181C", "#D4D4D4"), "light": ("#FFFFFF", "#1F2328")}


def find_chrome():
    if WINDOWS:
        return r"C:\Program Files\Google\Chrome\Application\chrome.exe"
    found = glob.glob(os.path.expanduser("~/.cache/ms-playwright/chromium-*/chrome-linux*/chrome"))
    return found[0] if found else "chromium"


def slots(sizes):
    return [(name, size) for name, _ in FONTS for size in sizes]


def chrome(path, scheme, sizes, args, height):
    background, ink = SCHEMES[scheme]
    faces = "".join(f'@font-face{{font-family:"{name}";src:url("{file.as_uri()}")}}' for name, file in FONTS)
    escaped = TEXT.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    rows = "".join(
        f"<div style=\"top:{index * args.line}px;font-family:'{name}';font-size:{size}px\">{escaped}</div>"
        for index, (name, size) in enumerate(slots(sizes))
    )
    page = path.with_suffix(".html")
    html = f"""<!doctype html><meta charset=utf-8><style>{faces}
html,body{{margin:0;background:{background};color:{ink}}}
div{{position:absolute;left:10px;height:{args.line}px;line-height:{args.line}px;white-space:nowrap}}
</style>{rows}"""
    # Chromium's own drawing only changes with the page, and it can take minutes under WSL.
    if path.exists() and page.exists() and page.read_text(encoding="utf-8") == html:
        return
    page.write_text(html, encoding="utf-8")
    subprocess.run(
        [args.chrome, "--headless=new", "--no-sandbox", "--force-device-scale-factor=1", "--hide-scrollbars",
         "--allow-file-access-from-files", "--virtual-time-budget=4000", f"--window-size={args.width},{height}",
         f"--user-data-dir={OUT / 'chrome-profile'}", f"--screenshot={path}", page.as_uri(), *args.chrome_flag],
        check=True, capture_output=True, timeout=600,
    )


def strata(path, scheme, sizes, args, height):
    background, ink = SCHEMES[scheme]
    resources = OUT / "resources"
    (resources / "ui").mkdir(parents=True, exist_ok=True)
    fonts_dir = resources / "assets/strata/fonts"
    fonts_dir.mkdir(parents=True, exist_ok=True)
    for file in ("default.ttf", "medium.ttf", "mono.ttf"):
        shutil.copyfile(BUNDLED / file, fonts_dir / file)
    for name, file in FONTS:
        # copyfile, not copy: a system font's read-only mode would stop the next run replacing it.
        if not (fonts_dir / f"compare-{name}.ttf").exists():
            shutil.copyfile(file, fonts_dir / f"compare-{name}.ttf")
    rows = "".join(
        f'    Panel(style: {{ background: #00000000; border: null; padding: 0 }}, layout: {{ width: {args.width}, height: {args.line}, position: "ABSOLUTE", inset: {{ left: 10, top: {index * args.line} }} }}) {{\n'
        f'      Text(text: "{TEXT}", style: {{ font: "compare:{name}"; pixelSize: {size}; color: {ink}FF }}, layout: {{ alignSelf: "CENTER" }})\n    }}\n'
        for index, (name, size) in enumerate(slots(sizes))
    )
    (resources / "ui/compare.strata").write_text(
        f"overlay Compare {{\n  root Panel(style: {{ background: {background}FF; border: null; radius: 0; padding: 0 }}, layout: {{ width: {args.width}, height: {height} }}) {{\n{rows}  }}\n}}\n",
        encoding="utf-8",
    )
    scenario = {
        "version": 1,
        "application": {"id": "compare", "module": "ui/compare.strata", "root": "Compare", "actions": []},
        "surface": {
            "id": "compare", "role": "overlay", "backend": "d3d11" if WINDOWS else "reference",
            "width": args.width, "height": height, "scale": 1,
            "clearColor": background + "ff", "textAntialiasing": args.antialiasing, "textRasterizer": args.raster,
            "fonts": [
                {"id": "strata:fonts/default-medium", "resource": "assets/strata/fonts/medium.ttf"},
                {"id": "strata:fonts/default", "resource": "assets/strata/fonts/default.ttf"},
                *({"id": f"compare:{name}", "resource": f"assets/strata/fonts/compare-{name}.ttf"} for name, _ in FONTS),
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
    subprocess.run([args.headless, "--resources", resources, "--scenario", scenario_path, "--output", capture], check=True)
    shutil.copy(next(capture.rglob("*.png")), path)


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
    parser.add_argument("--chrome", default=find_chrome())
    parser.add_argument("--chrome-flag", action="append", default=[])
    parser.add_argument("--headless", default=str(ROOT / "build/cmake/windows-x64/native/RelWithDebInfo/strata_headless.exe"))
    args = parser.parse_args()
    sizes = [int(size) for size in args.sizes.split(",")]
    height = args.line * len(FONTS) * len(sizes)
    OUT.mkdir(parents=True, exist_ok=True)
    weights = np.array([0.2126, 0.7152, 0.0722])
    for scheme, (background, ink) in SCHEMES.items():
        # One screenshot per set of flags: Chromium's gray text is not its subpixel text.
        flags = "".join(flag.strip("-").replace("=", "-") + "-" for flag in args.chrome_flag)
        chrome_png = OUT / f"chrome-{flags}{scheme}.png"
        strata_png = OUT / f"strata-{scheme}.png"
        chrome(chrome_png, scheme, sizes, args, height)
        strata(strata_png, scheme, sizes, args, height)
        theirs, ours = load(chrome_png), load(strata_png)
        back, front = colour(background) @ weights, colour(ink) @ weights
        ratios = []
        print(f"--- {scheme}: font, size, ink strata/chromium, err at the best alignment, moved by (rows, columns)")
        zoomed = []
        for index, (name, size) in enumerate(slots(sizes)):
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
    print(f"Pairs of lines, Chromium above Strata, three times as large: {OUT}")


main()
