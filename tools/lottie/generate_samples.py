"""Writes the showcase's LOTTIE page animations to src/main/resources/assets/strata/lottie.

Each animation is authored here as plain Bodymovin (Lottie) JSON, so the files are ours and show
the features UI animations lean on:

    python tools/lottie/generate_samples.py

spinner    trim paths (start, end and offset) on a gradient stroke, with layer rotation
success    a bouncy (overshooting bezier) scale, a parented check drawn on by trim, a ring burst
like       a pressed-then-popping heart, a repeater burst of dots, an expanding ring
toggle     a morphing knob path, a separated-position slide, colour keyframes; two segments
reveal     masks (add, subtract, expansion, opacity), alpha and luma track mattes, a precomp
           with time remap, a dot following a spatial bezier with auto-orient, skew
badge      a polystar with round corners and hold keyframes, a radial gradient, dashes with
           a moving offset, merge paths (subtract) and a polygon
blend      layer blend modes (multiply, screen, overlay, difference, add) and a layer faded as
           one picture
feather    a feathered mask sweeping over a card, and an inverted feathered vignette
matte      a gradient luma matte wiping stripes in, and a gradient alpha matte
type       text layers in a system font: a range selector's wave of position, scale and colour,
           and a typewriter reveal
photo      image layers, one embedded as a data URI and one a file beside the animation, under
           a rounded mask with a drop shadow
effects    layer effects: drop shadow, an animated gaussian blur, fill, tint, tritone and levels
"""

import base64
import json
import math
import pathlib
import struct
import zlib

OUT = pathlib.Path(__file__).resolve().parents[2] / "src/main/resources/assets/strata/lottie"

# Easing handles: (out x, out y) leaves a keyframe, (in x, in y) arrives at the next.
EASE = ((0.33, 0.0), (0.67, 1.0))
EASE_OUT = ((0.2, 0.0), (0.1, 1.0))
LINEAR = ((0.0, 0.0), (1.0, 1.0))
BACK = ((0.34, 1.56), (0.64, 1.0))  # overshoots its target, then settles


def value(v):
    return {"a": 0, "k": v}


def keys(*frames, ease=EASE):
    """Keyframes from (time, value[, ease | "hold" | {"to":..,"ti":..}]) tuples."""
    out = []
    for index, frame in enumerate(frames):
        t, v = frame[0], frame[1]
        extra = frame[2] if len(frame) > 2 else ease
        entry = {"t": t, "s": v if isinstance(v, list) else [v]}
        if index + 1 < len(frames):
            if extra == "hold":
                entry["h"] = 1
            else:
                spatial = None
                handles = ease
                if isinstance(extra, dict):
                    spatial = extra
                else:
                    handles = extra
                (ox, oy), (ix, iy) = handles
                entry["o"] = {"x": [ox], "y": [oy]}
                entry["i"] = {"x": [ix], "y": [iy]}
                if spatial:
                    entry["to"] = spatial["to"]
                    entry["ti"] = spatial["ti"]
        out.append(entry)
    return {"a": 1, "k": out}


def rgb(hex_color, alpha=1.0):
    hex_color = hex_color.lstrip("#")
    return [int(hex_color[i:i + 2], 16) / 255 for i in (0, 2, 4)] + [alpha]


def transform(p=(0, 0), a=(0, 0), s=(100, 100), r=0, o=100, sk=0, sa=0):
    def prop(x):
        return x if isinstance(x, dict) else value(list(x) if isinstance(x, tuple) else x)
    return {"ty": "tr", "p": prop(p), "a": prop(a), "s": prop(s), "r": prop(r), "o": prop(o),
            "sk": prop(sk), "sa": prop(sa), "nm": "Transform"}


def group(name, *items, **tr):
    return {"ty": "gr", "nm": name, "it": list(items) + [transform(**tr)]}


def ellipse(size, p=(0, 0), d=1):
    return {"ty": "el", "p": value(list(p)), "s": size if isinstance(size, dict) else value(list(size)), "d": d}


def rect(size, p=(0, 0), r=0, d=1):
    return {"ty": "rc", "p": p if isinstance(p, dict) else value(list(p)),
            "s": size if isinstance(size, dict) else value(list(size)),
            "r": r if isinstance(r, dict) else value(r), "d": d}


