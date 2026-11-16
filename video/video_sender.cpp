// video_sender — webcam -> strips -> UDP as a standalone program (Person 1, video/).
// A thin wrapper around video::VideoSender, kept for experiments without the call app.
//
//   video_sender <dest_ip> [--port 5004] [--fps 15] [--quality 70] [--strips 10]
//                          [--width 640] [--height 480] [--camera 0] [--log PATH]
//
// --strips 1 sends the whole frame as one JPEG. Default log: logs/video_tx_<date>_<time>.csv
// Stop with Ctrl+C (Ctrl+Z only pauses it and keeps the camera busy).
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>
#include <thread>

#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "video/app_util.hpp"
#include "video/video_pipeline.hpp"

static int usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s <dest_ip> [--port 5004] [--fps 15] [--quality 70] [--strips 10]\n"
                 "          [--width 640] [--height 480] [--camera 0] [--log PATH]\n",
                 prog);
    return 2;
}

int main(int argc, char** argv) {
    if (argc < 2) return usage(argv[0]);
    std::string dest = argv[1];
    uint16_t port = proto::kVideoPort;
    video::SenderOptions opt;
    std::string log_path = video::app::default_log("video_tx");

    try {
        for (int i = 2; i < argc; i += 2) {
            std::string a = argv[i];
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
            else if (a == "--log")
                log_path = v;
            else
                return usage(argv[0]);
        }
    } catch (const std::exception&) {  // stoi on something that isn't a number
        return usage(argv[0]);
    }

    video::app::install_stop_handlers();
    metrics::init(log_path);

    video::VideoSender tx;
    if (!tx.start(dest, port, opt)) {
        std::fprintf(stderr, "video_sender: %s\n", tx.error().c_str());
        return 1;
    }
    std::printf("sending %dx%d @ %d fps, quality %d, %d strips -> %s:%u  (log %s)\n", opt.width,
                opt.height, opt.fps, opt.quality, opt.strips, dest.c_str(), port, log_path.c_str());

    while (!video::app::g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::printf("%s\n", tx.stats().c_str());
        std::fflush(stdout);
    }
    tx.stop();
    return 0;
}