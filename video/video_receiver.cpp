// Receives video packets, rebuilds frames, decodes and shows them.
//
//   video_receiver [--port 5004] [--timeout-ms 100] [--log logs/video_rx.csv]
//
// Press q in the video window (or ctrl+c) to quit. Prints one stats line per second.
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "metrics.hpp"
#include "reassembler.hpp"
#include "udp_socket.hpp"

static std::atomic<bool> g_stop{false};
static void on_sigint(int) {
    g_stop = true;
}

int main(int argc, char** argv) {
    uint16_t port = proto::kVideoPort;
    int timeout_ms = 100;
    std::string log_path = "logs/video_rx.csv";
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        std::string v = argv[i + 1];
        if (a == "--port")
            port = static_cast<uint16_t>(std::stoi(v));
        else if (a == "--timeout-ms")
            timeout_ms = std::stoi(v);
        else if (a == "--log")
            log_path = v;
    }

    std::signal(SIGINT, on_sigint);
    metrics::init(log_path);

    UdpSocket sock(port);
    // a 480p frame is ~20-40 packets arriving in one burst. the default buffer (~200 KB)
    // fills up while we're busy drawing. linux caps this at net.core.rmem_max, so also run:
    //   sudo sysctl -w net.core.rmem_max=8388608
    sock.set_recv_buffer(4 << 20);

    net::Reassembler reasm(static_cast<uint32_t>(timeout_ms) * 1000);
    std::vector<uint8_t> buf(2048);
    std::string from_ip;
    uint16_t from_port = 0;
    net::Frame frame;

    auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int shown = 0, bad_jpeg = 0;
    size_t bytes = 0;
    net::Reassembler::Stats last{};

    std::printf("listening for video on port %u\n", port);

    while (!g_stop) {
        // short timeout so stats and expiry keep running even when nothing arrives
        ssize_t n = sock.recv_from(buf.data(), buf.size(), from_ip, from_port, 20);

        if (n > 0) {
            proto::Header h;
            if (proto::read_header(buf.data(), n, h) && h.type == proto::Type::Video) {
                metrics::packet('R', h, n);
                bytes += n;

                if (reasm.push(h, buf.data() + proto::kHeaderSize, n - proto::kHeaderSize,
                               proto::now_us(), frame)) {
                    cv::Mat img = cv::imdecode(frame.data, cv::IMREAD_COLOR);
                    if (img.empty()) {
                        bad_jpeg++;
                    } else {
                        cv::imshow("lancall video", img);
                        shown++;
                        metrics::event("frame_shown", frame.frame_id);
                    }
                }
            }
        }

        reasm.expire(proto::now_us());
        // waitKey is what actually paints the window, so it has to run every loop
        if ((cv::waitKey(1) & 0xFF) == 'q') break;

        if (std::chrono::steady_clock::now() >= report_at) {
            auto& s = reasm.stats();
            std::printf("fps %d | %.2f Mbps | dropped frames %llu | late %llu | bad jpeg %d\n",
                        shown, bytes * 8 / 1e6,
                        (unsigned long long)(s.dropped_incomplete - last.dropped_incomplete),
                        (unsigned long long)(s.late_pieces - last.late_pieces), bad_jpeg);
            last = s;
            shown = 0;
            bytes = 0;
            bad_jpeg = 0;
            report_at += std::chrono::seconds(1);
        }
    }
    return 0;
}