def path(vertices, closed=True, in_tangents=None, out_tangents=None):
    n = len(vertices)
    return {"c": closed, "v": [list(v) for v in vertices],
            "i": [list(t) for t in (in_tangents or [(0, 0)] * n)],
            "o": [list(t) for t in (out_tangents or [(0, 0)] * n)]}


def shape(ks):
    return {"ty": "sh", "ks": ks, "d": 1}


def oval(cx, cy, rx, ry):
    """A four-vertex ellipse outline, as After Effects keys one, for morphing."""
    k = 0.5523
    return path([(cx, cy - ry), (cx + rx, cy), (cx, cy + ry), (cx - rx, cy)],
                in_tangents=[(-rx * k, 0), (0, -ry * k), (rx * k, 0), (0, ry * k)],
                out_tangents=[(rx * k, 0), (0, ry * k), (-rx * k, 0), (0, -ry * k)])


def fill(color, o=100, r=1):
    return {"ty": "fl", "c": color if isinstance(color, dict) else value(color),
            "o": o if isinstance(o, dict) else value(o), "r": r}


def stroke(color, w, o=100, lc=2, lj=2, ml=4, dashes=None):
    item = {"ty": "st", "c": color if isinstance(color, dict) else value(color),
            "o": o if isinstance(o, dict) else value(o),
            "w": w if isinstance(w, dict) else value(w), "lc": lc, "lj": lj, "ml": ml}
    if dashes:
        item["d"] = dashes
    return item


def gradient(kind, stops, start, end, t=1, w=None, opacity_stops=None):
    flat = []
    for offset, color in stops:
        flat += [offset] + rgb(color)[:3]
    for offset, alpha in opacity_stops or []:
        flat += [offset, alpha]
    item = {"ty": kind, "o": value(100), "s": value(list(start)), "e": value(list(end)), "t": t,
            "g": {"p": len(stops), "k": value(flat)}, "r": 1}
    if kind == "gs":
        item.update({"w": value(w), "lc": 2, "lj": 2, "ml": 4})
    return item


def trim(s, e, o=0, m=1):
    def prop(x):
        return x if isinstance(x, dict) else value(x)
    return {"ty": "tm", "s": prop(s), "e": prop(e), "o": prop(o), "m": m}


def layer(index, name, shapes, op, ks=None, parent=None, **extra):
    base = {"ddd": 0, "ind": index, "ty": 4, "nm": name, "sr": 1, "ks": ks or layer_ks(),
            "ao": 0, "shapes": shapes, "ip": 0, "op": op, "st": 0, "bm": 0}
    if parent is not None:
        base["parent"] = parent
    base.update(extra)
    return base


def layer_ks(p=(0, 0), a=(0, 0), s=(100, 100), r=0, o=100, sk=None, sa=None):
    def prop(x, three=False):
        if isinstance(x, dict):
            return x
        if isinstance(x, tuple):
            return value(list(x) + ([0] if three else []))
        return value(x)
    ks = {"o": prop(o), "r": prop(r), "p": prop(p, True), "a": prop(a, True), "s": prop(s, True)}
    if sk is not None:
        ks["sk"] = prop(sk)
        ks["sa"] = prop(sa or 0)
    return ks


def composition(name, w, h, op, layers, fr=60, assets=None):
    return {"v": "5.7.4", "fr": fr, "ip": 0, "op": op, "w": w, "h": h, "nm": name, "ddd": 0,
            "assets": assets or [], "layers": layers}


def spinner():
    op = 120
    track = layer(2, "track", [group("ring", ellipse((120, 120)),
                                     stroke(rgb("#8a93a6"), 14, o=28))], op,
                  ks=layer_ks(p=(100, 100)))
    # The visible arc grows, then shrinks from its tail; the offset moves on by the 24% the tail
    # travelled, so the last frame meets the first and the loop has no seam.
    arc = group(
        "arc",
        ellipse((120, 120)),
        trim(keys((0, 0), (60, 0), (op, 76)), keys((0, 2), (60, 78), (op, 78)),
             keys((0, 0), (op, 86.4), ease=LINEAR)),
        gradient("gs", [(0, "#38bdf8"), (0.5, "#818cf8"), (1, "#e879f9")], (-60, -60), (60, 60),
                 w=14),
    )
    spin = layer(1, "arc", [arc], op,
                 ks=layer_ks(p=(100, 100), r=keys((0, 0), (op, 360), ease=LINEAR)))
    return composition("spinner", 200, 200, op, [spin, track])


