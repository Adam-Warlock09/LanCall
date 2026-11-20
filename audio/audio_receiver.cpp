// audio_receiver — UDP -> playout buffer -> speaker (Person 2, audio/). A thin wrapper around
// audio::AudioReceiver (audio_pipeline.hpp), kept for single-stream experiments.
//
// Listens on proto::kAudioPort for 652-byte audio packets (see audio_sender), logs each as an
// R row, measures loss and jitter, and plays through a playout buffer (default 60 ms). Every
// underrun, concealed chunk, late or duplicate packet, trim and resync is logged as an event.
// Prints one stats line a second and a whole-run summary at exit.
//
// Usage: audio_receiver [--from IP] [--id N] [--keepalive IP[:PORT]] [--out N] [--buffer-ms MS]
//                      [--log PATH] [--list]
//   --from IP        accept audio only from this address (default: anyone; since phase 4,
//                    senders with different --stream ids are played together, mixed)
//   --id N           our member id, 1-255, written into our receiver reports (default 0)
//   --keepalive IP[:PORT]  send a 12-byte "still here" packet there every 2 s (port default
//                    5014), stream id = --id: a relay forwards to members it hears from, and a
//                    viewer sends no media (phase 7, experiment E10). Needs --id
//   --out N          PortAudio output device (default: system default; see --list)
//   --buffer-ms MS   playout buffer before playback starts, 20-600 (default 60)
//   --log PATH       CSV log file (default logs/audio_receiver_<date>_<time>.csv)
//   --list           list audio devices and exit

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio/app_util.hpp"
#include "audio/audio_pipeline.hpp"
#include "audio/pa_audio.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/udp_socket.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s [--from IP] [--id N] [--keepalive IP[:PORT]] [--out N] [--buffer-ms MS] "
        "[--log PATH] [--list]\n",
        prog);
}

constexpr auto kKeepaliveEvery = std::chrono::seconds(2);  // the relay forgets after 5 s

// A bare header: type Keepalive, stream_id = our member id (what the relay files us under).
std::vector<uint8_t> keepalive(uint8_t id, uint16_t seq) {
    proto::Header h{};
    h.type = proto::Type::Keepalive;
    h.stream_id = id;
    h.seq = seq;
    h.timestamp_us = proto::now_us();
    h.frag_count = 1;
    std::vector<uint8_t> out(proto::kHeaderSize);
    proto::write_header(h, out.data());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_path, from_ip;
    int out_dev = -1, buffer_ms = 60, my_id = 0;
    proto::Endpoint keep_to;  // ip empty = no keepalives
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--from" && i + 1 < argc) {
            from_ip = argv[++i];
            if (!audio::app::is_ipv4(from_ip)) return usage(argv[0]), 2;
        } else if (a == "--id" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 1, 255, my_id)) return usage(argv[0]), 2;
        } else if (a == "--keepalive" && i + 1 < argc) {
            if (!audio::app::parse_endpoint(argv[++i], proto::kRelayPort, keep_to))
                return usage(argv[0]), 2;
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

    if (list) {
        audio::PaSystem pa;
        if (!pa.ok()) {
            std::fprintf(stderr, "%s\n", pa.error().c_str());
            return 1;
        }
        audio::PaSystem::list_devices(stdout);
        return 0;
    }
    if (!keep_to.ip.empty() && my_id == 0) {
        std::fprintf(stderr, "--keepalive needs --id (the relay ignores stream id 0)\n");
        return 2;
    }
    if (log_path.empty()) log_path = audio::app::default_log_path("audio_receiver");

    audio::app::install_stop_handlers();
    metrics::init(log_path);

    audio::ReceiverOptions opt;
    opt.out_device = out_dev;
    opt.buffer_ms = buffer_ms;
    opt.my_id = static_cast<uint8_t>(my_id);
    audio::AudioReceiver rx;
    if (!rx.start(proto::kAudioPort, from_ip, opt)) {
        std::fprintf(stderr, "%s\n", rx.error().c_str());
        return 1;
    }
    std::printf("audio_receiver on UDP port %u%s%s  log %s  (Ctrl+C to stop)\n", proto::kAudioPort,
                from_ip.empty() ? "" : " from ", from_ip.c_str(), log_path.c_str());

    std::unique_ptr<UdpSocket> keep_sock;
    if (!keep_to.ip.empty()) {
        try {
            keep_sock = std::make_unique<UdpSocket>();
        } catch (const std::exception& e) {
            std::fprintf(stderr, "keepalive socket: %s\n", e.what());
            return 1;
        }
        std::printf("keepalive -> %s:%u every 2 s as member %d\n", keep_to.ip.c_str(), keep_to.port,
                    my_id);
    }
    uint16_t keep_seq = 0;

    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    auto next_report = t_start + std::chrono::seconds(1);
    auto next_keep = t_start;
    while (!audio::app::g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
        if (keep_sock && now >= next_keep) {
            const auto k = keepalive(static_cast<uint8_t>(my_id), keep_seq++);
            keep_sock->send_to(keep_to.ip, keep_to.port, k.data(), k.size());
            next_keep = now + kKeepaliveEvery;
        }
        if (now < next_report) continue;
        next_report += std::chrono::seconds(1);
        std::printf("t=%4.0fs  %s\n", std::chrono::duration<double>(now - t_start).count(),
                    rx.stats().c_str());
        std::fflush(stdout);
    }

    rx.stop();
    std::printf("\nstopped. %s\nlog: %s\n", rx.stats().c_str(), log_path.c_str());
    return 0;
}
