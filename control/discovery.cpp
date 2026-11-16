// Peer discovery on the floor network (Person 1, control/). The beacon format and the
// rules are in discovery.hpp; the network-free parts are in discovery_detail.hpp.
//
// One thread does everything: it sends our beacon every 2 s and, in between, listens for
// everyone else's. Beacons go to 255.255.255.255, which stays on our own floor's /23. With a
// directory, the beacon also goes straight to it, and its PEER replies (peers on other
// floors) arrive on this same socket.
#include "control/discovery.hpp"

#include <arpa/inet.h>

#include <atomic>
#include <cstdio>
#include <exception>
#include <mutex>
#include <random>
#include <thread>

#include "common/protocol.hpp"
#include "common/udp_socket.hpp"
#include "control/discovery_detail.hpp"

namespace control {

namespace {

std::string random_id() {
    std::random_device rd;
    char buf[9];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rd()));
    return buf;
}

}  // namespace

struct Discovery::Impl {
    std::string name;
    std::string id;  // random per run, so we can recognise (and skip) our own beacons
    uint16_t sig = 0;
    std::string directory;  // directory IP for other floors, empty = this floor only

    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    mutable std::mutex mu;
    detail::PeerTable table;
    std::string error;

    void loop() {
        const std::string beacon = detail::make_beacon(id, name, sig);
        std::vector<uint8_t> buf(512);
        std::string ip;
        uint16_t port = 0;
        uint32_t next_beacon = proto::now_us();

        while (!stop) {
            uint32_t now = proto::now_us();
            if (static_cast<int32_t>(now - next_beacon) >= 0) {
                const auto* bytes = reinterpret_cast<const uint8_t*>(beacon.data());
                bool ok =
                    sock->send_to("255.255.255.255", proto::kDiscoveryPort, bytes, beacon.size());
                // the same line goes straight to the directory, which crosses floor routers
                bool dir_ok = directory.empty() ||
                              sock->send_to(directory, proto::kDirectoryPort, bytes, beacon.size());
                next_beacon = now + detail::kBeaconEveryUs;
                std::lock_guard<std::mutex> lock(mu);
                if (!ok)
                    error = "can't send beacons (cable unplugged?)";
                else if (!dir_ok)
                    error = "can't reach the directory at " + directory;
                else
                    error.clear();
                table.prune(now);
            }

            // sleep until the next beacon is due, but wake up for anything that arrives.
            // capped at 200 ms so stop() never waits long
            int32_t wait_ms = static_cast<int32_t>(next_beacon - proto::now_us()) / 1000;
            if (wait_ms < 0) wait_ms = 0;
            if (wait_ms > 200) wait_ms = 200;

            ssize_t n = sock->recv_from(buf.data(), buf.size(), ip, port, wait_ms);
            if (n <= 0) continue;

            std::string text(reinterpret_cast<const char*>(buf.data()), n);
            std::string pid, pname;
            uint16_t psig = 0;

            if (detail::parse_beacon(text, pid, pname, psig)) {
                if (pid == id) continue;  // our own broadcast coming back
                Peer p;
                p.name = pname;
                p.ip = ip;  // where the beacon came from, not anything the beacon claims
                p.signal_port = psig;
                p.last_seen_us = proto::now_us();
                std::lock_guard<std::mutex> lock(mu);
                table.seen(pid, p);
                continue;
            }

            // a PEER line counts only if it really came from our directory, otherwise anyone
            // on the floor could add fake peers to everyone's list
            Peer p;
            uint32_t age_ms = 0;
            if (directory.empty() || ip != directory) continue;
            if (!detail::parse_peer_line(text, pid, p, age_ms) || pid == id) continue;
            if (age_ms > detail::kPeerTimeoutUs / 1000) continue;
            p.last_seen_us = proto::now_us() - age_ms * 1000;
            std::lock_guard<std::mutex> lock(mu);
            table.seen_remote(pid, p);
        }
    }
};

Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}
Discovery::~Discovery() {
    stop();
}

bool Discovery::start(const std::string& my_name, uint16_t signal_port,
                      const std::string& directory_ip) {
    if (impl_->running) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error = "already running";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;

    if (!valid_name(my_name)) {
        d.error = "bad name '" + my_name + "' (letters, digits, - and _, at most 32)";
        return false;
    }
    if (signal_port == 0) {
        d.error = "signal port can't be 0";
        return false;
    }
    in_addr tmp{};
    if (!directory_ip.empty() && inet_pton(AF_INET, directory_ip.c_str(), &tmp) != 1) {
        d.error = "not an IPv4 address: " + directory_ip;
        return false;
    }
    d.directory = directory_ip;
    try {
        // SO_REUSEADDR (set by UdpSocket) lets two copies on one laptop share this port,
        // and broadcasts are delivered to both, which is handy for testing
        d.sock = std::make_unique<UdpSocket>(proto::kDiscoveryPort);
    } catch (const std::exception& e) {
        d.error = e.what();
        return false;
    }
    d.sock->enable_broadcast();
    d.name = my_name;
    d.sig = signal_port;
    d.id = random_id();

    d.running = true;
    d.worker = std::thread([&d] { d.loop(); });
    return true;
}

void Discovery::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    if (d.worker.joinable()) d.worker.join();
    d.running = false;
}

std::vector<Peer> Discovery::peers() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->table.alive(proto::now_us());
}

std::string Discovery::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    for (char c : name) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

}  // namespace control