def success():
    op = 90
    disc = layer(1, "disc", [group("disc", ellipse((140, 140)), fill(rgb("#22c55e")))], op,
                 ks=layer_ks(p=(100, 100), s=keys((0, [0, 0, 100], BACK), (24, [100, 100, 100]))))
    # The check is parented to the disc: it rides the disc's bounce.
    check = layer(
        2, "check",
        [group("check",
               shape(value(path([(-32, 2), (-9, 25), (34, -20)], closed=False))),
               trim(0, keys((18, 0), (48, 100), ease=EASE_OUT)),
               stroke(rgb("#ffffff"), 13))],
        op, ks=layer_ks(), parent=1)
    ring = layer(
        3, "ring",
        [group("ring", ellipse(keys((8, [140, 140]), (40, [196, 196]), ease=EASE_OUT)),
               stroke(rgb("#86efac"), keys((8, 10), (40, 0), ease=EASE_OUT)))],
        op, ks=layer_ks(p=(100, 100), o=keys((8, 90), (40, 0))), ip=8)
    return composition("success", 200, 200, op, [check, disc, ring])


def heart_path():
    v = [(0, 34), (-44, -6), (-24, -38), (0, -22), (24, -38), (44, -6)]
    i = [(10, -6), (0, 22), (-16, 0), (-8, -12), (-12, 0), (6, -14)]
    o = [(-10, -6), (-6, -14), (12, 0), (8, -12), (16, 0), (0, 22)]
    return path(v, in_tangents=i, out_tangents=o)


def like():
    op = 75
    heart = layer(
        1, "heart",
        [group("heart", shape(value(heart_path())),
               fill(keys((0, rgb("#9ca3af")), (10, rgb("#ff3b5c")), ease=LINEAR)))],
        op,
        ks=layer_ks(p=(100, 104), s=keys((0, [100, 100, 100]), (8, [72, 72, 100], BACK),
                                          (28, [118, 118, 100]), (44, [100, 100, 100]))))
    # One dot, repeated eight times round the centre: it flies out and shrinks away.
    dot = group(
        "dot",
        ellipse((12, 12)),
        fill(rgb("#ffb703")),
        p=keys((10, [0, -44]), (40, [0, -86]), ease=EASE_OUT),
        s=keys((10, [100, 100]), (40, [0, 0])),
    )
    accent = group(
        "accent",
        ellipse((8, 8)),
        fill(rgb("#fb7185")),
        p=keys((12, [0, -40]), (42, [0, -70]), ease=EASE_OUT),
        s=keys((12, [100, 100]), (42, [0, 0])),
        r=22.5,
    )
    repeater = {"ty": "rp", "c": value(8), "o": value(0), "m": 1,
                "tr": {"ty": "tr", "a": value([0, 0]), "p": value([0, 0]), "s": value([100, 100]),
                       "r": value(45), "so": value(100), "eo": value(70)}}
    burst = layer(2, "burst", [group("burst", dot, accent, repeater)], op,
                  ks=layer_ks(p=(100, 104)), ip=10)
    ring = layer(
        3, "ring",
        [group("ring", ellipse(keys((6, [30, 30]), (30, [170, 170]), ease=EASE_OUT)),
               stroke(rgb("#ff3b5c"), keys((6, 18), (30, 0), ease=EASE_OUT)))],
        op, ks=layer_ks(p=(100, 104)), ip=6)
    return composition("like", 200, 200, op, [heart, burst, ring])


def toggle():
    op = 60
    off, on = rgb("#4b5563"), rgb("#22c55e")
    track = layer(
        2, "track",
        [group("track", rect((176, 88), r=44),
               fill(keys((0, off), (24, on), (30, on), (54, off), ease=EASE)))],
        op, ks=layer_ks(p=(120, 70)))
    # The knob stretches into a pill on its way across and settles round at the far side.
    knob_shape = keys((0, [oval(0, 0, 34, 34)]), (12, [oval(0, 0, 46, 31)]),
                      (24, [oval(0, 0, 34, 34)]), (30, [oval(0, 0, 34, 34)]),
                      (42, [oval(0, 0, 46, 31)]), (54, [oval(0, 0, 34, 34)]))
    knob = layer(
        1, "knob",
        [group("knob", shape(knob_shape), fill(rgb("#ffffff"))),
         group("shadow", shape(knob_shape), fill(rgb("#000000"), o=22), p=(0, 4))],
        op,
        ks={"o": value(100), "r": value(0), "a": value([0, 0, 0]), "s": value([100, 100, 100]),
            "p": {"s": True, "x": keys((0, 76), (24, 164), (30, 164), (54, 76), ease=EASE),
                  "y": value(70)}})
    return composition("toggle", 240, 140, op, [knob, track])


