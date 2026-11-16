// Video send and receive pipelines (Person 1, video/).
// This is the logic that used to live in video_sender.cpp and video_receiver.cpp, moved
// into objects the lancall app can start and stop. The two programs are now thin wrappers.
#include "video/video_pipeline.hpp"

#include <arpa/inet.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/udp_socket.hpp"
#include "net/packetizer.hpp"
#include "net/reassembler.hpp"
#include "video/strip.hpp"

namespace video {

namespace {

using Clock = std::chrono::steady_clock;

bool is_ipv4(const std::string& s) {
    in_addr a{};
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

}  // namespace

// ---------------------------------------------------------------------------- sender

struct VideoSender::Impl {
    SenderOptions opt;
    std::string dest;
    uint16_t port = 0;

    cv::VideoCapture cap;
    std::unique_ptr<UdpSocket> sock;
    std::thread grabber;
    std::thread sender;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    // newest camera frame. the grabber replaces it, the sender and self_view() read it
    mutable std::mutex frame_mu;
    std::condition_variable frame_cv;
    cv::Mat latest;
    uint32_t latest_ts = 0;
    bool fresh = false;  // sender hasn't taken this one yet

    mutable std::mutex info_mu;
    std::string stats = "tx idle";
    std::string error;

    void set_error(const std::string& e) {
        std::lock_guard<std::mutex> lock(info_mu);
        error = e;
    }

    void grab_loop() {
        cv::Mat m;
        while (!stop) {
            if (!cap.read(m) || m.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            // timestamp at capture, not at send, so later we can measure true end-to-end delay
            uint32_t ts = proto::now_us();
            std::lock_guard<std::mutex> lock(frame_mu);
            // moving leaves m empty, so the next read() gets a fresh buffer and never writes
            // into pixels the sender may still be encoding
            latest = std::move(m);
            latest_ts = ts;
            fresh = true;
            frame_cv.notify_one();
        }
    }

    // waits up to 100 ms for a frame the sender hasn't taken yet
    bool take(cv::Mat& out, uint32_t& ts) {
        std::unique_lock<std::mutex> lock(frame_mu);
        if (!frame_cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return fresh; }))
            return false;
        out = latest;  // shares the pixels, which is fine: the grabber never touches them again
        ts = latest_ts;
        fresh = false;
        return true;
    }

