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
"""

import json
import math
import pathlib

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


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    for name, build in [("spinner", spinner), ("success", success), ("like", like),
                        ("toggle", toggle), ("reveal", reveal), ("badge", badge)]:
        document = build()
        text = json.dumps(document, separators=(",", ":"))
        (OUT / f"{name}.json").write_text(text + "\n", encoding="utf-8", newline="\n")
        print(f"{name}.json: {len(text)} bytes")


if __name__ == "__main__":
    main()
