# RTP-Style Audio/Video Streaming over UDP

A from-scratch C++ implementation of real-time audio/video streaming using custom RTP-style packets over raw UDP sockets — no libraries beyond the C++ standard library and OS sockets.

```
[WAV file] ──┐
             ├──> sender ──UDP packets──> receiver ──> [audio_out.wav]
[raw frames]─┘   (RTP-style header)              └──> [frame_*.raw]
```

---

## Project Structure

```
rtp-streamer/
├── sender.cpp           # Reads WAV + raw frames, packetizes, streams
├── receiver.cpp         # Receives, reorders, reassembles, writes output
├── rtp_header.h         # RTPHeader struct + constants + byte-order helpers
├── common.h             # WAV I/O, socket helpers, logging, StreamStats
├── gen_test_audio.py    # Generates input/audio.wav (5s, 8kHz, 16-bit mono)
├── gen_test_frames.py   # Generates input/frames/frame_*.raw (60 RGB24 frames)
├── Makefile             # Cross-platform build (Linux / macOS / Windows MinGW)
├── input/
│   ├── audio.wav
│   └── frames/          # frame_0001.raw … frame_0060.raw
└── output/
    ├── audio_out.wav
    └── frames_out/      # reassembled frames
```

---

## Requirements

| Tool | Version | Notes |
|------|---------|-------|
| g++ | ≥ 9.0 | C++17 required (`-std=c++17`) |
| GNU Make | any | Or build manually — see below |
| Python 3 | ≥ 3.6 | Only for generating test data |
| FFmpeg | optional | To preview raw frames (see below) |

