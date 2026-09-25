#!/usr/bin/env python3
"""Procedural sound effects for OxCity, pure python (no numpy), mono 16-bit 22.05 kHz wav.

    python3 tools/assetgen/sounds.py            # writes into game/assets/Audio
"""

from __future__ import annotations

import math
import random
import struct
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT_DIR = ROOT / "game" / "assets" / "Audio"
RATE = 22050


def seconds(n: float) -> int:
    return int(n * RATE)


def write(path: Path, samples: list[float], gain: float = 0.9):
    peak = max(1e-6, max(abs(s) for s in samples))
    scale = gain / peak
    frames = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s * scale)) * 32767)) for s in samples)
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(frames)


def envelope(n: int, attack: float, release: float) -> list[float]:
    a, r = max(1, seconds(attack)), max(1, seconds(release))
    out = []
    for i in range(n):
        v = 1.0
        if i < a:
            v = i / a
        if i > n - r:
            v = min(v, (n - i) / r)
        out.append(v)
    return out


def lowpass(samples: list[float], alpha: float) -> list[float]:
    out, y = [], 0.0
    for s in samples:
        y += alpha * (s - y)
        out.append(y)
    return out


def noise(n: int, rng: random.Random) -> list[float]:
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


# ---------------------------------------------------------------------------------------------------------------------
# Everything is kept in the low mids: fundamentals under ~900 Hz, band limited harmonics instead of square waves,
# and a gentle low-pass on the way out. The first version had a 2.2 kHz square wave pager and a 2.7 kHz alarm bell,
# which on headphones was painful.


def soft(samples: list[float], cutoff_hz: float = 3500.0) -> list[float]:
    """one pole low-pass at `cutoff_hz` plus a 3 ms fade in so nothing starts with a click"""
    alpha = 1.0 - math.exp(-2.0 * math.pi * cutoff_hz / RATE)
    out = lowpass(samples, alpha)
    fade = seconds(0.003)
    for i in range(min(fade, len(out))):
        out[i] *= i / fade
    return out


def tone(f: float, t: float, harmonics=((1, 1.0), (2, 0.3), (3, 0.12))) -> float:
    return sum(a * math.sin(2 * math.pi * f * k * t) for k, a in harmonics)


def engine_loop() -> list[float]:
    # an integer number of cycles of every partial in exactly one second, so the loop is seamless
    n = seconds(1.0)
    out = []
    for i in range(n):
        t = i / RATE
        base = 40.0
        s = 0.0
        for k, amp in ((1, 1.0), (2, 0.5), (3, 0.25), (4, 0.12)):
            s += amp * math.sin(2 * math.pi * base * k * t)
        s *= 0.75 + 0.25 * math.sin(2 * math.pi * 20.0 * t) ** 2
        out.append(math.tanh(1.2 * s))
    return soft(out, 900.0)


def siren() -> list[float]:
    # wail between ~440 and ~700 Hz, mostly fundamental
    n = seconds(2.0)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 570.0 + 130.0 * math.sin(2 * math.pi * 0.5 * t - math.pi / 2)
        phase += 2 * math.pi * f / RATE
        out.append(0.8 * math.sin(phase) + 0.15 * math.sin(2 * phase))
    return soft(out, 1800.0)


def horn() -> list[float]:
    n = seconds(0.45)
    env = envelope(n, 0.015, 0.06)
    out = [tone(262.0, i / RATE, ((1, 1.0), (2, 0.4), (3, 0.2))) + tone(330.0, i / RATE, ((1, 0.8), (2, 0.3)))
           for i in range(n)]
    return soft([0.5 * s * e for s, e in zip(out, env)], 1600.0)


def gunshot(rng) -> list[float]:
    # mostly body: a low thump and a short, dark crack
    n = seconds(0.5)
    nz = lowpass(noise(n, rng), 0.3)
    out = []
    for i in range(n):
        t = i / RATE
        crack = 0.6 * nz[i] * math.exp(-t * 55.0)
        thump = 1.0 * math.sin(2 * math.pi * (55.0 + 60.0 * math.exp(-t * 30.0)) * t) * math.exp(-t * 14.0)
        tail = 0.18 * nz[i] * math.exp(-t * 6.0)
        out.append(crack + thump + tail)
    return soft(out, 2200.0)


