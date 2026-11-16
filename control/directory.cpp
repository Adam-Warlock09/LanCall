// Peer directory for calls across floors (Person 1, control/). The format is in
// directory.hpp; the peer table and line formats are shared with discovery
// (discovery_detail.hpp).
//
// One thread: wait for a beacon, remember who sent it and from where, then send that sender
// one PEER line for every other peer still alive. Clients beacon every 2 s, so every client
// gets a fresh copy of the list every 2 s without the directory keeping any timers of its own.
#include "control/directory.hpp"

#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "common/udp_socket.hpp"
#include "control/discovery_detail.hpp"

namespace control {

struct DirectoryServer::Impl {
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    mutable std::mutex mu;
    detail::PeerTable table;
    std::string error;

    void loop() {
        std::vector<uint8_t> buf(512);
        std::string ip;
        uint16_t port = 0;
        uint32_t next_prune = proto::now_us();

        while (!stop) {
            ssize_t n = sock->recv_from(buf.data(), buf.size(), ip, port, 200);
            uint32_t now = proto::now_us();

            if (static_cast<int32_t>(now - next_prune) >= 0) {
                std::lock_guard<std::mutex> lock(mu);
                table.prune(now);
                next_prune = now + detail::kBeaconEveryUs;
            }
            if (n <= 0) continue;

            std::string text(reinterpret_cast<const char*>(buf.data()), n);
            std::string id, name;
            uint16_t sig = 0;
            if (!detail::parse_beacon(text, id, name, sig)) continue;

            Peer p;
            p.name = name;
            p.ip = ip;  // where we heard it from, which is what other floors must call
            p.signal_port = sig;
            p.last_seen_us = now;

            std::vector<std::pair<std::string, Peer>> others;
            {
                std::lock_guard<std::mutex> lock(mu);
                table.seen(id, p);
                others = table.entries(now);
            }
            for (const auto& [oid, other] : others) {
                if (oid == id) continue;  // nobody needs to be told about themselves
                uint32_t age_ms = (now - other.last_seen_us) / 1000;
                std::string line = detail::make_peer_line(oid, other, age_ms);
                sock->send_to(ip, port, reinterpret_cast<const uint8_t*>(line.data()), line.size());
            }
        }
    }
};

DirectoryServer::DirectoryServer() : impl_(std::make_unique<Impl>()) {}
DirectoryServer::~DirectoryServer() {
    stop();
}

bool DirectoryServer::start(uint16_t port) {
    if (impl_->running) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error = "already running";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;
    try {
        d.sock = std::make_unique<UdpSocket>(port);
    } catch (const std::exception& e) {
        d.error = e.what();
        return false;
    }
    d.running = true;
    d.worker = std::thread([&d] { d.loop(); });
    return true;
}

void DirectoryServer::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.worker.joinable()) d.worker.join();
    d.running = false;
}

bool DirectoryServer::running() const {
    return impl_->running;
}

size_t DirectoryServer::peer_count() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->table.entries(proto::now_us()).size();
}

std::string DirectoryServer::table() const {
    uint32_t now = proto::now_us();
    std::vector<std::pair<std::string, Peer>> list;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        list = impl_->table.entries(now);
    }
    std::string out;
    char line[128];
    for (const auto& kv : list) {
        const Peer& p = kv.second;
        std::snprintf(line, sizeof(line), "  %-16s %-15s port %-5u seen %.1f s ago\n",
                      p.name.c_str(), p.ip.c_str(), p.signal_port, (now - p.last_seen_us) / 1e6);
        out += line;
    }
    return out;
}

std::string DirectoryServer::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

}  // namespace control