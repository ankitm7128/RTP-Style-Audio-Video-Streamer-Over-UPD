/**
 * rtp_header.h -- Custom RTP-style packet header
 *
 * Mirrors the essential fields of RFC 3550 RTP without the full optional
 * extension / CSRC machinery. Every UDP datagram is laid out as:
 *
 *   [ RTPHeader (12 bytes, packed) ][ payload bytes ]
 *
 * All multi-byte fields are stored in NETWORK byte order (big-endian).
 * Use make_header() to construct and ntoh_header() to decode on receipt.
 */

#pragma once

#include <cstdint>
#include <cstring>

// Byte-order helpers: htons/htonl/ntohs/ntohl.
// Must be pulled in here because rtp_header.h may be included before common.h.
#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>   // provides htons/htonl/ntohs/ntohl on Windows
#else
  #include <arpa/inet.h>  // provides htons/htonl/ntohs/ntohl on POSIX
#endif


// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint8_t  PAYLOAD_AUDIO   = 0;   ///< payload_type: PCM audio chunk
constexpr uint8_t  PAYLOAD_VIDEO   = 1;   ///< payload_type: raw-frame fragment

constexpr uint16_t DEFAULT_PORT    = 5004; ///< well-known RTP port
constexpr uint32_t AUDIO_SSRC      = 0xDEADBEEF; ///< fixed SSRC for audio stream
constexpr uint32_t VIDEO_SSRC      = 0xCAFEBABE; ///< fixed SSRC for video stream

/// Maximum bytes placed in the payload section of one UDP datagram.
/// Chosen to stay well below the Ethernet MTU (1500) after IP+UDP headers.
constexpr size_t   MAX_PAYLOAD     = 1400;

/// Audio chunk size in bytes: 320 bytes = 160 samples × 2 bytes/sample
/// At 8 kHz this equals exactly 20 ms — the standard VoIP packetisation interval.
constexpr size_t   AUDIO_CHUNK_BYTES = 320;

/// Audio clock rate (samples per second).  Timestamp unit = 1 sample.
constexpr uint32_t AUDIO_CLOCK_RATE = 8000;

/// Audio samples per 20 ms chunk  (= AUDIO_CHUNK_BYTES / 2 for 16-bit mono).
constexpr uint32_t AUDIO_TS_STEP   = 160;

/// Video frame dimensions used by the test generator (raw RGB24).
constexpr uint32_t FRAME_WIDTH     = 640;
constexpr uint32_t FRAME_HEIGHT    = 480;
constexpr size_t   FRAME_BYTES     = FRAME_WIDTH * FRAME_HEIGHT * 3; // RGB24

/// Target video frame rate.
constexpr uint32_t VIDEO_FPS       = 15;
/// Clock ticks per video frame (90 kHz RTP video clock).
constexpr uint32_t VIDEO_TS_STEP   = 90000 / VIDEO_FPS; // 6000

// ---------------------------------------------------------------------------
// RTPHeader struct  (12 bytes, packed, no padding)
//
// Field layout (network/big-endian byte order on the wire):
//   Offset  Size  Field
//   0       2     sequence_number
//   2       4     timestamp
//   6       1     payload_type
//   7       1     marker
//   8       4     ssrc
//   -------------------------
//   Total  12 bytes
// ---------------------------------------------------------------------------

#pragma pack(push, 1)
struct RTPHeader {
    uint16_t sequence_number; ///< per-packet counter; wraps at 65535
    uint32_t timestamp;       ///< media clock; audio=sample#, video=90kHz tick
    uint8_t  payload_type;    ///< PAYLOAD_AUDIO or PAYLOAD_VIDEO
    uint8_t  marker;          ///< 1 = last fragment of a video frame / end of audio chunk
    uint32_t ssrc;            ///< stream source identifier
};
#pragma pack(pop)

// 2 + 4 + 1 + 1 + 4 = 12 bytes when packed
static_assert(sizeof(RTPHeader) == 12,
              "RTPHeader must be exactly 12 bytes (packed)");

// ---------------------------------------------------------------------------
// Byte-order helpers
// ---------------------------------------------------------------------------

/// Convert an RTPHeader from host byte order -> network byte order in-place.
inline void hton_header(RTPHeader& h) {
    h.sequence_number = htons(h.sequence_number);
    h.timestamp       = htonl(h.timestamp);
    h.ssrc            = htonl(h.ssrc);
    // payload_type and marker are single bytes -- no conversion needed
}

/// Convert an RTPHeader from network byte order -> host byte order in-place.
inline void ntoh_header(RTPHeader& h) {
    h.sequence_number = ntohs(h.sequence_number);
    h.timestamp       = ntohl(h.timestamp);
    h.ssrc            = ntohl(h.ssrc);
}

/// Construct a ready-to-send (host-order) RTPHeader.
inline RTPHeader make_header(uint16_t seq,
                              uint32_t ts,
                              uint8_t  pt,
                              uint8_t  marker,
                              uint32_t ssrc) {
    RTPHeader h;
    h.sequence_number = seq;
    h.timestamp       = ts;
    h.payload_type    = pt;
    h.marker          = marker;
    h.ssrc            = ssrc;
    return h;
}
