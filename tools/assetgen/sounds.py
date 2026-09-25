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


def engine_loop() -> list[float]:
    # an integer number of cycles of every partial in exactly one second, so the loop is seamless
    n = seconds(1.0)
    out = []
    for i in range(n):
        t = i / RATE
        base = 50.0
        s = 0.0
        for k, amp in ((1, 1.0), (2, 0.6), (3, 0.35), (4, 0.25), (6, 0.12)):
            s += amp * math.sin(2 * math.pi * base * k * t)
        # firing pulses at 4x base give it a rumble
        s *= 0.7 + 0.3 * math.sin(2 * math.pi * 25.0 * t) ** 2
        out.append(math.tanh(1.8 * s))
    return out


def siren() -> list[float]:
    n = seconds(2.0)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 850.0 + 350.0 * math.sin(2 * math.pi * 0.5 * t - math.pi / 2)
        phase += 2 * math.pi * f / RATE
        out.append(0.6 * math.sin(phase) + 0.25 * math.sin(2 * phase) + 0.1 * math.sin(3 * phase))
    return out


def horn() -> list[float]:
    n = seconds(0.45)
    env = envelope(n, 0.01, 0.05)
    out = []
    for i in range(n):
        t = i / RATE
        s = sum(math.copysign(1.0, math.sin(2 * math.pi * f * t)) for f in (392.0, 494.0))
        out.append(0.4 * s * env[i])
    return lowpass(out, 0.25)


def gunshot(rng) -> list[float]:
    n = seconds(0.45)
    nz = noise(n, rng)
    out = []
    for i in range(n):
        t = i / RATE
        crack = nz[i] * math.exp(-t * 38.0)
        thump = 0.9 * math.sin(2 * math.pi * 70.0 * t) * math.exp(-t * 18.0)
        tail = 0.25 * nz[i] * math.exp(-t * 7.0)
        out.append(crack + thump + tail)
    return lowpass(out, 0.55)


def punch(rng) -> list[float]:
    n = seconds(0.18)
    nz = lowpass(noise(n, rng), 0.2)
    out = []
    for i in range(n):
        t = i / RATE
        out.append(1.5 * nz[i] * math.exp(-t * 45.0) + math.sin(2 * math.pi * (120.0 - 200.0 * t) * t) * math.exp(-t * 25.0))
    return out


def cash() -> list[float]:
    n = seconds(0.55)
    out = []
    for i in range(n):
        t = i / RATE
        s = 0.0
        if t < 0.08:
            s += math.sin(2 * math.pi * 1318.5 * t) * math.exp(-t * 20.0)
        if t >= 0.08:
            u = t - 0.08
            s += math.sin(2 * math.pi * 1760.0 * u) * math.exp(-u * 7.0)
            s += 0.4 * math.sin(2 * math.pi * 2637.0 * u) * math.exp(-u * 10.0)
        out.append(s)
    return out


def footstep(rng) -> list[float]:
    n = seconds(0.09)
    nz = lowpass(noise(n, rng), 0.35)
    return [nz[i] * math.exp(-(i / RATE) * 60.0) for i in range(n)]


def door(rng) -> list[float]:
    n = seconds(0.3)
    nz = lowpass(noise(n, rng), 0.15)
    out = []
    for i in range(n):
        t = i / RATE
        out.append(nz[i] * math.exp(-t * 30.0) + 0.8 * math.sin(2 * math.pi * 95.0 * t) * math.exp(-t * 22.0))
    return out


def crash(rng) -> list[float]:
    n = seconds(0.8)
    nz = noise(n, rng)
    out = []
    for i in range(n):
        t = i / RATE
        metal = sum(math.sin(2 * math.pi * f * t) for f in (433.0, 781.0, 1297.0, 2011.0)) * 0.25
        out.append((0.8 * nz[i] + metal) * math.exp(-t * 6.0))
    return lowpass(out, 0.5)


def alarm() -> list[float]:
    # classic bank bell, loops every second
    n = seconds(1.0)
    out = []
    for i in range(n):
        t = i / RATE
        strike = (t * 16.0) % 1.0
        s = math.sin(2 * math.pi * 1200.0 * t) + 0.5 * math.sin(2 * math.pi * 2750.0 * t)
        out.append(s * math.exp(-strike * 3.0) * 0.6)
    return out


def pager() -> list[float]:
    n = seconds(0.36)
    out = []
    for i in range(n):
        t = i / RATE
        on = (t % 0.12) < 0.07
        out.append(0.5 * math.copysign(1.0, math.sin(2 * math.pi * 2200.0 * t)) if on else 0.0)
    return lowpass(out, 0.4)


def wasted() -> list[float]:
    n = seconds(1.6)
    out, phase = [], 0.0
    for i in range(n):
        t = i / RATE
        f = 440.0 * (0.5 ** (t / 1.0))
        phase += 2 * math.pi * f / RATE
        out.append((math.sin(phase) + 0.3 * math.sin(3 * phase)) * math.exp(-t * 1.2))
    return out


def radio(rng) -> list[float]:
    """8 bar chiptune loop at 120bpm for the car radio"""
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
        s = 0.35 * (1 if math.sin(2 * math.pi * freq(bass_notes[half]) * t) > 0 else -1) * math.exp(-eighth * 6.0)
        li = int(t / beat) % len(lead)
        lt = t % beat
        s += 0.18 * (2 * ((freq(lead[li]) * t) % 1.0) - 1) * math.exp(-lt * 3.0)
        kick_t = t % beat
        s += 0.6 * math.sin(2 * math.pi * (60.0 + 90.0 * math.exp(-kick_t * 40)) * kick_t) * math.exp(-kick_t * 14.0)
        hat_t = (t + beat / 2) % beat
        s += 0.08 * rng.uniform(-1, 1) * math.exp(-hat_t * 60.0)
        out[i] = s
    return lowpass(out, 0.6)


def main():
    out_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else OUT_DIR
    rng = random.Random(1997)  # the year GTA came out
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
        "wasted": wasted(),
        "radio": radio(rng),
    }
    for name, samples in sounds.items():
        path = out_dir / f"{name}.wav"
        write(path, samples)
        print(f"wrote {path.relative_to(ROOT) if path.is_relative_to(ROOT) else path} ({len(samples) / RATE:.2f}s)")


if __name__ == "__main__":
    main()
