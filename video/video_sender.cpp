// Captures webcam frames, JPEG-encodes them strip by strip and sends them over UDP.
//
//   video_sender <dest_ip> [--port 5004] [--fps 15] [--quality 70] [--strips 10]
//                          [--width 640] [--height 480] [--camera 0] [--log logs/video_tx.csv]
//
// Two threads: one reads the camera as fast as the camera allows, the main thread
// grabs the newest frame every 1/fps seconds, encodes and sends it. That way a slow
// encode never builds up a queue of old frames - we just skip to the latest one.
//
// --strips 1 sends the whole frame as one JPEG (the original behaviour, useful to compare).
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include <opencv2/opencv.hpp>

#include "metrics.hpp"
#include "packetizer.hpp"
#include "strip.hpp"
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
    int fps = 15, quality = 70, width = 640, height = 480, camera = 0, strips = 10;
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
        else if (a == "--strips")
            strips = std::stoi(v);
        else if (a == "--log")
            log_path = v;
    }
    // strip index and count are one byte each on the wire
    if (strips < 1 || strips > 255 || strips > height) {
        std::fprintf(stderr, "--strips must be between 1 and min(255, height)\n");
        return 1;
    }

    std::signal(SIGINT, on_sigint);
    metrics::init(log_path);

    // go straight to V4L2. the default on Ubuntu is GStreamer, which adds its own
    // buffering (more delay) and prints that "cannot query video position" warning
    cv::VideoCapture cap(camera, cv::CAP_V4L2);
    if (!cap.isOpened()) cap.open(camera);
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
    std::vector<uint8_t> payload;

    auto frame_gap = std::chrono::microseconds(1000000 / fps);
    auto next = std::chrono::steady_clock::now();
    auto report_at = next + std::chrono::seconds(1);
    int sent_frames = 0, skipped = 0;
    size_t sent_bytes = 0, sent_pkts = 0;

    std::printf("sending %dx%d @ %d fps, quality %d, %d strips -> %s:%u\n", width, height, fps,
                quality, strips, dest.c_str(), port);

    while (!g_stop) {
        std::this_thread::sleep_until(next);
        next += frame_gap;

        cv::Mat frame;
        uint32_t ts;
        if (!latest.take(frame, ts)) continue;

        if (frame.cols != width || frame.rows != height)  // some cameras ignore the size hint
            cv::resize(frame, frame, cv::Size(width, height));
        // every strip gets the same capture timestamp, that's how the receiver
        // knows which strips belong to the same moment
        for (int s = 0; s < strips; ++s) {
            int y0 = s * height / strips;
            int y1 = (s + 1) * height / strips;
            cv::Mat part = frame(cv::Rect(0, y0, width, y1 - y0));  // a view, no copy
            cv::imencode(".jpg", part, jpeg, jpeg_params);

            video::StripInfo info{static_cast<uint8_t>(s), static_cast<uint8_t>(strips),
                                  static_cast<uint16_t>(y0), static_cast<uint16_t>(height)};
            payload.resize(video::kStripPrefix + jpeg.size());
            video::write_strip(info, payload.data());
            std::memcpy(payload.data() + video::kStripPrefix, jpeg.data(), jpeg.size());

            auto dgrams = pk.packetize(payload.data(), payload.size(), ts);
            if (dgrams.empty()) {  // only if one strip is over ~350 KB
                skipped++;
                continue;
            }
            for (auto& d : dgrams) {
                sock.send_to(dest, port, d.data(), d.size());
                proto::Header h;
                proto::read_header(d.data(), d.size(), h);
                metrics::packet('S', h, d.size());
            }
            sent_bytes += payload.size();
            sent_pkts += dgrams.size();
        }
        sent_frames++;

        if (std::chrono::steady_clock::now() >= report_at) {
            std::printf("fps %d | avg frame %zu B | %.2f Mbps | %.1f pkts/frame | skipped %d\n",
                        sent_frames, sent_frames ? sent_bytes / sent_frames : 0,
                        sent_bytes * 8 / 1e6, sent_frames ? double(sent_pkts) / sent_frames : 0.0,
                        skipped);
            sent_frames = 0;
            sent_bytes = 0;
            sent_pkts = 0;
            report_at += std::chrono::seconds(1);
        }
    }

    grabber.join();
    return 0;
}