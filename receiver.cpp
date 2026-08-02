/**
 * receiver.cpp -- RTP-style audio/video receiver
 *
 * Listens on a UDP port, parses RTP-style headers, demultiplexes audio/video,
 * reorders out-of-order packets, fills gaps, reassembles streams, and writes:
 *
 *   output/audio_out.wav          -- reassembled audio (PCM WAV)
 *   output/frames_out/frame_*.raw -- reassembled raw RGB24 video frames
 *
 * Usage:
 *   receiver [options]
 *     --port      <n>   listen port (default: 5004)
 *     --timeout   <s>   quit after N seconds with no packets (default: 3)
 *     --audio-only      skip video reassembly
 *     --video-only      skip audio reassembly
 *     --no-silence      don't insert silence for lost audio packets
 */

#include "rtp_header.h"
#include "common.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <iomanip>

// ---------------------------------------------------------------------------
// CLI options
// ---------------------------------------------------------------------------
struct Options {
    uint16_t port       = DEFAULT_PORT;
    int      timeout_s  = 3;
    bool     audio      = true;
    bool     video      = true;
    bool     silence    = true;  ///< insert silence for lost audio
};

static Options parse_args(int argc, char* argv[]) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--port"     && i+1 < argc) o.port      = static_cast<uint16_t>(std::stoi(argv[++i]));
        else if (a == "--timeout"  && i+1 < argc) o.timeout_s = std::stoi(argv[++i]);
        else if (a == "--audio-only")              o.video     = false;
        else if (a == "--video-only")              o.audio     = false;
        else if (a == "--no-silence")              o.silence   = false;
        else if (a == "--help") {
            std::cout <<
                "Usage: receiver [--port N] [--timeout S]\n"
                "                [--audio-only] [--video-only] [--no-silence]\n";
            std::exit(0);
        }
    }
    return o;
}

// ---------------------------------------------------------------------------
// Audio reorder buffer
//
//   key   = sequence_number (uint16_t, handles wrap-around with signed subtraction)
//   value = raw PCM bytes for that chunk
// ---------------------------------------------------------------------------
struct AudioBuffer {
    std::map<uint16_t, std::vector<uint8_t>> chunks;
    uint16_t next_expected = 0;
    bool     started       = false;
    std::vector<uint8_t> output;
    StreamStats& stats;
    bool fill_silence;

    explicit AudioBuffer(StreamStats& s, bool silence)
        : stats(s), fill_silence(silence) {}

    void insert(uint16_t seq, std::vector<uint8_t> payload) {
        if (started) {
            int16_t delta = static_cast<int16_t>(seq - next_expected);
            if (delta < 0) {
                // Very late / duplicate -- discard
                LOGW("Audio: discarding duplicate/very-late seq=" << seq);
                return;
            }
            if (delta > 0) {
                ++stats.packets_ooo;
            }
        }
        chunks.emplace(seq, std::move(payload));
        if (!started) {
            next_expected = seq;
            started = true;
        }
    }

    /// Flush all contiguously available chunks from next_expected onwards.
    void flush_contiguous() {
        while (!chunks.empty()) {
            auto it = chunks.find(next_expected);
            if (it == chunks.end()) break;
            output.insert(output.end(), it->second.begin(), it->second.end());
            chunks.erase(it);
            ++next_expected;
        }
    }

    /// Called when a gap (lost packet) is detected -- optionally insert silence.
    void handle_gap(uint16_t up_to_seq) {
        while (next_expected != up_to_seq) {
            ++stats.gaps_detected;
            LOG("Audio: gap at seq=" << next_expected
                << " -- " << (fill_silence ? "inserting silence" : "skipping"));
            if (fill_silence) {
                std::vector<uint8_t> silence_bytes(AUDIO_CHUNK_BYTES, 0);
                output.insert(output.end(), silence_bytes.begin(), silence_bytes.end());
            }
            ++next_expected;
        }
    }

    /// Drain everything remaining (called at end-of-stream).
    void drain() {
        for (auto& kv : chunks) {
            uint16_t seq = kv.first;
            if (static_cast<int16_t>(seq - next_expected) > 0)
                handle_gap(seq);
            output.insert(output.end(), kv.second.begin(), kv.second.end());
            next_expected = static_cast<uint16_t>(seq + 1);
        }
        chunks.clear();
    }
};

