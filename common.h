/**
 * common.h -- Shared utilities for rtp-streamer sender & receiver
 *
 * Covers:
 *   - Cross-platform socket initialisation (WinSock2 / POSIX)
 *   - WAV file read/write helpers
 *   - Portable sleep_ms()
 *   - Timestamp logging
 *   - StreamStats tracking and pretty-printing
 *
 * Compatibility: GCC 8+ (MinGW on Windows), GCC/Clang on Linux/macOS.
 * Does NOT use std::filesystem (broken in MinGW GCC 8) -- uses dirent.h /
 * WIN32 FindFile for directory listing in sender.cpp instead.
 */

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <numeric>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <sstream>

// ---------------------------------------------------------------------------
// Platform headers
// ---------------------------------------------------------------------------

#ifndef _WIN32
  #include <dirent.h>  // for list_files() on POSIX
#endif

// ---------------------------------------------------------------------------
// Platform sockets  (ws2tcpip.h MUST come before winsock2.h for inet_pton)
// ---------------------------------------------------------------------------

#ifdef _WIN32
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <ws2tcpip.h>
  #include <winsock2.h>
  typedef SOCKET sock_t;
  constexpr sock_t INVALID_SOCK = INVALID_SOCKET;
  inline int close_socket(sock_t s) { return closesocket(s); }
  inline int get_last_error()       { return WSAGetLastError(); }
#else
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int sock_t;
  constexpr sock_t INVALID_SOCK = -1;
  inline int close_socket(sock_t s) { return close(s); }
  inline int get_last_error()       { return errno; }
#endif

// ---------------------------------------------------------------------------
// Socket lifecycle helpers
// ---------------------------------------------------------------------------

/// Must be called once before any socket operation on Windows; no-op on POSIX.
inline void init_sockets() {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
        throw std::runtime_error("WSAStartup failed");
#endif
}

/// Cleanup; no-op on POSIX.
inline void cleanup_sockets() {
#ifdef _WIN32
    WSACleanup();
#endif
}

/// Create a UDP socket bound to the given local port (for receiver).
inline sock_t create_receiver_socket(uint16_t port, int timeout_ms = 500) {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCK)
        throw std::runtime_error("socket() failed: " + std::to_string(get_last_error()));

    // Allow rapid re-bind after process restart
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Set receive timeout so the receiver loop can check for end-of-stream
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#endif

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_socket(s);
        throw std::runtime_error("bind() failed on port " + std::to_string(port)
                                 + ": " + std::to_string(get_last_error()));
    }
    return s;
}

/// Create a plain UDP socket for sending (no bind needed).
inline sock_t create_sender_socket() {
    sock_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCK)
        throw std::runtime_error("socket() failed: " + std::to_string(get_last_error()));
    return s;
}

/// Populate a sockaddr_in for the given IPv4 address string and port.
inline sockaddr_in make_addr(const std::string& ip, uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    // inet_addr is universally available (inet_pton is missing in MinGW GCC 8)
    unsigned long res = inet_addr(ip.c_str());
    if (res == INADDR_NONE)
        throw std::runtime_error("Invalid IP address: " + ip);
    addr.sin_addr.s_addr = res;
    return addr;
}

// ---------------------------------------------------------------------------
// Portable sleep
// ---------------------------------------------------------------------------

inline void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------------------------------------------------------------------
// Logging with wall-clock timestamp
// ---------------------------------------------------------------------------

inline std::string now_str() {
    auto now = std::chrono::system_clock::now();
    auto ms  = std::chrono::duration_cast<std::chrono::milliseconds>(
                   now.time_since_epoch()) % 1000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm_info{};
#ifdef _WIN32
    localtime_s(&tm_info, &t);
#else
    localtime_r(&t, &tm_info);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_info, "%H:%M:%S")
        << '.' << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

#define LOG(msg)  std::cout << "[" << now_str() << "] " << msg << "\n"
#define LOGW(msg) std::cerr << "[" << now_str() << "] WARN: " << msg << "\n"
#define LOGE(msg) std::cerr << "[" << now_str() << "] ERR:  " << msg << "\n"

// ---------------------------------------------------------------------------
// WAV file structures
// ---------------------------------------------------------------------------

#pragma pack(push, 1)

struct WAVRiffChunk {
    char     chunk_id[4];    // "RIFF"
    uint32_t chunk_size;     // file size - 8
    char     format[4];      // "WAVE"
};

