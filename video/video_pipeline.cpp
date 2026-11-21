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
//
// Phase 5: the receiver NACKs missing packets (net/nack_tracker.hpp) to wherever the stream
// comes from; the sender resends them from its copy (net/retransmit_buffer.hpp), the relay
// from its own when it has one. A frame whose last strip is in but that still misses some
// waits up to kRepairWait for the repairs before it's queued.
//
// Phase 7: with a multicast group among the destinations, reports and NACKs come from the
// members' own addresses, so the sender takes them from anyone, and resends to the group.
#include "video/video_pipeline.hpp"

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
#include "common/nack.hpp"
#include "common/platform.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "common/udp_socket.hpp"
#include "net/loss_counter.hpp"
#include "net/nack_tracker.hpp"
#include "net/packetizer.hpp"
#include "net/reassembler.hpp"
#include "net/retransmit_buffer.hpp"
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

    // phase 5: what we sent lately, to answer NACKs. send thread stores, report thread reads
    std::mutex rtx_mu;
    net::RetransmitBuffer rtx;
    std::atomic<int> resent{0};  // this second, for the stats line

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

    // do reports and NACKs from `ip` count? Yes if we send to ip. Phase 7: also yes, from
    // anyone, while a multicast group is a destination: its members answer from their own
    // addresses, never the group's, and we can't know who has joined. (With encryption on,
    // only someone with the call's key gets a datagram through to us anyway.)
    bool accepts_feedback(const std::string& ip) const {
        std::lock_guard<std::mutex> lock(dest_mu);
        for (const auto& d : dests)
            if (d.ip == ip || proto::is_multicast_ipv4(d.ip)) return true;
        return false;
    }

    // a receiver (or the relay) at `ip` asks for these again: resend what we still have,
    // the same way the originals went. That's to ip if we send there, otherwise (phase 7) to
    // the group. A packet lost near us is lost for every member and they all ask, but the
    // buffer resends a packet at most twice, so one resend to the group serves them all
    // where resends to each asker wouldn't. Members that had it drop it as a duplicate
    void resend(const std::string& ip, const std::vector<uint16_t>& seqs) {
        proto::Endpoint to;
        {
            std::lock_guard<std::mutex> lock(dest_mu);
            auto it = std::find_if(dests.begin(), dests.end(),
                                   [&](const proto::Endpoint& e) { return e.ip == ip; });
            if (it == dests.end())
                it = std::find_if(dests.begin(), dests.end(), [](const proto::Endpoint& e) {
                    return proto::is_multicast_ipv4(e.ip);
                });
            if (it == dests.end()) return;
            to = *it;
        }
        int sent = 0;
        std::vector<uint8_t> d;
        for (uint16_t seq : seqs) {
            {
                std::lock_guard<std::mutex> lock(rtx_mu);
                const std::vector<uint8_t>* p = rtx.take(seq, proto::now_us());
                if (!p) continue;
                d = *p;
            }
            sock->send_to(to.ip, to.port, d.data(), d.size());
            proto::Header h;
            proto::read_header(d.data(), d.size(), h);
            metrics::packet('S', h, d.size());  // it's upload too
            sent++;
        }
        if (sent > 0) {
            metrics::event("nack_resent", sent);
            resent += sent;
        }
    }

    // phase 5 FEC: one parity packet after the strip's pieces (format in net/reassembler.hpp)
    void send_parity(const std::vector<net::Datagram>& strip,
                     const std::vector<proto::Endpoint>& to, size_t& up_bytes) {
        std::vector<std::vector<uint8_t>> payloads;
        payloads.reserve(strip.size());
        for (const auto& d : strip) payloads.emplace_back(d.begin() + proto::kHeaderSize, d.end());
        std::vector<uint8_t> body = net::make_parity(payloads);

        proto::Header h;
        proto::read_header(strip[0].data(), strip[0].size(), h);  // seq of piece 0, frame id
        h.type = proto::Type::VideoFec;
        h.frag_index = 0;
        std::vector<uint8_t> d(proto::kHeaderSize + body.size());
        proto::write_header(h, d.data());
        std::copy(body.begin(), body.end(), d.begin() + proto::kHeaderSize);
        for (const auto& e : to) {
            sock->send_to(e.ip, e.port, d.data(), d.size());
            metrics::packet('S', h, d.size());
            up_bytes += d.size();
        }
    }

    // receiver reports and NACKs come back to the socket we send from
    void report_loop() {
        std::vector<uint8_t> buf(512);
        std::string ip;
        uint16_t port = 0;
        while (!stop) {
            ssize_t n = sock->recv_from(buf.data(), buf.size(), ip, port, 100);
            if (n <= 0) continue;
            proto::Header h;
            proto::Nack nk;
            if (proto::read_nack_packet(buf.data(), n, h, nk)) {
                if (opt.nack && nk.media == proto::Type::Video) resend(ip, nk.seqs);
                continue;
            }
            proto::ReceiverReport r;
            if (!proto::read_report_packet(buf.data(), n, h, r)) continue;
            if (r.media != proto::Type::Video || !accepts_feedback(ip)) continue;

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

                    auto datagrams = pk.packetize(payload.data(), payload.size(), ts);
                    for (auto& d : datagrams) {
                        proto::Header h;
                        proto::read_header(d.data(), d.size(), h);
                        if (opt.nack) {
                            std::lock_guard<std::mutex> lock(rtx_mu);
                            rtx.store(h.seq, d.data(), d.size(), proto::now_us());
                        }
                        // one copy per destination, each logged, so the log's sent rate is
                        // what we actually upload (E5 compares that for mesh and relay)
                        for (const auto& e : to) {
                            sock->send_to(e.ip, e.port, d.data(), d.size());
                            metrics::packet('S', h, d.size());
                            up_bytes += d.size();
                        }
                        pkts++;
                    }
                    if (opt.fec && !datagrams.empty()) send_parity(datagrams, to, up_bytes);
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
                char line[256];
                std::snprintf(
                    line, sizeof(line),
                    "tx %d fps  %.1f Mbps up to %zu  %.0f pkt/frame  | L%d %dfps q%d%s | %s"
                    "  | resent %d",
                    frames, up_bytes * 8 / 1e6, ndest, frames ? double(pkts) / frames : 0.0, level,
                    st.fps, st.quality, opt.adaptive ? "" : " fixed", peer, resent.exchange(0));
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
    // buffering (more delay) and prints a "cannot query video position" warning.
    // Phase 8, Windows: DirectShow, which opens a webcam quickly and keeps 640x480; the
    // default there (Media Foundation) can take seconds to open. Either way, if that backend
    // can't open the camera, OpenCV's default gets a try
#if defined(_WIN32)
    d.cap.open(opt.camera, cv::CAP_DSHOW);
#else
    d.cap.open(opt.camera, cv::CAP_V4L2);
#endif
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
    if (!opt.multicast_if.empty() && !d.sock->set_multicast_interface(opt.multicast_if)) {
        d.cap.release();
        d.set_error("can't send multicast from " + opt.multicast_if);
        return false;
    }
    d.sock->set_multicast_loop(opt.multicast_loop);  // only matters when sending to a group

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
    bool group = false;
    for (const auto& e : to)
        if (is_ipv4(e.ip) && e.port != 0) {
            ok.push_back(e);
            ips.push_back(e.ip);
            group = group || proto::is_multicast_ipv4(e.ip);
        }
    {
        std::lock_guard<std::mutex> lock(d.dest_mu);
        d.dests = ok;
    }
    // reports from people we no longer send to shouldn't hold the quality down. With a group
    // there's no list of who's in it: someone who left drops out of the merge a couple of
    // seconds after their reports stop (ReportMerger::kForgetUs)
    std::lock_guard<std::mutex> lock(d.ladder_mu);
    if (!group) d.merger.keep_only(ips);
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
    // a frame missing strips waits this long for resent packets. a LAN round trip is ~1 ms,
    // and a lost resend is asked for again after 10 ms (net/nack_tracker.hpp), so 25 ms
    // leaves room for that retry; lip sync holds video longer than this anyway
    static constexpr auto kRepairWait = std::chrono::milliseconds(25);

    // everything about one sender's video. only the worker thread touches these
    struct Stream {
        // a strip has 100 ms to complete; strips wait independently, so an older one can still
        // be completed by a resent packet after a newer one is done
        net::Reassembler reasm{100000, true};
        cv::Mat canvas;
        bool have_canvas = false;
        uint32_t canvas_ts = 0;  // capture time of the strips being collected
        bool dirty = false;      // canvas changed since it was last queued
        bool pushed = false;     // the frame at canvas_ts has been queued already
        std::vector<bool> got;   // which strips of canvas_ts are in
        int got_count = 0;
        bool holding = false;  // last strip is in, waiting a little for repairs
        Clock::time_point hold_until;
        Clock::time_point heard;

        // phase 5: asking for lost packets again
        net::NackTracker nack;
        uint16_t nack_seq = 0;

        // for the stats line, reset every second
        int frames = 0;
        uint64_t strips_shown = 0, missing = 0;  // strips in / not in the frames queued
        int nacked = 0;
        int fec_fixed = 0;

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
    std::atomic<bool> nack_on{true};  // phase 7: set_nack(); the worker reads it once a round
    std::unique_ptr<UdpSocket> sock;
    std::mutex group_mu;  // phase 7: the multicast group joined, if any
    std::string group, group_iface;
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
        bool nack = true;  // nack_on, as of this round

        int bad = 0;
        size_t bytes = 0;
        auto report_at = Clock::now() + std::chrono::seconds(1);
        auto rr_last = Clock::now();
        auto rr_at = rr_last + std::chrono::milliseconds(proto::kReportEveryMs);

        auto finish = [&](uint8_t id, Stream& st) {
            push(id, st.canvas, st.canvas_ts);
            st.frames++;
            int lacking = static_cast<int>(st.got.size()) - st.got_count;
            st.strips_shown += st.got_count;
            st.missing += lacking;
            if (lacking > 0) metrics::event("frame_incomplete", lacking);
            st.dirty = false;
            st.pushed = true;
            st.holding = false;
        };

        // NACK whatever the tracker says is due, to where the stream comes from. With NACK
        // off the tracker still runs (so turning it back on doesn't look like one big gap)
        // and due() is still called, so what's missing ages out without being asked for
        auto send_nacks = [&](uint8_t id, Stream& st, uint32_t now_us) {
            auto seqs = st.nack.due(now_us);
            if (seqs.empty() || st.ip.empty() || !nack) return;
            proto::Nack nk;
            nk.media = proto::Type::Video;
            nk.reporter = my_id;
            nk.seqs = std::move(seqs);
            auto pkt = proto::make_nack_packet(id, st.nack_seq++, nk);
            sock->send_to(st.ip, st.port, pkt.data(), pkt.size());
            metrics::event("nack_sent", static_cast<double>(nk.seqs.size()));
            st.nacked += static_cast<int>(nk.seqs.size());
        };

        // a whole strip is in (straight off the wire, resent, or rebuilt)
        auto on_strip = [&](uint8_t id, Stream& st, const net::Frame& strip) {
            StripInfo info;
            if (!read_strip(strip.data.data(), strip.data.size(), info)) {
                bad++;
                return;
            }
            int32_t age = static_cast<int32_t>(st.canvas_ts - strip.timestamp_us);
            bool older = st.have_canvas && age > 0;

            if (!older && (!st.have_canvas || age < 0)) {
                // first strip of a newer capture: whatever we had is as complete as it'll get
                if (st.dirty) finish(id, st);
                st.have_canvas = true;
                st.canvas_ts = strip.timestamp_us;
                st.pushed = false;
                st.holding = false;
                st.got.assign(info.count, false);
                st.got_count = 0;
            }
            // a repaired strip of a frame we've moved past: its pixels are still newer than
            // what that band shows, unless the current frame already filled it
            if (older && info.index < st.got.size() && st.got[info.index]) return;

            // the packet came off the network: whatever OpenCV makes of it (a corrupt JPEG, an
            // allocation it can't do) is one bad packet, never an exception out of this thread
            cv::Mat part;
            try {
                cv::Mat jpeg(1, static_cast<int>(strip.data.size() - kStripPrefix), CV_8UC1,
                             const_cast<uint8_t*>(strip.data.data()) + kStripPrefix);
                part = cv::imdecode(jpeg, cv::IMREAD_COLOR);
                if (part.empty() || part.cols > kMaxFrameWidth) {
                    bad++;
                    return;
                }
                if (st.canvas.rows != info.frame_height || st.canvas.cols != part.cols)
                    st.canvas = cv::Mat::zeros(info.frame_height, part.cols, CV_8UC3);
            } catch (const cv::Exception&) {
                bad++;
                return;
            }
            if (info.y0 + part.rows > st.canvas.rows) {
                bad++;
                return;
            }
            part.copyTo(st.canvas(cv::Rect(0, info.y0, part.cols, part.rows)));
            if (older) return;

            if (st.got.size() != info.count) {  // the sender changed --strips mid-stream
                st.got.assign(info.count, false);
                st.got_count = 0;
            }
            if (!st.got[info.index]) {
                st.got[info.index] = true;
                st.got_count++;
            }
            // already queued: the repair is painted in and shows with the next frame
            if (st.pushed) return;
            st.dirty = true;

            if (st.got_count == static_cast<int>(st.got.size())) {
                finish(id, st);  // every strip is in
            } else if (info.index == info.count - 1) {
                // the last strip is in but some aren't. if packets are being asked for again,
                // give them a moment; otherwise there's nothing to wait for
                if (nack && st.nack.missing() > 0) {
                    st.holding = true;
                    st.hold_until = Clock::now() + kRepairWait;
                } else {
                    finish(id, st);
                }
            }
        };

        // a strip completed; if FEC rebuilt one of its packets, stop asking for that one
        auto strip_done = [&](uint8_t id, Stream& st, const net::Frame& strip) {
            if (strip.recovered) {
                st.nack.recovered(strip.recovered_seq, proto::now_us());
                st.fec_fixed++;
                metrics::event("fec_recovered", 1);
            }
            on_strip(id, st, strip);
        };

        // the parity packet of a strip (phase 5 FEC). not logged as a packet: the loss and
        // rate numbers in the log stay about the video itself
        auto handle_parity = [&](const proto::Header& h, ssize_t n) {
            bytes += n;
            Stream& st = streams[h.stream_id];
            st.heard = Clock::now();
            if (st.reasm.push_parity(h, buf.data() + proto::kHeaderSize, n - proto::kHeaderSize,
                                     proto::now_us(), strip))
                strip_done(h.stream_id, st, strip);
        };

        auto handle = [&](ssize_t n) {
            if (!allow.empty() && std::find(allow.begin(), allow.end(), from_ip) == allow.end())
                return;
            proto::Header h;
            if (!proto::read_header(buf.data(), n, h)) return;
            if (h.type == proto::Type::VideoFec) {
                handle_parity(h, n);
                return;
            }
            if (h.type != proto::Type::Video) return;
            metrics::packet('R', h, n);
            bytes += n;

            Stream& st = streams[h.stream_id];
            st.heard = Clock::now();
            // for the reports: where to send them (the relay, in relay mode), what arrived
            st.ip = from_ip;
            st.port = from_port;
            st.report_bytes += n;
            st.loss.on_packet(h.seq);
            size_t was_missing = st.nack.missing();
            st.nack.on_packet(h.seq, proto::now_us());
            // a new gap: ask right away. waiting until the socket is drained can be too late
            // for the frame, since the next one's first strip may complete meanwhile
            if (st.nack.missing() > was_missing) send_nacks(h.stream_id, st, proto::now_us());
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

            if (st.reasm.push(h, buf.data() + proto::kHeaderSize, n - proto::kHeaderSize,
                              proto::now_us(), strip))
                strip_done(h.stream_id, st, strip);
        };

        while (!stop) {
            {
                std::lock_guard<std::mutex> lock(src_mu);
                allow = allowed;
            }
            nack = nack_on;
            // while repairs are pending, wake often enough to retry NACKs and end holds on time
            bool pending = false;
            for (auto& [id, st] : streams) pending |= st.holding || (nack && st.nack.missing() > 0);
            ssize_t n =
                sock->recv_from(buf.data(), buf.size(), from_ip, from_port, pending ? 2 : 20);
            if (n > 0) handle(n);
            // empty out whatever else is waiting. this thread never draws anything, but
            // decoding is still slow enough that one packet per wakeup would fall behind
            for (int i = 0; i < 512; ++i) {
                n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 0);
                if (n <= 0) break;
                handle(n);
            }
            auto now = Clock::now();
            uint32_t now_us = proto::now_us();
            for (auto& [id, st] : streams) {
                st.reasm.expire(now_us);
                // the repairs didn't make it in time: show what we have
                if (st.holding && now >= st.hold_until) finish(id, st);
                send_nacks(id, st, now_us);  // retries, and anything not asked yet
            }

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
                int frames = 0, live = 0, nacked = 0, fec_fixed = 0;
                uint64_t shown = 0, missing = 0;
                double worst_loss = 0, worst_jitter = 0;
                for (auto it = streams.begin(); it != streams.end();) {
                    Stream& st = it->second;
                    frames += st.frames;
                    shown += st.strips_shown;
                    missing += st.missing;
                    nacked += st.nacked;
                    fec_fixed += st.fec_fixed;
                    worst_loss = std::max(worst_loss, st.last_loss_pct);
                    worst_jitter = std::max(worst_jitter, st.jitter_us);
                    st.frames = st.nacked = st.fec_fixed = 0;
                    st.strips_shown = st.missing = 0;
                    // someone who left (or restarted with another id) frees their canvas
                    if (now - st.heard > kStreamGone) {
                        it = streams.erase(it);
                    } else {
                        live++;
                        ++it;
                    }
                }
                double total = static_cast<double>(shown + missing);
                char line[192];
                std::snprintf(
                    line, sizeof(line),
                    "rx %d fps  %.1f Mbps  strips %.1f%% missing  loss %.1f%%  jitter "
                    "%.1f ms  nack %d  fec %d%s",
                    frames, bytes * 8 / 1e6, total > 0 ? 100.0 * missing / total : 0.0, worst_loss,
                    worst_jitter / 1000.0, nacked, fec_fixed,
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
    leave_group();
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

bool VideoReceiver::join_group(const std::string& group, const std::string& iface_ip) {
    Impl& d = *impl_;
    if (!d.sock) return false;
    leave_group();
    std::lock_guard<std::mutex> lock(d.group_mu);
    if (!proto::is_multicast_ipv4(group) || !d.sock->join_multicast(group, iface_ip)) {
        std::lock_guard<std::mutex> info(d.info_mu);
        d.error = "can't join multicast group " + group;
        return false;
    }
    d.group = group;
    d.group_iface = iface_ip;
    return true;
}

void VideoReceiver::set_nack(bool on) {
    impl_->nack_on = on;
}

void VideoSender::set_crypto(std::shared_ptr<const proto::MediaCrypto> crypto) {
    if (impl_->sock) impl_->sock->set_crypto(std::move(crypto));
}

void VideoReceiver::set_crypto(std::shared_ptr<const proto::MediaCrypto> crypto) {
    if (impl_->sock) impl_->sock->set_crypto(std::move(crypto));
}

void VideoReceiver::leave_group() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.group_mu);
    if (d.group.empty() || !d.sock) return;
    d.sock->leave_multicast(d.group, d.group_iface);
    d.group.clear();
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