// ---------------------------------------------------------------------------
// Video fragment + frame reassembly
//
//   Per-timestamp: map<seq, payload>
//   When marker=1 is seen for that timestamp, concatenate in seq order -> frame
// ---------------------------------------------------------------------------
struct FrameAssembler {
    struct FragmentSet {
        std::map<uint16_t, std::vector<uint8_t>> frags;
        bool     marker_seen = false;
        uint16_t marker_seq  = 0;
    };

    std::map<uint32_t, FragmentSet> pending;  // timestamp -> fragments
    uint32_t frames_written = 0;
    StreamStats& stats;
    std::string out_dir;

    explicit FrameAssembler(StreamStats& s, const std::string& dir)
        : stats(s), out_dir(dir) {}

    void insert(const RTPHeader& hdr, std::vector<uint8_t> payload) {
        auto& fset = pending[hdr.timestamp];
        fset.frags.emplace(hdr.sequence_number, std::move(payload));
        if (hdr.marker) {
            fset.marker_seen = true;
            fset.marker_seq  = hdr.sequence_number;
        }
    }

    /// Try to assemble and write any complete frames.
    void try_flush() {
        for (auto it = pending.begin(); it != pending.end(); ) {
            FragmentSet& fset = it->second;
            if (!fset.marker_seen) { ++it; continue; }

            uint16_t min_seq = fset.frags.begin()->first;
            size_t expected  = static_cast<size_t>(
                static_cast<uint16_t>(fset.marker_seq - min_seq + 1));

            if (fset.frags.size() < expected) { ++it; continue; }

            write_frame(it->first, fset, true);
            it = pending.erase(it);
        }
    }

    /// Force-write whatever we have for each timestamp (incomplete frames too).
    void drain() {
        for (auto& kv : pending) {
            write_frame(kv.first, kv.second, kv.second.marker_seen);
        }
        pending.clear();
    }

    void write_frame(uint32_t ts, const FragmentSet& fset, bool complete) {
        if (fset.frags.empty()) return;

        // Concatenate fragments in seq order
        std::vector<uint8_t> frame_data;
        for (const auto& kv : fset.frags)
            frame_data.insert(frame_data.end(), kv.second.begin(), kv.second.end());

        // Build output path: output/frames_out/frame_0001.raw
        std::ostringstream path;
        path << out_dir << "/frame_" << std::setfill('0') << std::setw(4)
             << (frames_written + 1) << ".raw";

        FILE* fp = fopen(path.str().c_str(), "wb");
        if (!fp) {
            LOGE("Video: cannot write " << path.str());
            return;
        }
        fwrite(frame_data.data(), 1, frame_data.size(), fp);
        fclose(fp);

        LOG("Video: wrote frame " << (frames_written + 1)
            << "  ts=" << ts
            << "  bytes=" << frame_data.size()
            << (complete ? "" : "  [INCOMPLETE -- fragment loss]"));

        ++frames_written;
    }
};

