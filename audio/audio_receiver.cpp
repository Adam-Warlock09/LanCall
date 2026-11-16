// audio_receiver — UDP -> playout buffer -> speaker (Person 2, audio/). A thin wrapper around
// audio::AudioReceiver (audio_pipeline.hpp), kept for single-stream experiments.
//
// Listens on proto::kAudioPort for 652-byte audio packets (see audio_sender), logs each as an
// R row, measures loss and jitter, and plays through a playout buffer (default 60 ms). Every
// underrun, concealed chunk, late or duplicate packet, trim and resync is logged as an event.
// Prints one stats line a second and a whole-run summary at exit.
//
// Usage: audio_receiver [--from IP] [--out N] [--buffer-ms MS] [--log PATH] [--list]
//   --from IP        accept audio only from this address (default: the first sender heard)
//   --out N          PortAudio output device (default: system default; see --list)
//   --buffer-ms MS   playout buffer before playback starts, 20-600 (default 60)
//   --log PATH       CSV log file (default logs/audio_receiver_<date>_<time>.csv)
//   --list           list audio devices and exit

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "audio/app_util.hpp"
#include "audio/audio_pipeline.hpp"
#include "audio/pa_audio.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [--from IP] [--out N] [--buffer-ms MS] [--log PATH] [--list]\n",
                 prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_path, from_ip;
    int out_dev = -1, buffer_ms = 60;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--from" && i + 1 < argc) {
            from_ip = argv[++i];
            if (!audio::app::is_ipv4(from_ip)) return usage(argv[0]), 2;
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
    if (log_path.empty()) log_path = audio::app::default_log_path("audio_receiver");

    audio::app::install_stop_handlers();
    metrics::init(log_path);

    audio::ReceiverOptions opt;
    opt.out_device = out_dev;
    opt.buffer_ms = buffer_ms;
    audio::AudioReceiver rx;
    if (!rx.start(proto::kAudioPort, from_ip, opt)) {
        std::fprintf(stderr, "%s\n", rx.error().c_str());
        return 1;
    }
    std::printf("audio_receiver on UDP port %u%s%s  log %s  (Ctrl+C to stop)\n", proto::kAudioPort,
                from_ip.empty() ? "" : " from ", from_ip.c_str(), log_path.c_str());

    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    auto next_report = t_start + std::chrono::seconds(1);
    while (!audio::app::g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
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
