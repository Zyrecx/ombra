"""Synthesize the toast sounds (src/ombra-ui/sounds/*.wav) from code: they are Ombra's own, GPL.

    py -I scripts/toast-sounds.py [OUT_DIR]

Standard library only. The WAVs are committed (ombra-ui's sounds.rc embeds them); rerun this after
changing a sound and commit both. Mono 16-bit at 24 kHz: the tones stay well under 12 kHz, and it
halves the size of 48 kHz.
"""
import math
import struct
import sys
import wave
from pathlib import Path

RATE = 24000
PEAK = 0.30  # about -10.5 dBFS: heard over a game without startling


def marimba(f, t):
    # A sine with a short 4th-harmonic strike on top: a soft mallet.
    return math.sin(2 * math.pi * f * t) + 0.35 * math.sin(2 * math.pi * f * 4.0 * t) * math.exp(-t / 0.03)


def pluck(f, t):
    return math.sin(2 * math.pi * f * t) * (1 + 0.5 * math.sin(2 * math.pi * f * 2 * t))


def render(notes, length):
    """notes: (start s, voice, Hz, gain, attack s, decay s). Normalized to PEAK, 20 ms fade-out."""
    n = int(RATE * length)
    out = [0.0] * n
    for start, voice, f, gain, attack, decay in notes:
        s0 = int(start * RATE)
        for i in range(s0, n):
            t = (i - s0) / RATE
            e = min(1.0, t / attack) * math.exp(-t / decay)
            if e < 1e-4 and t > attack:
                break
            out[i] += gain * e * voice(f, t)
    peak = max(abs(x) for x in out) or 1.0
    fade = int(0.02 * RATE)
    for i in range(fade):
        out[n - 1 - i] *= i / fade
    return [x / peak * PEAK for x in out]


A5, E6, D5, FS5 = 880.0, 1318.5, 587.3, 740.0

SOUNDS = {
    # A clip, recording or screenshot saved: two rising notes.
    "toast-success": ([(0, marimba, A5, 0.8, 0.002, 0.12), (0.075, marimba, E6, 1.0, 0.002, 0.18)], 0.45),
    # A failure or warning: one low note twice.
    "toast-problem": ([(0, marimba, D5, 1.0, 0.002, 0.10), (0.12, marimba, D5, 0.9, 0.002, 0.14)], 0.45),
    # The buffer on or off, nothing new to save: a short tick.
    "toast-neutral": ([(0, pluck, FS5, 1.0, 0.002, 0.05)], 0.25),
}


def main():
    out = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "src/ombra-ui/sounds"
    out.mkdir(parents=True, exist_ok=True)
    for name, (notes, length) in SOUNDS.items():
        path = out / f"{name}.wav"
        with wave.open(str(path), "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(RATE)
            w.writeframes(b"".join(struct.pack("<h", round(x * 32767)) for x in render(notes, length)))
        print(f"{path} {path.stat().st_size} bytes")


if __name__ == "__main__":
    main()
