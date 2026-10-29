"""Regenerates the image decoder fixtures and what each should decode to.

Run from anywhere with Pillow installed:  python native/tests/fixtures/images/generate.py

Each fixture is written beside this script with an `<name>.expected.png`: every frame as RGBA,
stacked top to bottom, which the decoder tests compare against (exactly for lossless formats,
within `tolerance` for lossy ones). The expectation is Pillow's decode, except where noted.
manifest.txt lists, one fixture per line:

    <fixture> <expected png> <width> <height> <loop count> <tolerance> <delay ms,delay ms,...>

The loop count is how many times the animation plays, 0 forever. Fully transparent pixels compare
by alpha alone: Pillow keeps a GIF palette colour under zero alpha where the decoder writes
transparent black.
"""

import io
import random
import struct
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageOps

HERE = Path(__file__).resolve().parent
MANIFEST = []


def scene(width, height, seed, alpha=False, colors=None):
    """A deterministic picture with gradients, edges and a little noise, so every codec path runs."""
    rng = random.Random(seed)
    image = Image.new("RGBA", (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            r = (x * 255) // max(1, width - 1)
            g = (y * 255) // max(1, height - 1)
            b = ((x + y) * 7 + rng.randrange(24)) & 0xFF
            a = 255
            if alpha:
                a = min(255, max(0, 40 + (x * 215) // max(1, width - 1)))
                if (x // 5 + y // 5) % 4 == 0:
                    a = 0
            pixels[x, y] = (r, g, b, a)
    draw = ImageDraw.Draw(image)
    draw.ellipse((width // 5, height // 6, width * 3 // 4, height * 5 // 6), fill=(250, 240, 20, 255))
    draw.line((0, height - 1, width - 1, 0), fill=(10, 20, 200, 255), width=2)
    if colors is not None:
        image = image.convert("RGB").quantize(colors, dither=Image.Dither.NONE).convert("RGBA")
    return image if alpha or colors is not None else image.convert("RGB")


def expected_frames(path):
    """Pillow's decode of every frame as RGBA."""
    frames = []
    delays = []
    with Image.open(path) as image:
        loop = image.info.get("loop")
        count = getattr(image, "n_frames", 1)
        for index in range(count):
            image.seek(index)
            frames.append(image.convert("RGBA"))
            delays.append(int(image.info.get("duration", 0) or 0))
    return frames, delays, loop


def record(name, frames, delays, loop, tolerance):
    width, height = frames[0].size
    stacked = Image.new("RGBA", (width, height * len(frames)))
    for index, frame in enumerate(frames):
        stacked.paste(frame, (0, height * index))
    expected = name + ".expected.png"
    stacked.save(HERE / expected, optimize=True)
    MANIFEST.append(
        f"{name} {expected} {width} {height} {loop} {tolerance} "
        + ",".join(str(delay) for delay in delays)
    )


def jpeg(name, image, tolerance=1, transpose=False, **options):
    path = HERE / name
    image.save(path, "JPEG", **options)
    with Image.open(path) as decoded:
        if transpose:
            decoded = ImageOps.exif_transpose(decoded)
        frames = [decoded.convert("RGBA")]
    record(name, frames, [0], 1, tolerance)


def from_pillow(name, loop_when_absent, tolerance, skip_first=False):
    frames, delays, loop = expected_frames(HERE / name)
    if skip_first:
        frames, delays = frames[1:], delays[1:]
    if len(frames) == 1:
        loop = 1
    elif loop is None:
        loop = loop_when_absent
    elif name.endswith(".gif") and loop != 0:
        # NETSCAPE2.0 stores repetitions after the first play; the decoder reports plays.
        loop += 1
    record(name, frames, delays, loop, tolerance)


# --- GIF, written by hand so every extension, palette and disposal is where the test wants it. ---


def lzw(indices, minimum_code_size):
    """GIF's variable-width LZW, emitting a clear code whenever the 12-bit table fills."""
    clear = 1 << minimum_code_size
    end = clear + 1
    out = bytearray()
    bit_buffer = 0
    bit_count = 0
    code_size = minimum_code_size + 1

    def emit(code):
        nonlocal bit_buffer, bit_count
        bit_buffer |= code << bit_count
        bit_count += code_size
        while bit_count >= 8:
            out.append(bit_buffer & 0xFF)
            bit_buffer >>= 8
            bit_count -= 8

    table = {}
    next_code = end + 1
    emit(clear)
    prefix = None
    for index in indices:
        if prefix is None:
            prefix = (index,)
            continue
        candidate = prefix + (index,)
        if candidate in table:
            prefix = candidate
            continue
        emit(table[prefix] if len(prefix) > 1 else prefix[0])
        if next_code < 4096:
            table[candidate] = next_code
            next_code += 1
            # The decoder adds each entry one code later, so it widens one code later too.
            if next_code > (1 << code_size) and code_size < 12:
                code_size += 1
        else:
            emit(clear)
            table = {}
            next_code = end + 1
            code_size = minimum_code_size + 1
        prefix = (index,)
    if prefix is not None:
        emit(table[prefix] if len(prefix) > 1 else prefix[0])
    emit(end)
    if bit_count:
        out.append(bit_buffer & 0xFF)
    blocks = bytearray([minimum_code_size])
    for start in range(0, len(out), 255):
        chunk = out[start : start + 255]
        blocks.append(len(chunk))
        blocks += chunk
    blocks.append(0)
    return bytes(blocks)


def palette_bytes(colors, size):
    data = bytearray()
    for index in range(size):
        data += bytes(colors[index] if index < len(colors) else (0, 0, 0))
    return bytes(data)


def gif_frame(left, top, width, height, indices, delay_cs, disposal, transparent=None,
              local_palette=None, interlace=False, local_size_bits=2):
    data = bytearray()
    packed = (disposal << 2) | (1 if transparent is not None else 0)
    data += b"\x21\xF9\x04" + struct.pack("<BHB", packed, delay_cs, transparent or 0) + b"\x00"
    flags = 0
    if local_palette is not None:
        flags |= 0x80 | (local_size_bits - 1)
    if interlace:
        flags |= 0x40
        rows = [indices[y * width : (y + 1) * width] for y in range(height)]
        order = (list(range(0, height, 8)) + list(range(4, height, 8)) + list(range(2, height, 4))
                 + list(range(1, height, 2)))
        indices = [value for y in order for value in rows[y]]
    data += b"\x2C" + struct.pack("<HHHHB", left, top, width, height, flags)
    if local_palette is not None:
        data += palette_bytes(local_palette, 1 << local_size_bits)
    data += lzw(indices, max(2, local_size_bits if local_palette is not None else 3))
    return bytes(data)


def handmade_gif():
    """A GIF exercising every extension, palette and disposal, with its expected frames composited
    here as browsers composite them: Pillow disposes to the background colour, not to transparent,
    when a frame has no transparent index, so it cannot be the reference for this one."""
    width, height = 20, 18
    global_colors = [(0, 0, 0), (255, 255, 255), (220, 30, 30), (30, 200, 60), (40, 60, 230),
                     (240, 220, 20), (128, 0, 128), (0, 160, 160)]
    out = bytearray(b"GIF89a")
    out += struct.pack("<HHBBB", width, height, 0xF2, 0, 0)  # 8-colour global table
    out += palette_bytes(global_colors, 8)
    out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", 2) + b"\x00"
    out += b"\x21\xFE\x05hello\x00"  # a comment the decoder skips
    local = [(255, 128, 0), (0, 0, 0), (90, 90, 255), (10, 250, 250)]
    block_w, block_h = 9, 11
    frames = [
        # The whole canvas, stripes of the global colours, no transparency.
        dict(rect=(0, 0, width, height), delay=10, disposal=1, transparent=None, palette=None,
             interlace=False,
             indices=[((x // 3) + (y // 4)) % 8 for y in range(height) for x in range(width)]),
        # An interlaced block with its own palette and a transparent hole, restored afterwards.
        dict(rect=(3, 2, block_w, block_h), delay=20, disposal=3, transparent=1, palette=local,
             interlace=True,
             indices=[1 if (x + y) % 5 == 0 else (0 if x < 4 else 2 if y < 6 else 3)
                      for y in range(block_h) for x in range(block_w)]),
        # A band cleared to transparent afterwards.
        dict(rect=(5, 12, 14, 4), delay=0, disposal=2, transparent=None, palette=None,
             interlace=False, indices=[5 if x % 2 else 7 for y in range(4) for x in range(14)]),
        # A small mark, drawn after the band was cleared and the block restored.
        dict(rect=(1, 1, 3, 2), delay=7, disposal=1, transparent=0, palette=None,
             interlace=False, indices=[6, 0, 6, 0, 6, 0]),
    ]
    canvas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    expected = []
    for frame in frames:
        left, top, w, h = frame["rect"]
        out += gif_frame(left, top, w, h, frame["indices"], frame["delay"], frame["disposal"],
                         transparent=frame["transparent"], local_palette=frame["palette"],
                         interlace=frame["interlace"])
        saved = canvas.copy()
        colors = frame["palette"] or global_colors
        for y in range(h):
            for x in range(w):
                index = frame["indices"][y * w + x]
                if index != frame["transparent"]:
                    canvas.putpixel((left + x, top + y), colors[index] + (255,))
        expected.append(canvas.copy())
        if frame["disposal"] == 2:
            canvas.paste((0, 0, 0, 0), (left, top, left + w, top + h))
        elif frame["disposal"] == 3:
            canvas = saved
    out += b"\x3B"
    return bytes(out), expected, [frame["delay"] * 10 for frame in frames]


def gif87a_still():
    width, height = 7, 5
    colors = [(0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)]
    out = bytearray(b"GIF87a")
    out += struct.pack("<HHBBB", width, height, 0x81, 0, 0)
    out += palette_bytes(colors, 4)
    indices = [(x * 3 + y) % 4 for y in range(height) for x in range(width)]
    out += b"\x2C" + struct.pack("<HHHHB", 0, 0, width, height, 0)
    out += lzw(indices, 2)
    out += b"\x3B"
    return bytes(out)


# --- WebP animation assembled from libwebp's own still frames. ---


def webp_chunks(image, **options):
    """The frame chunks (ALPH, VP8 or VP8L) of a still WebP libwebp encodes."""
    buffer = io.BytesIO()
    image.save(buffer, "WEBP", **options)
    data = buffer.getvalue()
    chunks = b""
    offset = 12
    while offset < len(data):
        kind = data[offset : offset + 4]
        size = struct.unpack("<I", data[offset + 4 : offset + 8])[0]
        padded = size + (size & 1)
        if kind in (b"ALPH", b"VP8 ", b"VP8L"):
            chunks += data[offset : offset + 8 + padded]
        offset += 8 + padded
    return chunks


def riff_chunk(kind, payload):
    return kind + struct.pack("<I", len(payload)) + payload + (b"\0" if len(payload) & 1 else b"")


def u24(value):
    return struct.pack("<I", value)[:3]


def handmade_webp_animation():
    """Frames that blend over partial alpha and dispose to the background, which libwebp's own
    encoder rarely chooses for small images."""
    width, height = 24, 18
    frames = []
    base = scene(width, height, 10, alpha=True)
    frames.append((0, 0, base, {"lossless": True}, 60, True, False))
    patch = Image.new("RGBA", (10, 8), (0, 0, 0, 0))
    draw = ImageDraw.Draw(patch)
    draw.rectangle((0, 0, 9, 3), fill=(255, 30, 30, 128))
    draw.rectangle((2, 4, 7, 7), fill=(30, 255, 30, 255))
    frames.append((4, 6, patch, {"lossless": True}, 80, True, True))
    lossy = scene(12, 10, 11, alpha=True)
    frames.append((2, 2, lossy, {"quality": 80, "alpha_quality": 100}, 90, True, False))
    cover = Image.new("RGBA", (6, 6), (0, 0, 200, 200))
    frames.append((16, 10, cover, {"lossless": True}, 100, False, False))
    body = b"WEBP"
    vp8x = bytes([0x12, 0, 0, 0]) + u24(width - 1) + u24(height - 1)
    body += riff_chunk(b"VP8X", vp8x)
    body += riff_chunk(b"ANIM", bytes([255, 255, 255, 255]) + struct.pack("<H", 4))
    for x, y, image, options, duration, blend, dispose in frames:
        header = u24(x // 2) + u24(y // 2) + u24(image.width - 1) + u24(image.height - 1)
        header += u24(duration) + bytes([(0 if blend else 2) | (1 if dispose else 0)])
        body += riff_chunk(b"ANMF", header + webp_chunks(image, **options))
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main():
    # JPEG: libjpeg-turbo through Pillow, which decodes with the integer IDCT and fancy upsampling.
    photo = scene(37, 29, 1)
    jpeg("jpeg_baseline_420.jpg", photo, quality=85, subsampling=2)
    jpeg("jpeg_baseline_444.jpg", photo, quality=85, subsampling=0)
    jpeg("jpeg_baseline_422_restart.jpg", photo, quality=80, subsampling=1, restart_marker_blocks=3)
    jpeg("jpeg_progressive_420.jpg", photo, quality=85, subsampling=2, progressive=True)
    jpeg("jpeg_progressive_444.jpg", photo, quality=90, subsampling=0, progressive=True,
         restart_marker_blocks=2)
    jpeg("jpeg_gray.jpg", photo.convert("L"), quality=85)
    jpeg("jpeg_gray_progressive.jpg", photo.convert("L"), quality=85, progressive=True)
    jpeg("jpeg_cmyk.jpg", photo.convert("CMYK"), quality=90)
    for orientation in (6, 7):
        exif = Image.Exif()
        exif[0x0112] = orientation
        jpeg(f"jpeg_exif_orientation_{orientation}.jpg", scene(23, 14, orientation), quality=90,
             subsampling=2, exif=exif, transpose=True)

    # GIF.
    data, expected, delays = handmade_gif()
    (HERE / "gif_handmade.gif").write_bytes(data)
    record("gif_handmade.gif", expected, delays, 3, 0)  # NETSCAPE 2: three plays
    (HERE / "gif87a_still.gif").write_bytes(gif87a_still())
    from_pillow("gif87a_still.gif", 1, 0)
    frames = []
    for index in range(4):
        frame = Image.new("RGBA", (24, 20), (0, 0, 0, 0))
        draw = ImageDraw.Draw(frame)
        draw.rectangle((2 + index * 4, 3, 9 + index * 4, 12), fill=(200, 40 * index, 30, 255))
        draw.ellipse((12, 8 + index, 22, 18), fill=(20, 120, 220, 255))
        frames.append(frame)
    frames[0].save(HERE / "gif_pillow_transparent.gif", save_all=True, append_images=frames[1:],
                   duration=[50, 60, 70, 80], loop=0, disposal=2)
    from_pillow("gif_pillow_transparent.gif", 1, 0)

    # APNG: binary alpha only, since Pillow pastes rather than composites partial alpha.
    frames = []
    for index in range(4):
        frame = Image.new("RGBA", (22, 17), (0, 0, 0, 0))
        draw = ImageDraw.Draw(frame)
        draw.rectangle((0, 0, 21, 16), fill=(30, 30, 60, 255) if index == 0 else (0, 0, 0, 0))
        draw.rectangle((1 + index * 3, 2, 8 + index * 3, 9), fill=(250, 60 * index, 40, 255))
        draw.line((0, 16, 21, 16 - index * 4), fill=(20, 220, 90, 255))
        frames.append(frame)
    frames[0].save(HERE / "apng_blend_dispose.png", save_all=True, append_images=frames[1:],
                   duration=[40, 80, 120, 160], loop=3, disposal=[0, 1, 2, 0],
                   blend=[0, 1, 1, 0])
    from_pillow("apng_blend_dispose.png", 0, 0)
    frames[0].save(HERE / "apng_hidden_default.png", save_all=True, append_images=frames[1:3],
                   duration=[100, 30, 30], loop=0, default_image=True, disposal=0, blend=0)
    from_pillow("apng_hidden_default.png", 0, 0, skip_first=True)
    scene(9, 7, 4, alpha=True).save(HERE / "png_still.png")
    from_pillow("png_still.png", 1, 0)

    # WebP: libwebp through Pillow.
    scene(31, 23, 5, alpha=True).save(HERE / "webp_lossless.webp", lossless=True, quality=100,
                                      method=6)
    from_pillow("webp_lossless.webp", 1, 0)
    scene(26, 19, 6, colors=12).save(HERE / "webp_lossless_palette.webp", lossless=True, method=4)
    from_pillow("webp_lossless_palette.webp", 1, 0)
    scene(37, 29, 7).save(HERE / "webp_lossy.webp", quality=75, method=4)
    from_pillow("webp_lossy.webp", 1, 1)
    scene(30, 21, 8, alpha=True).save(HERE / "webp_lossy_alpha.webp", quality=70,
                                      alpha_quality=100, method=4)
    from_pillow("webp_lossy_alpha.webp", 1, 1)
    frames = []
    for index in range(3):
        frame = scene(28, 20, 9, alpha=True)
        draw = ImageDraw.Draw(frame)
        draw.rectangle((index * 6, 4, index * 6 + 8, 14), fill=(255, 0, 80 * index, 255))
        frames.append(frame)
    frames[0].save(HERE / "webp_anim_lossless.webp", save_all=True, append_images=frames[1:],
                   duration=[70, 90, 110], loop=0, lossless=True)
    from_pillow("webp_anim_lossless.webp", 0, 0)
    frames[0].save(HERE / "webp_anim_lossy.webp", save_all=True, append_images=frames[1:],
                   duration=[100, 100, 200], loop=2, lossless=False, quality=60,
                   allow_mixed=True)
    from_pillow("webp_anim_lossy.webp", 0, 1)
    (HERE / "webp_anim_handmade.webp").write_bytes(handmade_webp_animation())
    from_pillow("webp_anim_handmade.webp", 0, 1)

    (HERE / "manifest.txt").write_text("\n".join(MANIFEST) + "\n", encoding="ascii", newline="\n")
    for line in MANIFEST:
        name = line.split()[0]
        print(f"{name}: {(HERE / name).stat().st_size} bytes")


if __name__ == "__main__":
    sys.exit(main())