// ---------------------------------------------------------------------------
// Receive loop
// ---------------------------------------------------------------------------
static void run_receiver(const Options& opts) {
    // Prepare output directories (using make_dirs from common.h -- no filesystem)
    make_dirs("output");
    make_dirs("output/frames_out");

    LOG("Receiver listening on :" << opts.port
        << "  timeout=" << opts.timeout_s << "s");

    sock_t sock = create_receiver_socket(opts.port, 500 /* ms SO_RCVTIMEO */);

    StreamStats audio_stats, video_stats;
    AudioBuffer    audio_buf(audio_stats, opts.silence);
    FrameAssembler video_asm(video_stats, "output/frames_out");

    // Reconstruct default WAV fmt for 8kHz 16-bit mono output
    WAVFmtChunk default_fmt{};
    default_fmt.audio_format    = 1;   // PCM
    default_fmt.num_channels    = 1;
    default_fmt.sample_rate     = AUDIO_CLOCK_RATE;
    default_fmt.bits_per_sample = 16;
    default_fmt.block_align     = 2;
    default_fmt.byte_rate       = AUDIO_CLOCK_RATE * 2;

    std::vector<uint8_t> recv_buf(sizeof(RTPHeader) + MAX_PAYLOAD + 64);
    int  consecutive_timeouts = 0;
    int  max_timeouts         = opts.timeout_s * 2; // 500ms each
    bool eos_received         = false;

    auto last_packet_time = std::chrono::steady_clock::now();

    while (!eos_received) {
        sockaddr_in sender_addr{};
        socklen_t   sender_len = sizeof(sender_addr);

        int n = recvfrom(sock,
                         reinterpret_cast<char*>(recv_buf.data()),
                         static_cast<int>(recv_buf.size()),
                         0,
                         reinterpret_cast<sockaddr*>(&sender_addr),
                         &sender_len);

        if (n < 0) {
            ++consecutive_timeouts;
            auto now = std::chrono::steady_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  now - last_packet_time).count();

            // If we have been waiting >200ms with buffered audio, flush contiguous
            if (elapsed_ms > 200 && audio_buf.started) {
                audio_buf.flush_contiguous();
            }

            if (consecutive_timeouts >= max_timeouts) {
                LOG("Timeout -- no packets received for ~" << opts.timeout_s
                    << "s. Flushing.");
                break;
            }
            continue;
        }

        consecutive_timeouts = 0;
        last_packet_time = std::chrono::steady_clock::now();

        if (n < static_cast<int>(sizeof(RTPHeader))) {
            LOGW("Datagram too small (" << n << " bytes) -- discarding");
            continue;
        }

        // Parse header
        RTPHeader hdr;
        std::memcpy(&hdr, recv_buf.data(), sizeof(RTPHeader));
        ntoh_header(hdr);

        // Check for EOS sentinel (ssrc=0, payload_type=0xFF)
        if (hdr.ssrc == 0x00000000 && hdr.payload_type == 0xFF) {
            LOG("Received EOS sentinel -- flushing and exiting");
            eos_received = true;
            break;
        }

        size_t payload_len = static_cast<size_t>(n) - sizeof(RTPHeader);
        std::vector<uint8_t> payload(recv_buf.begin() + sizeof(RTPHeader),
                                     recv_buf.begin() + n);

        if (hdr.payload_type == PAYLOAD_AUDIO && opts.audio) {
            audio_stats.record_arrival();

            LOG("Audio RX   seq=" << std::setw(5) << hdr.sequence_number
                << "  ts=" << std::setw(8) << hdr.timestamp
                << "  bytes=" << payload_len);

            audio_buf.insert(hdr.sequence_number, std::move(payload));
            audio_buf.flush_contiguous();

        } else if (hdr.payload_type == PAYLOAD_VIDEO && opts.video) {
            video_stats.record_arrival();

            LOG("Video RX   seq=" << std::setw(5) << hdr.sequence_number
                << "  ts=" << std::setw(8) << hdr.timestamp
                << "  bytes=" << payload_len
                << (hdr.marker ? "  [MARKER]" : ""));

            video_asm.insert(hdr, std::move(payload));
            video_asm.try_flush();

        } else {
            // Silently ignore: either a valid type filtered by --audio-only /
            // --video-only, or a genuinely unknown type.
            if (hdr.payload_type != PAYLOAD_AUDIO && hdr.payload_type != PAYLOAD_VIDEO) {
                LOGW("Unknown payload_type=" << static_cast<int>(hdr.payload_type)
                     << " -- ignoring");
            }
        }
    }

    // -----------------------------------------------------------------------
    // Drain and write outputs
    // -----------------------------------------------------------------------
    if (opts.audio && audio_buf.started) {
        audio_buf.drain();
        const std::string out_path = "output/audio_out.wav";
        write_wav(out_path, default_fmt, audio_buf.output);
        LOG("Audio: wrote " << audio_buf.output.size()
            << " PCM bytes -> " << out_path);
    }

    if (opts.video) {
        video_asm.drain();
        LOG("Video: " << video_asm.frames_written
            << " frame(s) written to output/frames_out/");
    }

    close_socket(sock);

    // -----------------------------------------------------------------------
    // Final stats
    // -----------------------------------------------------------------------
    if (opts.audio) audio_stats.print("Audio RX");
    if (opts.video) video_stats.print("Video RX");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[]) {
    Options opts = parse_args(argc, argv);

    init_sockets();
    try {
        run_receiver(opts);
    } catch (const std::exception& e) {
        LOGE(e.what());
        cleanup_sockets();
        return 1;
    }
    cleanup_sockets();

    LOG("Receiver done.");
    return 0;
}
