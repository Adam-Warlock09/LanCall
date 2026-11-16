#include "call/call_logic.hpp"

#include <arpa/inet.h>

#include <cctype>
#include <cstdio>

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
            std::string line = s.my_name + " - in call with " + who;
            if (!s.media_parts.empty()) line += " (" + s.media_parts + ")";
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
        std::snprintf(line, sizeof line, "  %-20s %-15s seen %.1f s ago%s\n", p.name.c_str(),
                      p.ip.c_str(), ago_s < 0 ? 0.0 : ago_s, p.remote ? "  (other floor)" : "");
        out += line;
    }
    return out;
}

}  // namespace call