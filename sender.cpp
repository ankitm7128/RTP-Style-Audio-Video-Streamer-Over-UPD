/**
 * sender.cpp -- RTP-style audio/video sender
 *
 * Reads:
 *   input/audio.wav          -- raw PCM WAV (8 kHz, 16-bit, mono recommended)
 *   input/frames/frame_*.raw -- raw RGB24 frames (640x480 bytes each)
 *
 * Packetizes each stream with a custom RTP-style header and sends over UDP.
 * Simulates packet loss at a configurable drop percentage.
 *
 * Usage:
 *   sender [options]
 *     --host     <ip>    destination IP  (default: 127.0.0.1)
 *     --port     <n>     destination port (default: 5004)
 *     --drop-pct <n>     packet loss simulation 0-100 (default: 5)
 *     --audio-only       skip video
 *     --video-only       skip audio
 *     --loops    <n>     repeat audio stream N times (default: 1)
 */

#include "rtp_header.h"
#include "common.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <string>
#include <cstdlib>
#include <ctime>
#include <algorithm>

// ---------------------------------------------------------------------------
// CLI options
// ---------------------------------------------------------------------------
struct Options {
    std::string host     = "127.0.0.1";
    uint16_t    port     = DEFAULT_PORT;
    int         drop_pct = 5;
    bool        audio    = true;
    bool        video    = true;
    int         loops    = 1;
};

