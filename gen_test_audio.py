#!/usr/bin/env python3
"""
gen_test_audio.py — Generate a synthetic test WAV file for rtp-streamer.

Output: input/audio.wav
Format: 8000 Hz, 16-bit signed PCM, mono (standard VoIP format)
Content: 5-second multi-tone sine wave (440 Hz + 880 Hz) to simulate voice

No external libraries required — uses only Python standard library struct/wave.
"""

import wave
import struct
import math
import os

OUTPUT_PATH   = os.path.join("input", "audio.wav")
SAMPLE_RATE   = 8000       # Hz
DURATION_S    = 5          # seconds
BITS          = 16
CHANNELS      = 1
AMPLITUDE     = 16000      # max 32767; leave some headroom

TONES = [
    (440.0,  0.60),   # A4  — dominant tone
    (880.0,  0.25),   # A5  — harmonic
    (220.0,  0.15),   # A3  — sub-harmonic
]

def main():
    os.makedirs("input", exist_ok=True)
    total_samples = SAMPLE_RATE * DURATION_S
    samples = []

    for i in range(total_samples):
        t = i / SAMPLE_RATE
        value = 0.0
        for freq, weight in TONES:
            value += weight * math.sin(2 * math.pi * freq * t)
        # Apply a slow fade-in/fade-out envelope (first/last 0.25s)
        fade_samples = SAMPLE_RATE // 4
        if i < fade_samples:
            value *= i / fade_samples
        elif i > total_samples - fade_samples:
            value *= (total_samples - i) / fade_samples
        samples.append(int(value * AMPLITUDE))

    with wave.open(OUTPUT_PATH, 'w') as wf:
        wf.setnchannels(CHANNELS)
        wf.setsampwidth(BITS // 8)
        wf.setframerate(SAMPLE_RATE)
        raw = struct.pack(f'<{len(samples)}h', *samples)
        wf.writeframes(raw)

    size_kb = os.path.getsize(OUTPUT_PATH) / 1024
    print(f"[gen_test_audio] Wrote {OUTPUT_PATH}  "
          f"({DURATION_S}s, {SAMPLE_RATE}Hz, {BITS}-bit mono)  "
          f"{size_kb:.1f} KB")

if __name__ == "__main__":
    main()
