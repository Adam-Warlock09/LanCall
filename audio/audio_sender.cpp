// audio_sender — microphone -> UDP (Person 2, audio/). A thin wrapper around
// audio::AudioSender (audio_pipeline.hpp), kept for single-stream experiments.
//
// Every 20 ms chunk becomes one 652-byte packet (12-byte header + 320 int16 samples in network
// byte order) sent to <dest_ip>:proto::kAudioPort (or the port given), stamped with
// proto::now_us() at capture.
// Every packet sent is logged as an S row; Ctrl+C exits cleanly so the log is complete.
//
// Usage: audio_sender <dest_ip[:port]> [--also IP[:PORT]]... [--in N] [--stream ID] [--log PATH]
//                    [--list]
//   dest_ip[:port]    where to send (port default 5006). Through a relay: <relay_ip>:5014 with
//                     --stream set to a member id (phase 7, experiment E10)
//   --also IP[:PORT]  also send every chunk there (repeatable; port default 5006). Phase 4:
//                     the mesh fan-out of a group call without the app, e.g. to two laptops
//   --in N            PortAudio input device (default: system default; see --list)
//   --stream ID       stream_id in the header, 0-255 (default 0); a member id in a group call
//   --log PATH        CSV log file (default logs/audio_sender_<date>_<time>.csv)
//   --list            list audio devices and exit

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "audio/app_util.hpp"
#include "audio/audio_pipeline.hpp"
#include "audio/pa_audio.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <dest_ip[:port]> [--also IP[:PORT]]... [--in N] [--stream ID] "
                 "[--log PATH] [--list]\n",
                 prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::string log_path;
    proto::Endpoint dest;  // ip empty until given
    std::vector<proto::Endpoint> also;
    int in_dev = -1, stream_id = 0;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if (a == "--also" && i + 1 < argc) {
            proto::Endpoint e;
            if (!audio::app::parse_endpoint(argv[++i], proto::kAudioPort, e))
                return usage(argv[0]), 2;
            also.push_back(e);
        } else if (a == "--in" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 0, 1000, in_dev)) return usage(argv[0]), 2;
        } else if (a == "--stream" && i + 1 < argc) {
            if (!audio::app::parse_int(argv[++i], 0, 255, stream_id)) return usage(argv[0]), 2;
        } else if (a == "--log" && i + 1 < argc) {
            log_path = argv[++i];
        } else if (dest.ip.empty() && a[0] != '-') {
            if (!audio::app::parse_endpoint(a, proto::kAudioPort, dest)) return usage(argv[0]), 2;
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
    if (dest.ip.empty()) {
        usage(argv[0]);
        return 2;
    }
    if (log_path.empty()) log_path = audio::app::default_log_path("audio_sender");

    audio::app::install_stop_handlers();
    metrics::init(log_path);

    audio::SenderOptions opt;
    opt.in_device = in_dev;
    opt.stream_id = static_cast<uint8_t>(stream_id);
    audio::AudioSender tx;
    if (!tx.start(dest.ip, dest.port, opt)) {
        std::fprintf(stderr, "%s\n", tx.error().c_str());
        return 1;
    }
    std::printf("audio_sender -> %s:%u  stream %d  log %s  (Ctrl+C to stop)\n", dest.ip.c_str(),
                dest.port, stream_id, log_path.c_str());
    if (!also.empty()) {
        std::vector<proto::Endpoint> all = {dest};
        all.insert(all.end(), also.begin(), also.end());
        tx.set_destinations(all);
        for (const proto::Endpoint& e : also)
            std::printf("              also -> %s:%u\n", e.ip.c_str(), e.port);
    }

    using Clock = std::chrono::steady_clock;
    const auto t_start = Clock::now();
    auto next_report = t_start + std::chrono::seconds(1);
    while (!audio::app::g_stop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
        if (now < next_report) continue;
        next_report += std::chrono::seconds(1);
        std::printf("t=%4.0fs  %s\n", std::chrono::duration<double>(now - t_start).count(),
                    tx.stats().c_str());
        std::fflush(stdout);
    }

    tx.stop();
    std::printf("\nstopped. %s\nlog: %s\n", tx.stats().c_str(), log_path.c_str());
    return 0;
}
