// Video send and receive pipelines (Person 1, video/).
// This is the logic that used to live in video_sender.cpp and video_receiver.cpp, moved
// into objects the lancall app can start and stop. The two programs are now thin wrappers.
//
// Phase 3: the receiver sends a receiver report (common/report.hpp) every 500 ms to the
// address the video comes from. The sender reads them on a third thread and, with
// adaptive on, lets the quality ladder (quality_ladder.hpp) pick its fps and JPEG quality.
//
// Phase 4: the sender sends each strip to every destination (mesh: everyone else, relay:
// just the relay) and the ladder follows the worst receiver (report_merger.hpp). The
// receiver keeps a separate canvas, reassembler and report per stream_id.
#include "video/video_pipeline.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "common/udp_socket.hpp"
#include "net/loss_counter.hpp"
#include "net/packetizer.hpp"
#include "net/reassembler.hpp"
#include "video/quality_ladder.hpp"
#include "video/report_merger.hpp"
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

    // where every strip goes. set_destinations() replaces the list; the send thread copies
    // it once per frame and the report thread checks report addresses against it
    mutable std::mutex dest_mu;
    std::vector<proto::Endpoint> dests;

    cv::VideoCapture cap;
    std::unique_ptr<UdpSocket> sock;
    std::thread grabber;
    std::thread sender;
    std::thread reports;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    // what the receivers tell us. the report thread writes, the send thread reads
    std::mutex ladder_mu;
    std::unique_ptr<QualityLadder> ladder;
    ReportMerger merger;
    bool have_report = false;
    uint16_t peer_loss_permille = 0;  // worst receiver
    uint32_t peer_jitter_us = 0;
    int peer_receivers = 0;

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

    bool is_destination(const std::string& ip) const {
        std::lock_guard<std::mutex> lock(dest_mu);
        for (const auto& d : dests)
            if (d.ip == ip) return true;
        return false;
    }

    // receiver reports come back to the socket we send from
    void report_loop() {
        std::vector<uint8_t> buf(256);
        std::string ip;
        uint16_t port = 0;
        while (!stop) {
            ssize_t n = sock->recv_from(buf.data(), buf.size(), ip, port, 100);
            proto::Header h;
            proto::ReceiverReport r;
            if (n <= 0 || !proto::read_report_packet(buf.data(), n, h, r)) continue;
            if (r.media != proto::Type::Video || !is_destination(ip)) continue;

            uint32_t now = proto::now_us();
            std::lock_guard<std::mutex> lock(ladder_mu);
            auto w = merger.add(ip, r.reporter, r.loss_permille, r.jitter_us, now);
            have_report = true;
            peer_loss_permille = w.loss_permille;
            peer_jitter_us = w.jitter_us;
            peer_receivers = w.receivers;
            if (opt.adaptive) ladder->on_report(w.loss_permille, w.jitter_us, now);
        }
    }

    void send_loop() {
        net::Packetizer pk(proto::Type::Video, opt.stream_id);
        std::vector<int> jpeg_params{cv::IMWRITE_JPEG_QUALITY, opt.quality};
        std::vector<uchar> jpeg;
        std::vector<uint8_t> payload;

        auto next = Clock::now();
        auto report_at = next + std::chrono::seconds(1);
        int frames = 0;
        size_t up_bytes = 0, pkts = 0;  // up_bytes counts every copy: our real upload
        size_t ndest = 0;
        int last_level = -1;
        std::vector<proto::Endpoint> to;

        while (!stop) {
            {
                std::lock_guard<std::mutex> lock(dest_mu);
                to = dests;
            }
            ndest = to.size();

            // the ladder decides fps and quality for each frame. with adaptive off it just
            // stays on step 0, which is opt.fps and opt.quality
            QualityLadder::Step st;
            int level;
            {
                std::lock_guard<std::mutex> lock(ladder_mu);
                // while there's nobody to send to, missing reports aren't a sign of trouble
                if (to.empty())
                    ladder->idle(proto::now_us());
                else if (opt.adaptive)
                    ladder->tick(proto::now_us());
                st = ladder->step();
                level = ladder->level();
            }
            if (level != last_level) {
                metrics::event("video_level", level);
                last_level = level;
            }
            jpeg_params[1] = st.quality;

            auto gap = std::chrono::microseconds(1000000 / st.fps);
            std::this_thread::sleep_until(next);
            next += gap;
            // if we fell far behind (laptop suspended, debugger), don't try to catch up
            if (Clock::now() - next > std::chrono::seconds(1)) next = Clock::now() + gap;

            cv::Mat frame;
            uint32_t ts;
            // nobody to send to (between calls, or everyone left): still take the frame, so
            // the next one is fresh, but don't spend time encoding it
            if (take(frame, ts) && !to.empty()) {
                if (frame.cols != opt.width || frame.rows != opt.height)
                    cv::resize(frame, frame, cv::Size(opt.width, opt.height));

                // every strip gets the same capture timestamp, that's how the receiver
                // knows which strips belong to the same moment
                for (int s = 0; s < opt.strips; ++s) {
                    int y0 = s * opt.height / opt.strips;
                    int y1 = (s + 1) * opt.height / opt.strips;
                    cv::imencode(".jpg", frame(cv::Rect(0, y0, opt.width, y1 - y0)), jpeg,
                                 jpeg_params);

                    // how long this strip waited since capture, so the receiver can tell
                    // network jitter apart from our own frame-pick and encode delays
                    StripInfo info{static_cast<uint8_t>(s), static_cast<uint8_t>(opt.strips),
                                   static_cast<uint16_t>(y0), static_cast<uint16_t>(opt.height),
                                   send_delay_units(ts, proto::now_us())};
                    payload.resize(kStripPrefix + jpeg.size());
                    write_strip(info, payload.data());
                    std::memcpy(payload.data() + kStripPrefix, jpeg.data(), jpeg.size());

                    for (auto& d : pk.packetize(payload.data(), payload.size(), ts)) {
                        proto::Header h;
                        proto::read_header(d.data(), d.size(), h);
                        // one copy per destination, each logged, so the log's sent rate is
                        // what we actually upload (E5 compares that for mesh and relay)
                        for (const auto& e : to) {
                            sock->send_to(e.ip, e.port, d.data(), d.size());
                            metrics::packet('S', h, d.size());
                            up_bytes += d.size();
                        }
                        pkts++;
                    }
                }
                frames++;
            }

            if (Clock::now() >= report_at) {
                char peer[80] = "no reports";
                {
                    std::lock_guard<std::mutex> lock(ladder_mu);
                    auto w = merger.worst(proto::now_us());
                    if (have_report && w.receivers > 1)
                        std::snprintf(peer, sizeof(peer), "worst of %d: loss %.1f%% jitter %.1f ms",
                                      w.receivers, w.loss_permille / 10.0, w.jitter_us / 1000.0);
                    else if (have_report)
                        std::snprintf(peer, sizeof(peer), "peer loss %.1f%% jitter %.1f ms",
                                      peer_loss_permille / 10.0, peer_jitter_us / 1000.0);
                }
                char line[224];
                std::snprintf(
                    line, sizeof(line),
                    "tx %d fps  %.1f Mbps up to %zu  %.0f pkt/frame  | L%d %dfps q%d%s | %s",
                    frames, up_bytes * 8 / 1e6, ndest, frames ? double(pkts) / frames : 0.0, level,
                    st.fps, st.quality, opt.adaptive ? "" : " fixed", peer);
                {
                    std::lock_guard<std::mutex> lock(info_mu);
                    stats = line;
                }
                frames = 0;
                up_bytes = pkts = 0;
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
    d.dests = {{dest_ip, port}};
    d.ladder = std::make_unique<QualityLadder>(opt.fps, opt.quality);

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
    d.reports = std::thread([&d] { d.report_loop(); });
    return true;
}

void VideoSender::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.grabber.joinable()) d.grabber.join();
    if (d.sender.joinable()) d.sender.join();
    if (d.reports.joinable()) d.reports.join();
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

void VideoSender::set_destinations(const std::vector<proto::Endpoint>& to) {
    Impl& d = *impl_;
    std::vector<proto::Endpoint> ok;
    std::vector<std::string> ips;
    for (const auto& e : to)
        if (is_ipv4(e.ip) && e.port != 0) {
            ok.push_back(e);
            ips.push_back(e.ip);
        }
    {
        std::lock_guard<std::mutex> lock(d.dest_mu);
        d.dests = ok;
    }
    // reports from people we no longer send to shouldn't hold the quality down
    std::lock_guard<std::mutex> lock(d.ladder_mu);
    d.merger.keep_only(ips);
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
    static constexpr auto kStreamGone = std::chrono::seconds(5);  // forget a silent stream
    static constexpr auto kActive = std::chrono::seconds(2);      // for active_streams()

    // everything about one sender's video. only the worker thread touches these
    struct Stream {
        net::Reassembler reasm{100000};  // a strip has 100 ms to complete
        cv::Mat canvas;
        uint32_t canvas_ts = 0;  // capture time of the strips being collected
        bool dirty = false;      // canvas changed since it was last queued
        bool have_id = false;
        uint16_t last_id = 0;
        Clock::time_point heard;

        // for the stats line, reset every second
        int frames = 0, strips_ok = 0;
        uint64_t missing = 0;

        // for the receiver reports: where this stream comes from, and how it arrives
        std::string ip;
        uint16_t port = 0;
        net::LossCounter loss;
        uint16_t report_seq = 0;
        size_t report_bytes = 0;
        double jitter_us = 0;
        bool have_transit = false;
        int32_t last_transit = 0;
        double last_loss_pct = 0;
    };

    uint8_t my_id = 0;
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    // empty = anyone. set_allowed_sources() replaces it, the worker copies it
    mutable std::mutex src_mu;
    std::vector<std::string> allowed;

    mutable std::mutex queue_mu;
    std::deque<VideoFrame> queue;
    std::map<uint8_t, Clock::time_point> last_frame;  // per stream, for active_streams()

    mutable std::mutex info_mu;
    std::string stats = "rx idle";
    std::string error;

    void push(uint8_t id, const cv::Mat& canvas, uint32_t ts) {
        VideoFrame f;
        f.image = canvas.clone();  // the canvas keeps changing, the app needs its own copy
        f.capture_us = ts;
        f.stream_id = id;
        std::lock_guard<std::mutex> lock(queue_mu);
        // at most kMaxQueued per stream: drop this stream's oldest, not someone else's
        size_t mine = 0;
        for (const auto& q : queue)
            if (q.stream_id == id) mine++;
        if (mine >= kMaxQueued)
            for (auto it = queue.begin(); it != queue.end(); ++it)
                if (it->stream_id == id) {
                    queue.erase(it);
                    break;
                }
        queue.push_back(std::move(f));
        last_frame[id] = Clock::now();
    }

    void loop() {
        std::vector<uint8_t> buf(2048);
        std::string from_ip;
        uint16_t from_port = 0;
        net::Frame strip;
        std::map<uint8_t, Stream> streams;
        std::vector<std::string> allow;

        int bad = 0;
        size_t bytes = 0;
        auto report_at = Clock::now() + std::chrono::seconds(1);
        auto rr_last = Clock::now();
        auto rr_at = rr_last + std::chrono::milliseconds(proto::kReportEveryMs);

        auto finish = [&](uint8_t id, Stream& st) {
            push(id, st.canvas, st.canvas_ts);
            st.frames++;
            st.dirty = false;
        };

        auto handle = [&](ssize_t n) {
            if (!allow.empty() && std::find(allow.begin(), allow.end(), from_ip) == allow.end())
                return;
            proto::Header h;
            if (!proto::read_header(buf.data(), n, h) || h.type != proto::Type::Video) return;
            metrics::packet('R', h, n);
            bytes += n;

            Stream& st = streams[h.stream_id];
            st.heard = Clock::now();
            // for the reports: where to send them (the relay, in relay mode), what arrived
            st.ip = from_ip;
            st.port = from_port;
            st.report_bytes += n;
            st.loss.on_packet(h.seq);
            // jitter (RFC 3550 style) on the first packet of each strip, which starts with
            // the strip prefix. transit is measured from when the strip was SENT (capture
            // time + the sender's own delay), so only the network's variation is left
            // (issue #6)
            StripInfo first;
            if (h.frag_index == 0 &&
                read_strip(buf.data() + proto::kHeaderSize, n - proto::kHeaderSize, first)) {
                uint32_t sent_us = h.timestamp_us + uint32_t(first.send_delay_100us) * 100;
                int32_t transit = static_cast<int32_t>(proto::now_us() - sent_us);
                if (st.have_transit) {
                    double d = std::abs(static_cast<double>(transit - st.last_transit));
                    st.jitter_us += (d - st.jitter_us) / 16.0;
                }
                st.have_transit = true;
                st.last_transit = transit;
            }

            if (!st.reasm.push(h, buf.data() + proto::kHeaderSize, n - proto::kHeaderSize,
                               proto::now_us(), strip))
                return;

            // each strip has its own frame id, so a jump from 40 to 43 means 41 and 42 never
            // made it, including strips where every packet was lost. a big or negative
            // jump means the sender restarted, so skip it
            if (st.have_id) {
                int gap = static_cast<int16_t>(strip.frame_id - st.last_id) - 1;
                if (gap > 0 && gap < 1000) st.missing += gap;
            }
            st.have_id = true;
            st.last_id = strip.frame_id;

            StripInfo info;
            if (!read_strip(strip.data.data(), strip.data.size(), info)) {
                bad++;
                return;
            }

            // first strip of a newer capture: whatever we had is as complete as it will get
            if (st.dirty && strip.timestamp_us != st.canvas_ts) finish(h.stream_id, st);
            st.canvas_ts = strip.timestamp_us;

            cv::Mat jpeg(1, static_cast<int>(strip.data.size() - kStripPrefix), CV_8UC1,
                         strip.data.data() + kStripPrefix);
            cv::Mat part = cv::imdecode(jpeg, cv::IMREAD_COLOR);
            if (part.empty()) {
                bad++;
                return;
            }
            if (st.canvas.rows != info.frame_height || st.canvas.cols != part.cols)
                st.canvas = cv::Mat::zeros(info.frame_height, part.cols, CV_8UC3);
            if (info.y0 + part.rows > st.canvas.rows) {
                bad++;
                return;
            }
            part.copyTo(st.canvas(cv::Rect(0, info.y0, part.cols, part.rows)));
            st.strips_ok++;
            st.dirty = true;

            // last strip arrived, no need to wait for the next frame
            if (info.index == info.count - 1) finish(h.stream_id, st);
        };

        while (!stop) {
            {
                std::lock_guard<std::mutex> lock(src_mu);
                allow = allowed;
            }
            ssize_t n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 20);
            if (n > 0) handle(n);
            // empty out whatever else is waiting. this thread never draws anything, but
            // decoding is still slow enough that one packet per wakeup would fall behind
            for (int i = 0; i < 512; ++i) {
                n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 0);
                if (n <= 0) break;
                handle(n);
            }
            for (auto& [id, st] : streams) st.reasm.expire(proto::now_us());

            auto now = Clock::now();
            if (now >= rr_at) {
                auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - rr_last).count();
                for (auto& [id, st] : streams) {
                    auto iv = st.loss.take_interval();
                    // nothing arrived: stay quiet. the sender treats silence as trouble
                    if (iv.expected == 0 || st.ip.empty()) {
                        st.report_bytes = 0;
                        continue;
                    }
                    proto::ReceiverReport r;
                    r.media = proto::Type::Video;
                    r.reporter = my_id;
                    r.highest_seq = st.loss.highest_seq();
                    r.loss_permille = iv.loss_permille();
                    r.interval_ms = static_cast<uint16_t>(ms);
                    r.jitter_us = static_cast<uint32_t>(st.jitter_us);
                    r.received_kbps =
                        ms > 0
                            ? static_cast<uint32_t>(st.report_bytes * 8 / static_cast<size_t>(ms))
                            : 0;
                    auto pkt = proto::make_report_packet(id, st.report_seq++, r);
                    sock->send_to(st.ip, st.port, pkt.data(), pkt.size());
                    st.last_loss_pct = r.loss_permille / 10.0;
                    st.report_bytes = 0;
                }
                rr_last = now;
                rr_at = now + std::chrono::milliseconds(proto::kReportEveryMs);
            }

            if (now >= report_at) {
                int frames = 0, strips_ok = 0, live = 0;
                uint64_t missing = 0;
                double worst_loss = 0, worst_jitter = 0;
                for (auto it = streams.begin(); it != streams.end();) {
                    Stream& st = it->second;
                    frames += st.frames;
                    strips_ok += st.strips_ok;
                    missing += st.missing;
                    worst_loss = std::max(worst_loss, st.last_loss_pct);
                    worst_jitter = std::max(worst_jitter, st.jitter_us);
                    st.frames = st.strips_ok = 0;
                    st.missing = 0;
                    // someone who left (or restarted with another id) frees their canvas
                    if (now - st.heard > kStreamGone) {
                        it = streams.erase(it);
                    } else {
                        live++;
                        ++it;
                    }
                }
                double total = strips_ok + static_cast<double>(missing);
                char line[160];
                std::snprintf(
                    line, sizeof(line),
                    "rx %d fps  %.1f Mbps  strips %.1f%% missing  loss %.1f%%  jitter "
                    "%.1f ms%s",
                    frames, bytes * 8 / 1e6, total > 0 ? 100.0 * missing / total : 0.0, worst_loss,
                    worst_jitter / 1000.0,
                    live > 1 ? ("  (" + std::to_string(live) + " streams, worst)").c_str() : "");
                {
                    std::lock_guard<std::mutex> lock(info_mu);
                    stats = line;
                    if (bad) error = std::to_string(bad) + " bad strips in the last second";
                }
                bad = 0;
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

bool VideoReceiver::start(uint16_t port, const std::string& only_from_ip, uint8_t my_id) {
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
    if (!only_from_ip.empty()) d.allowed = {only_from_ip};
    d.my_id = my_id;

    try {
        d.sock = std::make_unique<UdpSocket>(port);
    } catch (const std::exception& e) {
        d.error = e.what();
        return false;
    }
    // a frame is ~30 packets arriving in a burst, times the number of senders. linux caps
    // this at net.core.rmem_max, so also run:  sudo sysctl -w net.core.rmem_max=8388608
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

void VideoReceiver::set_allowed_sources(const std::vector<std::string>& ips) {
    std::vector<std::string> ok;
    for (const auto& ip : ips)
        if (is_ipv4(ip)) ok.push_back(ip);
    std::lock_guard<std::mutex> lock(impl_->src_mu);
    impl_->allowed = ok;
}

std::vector<uint8_t> VideoReceiver::active_streams() const {
    std::lock_guard<std::mutex> lock(impl_->queue_mu);
    std::vector<uint8_t> out;
    auto now = Clock::now();
    for (const auto& [id, t] : impl_->last_frame)  // a std::map, so already sorted
        if (now - t <= Impl::kActive) out.push_back(id);
    return out;
}

}  // namespace video