struct WAVFmtChunk {
    // NOTE: subchunk1_id ("fmt ") and subchunk1_size (16) are NOT included here.
    // The read_wav() loop reads the chunk id+size before calling fread(), so this
    // struct holds only the PCM format fields that follow the chunk header.
    uint16_t audio_format;    // 1 = PCM
    uint16_t num_channels;    // 1 = mono, 2 = stereo
    uint32_t sample_rate;     // e.g. 8000
    uint32_t byte_rate;       // SampleRate x NumChannels x BitsPerSample/8
    uint16_t block_align;     // NumChannels x BitsPerSample/8
    uint16_t bits_per_sample; // 8, 16, ...
};

struct WAVDataChunk {
    char     subchunk2_id[4]; // "data"
    uint32_t subchunk2_size;  // number of raw PCM bytes
};

#pragma pack(pop)

// ---------------------------------------------------------------------------
// WAV read helpers
// ---------------------------------------------------------------------------

struct WAVFile {
    WAVFmtChunk fmt{};
    std::vector<uint8_t> pcm; ///< raw PCM bytes (after the WAV headers)
};

/// Read a standard WAV file (PCM format) and return its fmt chunk + raw PCM.
inline WAVFile read_wav(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("Cannot open WAV: " + path);

    WAVRiffChunk riff{};
    if (fread(&riff, sizeof(riff), 1, f) != 1)
        throw std::runtime_error("Truncated RIFF header");
    if (std::strncmp(riff.chunk_id, "RIFF", 4) != 0 ||
        std::strncmp(riff.format,   "WAVE", 4) != 0)
        throw std::runtime_error("Not a WAV file: " + path);

    WAVFile wav{};
    bool got_fmt = false, got_data = false;

    while (!feof(f)) {
        char id[4];
        uint32_t size = 0;
        if (fread(id, 4, 1, f) != 1) break;
        if (fread(&size, 4, 1, f) != 1) break;

        if (std::strncmp(id, "fmt ", 4) == 0) {
            if (size < 16) throw std::runtime_error("fmt chunk too small");
            if (fread(&wav.fmt, sizeof(wav.fmt), 1, f) != 1)
                throw std::runtime_error("Cannot read fmt chunk");
            if (size > sizeof(wav.fmt)) fseek(f, (long)(size - sizeof(wav.fmt)), SEEK_CUR);
            got_fmt = true;
        } else if (std::strncmp(id, "data", 4) == 0) {
            wav.pcm.resize(size);
            if (fread(wav.pcm.data(), 1, size, f) != size)
                throw std::runtime_error("Cannot read PCM data");
            got_data = true;
            break;
        } else {
            // skip unknown chunk
            fseek(f, (long)size, SEEK_CUR);
        }
    }
    fclose(f);

    if (!got_fmt)  throw std::runtime_error("No fmt  chunk in: " + path);
    if (!got_data) throw std::runtime_error("No data chunk in: " + path);

    return wav;
}

// ---------------------------------------------------------------------------
// WAV write helper
// ---------------------------------------------------------------------------

/// Write raw PCM bytes back to a WAV file using the given fmt parameters.
inline void write_wav(const std::string& path,
                      const WAVFmtChunk& fmt,
                      const std::vector<uint8_t>& pcm) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("Cannot open output WAV: " + path);

    uint32_t pcm_size = static_cast<uint32_t>(pcm.size());
    // RIFF chunk_size = 4 ("WAVE") + 8 (fmt id+size) + 16 (fmt data) + 8 (data id+size) + pcm
    uint32_t fmt_data_size = static_cast<uint32_t>(sizeof(WAVFmtChunk)); // 16 bytes

    WAVRiffChunk riff{};
    std::memcpy(riff.chunk_id, "RIFF", 4);
    riff.chunk_size = 4 + 8 + fmt_data_size + 8 + pcm_size;
    std::memcpy(riff.format, "WAVE", 4);

    fwrite(&riff,          sizeof(riff),      1, f);
    fwrite("fmt ",         4,                 1, f);
    fwrite(&fmt_data_size, 4,                 1, f);
    fwrite(&fmt,           sizeof(WAVFmtChunk), 1, f);
    fwrite("data",         4,                 1, f);
    fwrite(&pcm_size,      4,                 1, f);
    fwrite(pcm.data(),     1,       pcm_size,  f);

    fclose(f);
}

// ---------------------------------------------------------------------------
// Directory listing (replaces std::filesystem, broken in MinGW GCC 8)
// ---------------------------------------------------------------------------

