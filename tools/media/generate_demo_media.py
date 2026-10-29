"""Regenerates the showcase's MEDIA page assets in src/main/resources/assets/strata/media.

Needs Pillow and numpy, and ffmpeg (with libx264) on PATH for the video:

    python tools/media/generate_demo_media.py
"""

import math
import pathlib
import subprocess

import numpy as np
from PIL import Image, ImageChops, ImageDraw, ImageFilter

OUT = pathlib.Path(__file__).resolve().parents[2] / "src/main/resources/assets/strata/media"


def hsv(h, s, v):
    i = int(h * 6) % 6
    f = h * 6 - int(h * 6)
    p, q, t = v * (1 - s), v * (1 - f * s), v * (1 - (1 - f) * s)
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i]
    return int(r * 255), int(g * 255), int(b * 255)


def orbit_gif():
    """Dots orbiting at different speeds, leaving trails: a looping GIF."""
    size, frames = 240, 48
    images = []
    for frame in range(frames):
        t = frame / frames
        image = Image.new("RGB", (size, size), (14, 18, 30))
        draw = ImageDraw.Draw(image)
        for ring in range(5):
            radius = 22 + ring * 20
            draw.ellipse([size / 2 - radius] * 2 + [size / 2 + radius] * 2, outline=(32, 40, 60))
            for trail in range(8):
                angle = 2 * math.pi * (t * (ring + 1) - trail * 0.012 * (ring + 1)) + ring
                x = size / 2 + radius * math.cos(angle)
                y = size / 2 + radius * math.sin(angle)
                dot = 7 - trail * 0.7
                shade = 1 - trail / 9
                color = tuple(int(c * shade) for c in hsv(ring / 5, 0.65, 1))
                draw.ellipse([x - dot, y - dot, x + dot, y + dot], fill=color)
        images.append(image.quantize(colors=64, method=Image.Quantize.MEDIANCUT))
    images[0].save(OUT / "orbit.gif", save_all=True, append_images=images[1:], duration=40, loop=0)


def tunnel_gif():
    """Four hundred frames of a tunnel rushing past: decoded, they take more than an animation is
    held as (92 MB), so it plays from its bytes."""
    width, height, frames = 320, 180, 400
    y, x = np.mgrid[0:height, 0:width]
    dx, dy = x - width / 2, y - height / 2
    radius = np.hypot(dx, dy) + 1e-3
    angle = np.arctan2(dy, dx)
    colors = [hsv(i / 16, 0.7, 0.35 + 0.65 * (i % 2)) for i in range(16)]
    palette = [c for color in colors for c in color] + [0] * (768 - 48)
    images = []
    for frame in range(frames):
        t = frame / frames
        ring = np.floor(1200.0 / radius + t * 40.0).astype(np.int64)
        sector = np.floor((angle / np.pi + 1.0 + t * 4.0) * 3.0).astype(np.int64)
        index = ((ring + sector) % 16).astype(np.uint8)
        image = Image.fromarray(index, "P")
        image.putpalette(palette)
        images.append(image)
    images[0].save(OUT / "tunnel.gif", save_all=True, append_images=images[1:], duration=30, loop=0)


def rings_apng():
    """Translucent rings breathing outward: an APNG with real alpha."""
    size, frames = 220, 40
    images = []
    for frame in range(frames):
        t = frame / frames
        image = Image.new("RGBA", (size, size), (0, 0, 0, 0))
        draw = ImageDraw.Draw(image)
        for ring in range(6):
            phase = (t + ring / 6) % 1
            radius = 8 + phase * 100
            alpha = int(255 * (1 - phase) ** 1.5)
            color = hsv(0.55 + 0.25 * phase, 0.7, 1) + (alpha,)
            draw.ellipse([size / 2 - radius] * 2 + [size / 2 + radius] * 2, outline=color, width=6)
        images.append(image)
    images[0].save(OUT / "rings.png", save_all=True, append_images=images[1:], duration=45, loop=0,
                   disposal=1, blend=0)