def star_layer(index, op):
    star = {"ty": "sr", "sy": 1, "d": 1, "p": value([0, 0]), "pt": value(5),
            "r": keys((0, 0), (op, 144), ease=LINEAR), "or": value(70), "ir": value(32),
            "os": value(0), "is": value(0)}
    return layer(index, "star matte", [group("star", star, fill(rgb("#ffffff")))], op,
                 ks=layer_ks(p=(80, 80)), td=1)


def stripes_layer(index, op):
    bar = group("bar", rect((14, 220)), fill(rgb("#f97316")))
    repeater = {"ty": "rp", "c": value(10), "o": value(-4.5), "m": 1,
                "tr": {"ty": "tr", "a": value([0, 0]), "p": value([24, 0]), "s": value([100, 100]),
                       "r": value(0), "so": value(100), "eo": value(100)}}
    second = group("bar", rect((10, 220)), fill(rgb("#facc15")), p=(12, 0))
    return layer(index, "stripes", [group("stripes", bar, second, repeater)], op,
                 ks=layer_ks(p=keys((0, [80, 80, 0]), (op, [104, 80, 0]), ease=LINEAR), r=30), tt=1)


def reveal():
    op = 120
    # A precomposition: stripes seen through a turning star (an alpha matte).
    assets = [{"id": "starred", "w": 160, "h": 160, "layers": [star_layer(1, op), stripes_layer(2, op)]}]
    precomp = {"ddd": 0, "ind": 4, "ty": 0, "nm": "starred", "refId": "starred", "sr": 1,
               "ks": layer_ks(p=(60, 60), a=(80, 80), s=(62, 62)),
               "ao": 0, "w": 160, "h": 160, "ip": 0, "op": op, "st": 0, "bm": 0,
               # Time remap plays the precomposition at half speed: 2 s of it over 4 s.
               "tm": keys((0, 0), (op, 1.0), ease=LINEAR)}
    # A card revealed by a growing circle (add), with a hole (subtract) and a feathered-looking
    # second ring from expansion and opacity.
    masks = [
        {"inv": False, "mode": "a", "nm": "grow",
         "pt": keys((0, [oval(120, 120, 2, 2)]), (50, [oval(120, 120, 150, 150)]), ease=EASE),
         "o": value(100), "x": keys((0, 0), (60, 0), (90, 8), ease=EASE)},
        {"inv": False, "mode": "s", "nm": "hole",
         "pt": value(oval(120, 120, 26, 26)), "o": keys((40, 0), (70, 100)), "x": value(0)},
    ]
    card = layer(
        5, "card",
        [group("card", rect((200, 200), p=(120, 120), r=28),
               gradient("gf", [(0, "#fde68a"), (0.55, "#f472b6"), (1, "#6d28d9")], (120, 120),
                        (230, 120), t=2))],
        op, ks=layer_ks(), masksProperties=masks, hasMask=True)
    # Luma matte: three bars of different greys let the blue through by their brightness.
    greys = layer(
        2, "greys",
        [group("g1", rect((52, 26), p=(64, 206)), fill(rgb("#ffffff"))),
         group("g2", rect((52, 26), p=(120, 206)), fill(rgb("#999999"))),
         group("g3", rect((52, 26), p=(176, 206)), fill(rgb("#444444")))],
        op, ks=layer_ks(), td=1)
    blue = layer(3, "lit", [group("lit", rect((180, 30), p=(120, 206), r=8), fill(rgb("#38bdf8")))],
                 op, ks=layer_ks(sk=keys((0, 0), (60, -12), (op, 0)), sa=0), tt=3)
    # A comet following a curved (spatial bezier) path, turned along it.
    comet = layer(
        1, "comet",
        [group("comet",
               {"ty": "sr", "sy": 2, "d": 1, "p": value([0, 0]), "pt": value(3), "r": value(90),
                "or": value(9), "os": value(0)},
               fill(rgb("#ffffff")))],
        op,
        ks=layer_ks(p=keys((0, [40, 40, 0], {"to": [60, -40, 0], "ti": [-40, -60, 0]}),
                           (60, [200, 70, 0], {"to": [40, 60, 0], "ti": [60, 40, 0]}),
                           (op, [40, 40, 0]), ease=EASE)),
        ao=1)
    return composition("reveal", 240, 240, op, [comet, greys, blue, precomp, card], assets=assets)