    void send_loop() {
        net::Packetizer pk(proto::Type::Video, opt.stream_id);
        std::vector<int> jpeg_params{cv::IMWRITE_JPEG_QUALITY, opt.quality};
        std::vector<uchar> jpeg;
        std::vector<uint8_t> payload;

        auto gap = std::chrono::microseconds(1000000 / opt.fps);
        auto next = Clock::now();
        auto report_at = next + std::chrono::seconds(1);
        int frames = 0;
        size_t bytes = 0, pkts = 0;

        while (!stop) {
            std::this_thread::sleep_until(next);
            next += gap;
            // if we fell far behind (laptop suspended, debugger), don't try to catch up
            if (Clock::now() - next > std::chrono::seconds(1)) next = Clock::now() + gap;

            cv::Mat frame;
            uint32_t ts;
            if (take(frame, ts)) {
                if (frame.cols != opt.width || frame.rows != opt.height)
                    cv::resize(frame, frame, cv::Size(opt.width, opt.height));

                // every strip gets the same capture timestamp, that's how the receiver
                // knows which strips belong to the same moment
                for (int s = 0; s < opt.strips; ++s) {
                    int y0 = s * opt.height / opt.strips;
                    int y1 = (s + 1) * opt.height / opt.strips;
                    cv::imencode(".jpg", frame(cv::Rect(0, y0, opt.width, y1 - y0)), jpeg,
                                 jpeg_params);

                    StripInfo info{static_cast<uint8_t>(s), static_cast<uint8_t>(opt.strips),
                                   static_cast<uint16_t>(y0), static_cast<uint16_t>(opt.height)};
                    payload.resize(kStripPrefix + jpeg.size());
                    write_strip(info, payload.data());
                    std::memcpy(payload.data() + kStripPrefix, jpeg.data(), jpeg.size());

                    for (auto& d : pk.packetize(payload.data(), payload.size(), ts)) {
                        sock->send_to(dest, port, d.data(), d.size());
                        proto::Header h;
                        proto::read_header(d.data(), d.size(), h);
                        metrics::packet('S', h, d.size());
                        pkts++;
                    }
                    bytes += payload.size();
                }
                frames++;
            }

            if (Clock::now() >= report_at) {
                char line[96];
                std::snprintf(line, sizeof(line), "tx %d fps  %.1f Mbps  %.0f pkt/frame", frames,
                              bytes * 8 / 1e6, frames ? double(pkts) / frames : 0.0);
                {
                    std::lock_guard<std::mutex> lock(info_mu);
                    stats = line;
                }
                frames = 0;
                bytes = pkts = 0;
                report_at += std::chrono::seconds(1);
            }
        }
    }
};

VideoSender::VideoSender() : impl_(std::make_unique<Impl>()) {}
VideoSender::~VideoSender() {
    stop();
}

bool VideoSender::start(const std::string& dest_ip, uint16_t port, const SenderOptions& opt) {
    if (running()) {
        impl_->set_error("already running");
        return false;
    }
    // a fresh Impl each time, so nothing from a previous call leaks into this one
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;

    if (!is_ipv4(dest_ip)) {
        d.set_error("not an IPv4 address: " + dest_ip);
        return false;
    }
    // strip index and count are one byte each on the wire
    if (opt.strips < 1 || opt.strips > 255 || opt.strips > opt.height || opt.fps < 1 ||
        opt.fps > 60 || opt.quality < 1 || opt.quality > 100 || opt.width < 16 || opt.height < 16) {
        d.set_error("bad options (strips 1-255, fps 1-60, quality 1-100)");
        return false;
    }
    d.opt = opt;
    d.dest = dest_ip;
    d.port = port;

    // go straight to V4L2. the default on Ubuntu is GStreamer, which adds its own
    // buffering (more delay) and prints a "cannot query video position" warning
    d.cap.open(opt.camera, cv::CAP_V4L2);
    if (!d.cap.isOpened()) d.cap.open(opt.camera);
    if (!d.cap.isOpened()) {
        d.set_error("can't open camera " + std::to_string(opt.camera));
        return false;
    }
    d.cap.set(cv::CAP_PROP_FRAME_WIDTH, opt.width);
    d.cap.set(cv::CAP_PROP_FRAME_HEIGHT, opt.height);

    try {
        d.sock = std::make_unique<UdpSocket>(0);
    } catch (const std::exception& e) {
        d.cap.release();
        d.set_error(e.what());
        return false;
    }

    d.running = true;
    d.grabber = std::thread([&d] { d.grab_loop(); });
    d.sender = std::thread([&d] { d.send_loop(); });
    return true;
}

void VideoSender::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.grabber.joinable()) d.grabber.join();
    if (d.sender.joinable()) d.sender.join();
    d.cap.release();  // turns the camera light off
    d.running = false;
    {
        // so self_view() doesn't keep showing the last frame of a finished call
        std::lock_guard<std::mutex> lock(d.frame_mu);
        d.latest = cv::Mat();
    }
    std::lock_guard<std::mutex> lock(d.info_mu);
    d.stats = "tx stopped";
}

bool VideoSender::running() const {
    return impl_->running;
}

bool VideoSender::self_view(cv::Mat& out) const {
    std::lock_guard<std::mutex> lock(impl_->frame_mu);
    if (impl_->latest.empty()) return false;
    out = impl_->latest.clone();
    return true;
}

std::string VideoSender::stats() const {
    std::lock_guard<std::mutex> lock(impl_->info_mu);
    return impl_->stats;
}

std::string VideoSender::error() const {
    std::lock_guard<std::mutex> lock(impl_->info_mu);
    return impl_->error;
}

// ---------------------------------------------------------------------------- receiver

struct VideoReceiver::Impl {
    std::string only_from;
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    mutable std::mutex queue_mu;
    std::deque<VideoFrame> queue;

    mutable std::mutex info_mu;
    std::string stats = "rx idle";
    std::string error;

    void push(const cv::Mat& canvas, uint32_t ts) {
        VideoFrame f;
        f.image = canvas.clone();  // the canvas keeps changing, the app needs its own copy
        f.capture_us = ts;
        std::lock_guard<std::mutex> lock(queue_mu);
        if (queue.size() >= kMaxQueued) queue.pop_front();
        queue.push_back(std::move(f));
    }