def punch(rng) -> list[float]:
    n = seconds(0.18)
    nz = lowpass(noise(n, rng), 0.15)
    out = []
    for i in range(n):
        t = i / RATE
        out.append(0.9 * nz[i] * math.exp(-t * 45.0) + math.sin(2 * math.pi * (100.0 - 150.0 * t) * t) * math.exp(-t * 22.0))
    return soft(out, 1500.0)


def cash() -> list[float]:
    # two soft bell notes, E5 then A5, sine only
    n = seconds(0.5)
    out = []
    for i in range(n):
        t = i / RATE
        s = 0.0
        if t < 0.09:
            s += math.sin(2 * math.pi * 659.3 * t) * math.exp(-t * 18.0)
        if t >= 0.09:
            u = t - 0.09
            s += math.sin(2 * math.pi * 880.0 * u) * math.exp(-u * 8.0)
            s += 0.2 * math.sin(2 * math.pi * 1760.0 * u) * math.exp(-u * 14.0)
        out.append(s)
    return soft(out, 3000.0)


def footstep(rng) -> list[float]:
    n = seconds(0.09)
    nz = lowpass(noise(n, rng), 0.12)
    return soft([nz[i] * math.exp(-(i / RATE) * 60.0) for i in range(n)], 1200.0)


def door(rng) -> list[float]:
    n = seconds(0.3)
    nz = lowpass(noise(n, rng), 0.1)
    out = []
    for i in range(n):
        t = i / RATE
        out.append(nz[i] * math.exp(-t * 30.0) + 0.8 * math.sin(2 * math.pi * 85.0 * t) * math.exp(-t * 22.0))
    return soft(out, 1200.0)


def crash(rng) -> list[float]:
    n = seconds(0.8)
    nz = lowpass(noise(n, rng), 0.35)
    out = []
    for i in range(n):
        t = i / RATE
        metal = sum(math.sin(2 * math.pi * f * t) for f in (180.0, 317.0, 523.0, 811.0)) * 0.22
        thud = 0.8 * math.sin(2 * math.pi * 60.0 * t) * math.exp(-t * 12.0)
        out.append((0.6 * nz[i] + metal) * math.exp(-t * 6.0) + thud)
    return soft(out, 2500.0)


def alarm() -> list[float]:
    # bank bell, loops every second: a round 600 Hz ring at 8 strikes a second
    n = seconds(1.0)
    out = []
    for i in range(n):
        t = i / RATE
        strike = (t * 8.0) % 1.0
        s = math.sin(2 * math.pi * 600.0 * t) + 0.3 * math.sin(2 * math.pi * 1200.0 * t)
        out.append(s * math.exp(-strike * 4.0))
    return soft(out, 2000.0)


def pager() -> list[float]:
    # two short, rounded beeps at A5
    n = seconds(0.34)
    out = []
    for i in range(n):
        t = i / RATE
        local = t % 0.14
        on = local < 0.08
        env = math.sin(math.pi * local / 0.08) if on else 0.0
        out.append(math.sin(2 * math.pi * 880.0 * t) * env)
    return soft(out, 2500.0)


def death() -> list[float]:
    n = seconds(1.6)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 220.0 * (0.5 ** (t / 1.0))
        phase += 2 * math.pi * f / RATE
        out.append((math.sin(phase) + 0.25 * math.sin(2 * phase)) * math.exp(-t * 1.2))
    return soft(out, 1500.0)


def knife_swing(rng) -> list[float]:
    # whoosh: band limited noise swelling and fading, pitch of the band sweeping down
    n = seconds(0.22)
    nz = noise(n, rng)
    out, y1, y2 = [], 0.0, 0.0
    for i in range(n):
        t = i / RATE
        cutoff = 1400.0 - 3000.0 * t
        a = 1.0 - math.exp(-2.0 * math.pi * max(200.0, cutoff) / RATE)
        y1 += a * (nz[i] - y1)
        y2 += 0.02 * (y1 - y2)
        env = math.sin(math.pi * t / 0.22) ** 2
        out.append((y1 - y2) * env)
    return soft(out, 2500.0)