def badge():
    op = 120
    star = {"ty": "sr", "sy": 1, "d": 1, "p": value([0, 0]),
            "pt": keys((0, 5), (40, 6), (80, 8), (op, 8), ease="hold"),
            "r": keys((0, 0), (op, 90), ease=LINEAR), "or": value(62), "ir": value(40),
            "os": keys((0, 0), (60, 30), (op, 0)), "is": value(0)}
    corners = {"ty": "rd", "r": value(6)}
    medal = group("medal", star, corners,
                  gradient("gf", [(0, "#fff7ad"), (0.6, "#facc15"), (1, "#b45309")], (0, -20),
                           (0, 70), t=2))
    # A donut by merging two circles (subtract), outlined.
    donut = group("donut", ellipse((70, 70)), ellipse((44, 44)), {"ty": "mm", "mm": 3},
                  fill(rgb("#ffffff"), o=85), stroke(rgb("#b45309"), 3))
    dashes = [{"n": "d", "nm": "dash", "v": value(12)}, {"n": "g", "nm": "gap", "v": value(9)},
              {"n": "o", "nm": "offset", "v": keys((0, 0), (op, 84), ease=LINEAR)}]
    ring = group("ring", ellipse((172, 172)), stroke(rgb("#fde047"), 4, dashes=dashes, lc=1))
    hexagon = group("hexagon",
                    {"ty": "sr", "sy": 2, "d": 1, "p": value([0, 0]), "pt": value(6), "r": value(0),
                     "or": value(80), "os": value(0)},
                    stroke(rgb("#fbbf24"), 2, o=60))
    return composition("badge", 200, 200, op, [
        layer(1, "badge", [donut, medal, ring, hexagon], op, ks=layer_ks(p=(100, 100)))])


def png(width, height, pixel):
    """A PNG of `pixel(x, y) -> (r, g, b, a)` bytes."""
    rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width))
                    for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def sunset_png():
    """A small sunset: a sky gradient, a sun and hills, for the image layers."""
    size = 96

    def pixel(x, y):
        t = y / (size - 1)
        sky = (int(255 * (0.98 - 0.5 * t)), int(255 * (0.55 + 0.1 * t)), int(255 * (0.35 + 0.5 * t)))
        sun = math.hypot(x - 60, y - 44)
        if sun < 16:
            sky = (255, 236, 160)
        hill = 70 + 8 * math.sin(x / 9.0)
        if y > hill:
            sky = (40, int(90 + 40 * math.sin(x / 13.0)), 80)
        return sky + (255,)
    return png(size, size, pixel)


def solid_layer(index, name, color, w, h, op, ks=None, **extra):
    base = {"ddd": 0, "ind": index, "ty": 1, "nm": name, "sr": 1, "ks": ks or layer_ks(),
            "ao": 0, "sc": color, "sw": w, "sh": h, "ip": 0, "op": op, "st": 0, "bm": 0}
    base.update(extra)
    return base


def effect(type_, *controls):
    return {"ty": type_, "en": 1, "ef": [{"ty": 0, "v": c if isinstance(c, dict) else value(c)}
                                        for c in controls]}


def mask(points, mode="a", inverted=False, feather=0, opacity=100, expansion=0):
    # A keyframed outline comes as a property ({"a": 1, "k": ...}); a fixed one is wrapped in one.
    return {"mode": mode, "inv": inverted, "pt": points if "k" in points else value(points),
            "o": value(opacity), "x": value(expansion),
            "f": feather if isinstance(feather, dict) else value([feather, feather])}


