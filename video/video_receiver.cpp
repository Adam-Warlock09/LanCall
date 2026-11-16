// Receives video packets, rebuilds strips, pastes them onto a canvas and shows it.
//
//   video_receiver [--port 5004] [--timeout-ms 100] [--log logs/video_rx.csv]
//
// If a strip is lost, the canvas keeps that strip's previous pixels, so the picture
// keeps moving instead of freezing. Works with any --strips value on the sender.
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
#include "strip.hpp"
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

    // each strip is one "frame" as far as the reassembler is concerned
    net::Reassembler reasm(static_cast<uint32_t>(timeout_ms) * 1000);
    std::vector<uint8_t> buf(2048);
    std::string from_ip;
    uint16_t from_port = 0;
    net::Frame strip;

    cv::Mat canvas;
    uint32_t canvas_ts = 0;  // capture time of the strips currently being collected
    bool dirty = false;      // canvas changed since we last showed it

    auto report_at = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    int shown = 0, strips_ok = 0, bad = 0;
    size_t bytes = 0;
    net::Reassembler::Stats last{};

    auto show = [&] {
        cv::imshow("lancall video", canvas);
        shown++;
        dirty = false;
        metrics::event("frame_shown", canvas_ts);
    };

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
                               proto::now_us(), strip)) {
                    video::StripInfo info;
                    if (!video::read_strip(strip.data.data(), strip.data.size(), info)) {
                        bad++;
                    } else {
                        // first strip of a newer capture: whatever we had is as complete as it
                        // will get (its last strip may have been lost), so show it now
                        if (dirty && strip.timestamp_us != canvas_ts) show();
                        canvas_ts = strip.timestamp_us;

                        cv::Mat jpeg(1, static_cast<int>(strip.data.size() - video::kStripPrefix),
                                     CV_8UC1, strip.data.data() + video::kStripPrefix);
                        cv::Mat part = cv::imdecode(jpeg, cv::IMREAD_COLOR);

                        if (part.empty()) {
                            bad++;
                        } else {
                            // (re)make the canvas on the first strip or if the size changed
                            if (canvas.rows != info.frame_height || canvas.cols != part.cols)
                                canvas = cv::Mat::zeros(info.frame_height, part.cols, CV_8UC3);

                            if (info.y0 + part.rows <= canvas.rows) {
                                part.copyTo(canvas(cv::Rect(0, info.y0, part.cols, part.rows)));
                                strips_ok++;
                                dirty = true;
                            } else {
                                bad++;
                            }
                        }
                        // last strip of the frame arrived, no need to wait for the next frame
                        if (dirty && info.index == info.count - 1) show();
                    }
                }
            }
        }

        reasm.expire(proto::now_us());
        // waitKey is what actually paints the window, so it has to run every loop
        if ((cv::waitKey(1) & 0xFF) == 'q') break;

        if (std::chrono::steady_clock::now() >= report_at) {
            auto& s = reasm.stats();
            auto lost = s.dropped_incomplete - last.dropped_incomplete;
            std::printf("fps %d | %.2f Mbps | strips ok %d, lost %llu | late %llu | bad %d\n",
                        shown, bytes * 8 / 1e6, strips_ok, (unsigned long long)lost,
                        (unsigned long long)(s.late_pieces - last.late_pieces), bad);
            last = s;
            shown = 0;
            strips_ok = 0;
            bytes = 0;
            bad = 0;
            report_at += std::chrono::seconds(1);
        }
    }
    return 0;
}