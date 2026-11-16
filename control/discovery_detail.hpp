#pragma once
// The parts of discovery that don't need a network (Person 1, control/). Split out of
// discovery.cpp so the tests can check them directly. Not frozen.
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
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

// Everyone we've heard from, keyed by their per-run id (so a restarted laptop with the
// same name shows up as itself, not twice). Not thread-safe; Discovery locks around it.
class PeerTable {
public:
    void seen(const std::string& id, const Peer& p) { peers_[id] = p; }

    // peers heard within the timeout, sorted by name then ip
    std::vector<Peer> alive(uint32_t now_us) const {
        std::vector<Peer> out;
        for (const auto& kv : peers_)
            if (now_us - kv.second.last_seen_us <= kPeerTimeoutUs) out.push_back(kv.second);
        std::sort(out.begin(), out.end(), [](const Peer& a, const Peer& b) {
            return a.name != b.name ? a.name < b.name : a.ip < b.ip;
        });
        return out;
    }

    // forget peers that timed out, so the table doesn't grow forever
    void prune(uint32_t now_us) {
        for (auto it = peers_.begin(); it != peers_.end();) {
            if (now_us - it->second.last_seen_us > kPeerTimeoutUs)
                it = peers_.erase(it);
            else
                ++it;
        }
    }

    size_t size() const { return peers_.size(); }

private:
    std::map<std::string, Peer> peers_;
};

}  // namespace control::detail