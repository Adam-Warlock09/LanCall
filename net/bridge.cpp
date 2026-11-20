// Broadcast bridge (Person 2, net/, phase 7). The rules are in bridge.hpp.
#include "net/bridge.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "common/report.hpp"
#include "common/udp_socket.hpp"

namespace net {

namespace {
using Clock = std::chrono::steady_clock;

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
        .count();
}
}  // namespace

struct Bridge::Impl {
    BridgeOptions opt;
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};
    std::atomic<int64_t> last_media_ms{0};  // 0 = never

    mutable std::mutex mu;
    BridgeCounters counters;
    std::string stats = "bridge starting";
    std::string error;

    // the presenter's sockets (its audio and video senders are separate): where reports go
    uint16_t video_from = 0, audio_from = 0;

    void handle(const uint8_t* data, size_t n, const std::string& ip, uint16_t port,
                uint64_t& in_bytes, uint64_t& in_pkts, uint64_t& reports) {
        proto::Header h{};
        if (!proto::read_header(data, n, h)) {
            std::lock_guard<std::mutex> lock(mu);
            ++counters.dropped_bad;
            return;
        }
        const bool video = h.type == proto::Type::Video || h.type == proto::Type::VideoFec;
        if (video || h.type == proto::Type::Audio) {
            if (ip != opt.from_ip) {  // only the presenter may feed this floor's group
                std::lock_guard<std::mutex> lock(mu);
                ++counters.dropped_other;
                return;
            }
            (video ? video_from : audio_from) = port;
            sock->send_to(opt.group, video ? opt.video_port : opt.audio_port, data, n);
            last_media_ms = now_ms();
            in_bytes += n;
            ++in_pkts;
            std::lock_guard<std::mutex> lock(mu);
            ++counters.forwarded;
            return;
        }
        if (h.type == proto::Type::Report) {
            // a viewer on our floor: its report goes to the presenter's sender of that media
            proto::Header rh{};
            proto::ReceiverReport r;
            if (proto::read_report_packet(data, n, rh, r)) {
                const uint16_t to = r.media == proto::Type::Video ? video_from : audio_from;
                if (to != 0) {
                    sock->send_to(opt.from_ip, to, data, n);
                    ++reports;
                    std::lock_guard<std::mutex> lock(mu);
                    ++counters.reports_back;
                    return;
                }
            }
        }
        std::lock_guard<std::mutex> lock(mu);
        ++counters.dropped_other;  // NACKs (no resending for broadcasts), keepalives, ...
    }

    void loop() {
        std::vector<uint8_t> buf(2048);
        std::string ip;
        uint16_t port = 0;
        uint64_t in_bytes = 0, in_pkts = 0, reports = 0;  // this second
        auto next_stats = Clock::now() + std::chrono::seconds(1);

        while (!stop) {
            const ssize_t n = sock->recv_from(buf.data(), buf.size(), ip, port, 100);
            if (n > 0)
                handle(buf.data(), static_cast<size_t>(n), ip, port, in_bytes, in_pkts, reports);

            const auto now = Clock::now();
            if (now < next_stats) continue;
            next_stats += std::chrono::seconds(1);
            char line[200];
            if (!live_now()) {
                std::snprintf(line, sizeof line, "bridge %s -> %s: waiting for the presenter",
                              opt.from_ip.c_str(), opt.group.c_str());
            } else {
                std::snprintf(line, sizeof line,
                              "bridge %s -> %s: %.1f Mbps, %llu pkt/s, %llu reports/s back",
                              opt.from_ip.c_str(), opt.group.c_str(), in_bytes * 8 / 1e6,
                              static_cast<unsigned long long>(in_pkts),
                              static_cast<unsigned long long>(reports));
            }
            in_bytes = in_pkts = reports = 0;
            std::lock_guard<std::mutex> lock(mu);
            stats = line;
        }
    }

    bool live_now() const {
        const int64_t t = last_media_ms;
        return t != 0 && now_ms() - t <= opt.quiet_ms;
    }
};

Bridge::Bridge() : impl_(std::make_unique<Impl>()) {}

Bridge::~Bridge() {
    stop();
}

bool Bridge::start(const BridgeOptions& opt) {
    stop();
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;
    if (!proto::is_multicast_ipv4(opt.group)) {
        d.error = "'" + opt.group + "' is not a multicast group";
        return false;
    }
    if (opt.from_ip.empty()) {
        d.error = "no presenter address to take the broadcast from";
        return false;
    }
    try {
        d.sock = std::make_unique<UdpSocket>(opt.port);
    } catch (const std::exception& e) {
        d.error = std::string(e.what()) + " (is UDP port " + std::to_string(opt.port) + " free?)";
        return false;
    }
    d.sock->set_recv_buffer(1 << 20);  // a whole broadcast arrives here: ~460 packets a second
    if (!opt.iface_ip.empty() && !d.sock->set_multicast_interface(opt.iface_ip)) {
        d.error = "can't send multicast from " + opt.iface_ip;
        d.sock.reset();
        return false;
    }
    d.sock->set_multicast_loop(opt.loop);
    d.sock->set_multicast_ttl(1);  // this floor only
    d.opt = opt;
    d.running = true;
    d.worker = std::thread([&d] { d.loop(); });
    return true;
}

void Bridge::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.worker.joinable()) d.worker.join();
    d.sock.reset();
    d.running = false;
}

bool Bridge::running() const {
    return impl_->running;
}

bool Bridge::live() const {
    return impl_->running && impl_->live_now();
}

BridgeCounters Bridge::counters() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->counters;
}

std::string Bridge::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->stats;
}

std::string Bridge::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

}  // namespace net