def blend():
    op = 180
    backdrop = group("backdrop", rect((200, 200)),
                     gradient("gf", [(0, "#0f172a"), (0.5, "#1e3a8a"), (1, "#0ea5e9")], (-100, -100),
                              (100, 100)))
    layers = []
    # Four discs circling over the backdrop, each blending by its own mode.
    for index, (mode, color, phase) in enumerate([(1, "#f97316", 0), (2, "#a855f7", 90),
                                                  (3, "#22c55e", 180), (10, "#f43f5e", 270)]):
        angle = [phase, phase + 360]
        disc = group("disc", ellipse((92, 92)), fill(rgb(color)))
        layers.append(layer(index + 1, f"blend {mode}", [disc], op,
                            ks=layer_ks(p=(100, 100), a=(-34, 0), r=keys((0, angle[0]), (op, angle[1]), ease=LINEAR)),
                            bm=mode))
    # Overlapping squares faded as one picture: the overlap is no darker than the rest.
    pair = [group("left", rect((56, 56), p=(-16, 0), r=8), fill(rgb("#fde047"))),
            group("right", rect((56, 56), p=(16, 0), r=8), fill(rgb("#fde047")))]
    layers.append(layer(5, "faded pair", pair, op, ks=layer_ks(p=(100, 100), o=keys((0, 30), (90, 80), (op, 30)))))
    # A glow added (Add) over everything.
    glow = group("glow", ellipse((60, 60)), gradient("gf", [(0, "#ffffff"), (1, "#000000")], (0, 0), (30, 0), t=2))
    layers.insert(0, layer(6, "add glow", [glow], op, ks=layer_ks(p=keys((0, [40, 40, 0]), (90, [160, 160, 0]), (op, [40, 40, 0]))), bm=16))
    layers.append(layer(7, "backdrop", [backdrop], op, ks=layer_ks(p=(100, 100))))
    return composition("blend", 200, 200, op, layers)


def feather():
    op = 150
    card = group("card", rect((180, 180), r=22),
                 gradient("gf", [(0, "#fb7185"), (0.5, "#f59e0b"), (1, "#22d3ee")], (-90, -90), (90, 90)))
    stripes = group("lines", rect((180, 6)), fill(rgb("#ffffff"), o=35))
    repeater = {"ty": "rp", "c": value(12), "o": value(-6), "m": 1,
                "tr": {"ty": "tr", "a": value([0, 0]), "p": value([0, 16]), "s": value([100, 100]),
                       "r": value(0), "so": value(100), "eo": value(100)}}
    lines = group("stripes", stripes, repeater, r=-30)
    # A spot light: a circular mask whose feather softens as it sweeps across the card.
    spot = mask(keys((0, [oval(-50, -30, 46, 46)]), (75, [oval(50, 20, 46, 46)]),
                     (op, [oval(-50, -30, 46, 46)])),
                feather=keys((0, [4, 4]), (75, [36, 36]), (op, [4, 4])))
    content = layer(2, "revealed", [lines, card], op, ks=layer_ks(p=(100, 100)),
                    masksProperties=[spot])
    # A vignette: an inverted, feathered ellipse darkens the edges of the whole picture.
    vignette = solid_layer(1, "vignette", "#020617", 200, 200, op,
                           masksProperties=[mask(oval(100, 100, 84, 84), inverted=True, feather=30, opacity=85)])
    dim = layer(3, "dim card", [group("card", rect((180, 180), r=22), fill(rgb("#334155")))], op,
                ks=layer_ks(p=(100, 100)))
    return composition("feather", 200, 200, op, [vignette, content, dim])


def matte():
    op = 150
    # A luma matte: white fading to black over its right quarter slides across, so the stripes
    # fade in along its soft edge and out again.
    wipe = group("wipe", rect((800, 200)),
                 gradient("gf", [(0, "#ffffff"), (0.75, "#ffffff"), (1, "#000000")], (-400, 0), (400, 0)))
    wipe_layer = layer(1, "wipe matte", [wipe], op,
                       ks=layer_ks(p=keys((0, [-300, 100, 0]), (75, [80, 100, 0]), (op, [-300, 100, 0]))), td=1)
    bar = group("bar", rect((14, 260)), fill(rgb("#38bdf8")))
    repeater = {"ty": "rp", "c": value(12), "o": value(-6), "m": 1,
                "tr": {"ty": "tr", "a": value([0, 0]), "p": value([24, 0]), "s": value([100, 100]),
                       "r": value(0), "so": value(100), "eo": value(60)}}
    stripes = layer(2, "stripes", [group("stripes", bar, repeater)], op, ks=layer_ks(p=(100, 100), r=20), tt=3)
    # An alpha matte whose alpha is a radial gradient: a soft-edged window onto a card.
    window = group("window", ellipse((150, 150)),
                   gradient("gf", [(0, "#ffffff"), (1, "#ffffff")], (0, 0), (75, 0), t=2,
                            opacity_stops=[(0, 1), (0.6, 1), (1, 0)]))
    window_layer = layer(3, "window matte", [window], op,
                         ks=layer_ks(p=(100, 100), s=keys((0, [70, 70, 100]), (75, [120, 120, 100]), (op, [70, 70, 100]))), td=1)
    card = layer(4, "card", [group("card", rect((200, 200)),
                                   gradient("gf", [(0, "#c084fc"), (1, "#f472b6")], (-100, -100), (100, 100)))],
                 op, ks=layer_ks(p=(100, 100)), tt=1)
    return composition("matte", 200, 200, op, [wipe_layer, stripes, window_layer, card])


