"""Regenerates the showcase's NET page sounds in src/main/resources/assets/strata/media.

Needs only the standard library, and ffmpeg (with its AAC encoder) on PATH for the music loop:

    python tools/media/generate_demo_sounds.py
"""

import math
import pathlib
import random
import struct
import subprocess
import tempfile
import wave

OUT = pathlib.Path(__file__).resolve().parents[2] / "src/main/resources/assets/strata/media"
RATE = 44100


def write_wav(path: pathlib.Path, samples, rate: int = RATE) -> None:
    """16-bit mono PCM, the samples clipped to [-1, 1]."""
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(b"".join(
            struct.pack("<h", int(max(-1.0, min(1.0, sample)) * 32000)) for sample in samples))


def click():
    """A short wooden tick: a filtered noise burst over a falling tone, 40 ms."""
    random.seed(7)
    count = int(RATE * 0.04)
    low = 0.0
    for index in range(count):
        t = index / RATE
        envelope = math.exp(-t * 160.0)
        low += 0.35 * (random.uniform(-1.0, 1.0) - low)
        tone = math.sin(2.0 * math.pi * (1800.0 - 9000.0 * t) * t)
        yield 0.9 * envelope * (0.55 * low + 0.45 * tone)


def chime():
    """A bell of two partials, 1.2 s."""
    count = int(RATE * 1.2)
    for index in range(count):
        t = index / RATE
        attack = min(1.0, t / 0.004)
        yield attack * 0.5 * (math.exp(-t * 3.0) * math.sin(2 * math.pi * 880.0 * t) +
                              0.6 * math.exp(-t * 5.0) * math.sin(2 * math.pi * 1318.5 * t) +
                              0.25 * math.exp(-t * 9.0) * math.sin(2 * math.pi * 2637.0 * t))


def loop():
    """Eight seconds of a soft arpeggio over a bass that loops without a seam."""
    bpm, steps = 120, 32
    step = 60.0 / bpm / 2.0
    count = int(RATE * step * steps)
    chords = [(57, 60, 64, 69), (53, 57, 60, 65), (48, 52, 55, 60), (55, 59, 62, 67)]
    pattern = [0, 1, 2, 3, 2, 1, 2, 3]
    out = [0.0] * count
    for index in range(steps):
        chord = chords[index // 8]
        note = chord[pattern[index % 8]] + 12
        frequency = 440.0 * 2 ** ((note - 69) / 12)
        start = int(index * step * RATE)
        for offset in range(int(step * RATE * 1.8)):
            at = (start + offset) % count
            t = offset / RATE
            envelope = min(1.0, t / 0.01) * math.exp(-t * 4.0)
            out[at] += 0.22 * envelope * (math.sin(2 * math.pi * frequency * t) +
                                          0.3 * math.sin(4 * math.pi * frequency * t))
    for bar in range(4):
        frequency = 440.0 * 2 ** ((chords[bar][0] - 12 - 69) / 12)
        start = int(bar * 8 * step * RATE)
        length = int(8 * step * RATE)
        for offset in range(length):
            t = offset / RATE
            envelope = min(1.0, t / 0.02) * min(1.0, (length - offset) / (0.05 * RATE))
            out[start + offset] += 0.25 * envelope * math.sin(2 * math.pi * frequency * t)
    return out


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    write_wav(OUT / "click.wav", click())
    write_wav(OUT / "chime.wav", chime())
    with tempfile.TemporaryDirectory() as folder:
        raw = pathlib.Path(folder) / "loop.wav"
        write_wav(raw, loop())
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(raw), "-c:a", "aac",
                        "-b:a", "64k", str(OUT / "loop.m4a")], check=True)
    for name in ("click.wav", "chime.wav", "loop.m4a"):
        print(name, (OUT / name).stat().st_size, "bytes")


if __name__ == "__main__":
    main()