def wave_webp():
    """A plasma field drifting: an animated WebP."""
    width, height, frames = 320, 180, 36
    y, x = np.mgrid[0:height, 0:width] / 40.0
    images = []
    for frame in range(frames):
        t = 2 * math.pi * frame / frames
        v = np.sin(x + t) + np.sin(y * 1.3 - t) + np.sin((x + y) * 0.7 + t) + np.sin(np.hypot(x - 4, y - 2) * 1.5 - 2 * t)
        v = (v + 4) / 8
        rgb = np.stack([0.5 + 0.5 * np.sin(2 * math.pi * (v + 0.0)),
                        0.5 + 0.5 * np.sin(2 * math.pi * (v + 0.33)),
                        0.5 + 0.5 * np.sin(2 * math.pi * (v + 0.66))], axis=-1)
        images.append(Image.fromarray((rgb * 255).astype(np.uint8), "RGB"))
    images[0].save(OUT / "plasma.webp", save_all=True, append_images=images[1:], duration=50, loop=0,
                   quality=80)


def landscape_jpeg():
    """A sunset over layered hills: a progressive JPEG photo."""
    width, height = 960, 540
    sky = np.zeros((height, width, 3))
    rows = np.linspace(0, 1, height)[:, None]
    top, horizon = np.array([28, 30, 82]), np.array([255, 150, 92])
    sky[:] = (top + (horizon - top) * rows ** 1.6)[:, None, :]
    image = Image.fromarray(sky.astype(np.uint8), "RGB")
    glow = Image.new("RGB", (width, height))
    ImageDraw.Draw(glow).ellipse([560, 220, 760, 420], fill=(255, 220, 150))
    image = ImageChops.screen(image, glow.filter(ImageFilter.GaussianBlur(70)))
    draw = ImageDraw.Draw(image)
    draw.ellipse([615, 275, 705, 365], fill=(255, 236, 190))
    for layer, (base, color) in enumerate([(330, (88, 60, 96)), (380, (60, 42, 78)), (430, (38, 28, 58)),
                                           (480, (20, 16, 36))]):
        points = [(0, height)]
        for xx in range(0, width + 8, 8):
            yy = base - 40 * math.sin(xx / (140 + 40 * layer) + layer * 1.7) - 18 * math.sin(xx / 47 + layer)
            points.append((xx, yy))
        points.append((width, height))
        draw.polygon(points, fill=color)
    # A little grain, so the long gradients do not band.
    grain = np.random.default_rng(7).normal(0, 1.2, (height, width, 1))
    pixels = np.clip(np.asarray(image, dtype=np.float64) + grain, 0, 255).astype(np.uint8)
    Image.fromarray(pixels, "RGB").save(OUT / "sunset.jpg", quality=90, progressive=True, optimize=True)


def clip_mp4():
    """Ten seconds of a Mandelbrot zoom with a soft chord, as H.264 and AAC."""
    chord = "0.05*sin(2*PI*220*t)+0.035*sin(2*PI*277.18*t)+0.035*sin(2*PI*329.63*t)"
    subprocess.run([
        "ffmpeg", "-y", "-hide_banner", "-loglevel", "error",
        "-f", "lavfi", "-i", "mandelbrot=s=640x360:rate=30:start_scale=3:end_scale=0.002:end_pts=300",
        "-f", "lavfi", "-i", f"aevalsrc={chord}:s=44100",
        "-t", "10", "-af", "afade=t=in:d=1,afade=t=out:st=9:d=1",
        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-profile:v", "main", "-crf", "30", "-preset", "slow",
        "-c:a", "aac", "-b:a", "64k", "-movflags", "+faststart",
        str(OUT / "mandelbrot.mp4"),
    ], check=True)


if __name__ == "__main__":
    OUT.mkdir(parents=True, exist_ok=True)
    orbit_gif()
    tunnel_gif()
    rings_apng()
    wave_webp()
    landscape_jpeg()
    clip_mp4()
    for path in sorted(OUT.iterdir()):
        print(f"{path.name}: {path.stat().st_size} bytes")
