// audio_receiver — UDP -> playout buffer -> speaker (Person 2, audio/).
//
// Listens on proto::kAudioPort for 652-byte audio packets (see audio_sender), logs each as
// an R row, feeds StreamStats (loss, jitter) and the PlayoutBuffer (jitter buffer, default
// 60 ms). Missing chunks are played as silence; every underrun, concealed chunk, late or
// duplicate packet, trim and resync is logged as an event row. Prints loss and jitter once
// a second and totals at exit. Accepts packets from the first sender IP only (Phase 1 has
// one sender); a new source port from that IP is accepted (the sender restarted).
//
// Usage: audio_receiver [--out N] [--buffer-ms MS] [--log PATH] [--list]
//   --out N          PortAudio output device (default: system default; see --list)
//   --buffer-ms MS   playout buffer before playback starts, 20-600 (default 60)
//   --log PATH       CSV log file (default logs/audio_receiver_<date>_<time>.csv)
//   --list           list audio devices and exit

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <thread>

#include "audio/app_util.hpp"
#include "audio/pa_audio.hpp"
#include "audio/playout_buffer.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/udp_socket.hpp"
#include "metrics/stream_stats.hpp"

static_assert(audio::kSampleRate == proto::kAudioRate, "audio and protocol sample rates differ");
static_assert(audio::kChunkSamples == proto::kAudioChunk, "audio and protocol chunk sizes differ");

namespace {

constexpr size_t kPayloadBytes = sizeof(int16_t) * proto::kAudioChunk;  // 640
constexpr size_t kPacketBytes = proto::kHeaderSize + kPayloadBytes;     // 652
constexpr int kRecvBufferBytes = 1 << 20;
constexpr int kHeadroomChunks = 5;  // trim when more than prebuffer + 100 ms is queued

void usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [--out N] [--buffer-ms MS] [--log PATH] [--list]\n", prog);
}

// Logs the increase of a counter as one event row; returns the increase.
uint64_t log_delta(const char* name, uint64_t now, uint64_t& prev) {
    const uint64_t d = now > prev ? now - prev : 0;
    if (d > 0) metrics::event(name, static_cast<double>(d));
    prev = now;
    return d;
}

using Counters = audio::PlayoutBuffer::Counters;

