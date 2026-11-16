// Peer discovery on the floor network (Person 1, control/). The beacon format and the
// rules are in discovery.hpp; the network-free parts are in discovery_detail.hpp.
//
// One thread does everything: it sends our beacon every 2 s and, in between, listens for
// everyone else's. Beacons go to 255.255.255.255, which stays on our own floor's /23.
#include "control/discovery.hpp"

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
                bool ok =
                    sock->send_to("255.255.255.255", proto::kDiscoveryPort,
                                  reinterpret_cast<const uint8_t*>(beacon.data()), beacon.size());
                next_beacon = now + detail::kBeaconEveryUs;
                std::lock_guard<std::mutex> lock(mu);
                error = ok ? "" : "can't send beacons (cable unplugged?)";
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
            if (!detail::parse_beacon(text, pid, pname, psig) || pid == id) continue;

            Peer p;
            p.name = pname;
            p.ip = ip;  // where the beacon came from, not anything the beacon claims
            p.signal_port = psig;
            p.last_seen_us = proto::now_us();
            std::lock_guard<std::mutex> lock(mu);
            table.seen(pid, p);
        }
    }
};

Discovery::Discovery() : impl_(std::make_unique<Impl>()) {}
Discovery::~Discovery() {
    stop();
}

bool Discovery::start(const std::string& my_name, uint16_t signal_port) {
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