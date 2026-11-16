// Captures webcam frames, JPEG-encodes them and sends them over UDP.
//
//   video_sender <dest_ip> [--port 5004] [--fps 15] [--quality 70]
//                          [--width 640] [--height 480] [--camera 0] [--log logs/video_tx.csv]
//
// Two threads: one reads the camera as fast as the camera allows, the main thread
// grabs the newest frame every 1/fps seconds, encodes and sends it. That way a slow
// encode never builds up a queue of old frames - we just skip to the latest one.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/opencv.hpp>

#include "metrics.hpp"
#include "packetizer.hpp"
#include "udp_socket.hpp"

static std::atomic<bool> g_stop{false};
static void on_sigint(int) {
    g_stop = true;
}

// holds only the most recent camera frame
class LatestFrame {
public:
    void put(cv::Mat&& m, uint32_t ts) {
        std::lock_guard<std::mutex> lock(mu_);
        frame_ = std::move(m);
        ts_ = ts;
        fresh_ = true;
        cv_.notify_one();
    }
    // waits up to 100 ms for a frame we haven't taken yet
    bool take(cv::Mat& out, uint32_t& ts) {
        std::unique_lock<std::mutex> lock(mu_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(100), [&] { return fresh_; }))
            return false;
        out = frame_;  // cv::Mat copy is cheap, it shares the pixel buffer
        ts = ts_;
        fresh_ = false;
        return true;
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    cv::Mat frame_;
    uint32_t ts_ = 0;
    bool fresh_ = false;
};

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <dest_ip> [--fps 15] [--quality 70] ...\n", argv[0]);
        return 1;
    }
    std::string dest = argv[1];
    uint16_t port = proto::kVideoPort;
    int fps = 15, quality = 70, width = 640, height = 480, camera = 0;
    std::string log_path = "logs/video_tx.csv";
    for (int i = 2; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        std::string v = argv[i + 1];
        if (a == "--port")
            port = static_cast<uint16_t>(std::stoi(v));
        else if (a == "--fps")
            fps = std::stoi(v);
        else if (a == "--quality")
            quality = std::stoi(v);
        else if (a == "--width")
            width = std::stoi(v);
        else if (a == "--height")
            height = std::stoi(v);
        else if (a == "--camera")
            camera = std::stoi(v);
        else if (a == "--log")
            log_path = v;
    }

    std::signal(SIGINT, on_sigint);
    metrics::init(log_path);

    cv::VideoCapture cap(camera);
    if (!cap.isOpened()) {
        std::fprintf(stderr, "can't open camera %d\n", camera);
        return 1;
    }
    cap.set(cv::CAP_PROP_FRAME_WIDTH, width);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, height);

    LatestFrame latest;
    std::thread grabber([&] {
        cv::Mat m;
        while (!g_stop) {
            if (!cap.read(m) || m.empty()) continue;
            // timestamp at capture, not at send, so later we can measure true end-to-end delay
            latest.put(std::move(m), proto::now_us());
        }
    });

    UdpSocket sock(0);
    net::Packetizer pk(proto::Type::Video, /*stream_id=*/0);
    std::vector<int> jpeg_params{cv::IMWRITE_JPEG_QUALITY, quality};
    std::vector<uchar> jpeg;

    auto frame_gap = std::chrono::microseconds(1000000 / fps);
    auto next = std::chrono::steady_clock::now();
    auto report_at = next + std::chrono::seconds(1);
    int sent_frames = 0, skipped = 0;
    size_t sent_bytes = 0;

    std::printf("sending %dx%d @ %d fps, quality %d -> %s:%u\n", width, height, fps, quality,
                dest.c_str(), port);

    while (!g_stop) {
        std::this_thread::sleep_until(next);
        next += frame_gap;

        cv::Mat frame;
        uint32_t ts;
        if (!latest.take(frame, ts)) continue;

        if (frame.cols != width || frame.rows != height)  // some cameras ignore the size hint
            cv::resize(frame, frame, cv::Size(width, height));
        cv::imencode(".jpg", frame, jpeg, jpeg_params);

        auto dgrams = pk.packetize(jpeg.data(), jpeg.size(), ts);
        if (dgrams.empty()) {  // only if a frame is over ~350 KB
            skipped++;
            continue;
        }
        for (auto& d : dgrams) {
            sock.send_to(dest, port, d.data(), d.size());
            proto::Header h;
            proto::read_header(d.data(), d.size(), h);
            metrics::packet('S', h, d.size());
        }
        sent_frames++;
        sent_bytes += jpeg.size();

        if (std::chrono::steady_clock::now() >= report_at) {
            std::printf("fps %d | avg frame %zu B | %.2f Mbps | %zu pkts/frame | skipped %d\n",
                        sent_frames, sent_frames ? sent_bytes / sent_frames : 0,
                        sent_bytes * 8 / 1e6, dgrams.size(), skipped);
            sent_frames = 0;
            sent_bytes = 0;
            report_at += std::chrono::seconds(1);
        }
    }

    grabber.join();
    return 0;
}
