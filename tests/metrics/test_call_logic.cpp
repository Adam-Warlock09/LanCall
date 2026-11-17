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
    // phase 3: peers heard only through the directory are marked
    CHECK(out.find("other floor") == std::string::npos);
    control::Peer far = peer("Bob", "10.8.72.5", 5011, 10000000);
    far.remote = true;
    CHECK(call::format_peers({far}, 11000000).find("(other floor)") != std::string::npos);
}

// ---- phase 4 ----------------------------------------------------------------------------

control::Member member(uint8_t id, const std::string& name, const std::string& ip,
                       bool me = false) {
    control::Member m;
    m.id = id;
    m.name = name;
    m.ip = ip;
    m.me = me;
    return m;
}

const std::vector<control::Member> kRoom = {member(1, "Atul", "", true),
                                            member(2, "Aric", "10.8.66.20"),
                                            member(3, "Rishika", "10.8.66.31")};

void test_direct_route() {
    const call::Route r = call::direct_route("127.0.0.1");
    CHECK(r.audio_to.size() == 1 && r.audio_to[0].ip == "127.0.0.1");
    CHECK(r.audio_to[0].port == proto::kAudioPort);
    CHECK(r.video_to.size() == 1 && r.video_to[0].port == proto::kVideoPort);
    CHECK(r.sources == (std::vector<std::string>{"127.0.0.1"}));
}

void test_media_sources() {
    using control::MediaMode;
    // mesh: everyone else, never ourselves
    CHECK(call::media_sources(kRoom, MediaMode::Mesh, "") ==
          (std::vector<std::string>{"10.8.66.20", "10.8.66.31"}));
    // relay: only the relay, it forwards everyone's streams from its own address
    CHECK(call::media_sources(kRoom, MediaMode::Relay, "10.8.66.5") ==
          (std::vector<std::string>{"10.8.66.5"}));
    // relay mode without a relay address yet: as mesh
    CHECK(call::media_sources(kRoom, MediaMode::Relay, "").size() == 2);
    // unknown ips left out, the same ip once (two members behind one address)
    const std::vector<control::Member> odd = {member(1, "Atul", "", true), member(2, "X", ""),
                                              member(3, "Y", "10.8.66.40"),
                                              member(4, "Z", "10.8.66.40")};
    CHECK(call::media_sources(odd, MediaMode::Mesh, "") ==
          (std::vector<std::string>{"10.8.66.40"}));
    CHECK(call::media_sources({}, MediaMode::Mesh, "").empty());
}

void test_format_members() {
    using control::MediaMode;
    CHECK(call::format_members({}, MediaMode::Mesh, "") == "  not in a call\n");
    const std::string mesh = call::format_members(kRoom, MediaMode::Mesh, "");
    CHECK(mesh.find("1 Atul") != std::string::npos);
    CHECK(mesh.find("(you, host)") != std::string::npos);
    CHECK(mesh.find("3 Rishika") != std::string::npos);
    CHECK(mesh.find("10.8.66.31") != std::string::npos);
    CHECK(mesh.find("media: mesh") != std::string::npos);
    // seen from a member: the host is marked, we are "(you)"
    const std::vector<control::Member> as_member = {member(1, "Aric", "10.8.66.20"),
                                                    member(2, "Atul", "", true)};
    const std::string m = call::format_members(as_member, MediaMode::Relay, "10.8.66.20");
    CHECK(m.find("10.8.66.20  (host)") != std::string::npos);
    CHECK(m.find("(you)") != std::string::npos);
    CHECK(m.find("media: relay 10.8.66.20") != std::string::npos);
}

void test_member_label() {
    CHECK(call::member_label(member(2, "Aric", "10.8.66.20")) == "Aric");
    CHECK(call::member_label(member(2, "", "10.8.66.20")) == "10.8.66.20");
    CHECK(call::member_label(member(4, "", "")) == "member 4");
}

void test_group_status_lines() {
    StatusInfo s;
    s.my_name = "Atul";
    s.state = CallState::InCall;
    s.peer_name = "Aric";
    s.media_parts = "audio+video";
    s.people = 2;  // a room of two reads as before
    CHECK(status_line(s) == "Atul - in call with Aric (audio+video)");
    s.people = 4;
    CHECK(status_line(s) == "Atul - group call, 4 people (audio+video)");
    s.relay_ip = "10.8.66.5";
    s.rtt_ms = 0.64;
    CHECK(status_line(s) ==
          "Atul - group call, 4 people (audio+video) via relay 10.8.66.5 rtt 0.6 ms");
}

void test_keepalive_packet() {
    const std::vector<uint8_t> p = call::keepalive_packet(3, 7, 0xFFFFFF00u);
    CHECK(p.size() == proto::kHeaderSize);  // a bare header, nothing else
    proto::Header h{};
    CHECK(proto::read_header(p.data(), p.size(), h));
    CHECK(h.type == proto::Type::Keepalive);
    CHECK(h.stream_id == 3);  // our member id: what the relay files us under
    CHECK(h.seq == 7);
    CHECK(h.timestamp_us == 0xFFFFFF00u);
    CHECK(h.frag_index == 0 && h.frag_count == 1);
    CHECK(call::kKeepaliveEveryUs < 5000000u);  // well inside the relay's 5 s forget time
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
    RUN(test_direct_route);
    RUN(test_media_sources);
    RUN(test_format_members);
    RUN(test_member_label);
    RUN(test_group_status_lines);
    RUN(test_keepalive_packet);
    return check::result();
}