FONTS = {"list": [{"fName": "SegoeUI-Bold", "fFamily": "Segoe UI", "fStyle": "Bold", "ascent": 74},
                  {"fName": "SegoeUI", "fFamily": "Segoe UI", "fStyle": "Regular", "ascent": 74}]}


def text_layer(index, name, document, op, ks, animators=(), **extra):
    base = {"ddd": 0, "ind": index, "ty": 5, "nm": name, "sr": 1, "ks": ks, "ao": 0, "ip": 0,
            "op": op, "st": 0, "bm": 0,
            "t": {"d": {"k": document}, "p": {}, "m": {"g": 1, "a": value([0, 0])},
                  "a": list(animators)}}
    base.update(extra)
    return base


def document(text, size, font="SegoeUI-Bold", color=(1, 1, 1), justify=2, tracking=0, stroke=None):
    doc = {"s": size, "f": font, "t": text, "j": justify, "tr": tracking, "lh": size * 1.2,
           "ls": 0, "fc": list(color)}
    if stroke:
        doc.update({"sc": list(stroke[0]), "sw": stroke[1], "of": True})
    return doc


def selector(start=0, end=100, offset=0, shape=1, based_on=1, units=1, amount=100):
    def prop(x):
        return x if isinstance(x, dict) else value(x)
    return {"t": 0, "xe": value(0), "ne": value(0), "a": prop(amount), "b": based_on, "rn": 0,
            "sh": shape, "s": prop(start), "e": prop(end), "o": prop(offset), "r": units,
            "sm": value(100)}


def type_():
    op = 180
    wave = {"nm": "wave", "s": selector(start=0, end=34, offset=keys((0, -40), (op, 100), ease=LINEAR), shape=5),
            "a": {"p": value([0, -16]), "s": value([125, 125]), "fc": value([0.99, 0.8, 0.2, 1])}}
    title = text_layer(1, "title", [{"s": document("STRATA", 40, tracking=60,
                                                    stroke=((0.05, 0.1, 0.2), 2)), "t": 0}], op,
                       layer_ks(p=(100, 86)), [wave])
    typing = {"nm": "typewriter", "s": selector(start=keys((0, 0), (120, 100), ease=LINEAR), end=100),
              "a": {"o": value(0)}}
    caption = text_layer(2, "caption", [{"s": document("plays text layers", 18, font="SegoeUI",
                                                       color=(0.75, 0.85, 1.0)), "t": 0},
                                        {"s": document("in system fonts", 18, font="SegoeUI",
                                                       color=(0.6, 1.0, 0.8)), "t": 120}], op,
                         layer_ks(p=(100, 140)), [typing])
    backdrop = layer(3, "backdrop", [group("bg", rect((200, 200), r=24),
                                           gradient("gf", [(0, "#1e1b4b"), (1, "#0f766e")], (0, -100), (0, 100)))],
                     op, ks=layer_ks(p=(100, 100)))
    result = composition("type", 200, 200, op, [title, caption, backdrop])
    result["fonts"] = FONTS
    return result


def photo():
    op = 150
    data = "data:image/png;base64," + base64.b64encode(sunset_png()).decode("ascii")
    assets = [{"id": "embedded", "w": 96, "h": 96, "e": 1, "u": "", "p": data},
              {"id": "file", "w": 96, "h": 96, "e": 0, "u": "images/", "p": "sunset.png"}]
    shadow = effect(25, value([0, 0, 0, 1]), 170, 135, 8, 14)
    rounded = mask(path([(14, 0), (82, 0), (96, 14), (96, 82), (82, 96), (14, 96), (0, 82), (0, 14)]))
    embedded = {"ddd": 0, "ind": 1, "ty": 2, "nm": "embedded", "refId": "embedded", "sr": 1,
                "ks": layer_ks(p=keys((0, [62, 70, 0]), (75, [62, 58, 0]), (op, [62, 70, 0])), a=(48, 48),
                               r=keys((0, -8), (75, -2), (op, -8))),
                "ao": 0, "ip": 0, "op": op, "st": 0, "bm": 0, "masksProperties": [rounded],
                "ef": [shadow]}
    referenced = {"ddd": 0, "ind": 2, "ty": 2, "nm": "file", "refId": "file", "sr": 1,
                  "ks": layer_ks(p=(138, 132), a=(48, 48), r=6, s=(90, 90)),
                  "ao": 0, "ip": 0, "op": op, "st": 0, "bm": 0, "masksProperties": [rounded],
                  "ef": [shadow]}
    return composition("photo", 200, 200, op, [embedded, referenced], assets=assets)


