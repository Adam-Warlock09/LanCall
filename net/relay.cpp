// Group-call relay (Person 1, net/, phase 4). The forwarding rules are in relay.hpp.
//
// One thread, one socket. It never decodes anything: it reads the 12-byte header (and for
// reports and NACKs the body) and copies the datagram on. Everything the API reads (members,
// stats) sits behind one mutex, which the thread takes once per datagram.
//
// Phase 5: it keeps the last kCacheSlots video packets of every member, so a NACK for a
// packet lost between the relay and one receiver is answered right here, to that receiver
// only. If some of it never reached the relay, the receiver's NACK goes on to the sender
// as it is (with phase 6 encryption it's signed, and the relay can't sign a new one). The
// sender then resends everything it was asked for: what the relay already has is dropped
// here as a duplicate, and the rest goes to everyone (nobody had it).
#include "net/relay.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <map>
#include <mutex>
#include <thread>

#include "common/nack.hpp"
#include "common/report.hpp"
#include "common/udp_socket.hpp"

namespace net {

namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kCacheSlots = 512;  // per member, ~1.5 s of video; a power of 2, so seqs wrap
constexpr auto kCacheAge = std::chrono::seconds(1);  // older isn't worth resending anyway
}  // namespace

struct Relay::Impl {
    struct Member {
        std::string ip;
        Clock::time_point heard;
        // the sockets their video and audio come from: where reports about them go
        std::string video_ip, audio_ip;
        uint16_t video_port = 0, audio_port = 0;
        uint64_t in = 0, out = 0;

        // their recent video datagrams, slot = seq % kCacheSlots
        struct Cached {
            bool used = false;
            uint16_t seq = 0;
            Clock::time_point at;
            std::vector<uint8_t> data;
        };
        std::vector<Cached> cache;

        Cached* cached(uint16_t seq, Clock::time_point now) {
            if (cache.empty()) return nullptr;
            Cached& c = cache[seq % kCacheSlots];
            if (!c.used || c.seq != seq || now - c.at > kCacheAge) return nullptr;
            return &c;
        }
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
    uint64_t resent = 0;  // NACKed packets answered from the cache, this second

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

        // FEC parity (phase 5) travels with the video: same socket, same receivers
        if (h.type == proto::Type::Video || h.type == proto::Type::Audio ||
            h.type == proto::Type::VideoFec) {
            // stream_id 0 is a phase 3 sender, or someone outside a group call: there's no way
            // to tell two of them apart, so they aren't forwarded
            if (h.stream_id == 0) return;
            Member& src = seen(h.stream_id, from_ip, now);
            src.in++;
            bool video = h.type != proto::Type::Audio;
            (video ? src.video_ip : src.audio_ip) = from_ip;
            (video ? src.video_port : src.audio_port) = from_port;
            if (h.type == proto::Type::Video) {
                // a resend of something everyone already got from us: don't send it again
                if (src.cached(h.seq, now)) return;
                if (src.cache.empty()) src.cache.resize(kCacheSlots);
                auto& c = src.cache[h.seq % kCacheSlots];
                c.used = true;
                c.seq = h.seq;
                c.at = now;
                c.data.assign(data, data + n);
            }
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
        } else if (h.type == proto::Type::Nack) {
            handle_nack(data, n, from_ip, now);
        } else if (h.type == proto::Type::Keepalive) {
            if (h.stream_id != 0) seen(h.stream_id, from_ip, now).in++;
        }
    }

    // a receiver asks for packets of stream h.stream_id again
    void handle_nack(const uint8_t* data, size_t n, const std::string& from_ip,
                     Clock::time_point now) {
        proto::Header h;
        proto::Nack nk;
        if (!proto::read_nack_packet(data, n, h, nk)) return;
        if (nk.reporter != 0) seen(nk.reporter, from_ip, now).in++;
        auto it = members.find(h.stream_id);
        if (it == members.end()) return;
        Member& s = it->second;

        if (nk.media != proto::Type::Video) {  // nothing cached for audio: pass it on as is
            if (!s.audio_ip.empty() && s.audio_port != 0) {
                sock->send_to(s.audio_ip, s.audio_port, data, n);
                bytes_out += n;
            }
            return;
        }

        // answer what we have, to the one who asked
        std::string to = from_ip;
        if (nk.reporter != 0 && members.count(nk.reporter)) to = members[nk.reporter].ip;
        bool missing = false;
        for (uint16_t seq : nk.seqs) {
            if (auto* c = s.cached(seq, now)) {
                sock->send_to(to, opt.video_port, c->data.data(), c->data.size());
                bytes_out += c->data.size();
                resent++;
            } else {
                missing = true;
            }
        }
        // some never reached us: pass the NACK on to the sender unchanged
        if (missing && !s.video_ip.empty() && s.video_port != 0) {
            sock->send_to(s.video_ip, s.video_port, data, n);
            bytes_out += n;
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
                char line[128];
                std::snprintf(line, sizeof(line),
                              "relay %zu members, in %.1f Mbps, out %.1f Mbps, resent %llu",
                              members.size(), bytes_in * 8 / 1e6, bytes_out * 8 / 1e6,
                              static_cast<unsigned long long>(resent));
                stats = line;
                bytes_in = bytes_out = 0;
                resent = 0;
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
