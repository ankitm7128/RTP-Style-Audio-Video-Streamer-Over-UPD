#!/usr/bin/env python3
"""
gen_test_frames.py — Generate synthetic raw RGB24 video frames for rtp-streamer.

Output: input/frames/frame_0001.raw … frame_0060.raw
Format: 640 × 480 pixels, RGB24 (3 bytes per pixel), no header — pure raw bytes
Content: Animated colour gradient that cycles hue across frames (60 frames @ 15 fps = 4 s)

No external libraries required — uses only Python standard library.
"""

import os
import struct
import math

OUT_DIR    = os.path.join("input", "frames")
NUM_FRAMES = 60
WIDTH      = 640
HEIGHT     = 480

def hsv_to_rgb(h: float, s: float, v: float):
    """Convert HSV (0-1 each) to RGB (0-255 each)."""
    if s == 0.0:
        vi = int(v * 255)
        return vi, vi, vi
    i = int(h * 6)
    f = h * 6 - i
    p = v * (1 - s)
    q = v * (1 - f * s)
    t = v * (1 - (1 - f) * s)
    i %= 6
    if i == 0: r, g, b = v, t, p
    elif i == 1: r, g, b = q, v, p
    elif i == 2: r, g, b = p, v, t
    elif i == 3: r, g, b = p, q, v
    elif i == 4: r, g, b = t, p, v
    else:        r, g, b = v, p, q
    return int(r * 255), int(g * 255), int(b * 255)

def make_frame(frame_idx: int) -> bytes:
    """
    Generate a 640×480 RGB24 frame.
    Each pixel's hue = (col/WIDTH + frame_phase) mod 1.0
    Brightness varies by row for visual depth.
    A dark cross (every 80px) adds a grid pattern.
    """
    frame_phase = frame_idx / NUM_FRAMES   # hue offset advances each frame
    pixels = bytearray(WIDTH * HEIGHT * 3)
    pos = 0
    for row in range(HEIGHT):
        brightness = 0.65 + 0.35 * math.sin(math.pi * row / HEIGHT)
        for col in range(WIDTH):
            hue = (col / WIDTH + frame_phase) % 1.0
            sat = 1.0
            # Grid lines every 80 pixels
            if col % 80 == 0 or row % 60 == 0:
                r, g, b = 10, 10, 10
            else:
                r, g, b = hsv_to_rgb(hue, sat, brightness)
            pixels[pos]     = r
            pixels[pos + 1] = g
            pixels[pos + 2] = b
            pos += 3
    return bytes(pixels)

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    frame_bytes = WIDTH * HEIGHT * 3

    for i in range(NUM_FRAMES):
        data = make_frame(i)
        path = os.path.join(OUT_DIR, f"frame_{i+1:04d}.raw")
        with open(path, 'wb') as f:
            f.write(data)
        if (i + 1) % 10 == 0 or i == 0:
            print(f"[gen_test_frames] frame {i+1:03d}/{NUM_FRAMES}  "
                  f"{len(data)/1024:.1f} KB  -> {path}")

    print(f"\n[gen_test_frames] Done — {NUM_FRAMES} frames, "
          f"{frame_bytes} bytes each ({frame_bytes/1024:.1f} KB), "
          f"in {OUT_DIR}/")

if __name__ == "__main__":
    main()
