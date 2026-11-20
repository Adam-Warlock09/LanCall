// video_sender — webcam -> strips -> UDP as a standalone program (Person 1, video/).
// A thin wrapper around video::VideoSender, kept for experiments without the call app.
//
//   video_sender <dest_ip[:port],...> [--port 5004] [--fps 15] [--quality 70] [--strips 10]
//                [--width 640] [--height 480] [--camera 0] [--id 0] [--fixed] [--no-nack]
//                [--fec] [--log PATH]
//
// Several destinations (phase 4, mesh by hand): 10.8.66.20,10.8.64.7 sends every strip to
// both. A destination's own :port overrides --port, e.g. 127.0.0.1:26004. --id sets the
// stream id (= member id in a group call), which the relay and multi-stream receivers need;
// through a relay: video_sender <relay_ip>:5014 --id 2.
// --fixed turns the quality ladder off (fps and quality stay as given, for experiment E4).
// --no-nack ignores receivers' NACKs, so nothing lost is sent again (phase 5, experiment E8).
// --fec sends one XOR parity packet after every strip (phase 5), which rebuilds any one lost
// packet of that strip without asking.
// --strips 1 sends the whole frame as one JPEG. Default log: logs/video_tx_<date>_<time>.csv
// Stop with Ctrl+C (Ctrl+Z only pauses it and keeps the camera busy).
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "video/app_util.hpp"
#include "video/video_pipeline.hpp"

static int usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <dest_ip[:port],...> [--port 5004] [--fps 15] [--quality 70]\n"
                 "          [--strips 10] [--width 640] [--height 480] [--camera 0] [--id 0]\n"
                 "          [--fixed] [--no-nack] [--fec] [--log PATH]\n",
                 prog);
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage(argv[0]);
    std::string dest_arg = argv[1];
    uint16_t port = proto::kVideoPort;
    video::SenderOptions opt;
    std::string log_path = video::app::default_log("video_tx");

    try {
        for (int i = 2; i < argc; i += 2) {
            std::string a = argv[i];
            if (a == "--fixed" || a == "--no-nack" ||
                a == "--fec") {  // the options without a value
                if (a == "--fixed") opt.adaptive = false;
                if (a == "--no-nack") opt.nack = false;
                if (a == "--fec") opt.fec = true;
                i -= 1;
                continue;
            }
            if (i + 1 >= argc) return usage(argv[0]);
            std::string v = argv[i + 1];
            if (a == "--port")
                port = static_cast<uint16_t>(std::stoi(v));
            else if (a == "--fps")
                opt.fps = std::stoi(v);
            else if (a == "--quality")
                opt.quality = std::stoi(v);
            else if (a == "--strips")
                opt.strips = std::stoi(v);
            else if (a == "--width")
                opt.width = std::stoi(v);
            else if (a == "--height")
                opt.height = std::stoi(v);
            else if (a == "--camera")
                opt.camera = std::stoi(v);
            else if (a == "--id")
                opt.stream_id = static_cast<uint8_t>(std::stoi(v));
            else if (a == "--log")
                log_path = v;
            else
                return usage(argv[0]);
        }
    } catch (const std::exception&) {  // stoi on something that isn't a number
        return usage(argv[0]);
    }

    // ip or ip:port, comma separated
    std::vector<proto::Endpoint> dests;
    try {
        for (const auto& d : video::app::split(dest_arg)) {
            auto colon = d.find(':');
            if (colon == std::string::npos)
                dests.push_back({d, port});
            else
                dests.push_back(
                    {d.substr(0, colon), static_cast<uint16_t>(std::stoi(d.substr(colon + 1)))});
        }
    } catch (const std::exception&) {
        return usage(argv[0]);
    }
    if (dests.empty()) return usage(argv[0]);

    video::app::install_stop_handlers();
    metrics::init(log_path);

    video::VideoSender tx;
    if (!tx.start(dests[0].ip, dests[0].port, opt)) {
        std::fprintf(stderr, "video_sender: %s\n", tx.error().c_str());
        return 1;
    }
    if (dests.size() > 1) tx.set_destinations(dests);
    std::printf("sending %dx%d @ %d fps, quality %d, %d strips, stream %u ->", opt.width,
                opt.height, opt.fps, opt.quality, opt.strips, opt.stream_id);
    for (const auto& d : dests) std::printf(" %s:%u", d.ip.c_str(), d.port);
    std::printf("  (log %s)\n", log_path.c_str());

    while (!video::app::g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("%s\n", tx.stats().c_str());
        std::fflush(stdout);
    }
    tx.stop();
    return 0;
}