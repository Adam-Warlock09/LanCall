// Unit tests for call/call_logic.hpp (call targets, status line, peer list).

#include "call/call_logic.hpp"

#include <string>
#include <vector>

#include "check.hpp"

using call::resolve_target;
using call::status_line;
using call::StatusInfo;
using control::CallState;
using control::Peer;

namespace {

Peer peer(const std::string& name, const std::string& ip, uint16_t port = 5011, uint32_t seen = 0) {
    Peer p;
    p.name = name;
    p.ip = ip;
    p.signal_port = port;
    p.last_seen_us = seen;
    return p;
}

const std::vector<Peer> kPeers = {peer("Aric", "10.8.66.20"), peer("Rishika", "10.8.66.31", 6011),
                                  peer("akshat", "10.8.67.40")};

void test_by_name() {
    const auto t = resolve_target("Rishika", kPeers);
    CHECK(t.ok());
    CHECK(t.name == "Rishika");
    CHECK(t.ip == "10.8.66.31");
    CHECK(t.port == 6011);  // the port the peer announced, not the default
}

void test_name_ignoring_case() {
    const auto t = resolve_target("aric", kPeers);
    CHECK(t.ok());
    CHECK(t.ip == "10.8.66.20");
    const auto u = resolve_target("AKSHAT", kPeers);
    CHECK(u.ok());
    CHECK(u.name == "akshat");
}

void test_by_ip() {
    const auto known = resolve_target("10.8.66.20", kPeers);  // a known peer: name filled in
    CHECK(known.ok());
    CHECK(known.name == "Aric");
    const auto other_floor = resolve_target("10.8.64.119", kPeers);  // not discovered
    CHECK(other_floor.ok());
    CHECK(other_floor.name.empty());
    CHECK(other_floor.ip == "10.8.64.119");
    CHECK(other_floor.port == proto::kSignalPort);
}

void test_unknown_and_ambiguous() {
    const auto none = resolve_target("Bob", kPeers);
    CHECK(!none.ok());
    CHECK(none.error == "no peer named 'Bob' (type list, or call by IP)");

    const std::vector<Peer> twins = {peer("Aric", "10.8.66.20"), peer("Aric", "10.8.66.21")};
    const auto two = resolve_target("Aric", twins);
    CHECK(!two.ok());
    CHECK(two.error.find("10.8.66.20") != std::string::npos);
    CHECK(two.error.find("10.8.66.21") != std::string::npos);
    CHECK(two.error.find("call by IP") != std::string::npos);

    // An exact match wins over a case-insensitive one.
    const std::vector<Peer> cased = {peer("aric", "10.8.66.20"), peer("Aric", "10.8.66.21")};
    const auto exact = resolve_target("Aric", cased);
    CHECK(exact.ok());
    CHECK(exact.ip == "10.8.66.21");
    const auto loose = resolve_target("ARIC", cased);  // matches both ignoring case
    CHECK(!loose.ok());

    CHECK(!resolve_target("", kPeers).ok());
    CHECK(!resolve_target("Aric", {}).ok());
}

void test_default_port_when_unknown() {
    const auto t = resolve_target("Zero", {peer("Zero", "10.8.66.50", 0)});
    CHECK(t.ok());
    CHECK(t.port == proto::kSignalPort);
}

void test_status_lines() {
    StatusInfo s;
    s.my_name = "Atul";
    CHECK(status_line(s) == "Atul - idle (type list, call <name>)");

    s.state = CallState::Calling;
    s.peer_name = "Aric";
    s.peer_ip = "10.8.66.20";
    CHECK(status_line(s) == "Atul - calling Aric...");

    s.state = CallState::Ringing;
    CHECK(status_line(s) == "Atul - Aric (10.8.66.20) is calling: type accept or decline");

    s.state = CallState::InCall;
    s.media_parts = "audio+video";
    CHECK(status_line(s) == "Atul - in call with Aric (audio+video)");
    s.rtt_ms = 1.34;
    CHECK(status_line(s) == "Atul - in call with Aric (audio+video) rtt 1.3 ms");

    s.peer_name.clear();  // called by IP: show the IP, not "IP (IP)"
    s.state = CallState::Ringing;
    CHECK(status_line(s) == "Atul - 10.8.66.20 is calling: type accept or decline");
}

void test_peer_list() {
    CHECK(call::format_peers({}, 0).find("no peers yet") != std::string::npos);
    const std::string out =
        call::format_peers({peer("Aric", "10.8.66.20", 5011, 10000000)}, 11200000);
    CHECK(out.find("Aric") != std::string::npos);
    CHECK(out.find("10.8.66.20") != std::string::npos);
    CHECK(out.find("seen 1.2 s ago") != std::string::npos);
    // last_seen just before the 32-bit wrap, now just after it: still 1.0 s
    const std::string wrap =
        call::format_peers({peer("W", "1.2.3.4", 5011, 0xFFFFFFFFu - 499999)}, 500000);
    CHECK(wrap.find("seen 1.0 s ago") != std::string::npos);
}

}  // namespace

int main() {
    RUN(test_by_name);
    RUN(test_name_ignoring_case);
    RUN(test_by_ip);
    RUN(test_unknown_and_ambiguous);
    RUN(test_default_port_when_unknown);
    RUN(test_status_lines);
    RUN(test_peer_list);
    return check::result();
}