static Options parse_args(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--host"     && i+1 < argc) { o.host     = argv[++i]; }
        else if (a == "--port"     && i+1 < argc) { o.port     = static_cast<uint16_t>(std::stoi(argv[++i])); }
        else if (a == "--drop-pct" && i+1 < argc) { o.drop_pct = std::stoi(argv[++i]); }
        else if (a == "--loops"    && i+1 < argc) { o.loops    = std::stoi(argv[++i]); }
        else if (a == "--audio-only") { o.video = false; }
        else if (a == "--video-only") { o.audio = false; }
        else if (a == "--help") {
            std::cout <<
                "Usage: sender [--host IP] [--port N] [--drop-pct N]\n"
                "              [--audio-only] [--video-only] [--loops N]\n";
            std::exit(0);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Loss simulation helper
// ---------------------------------------------------------------------------
static bool should_drop(int drop_pct) {
    if (drop_pct <= 0) return false;
    return (std::rand() % 100) < drop_pct;
}

// ---------------------------------------------------------------------------
// Send a fully-constructed datagram  (header already in network byte order)
// ---------------------------------------------------------------------------
static bool send_packet(sock_t sock,
                        const sockaddr_in& dest,
                        const RTPHeader& hdr,
                        const uint8_t* payload,
                        size_t payload_len,
                        int drop_pct,
                        StreamStats& stats) {
    if (should_drop(drop_pct)) {
        ++stats.packets_dropped;
        ++stats.packets_sent;       // counted as "attempted"
        return false;               // packet was "lost"
    }

    // Build datagram: [header][payload]
    std::vector<uint8_t> buf(sizeof(RTPHeader) + payload_len);
    std::memcpy(buf.data(), &hdr, sizeof(RTPHeader));
    if (payload_len > 0)
        std::memcpy(buf.data() + sizeof(RTPHeader), payload, payload_len);

    int sent = sendto(sock,
                      reinterpret_cast<const char*>(buf.data()),
                      static_cast<int>(buf.size()),
                      0,
                      reinterpret_cast<const sockaddr*>(&dest),
                      sizeof(dest));
    if (sent < 0) {
        LOGE("sendto() failed: " << get_last_error());
        ++stats.packets_sent;
        return false;
    }
    ++stats.packets_sent;
    return true;
}

// ---------------------------------------------------------------------------
// Audio streaming
// ---------------------------------------------------------------------------
static void stream_audio(sock_t sock,
                         const sockaddr_in& dest,
                         const Options& opts,
                         StreamStats& stats) {
    const std::string wav_path = "input/audio.wav";
    LOG("Audio: reading " << wav_path);

    WAVFile wav;
    try {
        wav = read_wav(wav_path);
    } catch (const std::exception& e) {
        LOGE("Audio: " << e.what() << " -- skipping audio stream");
        return;
    }

    LOG("Audio: " << wav.pcm.size() << " PCM bytes, "
        << wav.fmt.sample_rate << " Hz, "
        << wav.fmt.num_channels << " ch, "
        << wav.fmt.bits_per_sample << "-bit");

    const size_t chunk = AUDIO_CHUNK_BYTES;
    uint16_t seq       = 0;
    uint32_t ts        = 0;

    for (int loop = 0; loop < opts.loops; ++loop) {
        size_t offset = 0;

        while (offset < wav.pcm.size()) {
            size_t bytes = std::min(chunk, wav.pcm.size() - offset);

            RTPHeader hdr = make_header(seq, ts,
                                        PAYLOAD_AUDIO,
                                        1,         // marker: every audio chunk is a "unit"
                                        AUDIO_SSRC);
            hton_header(hdr);

            bool sent = send_packet(sock, dest, hdr,
                                    wav.pcm.data() + offset, bytes,
                                    opts.drop_pct, stats);

            if (sent) {
                LOG("Audio TX   seq=" << std::setw(5) << seq
                    << "  ts=" << std::setw(8) << ts
                    << "  bytes=" << bytes);
            } else {
                LOG("Audio DROP seq=" << std::setw(5) << seq
                    << "  ts=" << std::setw(8) << ts
                    << "  [simulated loss]");
            }

            ++seq;
            ts     += AUDIO_TS_STEP;
            offset += bytes;

            sleep_ms(20);   // real-time pacing: 20 ms per audio chunk
        }

        LOG("Audio: loop " << (loop+1) << "/" << opts.loops << " done ("
            << (wav.pcm.size() / chunk) << " chunks)");
    }
}

// ---------------------------------------------------------------------------
// Video streaming
// ---------------------------------------------------------------------------
static void stream_video(sock_t sock,
                         const sockaddr_in& dest,
                         const Options& opts,
                         StreamStats& stats) {
    const std::string frames_dir = "input/frames";

    // list_files() uses WIN32 FindFile on Windows, dirent on POSIX
    std::vector<std::string> paths = list_files(frames_dir, ".raw");

    if (paths.empty()) {
        LOGW("Video: no .raw frames found in " << frames_dir << " -- skipping video");
        return;
    }

    LOG("Video: found " << paths.size() << " frames in " << frames_dir);

    uint16_t seq        = 0;
    uint32_t video_ts   = 0;
    uint32_t frame_idx  = 0;

    const int frame_interval_ms = 1000 / static_cast<int>(VIDEO_FPS); // ~66 ms at 15 fps

    for (const auto& path : paths) {
        // Read raw frame bytes
        FILE* fp = fopen(path.c_str(), "rb");
        if (!fp) {
            LOGW("Video: cannot open " << path << " -- skipping");
            ++frame_idx;
            video_ts += VIDEO_TS_STEP;
            continue;
        }
        fseek(fp, 0, SEEK_END);
        long fsize = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        std::vector<uint8_t> frame_data(static_cast<size_t>(fsize));
        fread(frame_data.data(), 1, static_cast<size_t>(fsize), fp);
        fclose(fp);

        // Fragment frame into MAX_PAYLOAD-size chunks
        size_t total   = frame_data.size();
        size_t n_frags = (total + MAX_PAYLOAD - 1) / MAX_PAYLOAD;
        size_t offset  = 0;

        LOG("Video TX  frame=" << std::setw(4) << frame_idx
            << "  ts=" << std::setw(8) << video_ts
            << "  size=" << total
            << "  frags=" << n_frags);

        for (size_t fi = 0; fi < n_frags; ++fi) {
            size_t payload_len = std::min(MAX_PAYLOAD, total - offset);
            uint8_t marker = (fi == n_frags - 1) ? 1 : 0;

            RTPHeader hdr = make_header(seq, video_ts,
                                        PAYLOAD_VIDEO,
                                        marker,
                                        VIDEO_SSRC);
            hton_header(hdr);

            bool sent = send_packet(sock, dest, hdr,
                                    frame_data.data() + offset, payload_len,
                                    opts.drop_pct, stats);
            if (!sent) {
                LOG("Video DROP seq=" << seq
                    << "  frag=" << fi << "/" << n_frags
                    << "  [simulated loss]");
            }

            ++seq;
            offset += payload_len;
        }

        ++frame_idx;
        video_ts += VIDEO_TS_STEP;

        sleep_ms(frame_interval_ms);   // pace at target FPS
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Options opts = parse_args(argc, argv);
    std::srand(static_cast<unsigned>(std::time(nullptr)));

    LOG("RTP Sender starting -- dest " << opts.host << ":" << opts.port
        << "  drop=" << opts.drop_pct << "%");

    init_sockets();
    StreamStats audio_stats, video_stats;

    try {
        sock_t sock = create_sender_socket();
        sockaddr_in dest = make_addr(opts.host, opts.port);

        if (opts.audio) stream_audio(sock, dest, opts, audio_stats);
        if (opts.video) stream_video(sock, dest, opts, video_stats);

        // Send a special "end-of-stream" marker packet so the receiver
        // knows to flush and exit rather than waiting for the timeout.
        // Sentinel: ssrc=0x00000000, payload_type=0xFF, marker=1
        {
            RTPHeader eos = make_header(0xFFFF, 0xFFFFFFFF, 0xFF, 1, 0x00000000);
            hton_header(eos);
            std::vector<uint8_t> buf(sizeof(RTPHeader));
            std::memcpy(buf.data(), &eos, sizeof(RTPHeader));
            sendto(sock,
                   reinterpret_cast<const char*>(buf.data()),
                   static_cast<int>(buf.size()),
                   0,
                   reinterpret_cast<const sockaddr*>(&dest),
                   sizeof(dest));
            LOG("Sent EOS sentinel packet");
        }

        close_socket(sock);
    } catch (const std::exception& e) {
        LOGE(e.what());
        cleanup_sockets();
        return 1;
    }

    cleanup_sockets();

    if (opts.audio) audio_stats.print("Audio TX");
    if (opts.video) video_stats.print("Video TX");

    LOG("Sender done.");
    return 0;
}
