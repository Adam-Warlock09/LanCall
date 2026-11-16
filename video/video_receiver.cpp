// video_receiver — UDP -> strips -> window as a standalone program (Person 1, video/).
// A thin wrapper around video::VideoReceiver, kept for experiments without the call app.
// Shows frames as soon as they're ready (no lip sync here, that's the app's job).
//
//   video_receiver [--port 5004] [--from IP] [--log PATH]
//
// --from IP ignores everyone else. Default log: logs/video_rx_<date>_<time>.csv
// Press q in the window or Ctrl+C to quit.
#include <chrono>
#include <cstdio>
#include <exception>
#include <string>

#include <opencv2/highgui.hpp>

#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "video/app_util.hpp"
#include "video/video_pipeline.hpp"

static int usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [--port 5004] [--from IP] [--log PATH]\n", prog);
    return 2;
}

int main(int argc, char** argv) {
    uint16_t port = proto::kVideoPort;
    std::string from;
    std::string log_path = video::app::default_log("video_rx");

    try {
        for (int i = 1; i < argc; i += 2) {
            std::string a = argv[i];
            if (i + 1 >= argc) return usage(argv[0]);
            std::string v = argv[i + 1];
            if (a == "--port")
                port = static_cast<uint16_t>(std::stoi(v));
            else if (a == "--from")
                from = v;
            else if (a == "--log")
                log_path = v;
            else
                return usage(argv[0]);
        }
    } catch (const std::exception&) {
        return usage(argv[0]);
    }

    video::app::install_stop_handlers();
    metrics::init(log_path);

    video::VideoReceiver rx;
    if (!rx.start(port, from)) {
        std::fprintf(stderr, "video_receiver: %s\n", rx.error().c_str());
        return 1;
    }
    std::printf("listening for video on port %u%s  (log %s)\n", port,
                from.empty() ? "" : (" from " + from).c_str(), log_path.c_str());

    auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int shown = 0;
    video::VideoFrame f, newest;

    while (!video::app::g_stop) {
        // if several frames piled up, only the newest is worth drawing
        bool got = false;
        while (rx.pop(f)) {
            newest = std::move(f);
            got = true;
        }
        if (got) {
            cv::imshow("lancall video", newest.image);
            metrics::event("frame_shown", newest.capture_us);
            shown++;
        }
        // waitKey is what actually paints the window, and it doubles as our ~10 ms sleep
        if ((cv::waitKey(10) & 0xFF) == 'q') break;

        if (std::chrono::steady_clock::now() >= report_at) {
            std::printf("%s  | shown %d fps\n", rx.stats().c_str(), shown);
            std::fflush(stdout);
            shown = 0;
            report_at += std::chrono::seconds(1);
        }
    }
    rx.stop();
    return 0;
}