/// Returns sorted list of full paths to files with the given extension
/// in the given directory.
inline std::vector<std::string> list_files(const std::string& dir,
                                            const std::string& ext) {
    std::vector<std::string> result;

#ifdef _WIN32
    WIN32_FIND_DATAA ffd;
    std::string pattern = dir + "\\*" + ext;
    HANDLE hFind = FindFirstFileA(pattern.c_str(), &ffd);
    if (hFind == INVALID_HANDLE_VALUE) return result;
    do {
        if (!(ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            result.push_back(dir + "\\" + ffd.cFileName);
        }
    } while (FindNextFileA(hFind, &ffd) != 0);
    FindClose(hFind);
#else
    // POSIX -- dirent.h is included at the top of this header
    DIR* d = opendir(dir.c_str());
    if (!d) return result;
    struct dirent* entry;
    while ((entry = readdir(d)) != nullptr) {
        std::string name = entry->d_name;
        if (name.size() >= ext.size() &&
            name.compare(name.size() - ext.size(), ext.size(), ext) == 0) {
            result.push_back(dir + "/" + name);
        }
    }
    closedir(d);
#endif

    std::sort(result.begin(), result.end());
    return result;
}

/// Create a directory (and parent dirs) if it doesn't exist.
inline void make_dirs(const std::string& path) {
#ifdef _WIN32
    // Use system() as a simple fallback -- fine for a demo project
    std::string cmd = "if not exist \"" + path + "\" mkdir \"" + path + "\"";
    system(cmd.c_str());
#else
    std::string cmd = "mkdir -p \"" + path + "\"";
    system(cmd.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Statistics tracker
// ---------------------------------------------------------------------------

struct StreamStats {
    // Counters
    uint64_t packets_sent     = 0;
    uint64_t packets_received = 0;
    uint64_t packets_dropped  = 0;  ///< simulated loss (sender-side)
    uint64_t packets_ooo      = 0;  ///< out-of-order arrivals (receiver-side)
    uint64_t gaps_detected    = 0;  ///< sequence number gaps filled with silence/skip

    // Jitter tracking (inter-arrival time in microseconds)
    std::vector<int64_t> inter_arrival_us;

    std::chrono::steady_clock::time_point last_arrival;
    bool first_packet = true;

    void record_arrival() {
        auto now = std::chrono::steady_clock::now();
        if (!first_packet) {
            int64_t delta = std::chrono::duration_cast<std::chrono::microseconds>(
                                now - last_arrival).count();
            inter_arrival_us.push_back(delta);
        }
        first_packet = false;
        last_arrival = now;
        ++packets_received;
    }

    double avg_jitter_us() const {
        if (inter_arrival_us.size() < 2) return 0.0;
        double mean = static_cast<double>(
            std::accumulate(inter_arrival_us.begin(), inter_arrival_us.end(), int64_t{0}))
            / static_cast<double>(inter_arrival_us.size());
        // jitter = mean absolute deviation from mean inter-arrival
        double sum = 0;
        for (auto v : inter_arrival_us)
            sum += std::fabs(static_cast<double>(v) - mean);
        return sum / static_cast<double>(inter_arrival_us.size());
    }

    int64_t max_inter_arrival_us() const {
        if (inter_arrival_us.empty()) return 0;
        return *std::max_element(inter_arrival_us.begin(), inter_arrival_us.end());
    }

    void print(const std::string& label) const {
        // ASCII box -- avoids multi-byte char literal issues on older compilers
        const int W = 40;
        std::string hline(W, '-');
        std::cout << "\n+" << hline << "+\n";
        std::cout << "| " << std::left << std::setw(W-1) << (label + " Stats") << "|\n";
        std::cout << "+" << hline << "+\n";
        auto row = [&](const std::string& k, const std::string& v) {
            std::cout << "| " << std::left  << std::setw(22) << k
                      << std::right << std::setw(W-23) << v << " |\n";
        };
        row("Packets sent",      std::to_string(packets_sent));
        row("Packets received",  std::to_string(packets_received));
        row("Simulated drops",   std::to_string(packets_dropped));
        row("Out-of-order",      std::to_string(packets_ooo));
        row("Gaps detected",     std::to_string(gaps_detected));
        std::ostringstream jit;
        jit << std::fixed << std::setprecision(1) << avg_jitter_us() << " us";
        row("Avg jitter",        jit.str());
        std::ostringstream mjit;
        mjit << max_inter_arrival_us() << " us";
        row("Max inter-arrival", mjit.str());
        std::cout << "+" << hline << "+\n";
    }
};
