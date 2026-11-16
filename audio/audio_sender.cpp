// audio_sender — microphone -> UDP (Person 2, audio/).
//
// Every 20 ms chunk (320 samples, 16 kHz mono int16) becomes one 652-byte packet:
//   12-byte header (common/protocol.hpp)  +  320 samples as int16 in network byte order
// sent to <dest_ip>:proto::kAudioPort. The header timestamp is the chunk's capture time on
// proto::now_us(), the same clock the video sender uses, for lip sync in Phase 2.
// Every packet sent is logged as an S row; Ctrl+C exits cleanly so the log is complete.
//
// Usage: audio_sender <dest_ip> [--in N] [--stream ID] [--log PATH] [--list]
//   --in N       PortAudio input device (default: system default; see --list)
//   --stream ID  stream_id in the header, 0-255 (default 0)
//   --log PATH   CSV log file (default logs/audio_sender_<date>_<time>.csv)
//   --list       list audio devices and exit

#include <arpa/inet.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>

#include "audio/app_util.hpp"
#include "audio/pa_audio.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/udp_socket.hpp"

static_assert(audio::kSampleRate == proto::kAudioRate, "audio and protocol sample rates differ");
static_assert(audio::kChunkSamples == proto::kAudioChunk, "audio and protocol chunk sizes differ");

namespace {

constexpr size_t kPayloadBytes = sizeof(int16_t) * proto::kAudioChunk;  // 640
constexpr size_t kPacketBytes = proto::kHeaderSize + kPayloadBytes;     // 652
static_assert(kPayloadBytes <= proto::kMaxPayload, "audio chunk does not fit one packet");

void usage(const char* prog) {
    std::fprintf(stderr, "usage: %s <dest_ip> [--in N] [--stream ID] [--log PATH] [--list]\n",
                 prog);
}

// Logs the increase of a counter as one event row.
void log_delta(const char* name, uint64_t now, uint64_t& prev) {
    if (now > prev) metrics::event(name, static_cast<double>(now - prev));
    prev = now;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dest, log_path;
    int in_dev = -1, stream_id = 0;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--in" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 0, 1000, in_dev)) return usage(argv[0]), 2;
        } else if (a == "--stream" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 0, 255, stream_id)) return usage(argv[0]), 2;
        } else if (a == "--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (dest.empty() && a[0] != '-') {
            dest = a;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    audio::PaSystem pa;
    if (!pa.ok()) {
        std::fprintf(stderr, "%s\n", pa.error().c_str());
        return 1;
    }
    if (list) {
        audio::PaSystem::list_devices(stdout);
        return 0;
    }
    if (!audio::app::is_ipv4(dest)) {
        usage(argv[0]);
        return 2;
    }
    if (log_path.empty()) log_path = audio::app::default_log_path("audio_sender");

    // UdpSocket throws std::runtime_error if it cannot create or bind its socket.
    std::unique_ptr<UdpSocket> sock;
    try {
        sock = std::make_unique<UdpSocket>();  // any free local port
    } catch (const std::exception& e) {
        std::fprintf(stderr, "network: %s\n", e.what());
        return 1;
    }

    audio::app::install_stop_handlers();
    metrics::init(log_path);

    audio::Capture mic(&proto::now_us);
    if (!mic.open(in_dev) || !mic.start()) {
        std::fprintf(stderr, "mic: %s\n", mic.error().c_str());
        return 1;
    }

    std::printf("audio_sender -> %s:%u  stream %d  mic '%s' (latency %.1f ms)\n", dest.c_str(),
                proto::kAudioPort, stream_id, mic.device_name().c_str(), mic.latency_ms());
    std::printf("  %zu-byte packets every 20 ms; log %s; Ctrl+C to stop\n", kPacketBytes,
                log_path.c_str());

    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    auto next_report = t_start + std::chrono::seconds(1);
    auto last_chunk = t_start;
    bool warned_no_audio = false;

    uint16_t seq = 0;
    uint8_t pkt[kPacketBytes];
    uint64_t sent = 0, send_errors = 0, sent_at_report = 0;
    uint64_t prev_overflows = 0, prev_driver = 0, prev_errors = 0;

    while (!audio::app::g_stop) {
        audio::AudioChunk c;
        if (mic.pop_wait(c, 100)) {
            last_chunk = Clock::now();
            warned_no_audio = false;

            proto::Header h{};
            h.type = proto::Type::Audio;
            h.stream_id = static_cast<uint8_t>(stream_id);
            h.seq = seq++;
            h.timestamp_us = c.capture_us;
            h.frame_id = 0;
            h.frag_index = 0;
            h.frag_count = 1;
            proto::write_header(h, pkt);

            uint8_t* payload = pkt + proto::kHeaderSize;
            for (int i = 0; i < proto::kAudioChunk; ++i) {
                const uint16_t be = htons(static_cast<uint16_t>(c.samples[i]));
                std::memcpy(payload + 2 * i, &be, sizeof be);
            }

            if (sock->send_to(dest, proto::kAudioPort, pkt, kPacketBytes)) {
                ++sent;
                metrics::packet('S', h, kPacketBytes);
            } else {
                ++send_errors;
            }
        }

        const auto now = Clock::now();
        if (!warned_no_audio && now - last_chunk > std::chrono::seconds(1)) {
            std::fprintf(stderr, "warning: no audio from the mic for 1 s\n");
            warned_no_audio = true;
        }
        log_delta("capture_overflow", mic.overflows(), prev_overflows);
        log_delta("driver_input_overflow", mic.driver_overflows(), prev_driver);
        log_delta("send_error", send_errors, prev_errors);

        if (now >= next_report) {
            const double secs = std::chrono::duration<double>(now - t_start).count();
            const uint64_t n = sent - sent_at_report;
            std::printf(
                "t=%4.0fs  sent %3llu pkt/s  %6.1f kbps  seq %5u  mic overflows %llu"
                "  send errors %llu\n",
                secs, static_cast<unsigned long long>(n), n * kPacketBytes * 8 / 1000.0,
                static_cast<unsigned>(seq), static_cast<unsigned long long>(mic.overflows()),
                static_cast<unsigned long long>(send_errors));
            std::fflush(stdout);
            sent_at_report = sent;
            next_report += std::chrono::seconds(1);
        }
    }

    mic.stop();
    const double secs = std::chrono::duration<double>(Clock::now() - t_start).count();
    std::printf(
        "\nstopped after %.1f s: sent %llu packets (%.1f/s), %llu send errors,"
        " %llu mic overflows\nlog: %s\n",
        secs, static_cast<unsigned long long>(sent), secs > 0 ? sent / secs : 0.0,
        static_cast<unsigned long long>(send_errors),
        static_cast<unsigned long long>(mic.overflows()), log_path.c_str());
    return 0;
}
