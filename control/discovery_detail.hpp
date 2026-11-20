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

// ---- phase 7 (Person 2): what a beacon announces besides the peer itself -----------------

constexpr size_t kMaxTitleBytes = 48;  // a broadcast's title, before escaping

// group: we send to multicast group proto::lancall_group(group), 1..254 (0 = nothing).
// title: non-empty = it's a broadcast anyone on the floor can watch.
struct Announce {
    uint8_t group = 0;
    std::string title;
};

// A title as it goes on the wire and on screen: control characters become spaces, and it's
// cut to kMaxTitleBytes without splitting a UTF-8 character.
inline std::string clean_title(std::string t) {
    for (char& c : t) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f) c = ' ';
    }
    if (t.size() > kMaxTitleBytes) {
        size_t cut = kMaxTitleBytes;
        // the first byte cut off is a continuation byte (10xxxxxx): the character it belongs
        // to started before the cut, so drop that whole character too
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
        t.resize(cut);
    }
    return t;
}

inline std::string make_beacon(const std::string& id, const std::string& name, uint16_t sig,
                               const Announce& a = {}) {
    std::string b = "LANCALL 1 id=" + id + " name=" + name + " sig=" + std::to_string(sig);
    if (a.group == 0 || a.group == 255) return b;
    b += " mc=" + std::to_string(a.group);
    const std::string title = clean_title(a.title);
    if (!title.empty()) b += " title=" + wire::escape(title);
    return b;
}

// Also reads the phase 7 fields. A bad or missing mc= just means no announcement, and a
// title only counts together with a group; the beacon itself is still valid either way.
inline bool parse_beacon(const std::string& text, std::string& id, std::string& name, uint16_t& sig,
                         Announce& a) {
    wire::Line l;
    if (!wire::parse(text, l) || l.verb != "LANCALL") return false;
    if (l.args.empty() || l.args[0] != "1") return false;  // we only speak version 1
    id = wire::get(l, "id");
    name = wire::get(l, "name");
    uint32_t port = 0;
    if (id.empty() || id.size() > 16 || !valid_name(name)) return false;
    if (!wire::get_u32(l, "sig", port) || port == 0 || port > 65535) return false;
    sig = static_cast<uint16_t>(port);

    a = Announce{};
    uint32_t group = 0;
    if (wire::get_u32(l, "mc", group) && group >= 1 && group <= 254) {
        a.group = static_cast<uint8_t>(group);
        std::string title;
        if (wire::unescape(wire::get(l, "title"), title)) a.title = clean_title(title);
    }
    return true;
}

// The pre-phase 7 reading, which ignores the extra fields (the directory uses this).
inline bool parse_beacon(const std::string& text, std::string& id, std::string& name,
                         uint16_t& sig) {
    Announce ignored;
    return parse_beacon(text, id, name, sig, ignored);
}

// Where a laptop with this beacon id starts looking for a free group: a hash of the id, so
// two laptops rarely start on the same number, and one laptop keeps its number across
// broadcasts in a run. FNV-1a, 1..254.
inline uint8_t first_group_choice(const std::string& id) {
    uint32_t h = 2166136261u;
    for (unsigned char c : id) {
        h ^= c;
        h *= 16777619u;
    }
    return static_cast<uint8_t>(1 + h % 254);
}

// The first group from first_group_choice(id) on, wrapping 254 -> 1, that no peer on our
// floor announces. Peers on other floors don't count: multicast never reaches them.
// 0 if all 254 are taken.
inline uint8_t pick_group(const std::string& id, const std::vector<Peer>& peers) {
    bool used[256] = {};
    for (const Peer& p : peers)
        if (!p.remote) used[p.group] = true;
    uint8_t g = first_group_choice(id);
    for (int i = 0; i < 254; ++i) {
        if (!used[g]) return g;
        g = g == 254 ? 1 : static_cast<uint8_t>(g + 1);
    }
    return 0;
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
            if (!local) {  // a group or broadcast heard on our floor earlier is over by now
                p.group = 0;
                p.broadcast_title.clear();
            }
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