**Windows:** Use [MSYS2](https://www.msys2.org/) with MinGW-w64, or WSL2.

---

## Build

```bash
# From the rtp-streamer/ directory:
make all
```

**Manual build (if make is unavailable):**

```bash
# Linux / macOS
g++ -std=c++17 -O2 -Wall -I. -o sender   sender.cpp
g++ -std=c++17 -O2 -Wall -I. -o receiver receiver.cpp

# Windows MinGW
g++ -std=c++17 -O2 -Wall -I. -o sender.exe   sender.cpp   -lws2_32
g++ -std=c++17 -O2 -Wall -I. -o receiver.exe receiver.cpp -lws2_32
```

---

## Generate Test Data

```bash
make gen-data
# or:
python gen_test_audio.py    # → input/audio.wav     (5s, 8kHz, 16-bit, mono)
python gen_test_frames.py   # → input/frames/*.raw  (60 frames, 640×480, RGB24)
```

---

## Run

Open **two terminals** in the `rtp-streamer/` directory.

**Terminal 1 — start receiver first:**
```bash
./receiver
# Windows:
receiver.exe
```

**Terminal 2 — start sender:**
```bash
./sender --drop-pct 5        # 5% simulated packet loss
# Windows:
sender.exe --drop-pct 5
```

### CLI Options

#### sender
| Option | Default | Description |
|--------|---------|-------------|
| `--host <ip>` | `127.0.0.1` | Destination IP |
| `--port <n>` | `5004` | Destination UDP port |
| `--drop-pct <n>` | `5` | Simulated loss percentage (0–100) |
| `--audio-only` | — | Send only audio |
| `--video-only` | — | Send only video |
| `--loops <n>` | `1` | Repeat audio stream N times |

#### receiver
| Option | Default | Description |
|--------|---------|-------------|
| `--port <n>` | `5004` | Listen port |
| `--timeout <s>` | `3` | Quit after N seconds of silence |
| `--audio-only` | — | Reassemble only audio |
| `--video-only` | — | Reassemble only video |
| `--no-silence` | — | Don't insert silence for lost audio |

---

## Output

After the sender finishes and the receiver exits:

```
output/
├── audio_out.wav          ← Reassembled PCM audio (playable in any media player)
└── frames_out/
    ├── frame_0001.raw     ← Reassembled raw RGB24 frames
    ├── frame_0002.raw
    └── …
```

**Preview a raw frame with FFmpeg:**
```bash
ffplay -f rawvideo -pixel_format rgb24 -video_size 640x480 output/frames_out/frame_0001.raw
# or convert to PNG:
ffmpeg -f rawvideo -pixel_format rgb24 -video_size 640x480 \
       -i output/frames_out/frame_0001.raw output/frame_0001.png
```

**Play reassembled audio:**
```bash
ffplay output/audio_out.wav
# or open in any media player (VLC, Windows Media Player, etc.)
```

---

## Packet Format

Every UDP datagram is structured as:

```
┌─────────────────────────────────────────────────────┐
│  RTPHeader  (11 bytes, packed, network byte order)  │
├─────────────────────────────────────────────────────┤
│  Payload  (up to 1400 bytes of PCM or frame data)   │
└─────────────────────────────────────────────────────┘
```

### RTPHeader fields

```cpp
struct RTPHeader {
    uint16_t sequence_number; // wraps at 65535
    uint32_t timestamp;       // audio: sample#; video: 90kHz tick
    uint8_t  payload_type;    // 0=audio, 1=video
    uint8_t  marker;          // 1=last fragment of frame / end of audio unit
    uint32_t ssrc;            // stream source identifier
};
```

| Field | Purpose | Interview point |
|-------|---------|-----------------|
| `sequence_number` | Detects missing/reordered packets | UDP doesn't guarantee order; receiver uses a sorted `std::map` keyed on this |
| `timestamp` | When this data *should* play | Independent of arrival time — enables jitter buffering |
| `marker` | Last fragment of a video frame | A 921 KB frame is split into ~658 fragments; `marker=1` tells the receiver "frame is complete, reassemble now" |
| `ssrc` | Stream source ID | Distinguishes audio vs video streams in real RTP; also used for SSRC collision detection |
| `payload_type` | Audio vs video demux | Used here to route to separate reorder buffers |

---

## Architecture Deep-Dive

### Sender

```
WAV → strip header → 320-byte chunks → RTPHeader (pt=0) → [loss sim] → sendto()
                      ↑ 20ms pacing                          ↑ rand() < drop%

Frames → fragment → 1400-byte chunks → RTPHeader (pt=1) → [loss sim] → sendto()
          ↑ same ts for all frags of one frame; last frag has marker=1
          ↑ 66ms pacing (15fps)
```

### Receiver

```
recvfrom() ──> parse header ──> demux by payload_type
                                     │
                   ┌─────────────────┴───────────────────┐
                   ▼                                     ▼
           Audio reorder buffer              Video fragment buffer
           map<seq, PCM chunk>               map<ts, map<seq, frag>>
                   │                                     │
           flush contiguous                  when marker=1 seen:
           insert silence on gap             concat frags → full frame
                   │                                     │
           output/audio_out.wav          output/frames_out/frame_*.raw
```

### Gap/Loss Handling

- **Audio**: If sequence numbers skip (gap > 10), silence (320 zero bytes) is inserted for each missing slot so the output stays in sync with the original timeline.
- **Video**: If fragments are lost before `marker=1` arrives, the partial frame is written with whatever data arrived (visually corrupt but documented); the gap is logged.
- **Timeout**: Receiver exits after ~3 seconds of no incoming packets (configurable). The sender also sends an explicit EOS sentinel packet.

---

## Stats Output

At the end of each run, both programs print a summary table:

```
╔══════════════════════════════════════╗
║  Audio Stats                         ║
╠══════════════════════════════════════╣
║  Packets sent          156           ║
║  Packets received      148           ║
║  Simulated drops         8           ║
║  Out-of-order            3           ║
║  Gaps detected           8           ║
║  Avg jitter         20341.7 µs       ║
║  Max inter-arrival   21003 µs        ║
╚══════════════════════════════════════╝
```

**Jitter** is computed as the mean absolute deviation of inter-arrival times — a conservative but valid proxy for what RFC 3550 defines as the *estimated jitter* (running EWMA of inter-arrival delta variance).

---

## What's Omitted (and Why)

This implementation deliberately omits a few real RTP features for scope:

| Feature | Omitted | Real RTP behaviour |
|---------|---------|-------------------|
| CSRC list | Yes | Optional; used in mixing scenarios |
| RTP extension header | Yes | Optional; rarely needed |
| RTCP (control protocol) | Yes | Separate port; carries receiver reports |
| SRTP (encryption) | Yes | Uses libsrtp; out of scope |
| SSRC collision detection | Partial | Fixed SSRCs; real RTP resolves collisions |
| Adaptive jitter buffer | Simplified | We use a fixed 200ms gap threshold |

---