def effects():
    op = 160
    # A drop shadow under a moving card.
    card = layer(1, "card", [group("card", rect((70, 50), r=10), fill(rgb("#f8fafc")))], op,
                 ks=layer_ks(p=keys((0, [56, 44, 0]), (80, [60, 36, 0]), (op, [56, 44, 0]))),
                 ef=[effect(25, value([0.02, 0.05, 0.15, 1]), 200, 150, keys((0, 3), (80, 12), (op, 3)), 16)])
    # A blur breathing in and out.
    blurred = layer(2, "blurred", [group("dots", ellipse((40, 40)), fill(rgb("#38bdf8")),
                                         p=(0, 0)), group("dot", ellipse((24, 24), p=(24, 10)),
                                                          fill(rgb("#f472b6")))], op,
                    ks=layer_ks(p=(146, 44)), ef=[effect(29, keys((0, 0), (80, 14), (op, 0)), 1, 0)])
    rainbow = group("rainbow", rect((70, 60), r=8),
                    gradient("gf", [(0, "#ef4444"), (0.33, "#eab308"), (0.66, "#22c55e"), (1, "#3b82f6")],
                             (-35, 0), (35, 0)))
    # Tint (black to navy, white to gold), tritone and levels on the same rainbow.
    tint = layer(3, "tint", [rainbow], op, ks=layer_ks(p=(56, 116)),
                 ef=[effect(20, value([0.05, 0.08, 0.3, 1]), value([1, 0.85, 0.4, 1]), keys((0, 0), (80, 100), (op, 0)))])
    tritone = layer(4, "tritone", [rainbow], op, ks=layer_ks(p=(146, 116)),
                    ef=[effect(23, value([1, 0.95, 0.85, 1]), value([0.85, 0.3, 0.5, 1]), value([0.1, 0.05, 0.25, 1]), 0)])
    composite = [0.15, 0.85, keys((0, 0.6), (80, 1.8), (op, 0.6)), 0, 1]
    channel = [0, 1, 1, 0, 1]
    controls = [0, 0, 0] + composite + [0, 0] + channel + [0, 0] + channel + [0, 0] + channel + [0, 0] + channel
    leveled = layer(5, "levels", [rainbow], op, ks=layer_ks(p=(56, 172), s=(100, 60)), ef=[effect(24, *controls)])
    filled = layer(6, "fill", [group("star", {"ty": "sr", "sy": 1, "d": 1, "p": value([0, 0]), "pt": value(5),
                                              "r": keys((0, 0), (op, 72), ease=LINEAR), "or": value(26), "ir": value(12),
                                              "os": value(0), "is": value(0)},
                                     gradient("gf", [(0, "#ffffff"), (1, "#64748b")], (0, -26), (0, 26)))],
                   op, ks=layer_ks(p=(146, 172)), ef=[effect(21, 0, 0, value([0.2, 0.9, 0.6, 1]), 0, 0, 0, 1)])
    return composition("effects", 200, 200, op, [card, blurred, tint, tritone, leveled, filled])


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "images").mkdir(parents=True, exist_ok=True)
    (OUT / "images" / "sunset.png").write_bytes(sunset_png())
    for name, build in [("spinner", spinner), ("success", success), ("like", like),
                        ("toggle", toggle), ("reveal", reveal), ("badge", badge),
                        ("blend", blend), ("feather", feather), ("matte", matte),
                        ("type", type_), ("photo", photo), ("effects", effects)]:
        document = build()
        text = json.dumps(document, separators=(",", ":"))
        (OUT / f"{name}.json").write_text(text + "\n", encoding="utf-8", newline="\n")
        print(f"{name}.json: {len(text)} bytes")


if __name__ == "__main__":
    main()