def stab(rng) -> list[float]:
    # a short wet thud: low body, a squelch of filtered noise
    n = seconds(0.28)
    nz = lowpass(noise(n, rng), 0.08)
    out = []
    for i in range(n):
        t = i / RATE
        body = math.sin(2 * math.pi * (90.0 - 120.0 * t) * t) * math.exp(-t * 20.0)
        squelch = 2.0 * nz[i] * math.exp(-t * 16.0) * (0.6 + 0.4 * math.sin(2 * math.pi * 35.0 * t))
        out.append(body + squelch)
    return soft(out, 1400.0)


def splat(rng) -> list[float]:
    # blood hitting the pavement: a few tiny low drips after a soft slap
    n = seconds(0.35)
    nz = lowpass(noise(n, rng), 0.1)
    out = []
    for i in range(n):
        t = i / RATE
        s = 1.4 * nz[i] * math.exp(-t * 28.0)
        for d, f in ((0.07, 330.0), (0.13, 280.0), (0.21, 360.0)):
            if t >= d:
                u = t - d
                s += 0.25 * math.sin(2 * math.pi * f * u * (1.0 + 2.0 * u)) * math.exp(-u * 40.0)
        out.append(s)
    return soft(out, 1800.0)


def radio(rng) -> list[float]:
    """4 bar loop at 120bpm for the car radio, square bass softened, triangle lead"""
    bpm = 120.0
    beat = 60.0 / bpm
    bars = 4
    n = seconds(beat * 4 * bars)
    out = [0.0] * n
    bass_notes = [45, 45, 48, 43, 45, 45, 50, 47]  # midi, one per half bar
    lead = [69, 72, 76, 74, 72, 69, 67, 69, 72, 76, 79, 76, 74, 72, 74, 76]

    def freq(m):
        return 440.0 * 2 ** ((m - 69) / 12)

    for i in range(n):
        t = i / RATE
        half = int(t / (beat * 2)) % len(bass_notes)
        eighth = t % (beat / 2)
        fb = freq(bass_notes[half])
        s = 0.4 * tone(fb, t, ((1, 1.0), (3, 0.25), (5, 0.08))) * math.exp(-eighth * 6.0)
        li = int(t / beat) % len(lead)
        lt = t % beat
        phase = (freq(lead[li] - 12) * t) % 1.0
        s += 0.2 * (4.0 * abs(phase - 0.5) - 1.0) * math.exp(-lt * 3.0)
        kick_t = t % beat
        s += 0.6 * math.sin(2 * math.pi * (50.0 + 70.0 * math.exp(-kick_t * 40)) * kick_t) * math.exp(-kick_t * 14.0)
        hat_t = (t + beat / 2) % beat
        s += 0.04 * rng.uniform(-1, 1) * math.exp(-hat_t * 60.0)
        out[i] = s
    return soft(out, 2500.0)


# peak level per sound. Loud, busy sounds sit lower so nothing jumps out at the player
LEVELS = {
    "engine_loop": 0.45,
    "siren": 0.35,
    "horn": 0.45,
    "gunshot": 0.5,
    "punch": 0.55,
    "cash": 0.4,
    "footstep": 0.35,
    "door": 0.5,
    "crash": 0.6,
    "alarm": 0.3,
    "pager": 0.3,
    "death": 0.45,
    "radio": 0.4,
    "knife_swing": 0.45,
    "stab": 0.6,
    "splat": 0.5,
}


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT_DIR
    rng = random.Random(1997)
    sounds = {
        "engine_loop": engine_loop(),
        "siren": siren(),
        "horn": horn(),
        "gunshot": gunshot(rng),
        "punch": punch(rng),
        "cash": cash(),
        "footstep": footstep(rng),
        "door": door(rng),
        "crash": crash(rng),
        "alarm": alarm(),
        "pager": pager(),
        "death": death(),
        "radio": radio(rng),
        "knife_swing": knife_swing(rng),
        "stab": stab(rng),
        "splat": splat(rng),
    }
    for name, samples in sounds.items():
        path = out_dir / f"{name}.wav"
        write(path, samples, LEVELS[name])
        print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path} ({len(samples) / RATE:.2f}s)")


if __name__ == "__main__":
    main()
