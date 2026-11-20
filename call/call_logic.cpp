#include "call/call_logic.hpp"

#include <cctype>
#include <cstdio>

#include "common/platform.hpp"

namespace call {

namespace {

bool is_ipv4(const std::string& s) {
    in_addr addr{};
    return inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

bool same_ignoring_case(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

Target from_peer(const control::Peer& p) {
    Target t;
    t.name = p.name;
    t.ip = p.ip;
    t.port = p.signal_port != 0 ? p.signal_port : proto::kSignalPort;
    return t;
}

std::string ips_of(const std::vector<const control::Peer*>& list) {
    std::string s;
    for (const control::Peer* p : list) s += (s.empty() ? "" : ", ") + p->name + " " + p->ip;
    return s;
}

}  // namespace

Target resolve_text_target(const std::string& arg, const std::vector<control::Member>& members,
                           const std::vector<control::Peer>& peers) {
    const control::Member* exact = nullptr;
    const control::Member* loose = nullptr;
    int loose_hits = 0;
    for (const control::Member& m : members) {
        if (m.me) continue;
        if (m.ip == arg || m.name == arg) {
            exact = &m;
            break;
        }
        if (same_ignoring_case(m.name, arg)) {
            loose = &m;
            loose_hits++;
        }
    }
    const control::Member* hit = exact ? exact : (loose_hits == 1 ? loose : nullptr);
    if (hit) {
        Target t;
        t.name = hit->name;
        t.ip = hit->ip;
        return t;
    }
    Target t = resolve_target(arg, peers);
    if (!t.ok() && arg.empty()) t.error = "usage: text <all|name|ip> <message>";
    return t;
}

Target resolve_target(const std::string& arg, const std::vector<control::Peer>& peers) {
    Target t;
    if (arg.empty()) {
        t.error = "usage: call <name|ip>";
        return t;
    }
    if (is_ipv4(arg)) {
        t.ip = arg;
        for (const control::Peer& p : peers) {
            if (p.ip == arg) return from_peer(p);  // a known peer: use its name and port
        }
        return t;
    }

    std::vector<const control::Peer*> exact, loose;
    for (const control::Peer& p : peers) {
        if (p.name == arg) exact.push_back(&p);
        if (same_ignoring_case(p.name, arg)) loose.push_back(&p);
    }
    const std::vector<const control::Peer*>& hits = exact.empty() ? loose : exact;
    if (hits.size() == 1) return from_peer(*hits[0]);
    if (hits.empty()) {
        t.error = "no peer named '" + arg + "' (type list, or call by IP)";
    } else {
        t.error = "several peers match '" + arg + "' (" + ips_of(hits) + "): call by IP";
    }
    return t;
}

std::string status_line(const StatusInfo& s) {
    const std::string who = !s.peer_name.empty() ? s.peer_name : s.peer_ip;
    switch (s.state) {
        case control::CallState::Idle:
            return s.my_name + " - idle (type list, call <name>)";
        case control::CallState::Calling:
            return s.my_name + " - calling " + who + "...";
        case control::CallState::Ringing:
            return s.my_name + " - " + who +
                   (s.peer_ip.empty() || who == s.peer_ip ? "" : " (" + s.peer_ip + ")") +
                   " is calling: type accept or decline";
        case control::CallState::InCall: {
            std::string line =
                s.people > 2 ? s.my_name + " - group call, " + std::to_string(s.people) + " people"
                             : s.my_name + " - in call with " + who;
            if (!s.media_parts.empty()) line += " (" + s.media_parts + ")";
            if (!s.relay_ip.empty()) line += " via relay " + s.relay_ip;
            if (!s.multicast_group.empty()) line += " via multicast " + s.multicast_group;
            if (s.rtt_ms > 0) {
                char rtt[32];
                std::snprintf(rtt, sizeof rtt, " rtt %.1f ms", s.rtt_ms);
                line += rtt;
            }
            return line;
        }
    }
    return s.my_name;
}

std::string format_peers(const std::vector<control::Peer>& peers, uint32_t now_us) {
    if (peers.empty())
        return "  no peers yet (beacons every 2 s; other floors only with --directory)\n";
    std::string out;
    for (const control::Peer& p : peers) {
        const double ago_s = static_cast<int32_t>(now_us - p.last_seen_us) / 1e6;
        char line[160];
        std::snprintf(line, sizeof line, "  %-20s %-15s seen %.1f s ago%s", p.name.c_str(),
                      p.ip.c_str(), ago_s < 0 ? 0.0 : ago_s, p.remote ? "  (other floor)" : "");
        out += line;
        if (!p.remote && p.group != 0 && !p.broadcast_title.empty())
            out += "  broadcasting \"" + p.broadcast_title + "\" (watch " + p.name + ")";
        out += "\n";
    }
    return out;
}

BroadcastTarget resolve_broadcast(const std::string& arg, const std::vector<control::Peer>& peers) {
    BroadcastTarget t;
    if (arg.empty()) {
        t.error = "usage: watch <name|ip>";
        return t;
    }
    // who arg means: an ip, an exact name, or else a name ignoring case
    std::vector<const control::Peer*> exact, loose;
    for (const control::Peer& p : peers) {
        if (p.ip == arg || p.name == arg) exact.push_back(&p);
        if (same_ignoring_case(p.name, arg)) loose.push_back(&p);
    }
    const std::vector<const control::Peer*>& matches = exact.empty() ? loose : exact;
    if (matches.empty()) {
        t.error = "no peer named '" + arg + "' (type list)";
        return t;
    }
    std::vector<const control::Peer*> live;
    bool far = false;
    for (const control::Peer* p : matches) {
        if (p->remote) far = true;
        if (!p->remote && p->group != 0 && !p->broadcast_title.empty()) live.push_back(p);
    }
    if (live.size() > 1) {
        t.error = "several broadcasts match '" + arg + "' (" + ips_of(live) +
                  "): be more specific (a name or IP from list)";
        return t;
    }
    if (live.empty()) {
        t.error = far && matches.size() == 1
                      ? arg + " is on another floor: broadcasts only reach their own floor"
                      : arg + " isn't broadcasting";
        return t;
    }
    t.name = live[0]->name;
    t.ip = live[0]->ip;
    t.title = live[0]->broadcast_title;
    t.group = live[0]->group;
    return t;
}

std::string broadcast_status(const std::string& my_name, const std::string& title,
                             const std::string& group_ip) {
    return my_name + " - broadcasting \"" + title + "\" on " + group_ip;
}

std::string watch_status(const std::string& my_name, const std::string& presenter,
                         const std::string& title) {
    return my_name + " - watching " + presenter + ": \"" + title + "\"";
}

Route direct_route(const std::string& peer_ip) {
    Route r;
    r.audio_to = {{peer_ip, proto::kAudioPort}};
    r.video_to = {{peer_ip, proto::kVideoPort}};
    r.sources = {peer_ip};
    return r;
}

std::vector<std::string> media_sources(const std::vector<control::Member>& members,
                                       control::MediaMode mode, const std::string& relay_ip) {
    if (mode == control::MediaMode::Relay && !relay_ip.empty()) return {relay_ip};
    // mesh, and multicast (phase 7): group packets carry their sender's own address
    std::vector<std::string> out;
    for (const control::Member& m : members) {
        if (m.me || m.ip.empty()) continue;
        bool seen = false;
        for (const std::string& ip : out) seen = seen || ip == m.ip;
        if (!seen) out.push_back(m.ip);
    }
    return out;
}

std::vector<uint8_t> keepalive_packet(uint8_t my_id, uint16_t seq, uint32_t now_us) {
    proto::Header h{};
    h.type = proto::Type::Keepalive;
    h.stream_id = my_id;
    h.seq = seq;
    h.timestamp_us = now_us;
    h.frame_id = 0;
    h.frag_index = 0;
    h.frag_count = 1;
    std::vector<uint8_t> out(proto::kHeaderSize);
    proto::write_header(h, out.data());
    return out;
}

std::string member_label(const control::Member& m) {
    if (!m.name.empty()) return m.name;
    if (!m.ip.empty()) return m.ip;
    return "member " + std::to_string(m.id);
}

std::string format_members(const std::vector<control::Member>& members, control::MediaMode mode,
                           const std::string& address) {
    if (members.empty()) return "  not in a call\n";
    std::string out;
    for (const control::Member& m : members) {
        std::string note = m.me ? "(you" : m.ip;
        if (m.id == 1) note += m.me ? ", host" : "  (host)";
        if (m.me) note += ")";
        char line[160];
        std::snprintf(line, sizeof line, "  %u %-20s %s\n", static_cast<unsigned>(m.id),
                      member_label(m).c_str(), note.c_str());
        out += line;
    }
    if (mode == control::MediaMode::Relay && !address.empty()) {
        out += "  media: relay " + address + " (everyone sends one copy to the relay)\n";
    } else if (mode == control::MediaMode::Multicast && !address.empty()) {
        out += "  media: multicast " + address + " (everyone sends one copy to the group)\n";
    } else {
        out += "  media: mesh (everyone sends to everyone)\n";
    }
    return out;
}

std::vector<control::Member> off_floor_members(const std::vector<control::Member>& members,
                                               const std::vector<control::Peer>& peers) {
    std::vector<control::Member> out;
    for (const control::Member& m : members) {
        if (m.me) continue;
        bool here = false;
        for (const control::Peer& p : peers) here = here || (!p.remote && p.ip == m.ip);
        if (!here) out.push_back(m);
    }
    return out;
}

}  // namespace call