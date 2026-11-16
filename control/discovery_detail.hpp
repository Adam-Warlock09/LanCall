#pragma once
// The parts of discovery and the directory that don't need a network (Person 1, control/).
// Split out so the tests can check them directly. Not frozen.
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "control/discovery.hpp"
#include "control/wire.hpp"

namespace control::detail {

constexpr uint32_t kBeaconEveryUs = 2000000;  // 2 s
constexpr uint32_t kPeerTimeoutUs = 6000000;  // 6 s, i.e. three missed beacons

inline std::string make_beacon(const std::string& id, const std::string& name, uint16_t sig) {
    return "LANCALL 1 id=" + id + " name=" + name + " sig=" + std::to_string(sig);
}

inline bool parse_beacon(const std::string& text, std::string& id, std::string& name,
                         uint16_t& sig) {
    wire::Line l;
    if (!wire::parse(text, l) || l.verb != "LANCALL") return false;
    if (l.args.empty() || l.args[0] != "1") return false;  // we only speak version 1
    id = wire::get(l, "id");
    name = wire::get(l, "name");
    uint32_t port = 0;
    if (id.empty() || id.size() > 16 || !valid_name(name)) return false;
    if (!wire::get_u32(l, "sig", port) || port == 0 || port > 65535) return false;
    sig = static_cast<uint16_t>(port);
    return true;
}

// directory -> client, one per known peer:
//   PEER 1 id=7c01aa2e name=Atul ip=10.8.66.20 sig=5011 age=1500
// age = ms since the directory last heard that peer, so the client can work out when it
// was really last seen (an optional extra on top of directory.hpp's format)
inline std::string make_peer_line(const std::string& id, const Peer& p, uint32_t age_ms) {
    return "PEER 1 id=" + id + " name=" + p.name + " ip=" + p.ip +
           " sig=" + std::to_string(p.signal_port) + " age=" + std::to_string(age_ms);
}

inline bool parse_peer_line(const std::string& text, std::string& id, Peer& p, uint32_t& age_ms) {
    wire::Line l;
    if (!wire::parse(text, l) || l.verb != "PEER") return false;
    if (l.args.empty() || l.args[0] != "1") return false;
    id = wire::get(l, "id");
    p.name = wire::get(l, "name");
    p.ip = wire::get(l, "ip");
    uint32_t port = 0;
    if (id.empty() || id.size() > 16 || !valid_name(p.name)) return false;
    if (p.ip.empty() || p.ip.size() > 15) return false;
    if (!wire::get_u32(l, "sig", port) || port == 0 || port > 65535) return false;
    p.signal_port = static_cast<uint16_t>(port);
    if (!wire::get_u32(l, "age", age_ms)) age_ms = 0;  // older directories may not send it
    return true;
}

// Everyone we've heard from, keyed by their per-run id (so a restarted laptop with the
// same name shows up as itself, not twice). A peer can be heard two ways: by broadcast on
// our own floor, or through the directory. Both are tracked, and a peer counts as local
// while its broadcasts are still fresh. Not thread-safe; callers lock around it.
class PeerTable {
public:
    // heard its broadcast beacon (same floor)
    void seen(const std::string& id, const Peer& p) {
        Entry& e = peers_[id];
        e.peer = p;
        e.local = true;
        e.local_us = p.last_seen_us;
    }

    // told about it by the directory. never overwrites a fresh local sighting's details
    void seen_remote(const std::string& id, const Peer& p) {
        Entry& e = peers_[id];
        if (!e.local) e.peer = p;
        e.remote = true;
        e.remote_us = p.last_seen_us;
    }

    // peers heard within the timeout, sorted by name then ip
    std::vector<Peer> alive(uint32_t now_us) const {
        std::vector<Peer> out;
        for (const auto& kv : entries(now_us)) out.push_back(kv.second);
        return out;
    }

    // same, with their ids (the directory needs those for its PEER lines)
    std::vector<std::pair<std::string, Peer>> entries(uint32_t now_us) const {
        std::vector<std::pair<std::string, Peer>> out;
        for (const auto& kv : peers_) {
            const Entry& e = kv.second;
            bool local = e.local && now_us - e.local_us <= kPeerTimeoutUs;
            bool remote = e.remote && now_us - e.remote_us <= kPeerTimeoutUs;
            if (!local && !remote) continue;
            Peer p = e.peer;
            p.remote = !local;
            p.last_seen_us = local ? e.local_us : e.remote_us;
            out.emplace_back(kv.first, p);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
            return a.second.name != b.second.name ? a.second.name < b.second.name
                                                  : a.second.ip < b.second.ip;
        });
        return out;
    }

    // forget peers that timed out both ways, so the table doesn't grow forever
    void prune(uint32_t now_us) {
        for (auto it = peers_.begin(); it != peers_.end();) {
            Entry& e = it->second;
            if (e.local && now_us - e.local_us > kPeerTimeoutUs) e.local = false;
            if (e.remote && now_us - e.remote_us > kPeerTimeoutUs) e.remote = false;
            if (!e.local && !e.remote)
                it = peers_.erase(it);
            else
                ++it;
        }
    }

    size_t size() const { return peers_.size(); }

private:
    struct Entry {
        Peer peer;
        bool local = false, remote = false;
        uint32_t local_us = 0, remote_us = 0;
    };
    std::map<std::string, Entry> peers_;
};

}  // namespace control::detail