void print_counters(const char* title, const Counters& c) {
    std::printf(
        "%s played %llu  concealed %llu  underruns %llu  late %llu  duplicates %llu"
        "  out-of-range %llu  trimmed %llu  resyncs %llu\n",
        title, static_cast<unsigned long long>(c.played),
        static_cast<unsigned long long>(c.concealed), static_cast<unsigned long long>(c.underruns),
        static_cast<unsigned long long>(c.late), static_cast<unsigned long long>(c.duplicates),
        static_cast<unsigned long long>(c.out_of_range), static_cast<unsigned long long>(c.trimmed),
        static_cast<unsigned long long>(c.resyncs));
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_path;
    int out_dev = -1, buffer_ms = 60;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--out" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 0, 1000, out_dev)) return usage(argv[0]), 2;
        } else if (a == "--buffer-ms" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 20, 600, buffer_ms)) return usage(argv[0]), 2;
        } else if (a == "--log" && i + 1 < argc) {
            log_path = argv[++i];
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
    if (log_path.empty()) log_path = audio::app::default_log_path("audio_receiver");

    // UdpSocket throws std::runtime_error if it cannot create or bind its socket.
    std::unique_ptr<UdpSocket> sock;
    try {
        sock = std::make_unique<UdpSocket>(proto::kAudioPort);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "network: %s (is UDP port %u free?)\n", e.what(), proto::kAudioPort);
        return 1;
    }
    sock->set_recv_buffer(kRecvBufferBytes);

    audio::app::install_stop_handlers();
    metrics::init(log_path);

    const int prebuffer = (buffer_ms + 19) / 20;  // round up to whole 20 ms chunks
    audio::PlayoutBuffer playout(prebuffer, prebuffer + kHeadroomChunks);
    audio::Playback spk(playout);
    if (!spk.open(out_dev) || !spk.start()) {
        std::fprintf(stderr, "speaker: %s\n", spk.error().c_str());
        return 1;
    }

    metrics::StreamStats stats("audio");

    std::printf("audio_receiver on UDP port %u  buffer %d ms (trim above %d ms)\n",
                proto::kAudioPort, prebuffer * 20, playout.max_chunks() * 20);
    std::printf("  speaker '%s' (latency %.1f ms); log %s; Ctrl+C to stop\n",
                spk.device_name().c_str(), spk.latency_ms(), log_path.c_str());

    std::atomic<bool> receiving{false};
    std::atomic<uint64_t> bad_packets{0}, other_senders{0}, recv_errors{0};

    std::thread rx([&] {
        uint8_t pkt[2048];
        int16_t samples[proto::kAudioChunk];
        std::string from_ip, sender_ip;
        uint16_t from_port = 0, sender_port = 0;
        while (!audio::app::g_stop) {
            const ssize_t n = sock->recv_from(pkt, sizeof pkt, from_ip, from_port, 100);
            if (n == 0) continue;  // timeout: re-check the stop flag
            if (n < 0) {
                recv_errors.fetch_add(1);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            const uint32_t arrival = proto::now_us();

            proto::Header h{};
            if (!proto::read_header(pkt, static_cast<size_t>(n), h) ||
                h.type != proto::Type::Audio || static_cast<size_t>(n) != kPacketBytes) {
                bad_packets.fetch_add(1);
                continue;
            }
            if (sender_ip.empty()) {
                sender_ip = from_ip;
                sender_port = from_port;
                std::printf("receiving audio from %s:%u\n", sender_ip.c_str(), sender_port);
                std::fflush(stdout);
                receiving.store(true);
            } else if (from_ip != sender_ip) {
                other_senders.fetch_add(1);
                continue;
            } else if (from_port != sender_port) {
                std::printf("sender %s changed port %u -> %u (restarted?)\n", sender_ip.c_str(),
                            sender_port, from_port);
                std::fflush(stdout);
                metrics::event("sender_port_changed", from_port);
                sender_port = from_port;
            }

            metrics::packet('R', h, static_cast<size_t>(n));
            stats.on_packet(h.seq, h.timestamp_us, static_cast<size_t>(n), arrival);

            const uint8_t* payload = pkt + proto::kHeaderSize;
            for (int i = 0; i < proto::kAudioChunk; ++i) {
                uint16_t be;
                std::memcpy(&be, payload + 2 * i, sizeof be);
                samples[i] = static_cast<int16_t>(ntohs(be));
            }
            playout.insert(h.seq, samples);
        }
    });

    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    auto next_report = t_start + std::chrono::seconds(1);
    Counters prev{}, report_prev{};
    uint64_t prev_bad = 0, prev_other = 0, prev_rx_err = 0;

    while (!audio::app::g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));

        // Log every change as events (the plan's "log every underrun").
        const Counters c = playout.counters();
        log_delta("underrun", c.underruns, prev.underruns);
        log_delta("concealed", c.concealed, prev.concealed);
        log_delta("late_drop", c.late, prev.late);
        log_delta("duplicate_drop", c.duplicates, prev.duplicates);
        log_delta("out_of_range", c.out_of_range, prev.out_of_range);
        log_delta("trimmed", c.trimmed, prev.trimmed);
        log_delta("resync", c.resyncs, prev.resyncs);
        log_delta("bad_packet", bad_packets.load(), prev_bad);
        log_delta("other_sender", other_senders.load(), prev_other);
        log_delta("recv_error", recv_errors.load(), prev_rx_err);

        const auto now = Clock::now();
        if (now < next_report) continue;
        next_report += std::chrono::seconds(1);

        const double secs = std::chrono::duration<double>(now - t_start).count();
        if (!receiving.load()) {
            if (static_cast<int>(secs) % 2 == 0) {
                std::printf("t=%4.0fs  waiting for audio on UDP port %u...\n", secs,
                            proto::kAudioPort);
                std::fflush(stdout);
            }
            continue;
        }
        const auto snap = stats.take_interval(proto::now_us());
        const int depth_ms = playout.depth_chunks() * 20;
        metrics::event("buffer_ms", depth_ms);
        std::printf("t=%4.0fs  %s  buffer %3d ms  concealed +%llu  underruns +%llu  late +%llu\n",
                    secs, stats.format(snap).c_str(), depth_ms,
                    static_cast<unsigned long long>(c.concealed - report_prev.concealed),
                    static_cast<unsigned long long>(c.underruns - report_prev.underruns),
                    static_cast<unsigned long long>(c.late - report_prev.late));
        std::fflush(stdout);
        report_prev = c;
    }

    rx.join();
    spk.stop();

    const double secs = std::chrono::duration<double>(Clock::now() - t_start).count();
    std::printf("\nstopped after %.1f s\n", secs);
    std::printf("total   %s\n", stats.format(stats.total()).c_str());
    print_counters("buffer ", playout.counters());
    std::printf("dropped packets: %llu malformed, %llu from other senders, %llu receive errors\n",
                static_cast<unsigned long long>(bad_packets.load()),
                static_cast<unsigned long long>(other_senders.load()),
                static_cast<unsigned long long>(recv_errors.load()));
    std::printf("log: %s\n", log_path.c_str());
    return 0;
}
