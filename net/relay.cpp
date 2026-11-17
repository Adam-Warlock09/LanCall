// Group-call relay (Person 1, net/, phase 4). The forwarding rules are in relay.hpp.
//
// One thread, one socket. It never decodes anything: it reads the 12-byte header (and for
// reports the 16-byte body) and copies the datagram on. Everything the API reads (members,
// stats) sits behind one mutex, which the thread takes once per datagram.
#include "net/relay.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

#include "common/report.hpp"
#include "common/udp_socket.hpp"

namespace net {

namespace {
using Clock = std::chrono::steady_clock;
}

struct Relay::Impl {
    struct Member {
        std::string ip;
        Clock::time_point heard;
        // the sockets their video and audio come from: where reports about them go
        std::string video_ip, audio_ip;
        uint16_t video_port = 0, audio_port = 0;
        uint64_t in = 0, out = 0;
    };

    RelayOptions opt;
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    mutable std::mutex mu;
    std::map<uint8_t, Member> members;
    std::string stats = "relay idle";
    std::string error;

    // in and out this second, for the stats line
    uint64_t bytes_in = 0, bytes_out = 0;

    // a member just sent us something
    Member& seen(uint8_t id, const std::string& ip, Clock::time_point now) {
        Member& m = members[id];
        m.ip = ip;
        m.heard = now;
        return m;
    }

    void handle(const uint8_t* data, size_t n, const std::string& from_ip, uint16_t from_port,
                Clock::time_point now) {
        proto::Header h;
        if (!proto::read_header(data, n, h)) return;
        bytes_in += n;

        if (h.type == proto::Type::Video || h.type == proto::Type::Audio) {
            // stream_id 0 is a phase 3 sender, or someone outside a group call: there's no way
            // to tell two of them apart, so they aren't forwarded
            if (h.stream_id == 0) return;
            Member& src = seen(h.stream_id, from_ip, now);
            src.in++;
            bool video = h.type == proto::Type::Video;
            (video ? src.video_ip : src.audio_ip) = from_ip;
            (video ? src.video_port : src.audio_port) = from_port;
            uint16_t to_port = video ? opt.video_port : opt.audio_port;
            for (auto& [id, m] : members) {
                if (id == h.stream_id) continue;
                sock->send_to(m.ip, to_port, data, n);
                m.out++;
                bytes_out += n;
            }
        } else if (h.type == proto::Type::Report) {
            proto::ReceiverReport r;
            if (!proto::read_report_packet(data, n, h, r)) return;
            if (r.reporter != 0) seen(r.reporter, from_ip, now).in++;
            // on to the sender of the stream it's about, exactly as the receiver wrote it
            auto it = members.find(h.stream_id);
            if (it == members.end()) return;
            Member& s = it->second;
            bool video = r.media == proto::Type::Video;
            const std::string& ip = video ? s.video_ip : s.audio_ip;
            uint16_t port = video ? s.video_port : s.audio_port;
            if (ip.empty() || port == 0) return;
            sock->send_to(ip, port, data, n);
            s.out++;
            bytes_out += n;
        } else if (h.type == proto::Type::Keepalive) {
            if (h.stream_id != 0) seen(h.stream_id, from_ip, now).in++;
        }
    }

    void loop() {
        std::vector<uint8_t> buf(2048);
        std::string from_ip;
        uint16_t from_port = 0;
        auto stats_at = Clock::now() + std::chrono::seconds(1);

        while (!stop) {
            ssize_t n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 100);
            auto now = Clock::now();
            if (n > 0) {
                std::lock_guard<std::mutex> lock(mu);
                handle(buf.data(), static_cast<size_t>(n), from_ip, from_port, now);
                // whatever else is already waiting, without going back to sleep
                for (int i = 0; i < 512; ++i) {
                    n = sock->recv_from(buf.data(), buf.size(), from_ip, from_port, 0);
                    if (n <= 0) break;
                    handle(buf.data(), static_cast<size_t>(n), from_ip, from_port, now);
                }
            }

            if (now >= stats_at) {
                std::lock_guard<std::mutex> lock(mu);
                for (auto it = members.begin(); it != members.end();)
                    if (now - it->second.heard > std::chrono::milliseconds(kForgetMs))
                        it = members.erase(it);
                    else
                        ++it;
                char line[96];
                std::snprintf(line, sizeof(line), "relay %zu members, in %.1f Mbps, out %.1f Mbps",
                              members.size(), bytes_in * 8 / 1e6, bytes_out * 8 / 1e6);
                stats = line;
                bytes_in = bytes_out = 0;
                stats_at = now + std::chrono::seconds(1);
            }
        }
    }
};

Relay::Relay() : impl_(std::make_unique<Impl>()) {}
Relay::~Relay() {
    stop();
}

bool Relay::start(const RelayOptions& opt) {
    if (running()) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error = "already running";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;
    d.opt = opt;
    try {
        d.sock = std::make_unique<UdpSocket>(opt.port);
    } catch (const std::exception& e) {
        d.error = e.what();
        return false;
    }
    // every member's video arrives here in bursts
    d.sock->set_recv_buffer(4 << 20);
    d.running = true;
    d.worker = std::thread([&d] { d.loop(); });
    return true;
}

void Relay::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.worker.joinable()) d.worker.join();
    d.running = false;
    std::lock_guard<std::mutex> lock(d.mu);
    d.members.clear();
    d.stats = "relay stopped";
}

bool Relay::running() const {
    return impl_->running;
}

std::vector<RelayMember> Relay::members() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    std::vector<RelayMember> out;
    for (const auto& [id, m] : impl_->members) out.push_back({id, m.ip, m.in, m.out});
    return out;
}

std::string Relay::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->stats;
}

std::string Relay::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

}  // namespace net