    void loop() {
        net::Reassembler reasm(100000);  // a strip has 100 ms to complete
        std::vector<uint8_t> buf(2048);
        std::string from_ip;
        uint16_t from_port = 0;
        net::Frame strip;

        cv::Mat canvas;
        uint32_t canvas_ts = 0;  // capture time of the strips being collected
        bool dirty = false;      // canvas changed since it was last queued

        int frames = 0, strips_ok = 0, bad = 0;
        uint64_t missing = 0;
        bool have_id = false;
        uint16_t last_id = 0;
        size_t bytes = 0;
        auto report_at = Clock::now() + std::chrono::seconds(1);

        auto finish = [&] {
            push(canvas, canvas_ts);
            frames++;
            dirty = false;
        };

        auto handle = [&](ssize_t n) {
            if (!only_from.empty() && from_ip != only_from) return;
            proto::Header h;
            if (!proto::read_header(buf.data(), n, h) || h.type != proto::Type::Video) return;
            metrics::packet('R', h, n);
            bytes += n;

            if (!reasm.push(h, buf.data() + proto::kHeaderSize, n - proto::kHeaderSize,
                            proto::now_us(), strip))
                return;

            // each strip has its own frame id, so a jump from 40 to 43 means 41 and 42 never
            // made it, including strips where every packet was lost. a big or negative
            // jump means the sender restarted, so skip it
            if (have_id) {
                int gap = static_cast<int16_t>(strip.frame_id - last_id) - 1;
                if (gap > 0 && gap < 1000) missing += gap;
            }
            have_id = true;
            last_id = strip.frame_id;

            StripInfo info;
            if (!read_strip(strip.data.data(), strip.data.size(), info)) {
                bad++;
                return;
            }

            // first strip of a newer capture: whatever we had is as complete as it will get
            if (dirty && strip.timestamp_us != canvas_ts) finish();
            canvas_ts = strip.timestamp_us;

            cv::Mat jpeg(1, static_cast<int>(strip.data.size() - kStripPrefix), CV_8UC1,
                         strip.data.data() + kStripPrefix);
            cv::Mat part = cv::imdecode(jpeg, cv::IMREAD_COLOR);
            if (part.empty()) {
                bad++;
                return;
            }
            if (canvas.rows != info.frame_height || canvas.cols != part.cols)
                canvas = cv::Mat::zeros(info.frame_height, part.cols, CV_8UC3);
            if (info.y0 + part.rows > canvas.rows) {
                bad++;
                return;
            }
            part.copyTo(canvas(cv::Rect(0, info.y0, part.cols, part.rows)));
            strips_ok++;
            dirty = true;

            // last strip arrived, no need to wait for the next frame
            if (info.index == info.count - 1) finish();
        };

        while (!stop) {
            ssize_t n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 20);
            if (n > 0) handle(n);
            // empty out whatever else is waiting. this thread never draws anything, but
            // decoding is still slow enough that one packet per wakeup would fall behind
            for (int i = 0; i < 512; ++i) {
                n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 0);
                if (n <= 0) break;
                handle(n);
            }
            reasm.expire(proto::now_us());

            if (Clock::now() >= report_at) {
                double total = strips_ok + static_cast<double>(missing);
                char line[96];
                std::snprintf(line, sizeof(line), "rx %d fps  %.1f Mbps  strips %.1f%% missing",
                              frames, bytes * 8 / 1e6, total > 0 ? 100.0 * missing / total : 0.0);
                {
                    std::lock_guard<std::mutex> lock(info_mu);
                    stats = line;
                    if (bad) error = std::to_string(bad) + " bad strips in the last second";
                }
                frames = strips_ok = bad = 0;
                missing = 0;
                bytes = 0;
                report_at += std::chrono::seconds(1);
            }
        }
    }
};

VideoReceiver::VideoReceiver() : impl_(std::make_unique<Impl>()) {}
VideoReceiver::~VideoReceiver() {
    stop();
}

bool VideoReceiver::start(uint16_t port, const std::string& only_from_ip) {
    if (running()) {
        std::lock_guard<std::mutex> lock(impl_->info_mu);
        impl_->error = "already running";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;

    if (!only_from_ip.empty() && !is_ipv4(only_from_ip)) {
        d.error = "not an IPv4 address: " + only_from_ip;
        return false;
    }
    d.only_from = only_from_ip;

    try {
        d.sock = std::make_unique<UdpSocket>(port);
    } catch (const std::exception& e) {
        d.error = e.what();
        return false;
    }
    // a frame is ~30 packets arriving in a burst. linux caps this at net.core.rmem_max,
    // so also run:  sudo sysctl -w net.core.rmem_max=8388608
    d.sock->set_recv_buffer(4 << 20);

    d.running = true;
    d.worker = std::thread([&d] { d.loop(); });
    return true;
}

void VideoReceiver::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.worker.joinable()) d.worker.join();
    d.running = false;
    std::lock_guard<std::mutex> lock(d.info_mu);
    d.stats = "rx stopped";
}

bool VideoReceiver::running() const {
    return impl_->running;
}

bool VideoReceiver::pop(VideoFrame& out) {
    std::lock_guard<std::mutex> lock(impl_->queue_mu);
    if (impl_->queue.empty()) return false;
    out = std::move(impl_->queue.front());
    impl_->queue.pop_front();
    return true;
}

size_t VideoReceiver::queued() const {
    std::lock_guard<std::mutex> lock(impl_->queue_mu);
    return impl_->queue.size();
}

std::string VideoReceiver::stats() const {
    std::lock_guard<std::mutex> lock(impl_->info_mu);
    return impl_->stats;
}

std::string VideoReceiver::error() const {
    std::lock_guard<std::mutex> lock(impl_->info_mu);
    return impl_->error;
}

}  // namespace video