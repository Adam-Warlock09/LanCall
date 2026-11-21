// Tests for signaling (Person 1): the text format and clock-sync math without a network,
// then real calls between Signaling objects over loopback, and (phase 6) connections driven
// by hand to check what's on the wire. Takes ~35 seconds.
// Exit code 0 = all passed. Uses TCP ports 25011-25013 on this laptop.
//
// Group calls need a different IP per member (the host tells members apart by IP), so those
// tests ring 127.0.0.2 and 127.0.0.3: on Linux and Windows the whole 127.0.0.0/8 is this
// laptop.
#include "tests/sock_compat.hpp"  // raw sockets on Linux and Windows (phase 8)

#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "control/secure_line.hpp"
#include "control/signaling.hpp"
#include "control/signaling_detail.hpp"
#include "control/wire.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using namespace control;
using Kind = CallEvent::Kind;

constexpr uint16_t kPortA = 25011, kPortB = 25012, kPortC = 25013;

static void test_wire() {
    std::puts("wire format");
    wire::Line l;
    CHECK(wire::parse("PONG t1=10 t2=20 t3=4294967295\r", l));
    CHECK(l.verb == "PONG" && wire::get(l, "t2") == "20");
    uint32_t v = 0;
    CHECK(wire::get_u32(l, "t3", v) && v == 4294967295u);
    CHECK(!wire::get_u32(l, "missing", v));
    CHECK(wire::parse("INVITE name=Aric", l) && wire::get(l, "name") == "Aric");
    CHECK(wire::parse("PING t1=99999999999", l) && !wire::get_u32(l, "t1", v));  // > 32 bits
    CHECK(!wire::parse("   ", l));

    std::puts("escaping free text into one field");
    const std::string text = "hi there = 100% done\nnext line, \xC3\xA9 ok";
    std::string esc = wire::escape(text), back;
    CHECK(esc.find(' ') == std::string::npos && esc.find('=') == std::string::npos);
    CHECK(esc.find('\n') == std::string::npos);
    CHECK(wire::unescape(esc, back) && back == text);
    CHECK(wire::parse("CHAT to=all text=" + esc, l) && wire::unescape(wire::get(l, "text"), back) &&
          back == text);
    CHECK(wire::escape("plain-text_1.0~") == "plain-text_1.0~");
    CHECK(!wire::unescape("bad%4", back) && !wire::unescape("bad%zz", back));
}

static void test_clock_math() {
    std::puts("clock sync math, including a huge offset and the 32-bit wrap");
    // peer clock is 0xF0000000 us (~67 min) ahead, 300 us there, 500 us back
    const uint32_t O = 0xF0000000u;
    uint32_t t1 = 0xFFFFFF00u;  // our clock about to wrap too
    uint32_t t2 = t1 + 300 + O;
    uint32_t t3 = t2 + 50;  // the peer took 50 us to answer
    uint32_t t4 = t3 - O + 500;
    auto s = detail::clock_sample(t1, t2, t3, t4);
    CHECK(s.rtt_us == 800);
    // uneven directions bias the estimate by half the difference: (300 - 500) / 2
    CHECK(s.offset_us == O - 100);

    // turning one of their times into ours with that offset lands within 100 us
    uint32_t ours = t2 - s.offset_us;
    CHECK(static_cast<int32_t>(ours - (t1 + 300)) == 100);
}

static void test_clock_filter() {
    std::puts("clock filter keeps the lowest round trip of the last 8");
    detail::ClockFilter f;
    detail::ClockSample s;
    CHECK(!f.best(s));
    f.add({111, 900});
    f.add({222, 300});
    f.add({333, 700});
    f.add({444, -5});       // impossible, ignored
    f.add({555, 1500000});  // a 1.5 s round trip: TCP resent it, ignored
    f.add({666, 150});      // the best real sample
    CHECK(f.best(s) && s.offset_us == 666);

    detail::ClockFilter only_bad;
    only_bad.add({777, 1600000});
    CHECK(!only_bad.best(s));                        // no offset at all beats a wrong one
    for (int i = 0; i < 8; ++i) f.add({999, 1000});  // pushes the older ones out
    CHECK(f.best(s) && s.offset_us == 999);
}

// waits for an event of the given kind, skipping others
static bool wait_for(Signaling& s, Kind kind, CallEvent* out = nullptr, int timeout_ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    CallEvent e;
    while (std::chrono::steady_clock::now() < end) {
        while (s.poll_event(e))
            if (e.kind == kind) {
                if (out) *out = e;
                return true;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

// waits until cond() holds, for things that settle a moment after an event
static bool eventually(const std::function<bool()>& cond, int timeout_ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (cond()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return cond();
}

static std::string ids_and_names(const std::vector<Member>& ms) {
    std::string s;
    for (const auto& m : ms) s += std::to_string(m.id) + ":" + m.name + (m.me ? "* " : " ");
    return s;
}

static void test_full_call() {
    std::puts("call, accept, clock sync, busy, hang up (loopback)");
    Signaling a, b, c;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(c.start("Third", kPortC));
    if (failures) {
        std::printf("  (couldn't start: %s / %s / %s)\n", a.error().c_str(), b.error().c_str(),
                    c.error().c_str());
        return;
    }

    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(a.state() == CallState::Calling);
    CHECK(!a.call("127.0.0.1", kPortB));  // only one call at a time

    CallEvent e;
    CHECK(wait_for(b, Kind::Incoming, &e));
    CHECK(e.peer_name == "Aric" && e.peer_ip == "127.0.0.1");
    CHECK(b.state() == CallState::Ringing);

    b.accept();
    CHECK(wait_for(a, Kind::Accepted, &e) && e.peer_name == "Atul");
    CHECK(wait_for(b, Kind::Accepted));
    CHECK(a.state() == CallState::InCall && b.state() == CallState::InCall);

    // same laptop, so the offset should be about zero and the round trip tiny
    uint32_t off = 1;
    bool synced = false;
    for (int i = 0; i < 300 && !synced; ++i) {
        synced = a.clock_offset(off);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(synced);
    CHECK(static_cast<int32_t>(off) > -5000 && static_cast<int32_t>(off) < 5000);
    CHECK(a.rtt_ms() >= 0.0 && a.rtt_ms() < 50.0);

    // someone else calls Atul mid-call
    CHECK(c.call("127.0.0.1", kPortB));
    CHECK(wait_for(c, Kind::Declined, &e) && e.reason == "busy");
    CHECK(b.state() == CallState::InCall);

    a.hang_up();
    CHECK(a.state() == CallState::Idle);
    CHECK(wait_for(a, Kind::Ended, &e) && e.reason == "hung up");
    CHECK(wait_for(b, Kind::Ended, &e) && e.reason == "peer hung up");
    CHECK(b.state() == CallState::Idle);
}

// phase 10: hang up and call someone else at once. hang_up() used to leave its clean-up to the
// worker, which then closed every connection, the new call's too, and wiped its key
static void test_quick_recall() {
    std::puts("hang up, then call someone else straight away (loopback)");
    Signaling a, b, c;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(c.start("Third", kPortC));
    if (failures) return;

    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));

    a.hang_up();
    CHECK(a.call("127.0.0.1", kPortC));  // before a's worker has run the hang-up
    CallEvent e;
    CHECK(wait_for(c, Kind::Incoming, &e) && e.peer_name == "Aric");
    CHECK(wait_for(b, Kind::Ended));  // the old call still ends properly
    c.accept();
    CHECK(wait_for(a, Kind::Accepted, &e) && e.peer_name == "Third");
    CHECK(a.state() == CallState::InCall && c.state() == CallState::InCall);
    a.hang_up();
    CHECK(wait_for(c, Kind::Ended));
}

static void test_decline_refused_cancel() {
    std::puts("decline, refused, cancel (loopback)");
    Signaling a, b;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CallEvent e;

    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.decline();
    CHECK(wait_for(a, Kind::Declined, &e) && e.reason == "declined");
    CHECK(a.state() == CallState::Idle && b.state() == CallState::Idle);

    // nothing listening on kPortC now
    CHECK(a.call("127.0.0.1", kPortC));
    CHECK(wait_for(a, Kind::Failed, &e));
    CHECK(a.state() == CallState::Idle);

    // caller gives up while it's ringing
    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    a.hang_up();
    CHECK(wait_for(a, Kind::Failed, &e) && e.reason == "cancelled");
    CHECK(wait_for(b, Kind::Ended, &e) && e.reason == "caller hung up");

    // the callee's program quits mid-call
    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));
    b.stop();
    CHECK(wait_for(a, Kind::Ended));
    CHECK(a.state() == CallState::Idle);
}

static void test_room_of_two() {
    std::puts("a call is a room of two: ids, members, destinations, offsets");
    Signaling a, b;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(a.members().empty() && a.my_id() == 0);

    CHECK(a.call("127.0.0.1", kPortB));
    CallEvent e;
    CHECK(wait_for(b, Kind::Incoming, &e));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted, &e) && e.member_id == 2);
    CHECK(wait_for(b, Kind::Accepted, &e) && e.member_id == 1);

    CHECK(a.is_host() && a.my_id() == 1);
    CHECK(!b.is_host() && b.my_id() == 2);  // known from INVITE id=2, before any WELCOME
    CHECK(ids_and_names(a.members()) == "1:Aric* 2:Atul ");
    CHECK(ids_and_names(b.members()) == "1:Aric 2:Atul* ");

    auto da = a.media_destinations(5004);
    CHECK(da.size() == 1 && da[0].ip == "127.0.0.1" && da[0].port == 5004);
    CHECK(b.media_destinations(5006).size() == 1);

    uint32_t off = 1;
    CHECK(a.clock_offset(uint8_t(1), off) && off == 0);  // ourselves
    CHECK(eventually([&] { return b.clock_offset(uint8_t(1), off); }));
    CHECK(static_cast<int32_t>(off) > -5000 && static_cast<int32_t>(off) < 5000);
    CHECK(!a.clock_offset(uint8_t(5), off));  // nobody has id 5

    // the member hangs up: for the host that's the end of the call, not a Left
    b.hang_up();
    CHECK(wait_for(a, Kind::Ended, &e) && e.reason == "peer hung up");
    CHECK(a.state() == CallState::Idle && a.members().empty());
}

static void test_group_call() {
    std::puts("group call: invite mid-call, roster, mesh/relay, chained offsets, leaving");
    Signaling a, b, c;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(c.start("Third", kPortC));
    CallEvent e;

    CHECK(a.call("127.0.0.2", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));
    CHECK(!b.invite("127.0.0.3", kPortC));  // only the host adds people
    CHECK(!a.invite("127.0.0.2", kPortB));  // already in

    // the host adds a third person
    CHECK(a.invite("127.0.0.3", kPortC));
    CHECK(a.state() == CallState::InCall);
    CHECK(wait_for(c, Kind::Incoming, &e) && e.peer_name == "Aric");
    c.accept();
    CHECK(c.my_id() == 3);
    CHECK(wait_for(a, Kind::Joined, &e) && e.member_id == 3 && e.peer_name == "Third");
    CHECK(wait_for(b, Kind::Joined, &e) && e.member_id == 3 && e.peer_ip == "127.0.0.3");
    CHECK(wait_for(c, Kind::Joined, &e) && e.member_id == 2 && e.peer_name == "Atul");
    CHECK(ids_and_names(a.members()) == "1:Aric* 2:Atul 3:Third ");
    CHECK(ids_and_names(b.members()) == "1:Aric 2:Atul* 3:Third ");
    CHECK(ids_and_names(c.members()) == "1:Aric 2:Atul 3:Third* ");
    // phase 6: someone added mid-call gets the same media key, and a code with the host
    std::array<uint8_t, kMediaKeyBytes> key_a{}, key_c{};
    CHECK(a.media_key(key_a) && c.media_key(key_c) && key_a == key_c);
    CHECK(!a.safety_code(3).empty() && a.safety_code(3) == c.safety_code(1));
    CHECK(a.safety_code(3) != a.safety_code(2));  // a code per connection
    CHECK(b.safety_code(3).empty());              // members share no connection

    // mesh: everyone sends to the two others
    auto db = b.media_destinations(5004);
    CHECK(db.size() == 2);
    bool to_c = false;
    for (const auto& d : db) to_c |= d.ip == "127.0.0.3" && d.port == 5004;
    CHECK(to_c);

    // relay: everyone sends one copy to the relay
    a.set_media(MediaMode::Relay, "127.0.0.9");
    CHECK(wait_for(a, Kind::MediaChanged));
    CHECK(wait_for(b, Kind::MediaChanged));
    CHECK(wait_for(c, Kind::MediaChanged));
    CHECK(b.media_mode() == MediaMode::Relay && c.relay_ip() == "127.0.0.9");
    auto dc = c.media_destinations(5004);
    CHECK(dc.size() == 1 && dc[0].ip == "127.0.0.9" && dc[0].port == proto::kRelayPort);
    b.set_media(MediaMode::Mesh);  // not the host: no effect on the call

    // multicast (phase 7): everyone sends one copy to the group, on the normal ports
    a.set_media(MediaMode::Multicast, "10.0.0.7");  // not a multicast address: ignored
    a.set_media(MediaMode::Multicast, proto::lancall_group(5));
    CHECK(wait_for(c, Kind::MediaChanged) && c.media_mode() == MediaMode::Multicast);
    CHECK(c.multicast_group() == "239.255.77.5" && c.relay_ip().empty());
    auto dm = c.media_destinations(5006);
    CHECK(dm.size() == 1 && dm[0].ip == "239.255.77.5" && dm[0].port == 5006);
    CHECK(wait_for(b, Kind::MediaChanged) && b.multicast_group() == "239.255.77.5");

    a.set_media(MediaMode::Mesh);
    CHECK(wait_for(c, Kind::MediaChanged) && c.media_mode() == MediaMode::Mesh);
    CHECK(c.multicast_group().empty());

    // b's offset to c goes through the host: same laptop, so about zero
    uint32_t off = 1;
    CHECK(eventually([&] { return b.clock_offset(uint8_t(3), off); }, 6000));
    CHECK(static_cast<int32_t>(off) > -5000 && static_cast<int32_t>(off) < 5000);

    // someone leaves; the other two carry on
    c.hang_up();
    CHECK(wait_for(c, Kind::Ended, &e) && e.reason == "hung up");
    CHECK(wait_for(a, Kind::Left, &e) && e.member_id == 3);
    CHECK(wait_for(b, Kind::Left, &e) && e.member_id == 3 && e.reason == "hung up");
    CHECK(a.state() == CallState::InCall && b.state() == CallState::InCall);
    CHECK(ids_and_names(b.members()) == "1:Aric 2:Atul* ");

    // a declined invite doesn't end the call
    CHECK(a.invite("127.0.0.3", kPortC));
    CHECK(wait_for(c, Kind::Incoming));
    c.decline();
    CHECK(wait_for(a, Kind::Declined, &e) && e.reason == "declined");
    CHECK(a.state() == CallState::InCall);

    // back in, then the host hangs up: over for everyone
    CHECK(a.invite("127.0.0.3", kPortC));
    CHECK(wait_for(c, Kind::Incoming));
    c.accept();
    CHECK(wait_for(a, Kind::Joined, &e) && e.member_id == 3);  // the freed id again
    a.hang_up();
    CHECK(wait_for(b, Kind::Ended, &e) && e.reason == "peer hung up");
    CHECK(wait_for(c, Kind::Ended, &e) && e.reason == "peer hung up");
    CHECK(b.state() == CallState::Idle && c.state() == CallState::Idle);
}

static void test_member_dropped() {
    std::puts("a member's program quits mid-call (loopback)");
    Signaling a, b, c;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(c.start("Third", kPortC));
    CallEvent e;

    CHECK(a.call("127.0.0.2", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));  // invite() needs the call to be up
    CHECK(a.invite("127.0.0.3", kPortC));
    CHECK(wait_for(c, Kind::Incoming));
    c.accept();
    CHECK(wait_for(b, Kind::Joined));

    c.stop();  // sends BYE on the way out
    CHECK(wait_for(a, Kind::Left, &e) && e.member_id == 3);
    CHECK(wait_for(b, Kind::Left, &e) && e.member_id == 3);
    CHECK(a.state() == CallState::InCall);
    a.hang_up();
    CHECK(wait_for(b, Kind::Ended));
}

static bool wait_chat(Signaling& s, ChatEvent& out, int timeout_ms = 3000);
static bool no_chat(Signaling& s, int ms = 300);

// A connection driven by hand, to test what goes over the wire
struct Raw {
    testsock::Handle fd = testsock::kNone;
    std::string buf;
    ~Raw() {
        if (fd != testsock::kNone) testsock::close(fd);
    }
    bool line(std::string& out, int timeout_ms = 3000) {
        auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            auto nl = buf.find('\n');
            if (nl != std::string::npos) {
                out = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                return true;
            }
            auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                            end - std::chrono::steady_clock::now())
                            .count();
            if (left <= 0 || !testsock::readable(fd, static_cast<int>(left))) return false;
            char tmp[2048];
            const long long n = testsock::recv_bytes(fd, tmp, sizeof(tmp));
            if (n <= 0) return false;
            buf.append(tmp, static_cast<size_t>(n));
        }
    }
    void send_text(const std::string& l) {
        std::string s = l + "\n";
        testsock::send_bytes(fd, s.data(), s.size());
    }
    // true if the other side closed the connection within timeout_ms
    bool closed(int timeout_ms = 3000) {
        std::string ignored;
        while (line(ignored, timeout_ms)) {
        }
        // closed = readable now, and the read says end of stream (or, on Windows, reset)
        if (!testsock::readable(fd, 0)) return false;
        char c;
        const long long n = testsock::recv_bytes(fd, &c, 1);
        return n == 0 || (n < 0 && !testsock::would_block());
    }
};

static testsock::Handle listen_on(uint16_t port) {
    const testsock::Handle ls = socket(AF_INET, SOCK_STREAM, 0);
    testsock::set_option(ls, SOL_SOCKET, SO_REUSEADDR, 1);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 1) != 0) {
        testsock::close(ls);
        return testsock::kNone;
    }
    return ls;
}

static testsock::Handle accept_one(testsock::Handle ls) {
    if (!testsock::readable(ls, 3000)) return testsock::kNone;
    return ::accept(ls, nullptr, nullptr);
}

static testsock::Handle connect_to(uint16_t port) {
    const testsock::Handle fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        testsock::close(fd);
        return testsock::kNone;
    }
    return fd;
}

static std::string pk_of(const std::string& hello) {
    wire::Line l;
    return wire::parse(hello, l) && l.verb == "HELLO" ? wire::get(l, "pk") : "";
}

static void test_encryption() {
    std::puts("phase 6: keys and safety codes match, the wire is encrypted");
    Signaling a, b;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CallEvent e;
    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));
    std::array<uint8_t, kMediaKeyBytes> ka{}, kb{};
    CHECK(a.media_key(ka) && b.media_key(kb) && ka == kb);
    std::string code = a.safety_code(2);
    CHECK(code.size() == 9 && code[4] == ' ' && code == b.safety_code(1));
    CHECK(a.safety_code(1).empty() && b.safety_code(3).empty());  // ourselves, nobody
    a.hang_up();
    CHECK(wait_for(b, Kind::Ended));
    CHECK(!a.media_key(ka));

    // a new call gets a new key and new codes
    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));
    std::array<uint8_t, kMediaKeyBytes> k2{};
    CHECK(a.media_key(k2) && k2 != ka && a.safety_code(2) != code);
    a.hang_up();
    CHECK(wait_for(b, Kind::Ended));

    std::puts("phase 6: a caller that doesn't encrypt is told old-version");
    std::string l;
    {
        Raw old;
        old.fd = connect_to(kPortB);
        CHECK(old.fd != testsock::kNone);
        old.send_text("INVITE name=Old");  // no HELLO first: an older caller
        CHECK(old.line(l) && l == "DECLINE reason=old-version");
        CHECK(b.state() == CallState::Idle);
    }

    std::puts("phase 6: a message in the clear isn't delivered");
    {
        Raw old;
        old.fd = connect_to(kPortB);
        old.send_text("MSG name=Old text=hello");
        CHECK(old.closed());
        CHECK(no_chat(b));
    }

    std::puts("phase 6: calling, or writing to, a lancall that doesn't answer HELLO fails");
    const testsock::Handle ls = listen_on(kPortC);
    CHECK(ls != testsock::kNone);
    CHECK(a.call("127.0.0.1", kPortC));
    {
        Raw callee;
        callee.fd = accept_one(ls);
        CHECK(callee.line(l) && !pk_of(l).empty());
        // an older build ignores HELLO, waits for an INVITE that never comes, hangs up
    }
    CHECK(wait_for(a, Kind::Failed, &e) && e.reason.find("older LanCall") != std::string::npos);
    CHECK(a.text_to("127.0.0.1", "hi", kPortC));
    {
        Raw old;
        old.fd = accept_one(ls);
        CHECK(old.line(l) && !pk_of(l).empty());  // our HELLO, not the message
    }
    ChatEvent m;
    CHECK(wait_chat(a, m) && m.kind == ChatEvent::Kind::Failed &&
          m.reason.find("older lancall") != std::string::npos);
    testsock::close(ls);

    std::puts("phase 6: a line that doesn't decrypt ends the connection");
    Raw evil;
    evil.fd = connect_to(kPortB);
    SecureLine sec;
    std::string hello;
    evil.send_text("HELLO v=1 pk=" + sec.public_hex());  // the caller speaks first
    CHECK(evil.line(hello));                             // b answers with its own
    CHECK(sec.start(pk_of(hello), true));
    std::string key(64, 'a');
    evil.send_text("S " + sec.seal("INVITE name=Evil id=2 key=" + key));
    CHECK(wait_for(b, Kind::Incoming, &e) && e.peer_name == "Evil");
    std::string tampered = sec.seal("BYE");
    tampered[0] = tampered[0] == '0' ? '1' : '0';
    evil.send_text("S " + tampered);
    CHECK(wait_for(b, Kind::Ended, &e) && e.reason == "caller gone");
    CHECK(evil.closed());

    std::puts("phase 6: a message is encrypted on the wire too");
    Raw reader;
    reader.fd = connect_to(kPortB);
    SecureLine rsec;
    reader.send_text("HELLO v=1 pk=" + rsec.public_hex());
    CHECK(reader.line(hello) && rsec.start(pk_of(hello), true));
    reader.send_text("S " + rsec.seal("MSG name=Reader text=" + wire::escape("secret plan")));
    std::string ok, plain;
    CHECK(reader.line(ok) && ok.rfind("S ", 0) == 0 && rsec.open(ok.substr(2), plain) &&
          plain == "OK");
    CHECK(wait_chat(b, m) && m.name == "Reader" && m.text == "secret plan");
}

static void test_silent_member() {
    std::puts("a member that goes silent without closing (cable pulled): gone after 10 s");
    Signaling a;
    CHECK(a.start("Aric", kPortA));

    // a pretend member: does the handshake, answers the INVITE, then never says a word
    const testsock::Handle ls = listen_on(kPortB);
    CHECK(ls != testsock::kNone);
    CHECK(a.call("127.0.0.1", kPortB));
    Raw ghost;
    ghost.fd = accept_one(ls);
    SecureLine sec;
    std::string l, invite;
    CHECK(ghost.line(l));  // a's HELLO
    ghost.send_text("HELLO v=1 pk=" + sec.public_hex());
    CHECK(sec.start(pk_of(l), false));
    CHECK(ghost.line(l) && l.rfind("S ", 0) == 0 && sec.open(l.substr(2), invite));
    CHECK(invite.rfind("INVITE name=Aric id=2 key=", 0) == 0);
    ghost.send_text("S " + sec.seal("ACCEPT name=Ghost"));

    CallEvent e;
    CHECK(wait_for(a, Kind::Accepted, &e) && e.peer_name == "Ghost");
    auto t0 = std::chrono::steady_clock::now();
    CHECK(wait_for(a, Kind::Ended, &e, 14000) && e.reason == "no response");
    auto waited = std::chrono::steady_clock::now() - t0;
    CHECK(waited > std::chrono::seconds(8));  // not before the timeout
    CHECK(a.state() == CallState::Idle);
    testsock::close(ls);
}

// waits for a chat event, skipping none (chat tests expect exactly what they check)
static bool wait_chat(Signaling& s, ChatEvent& out, int timeout_ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < end) {
        if (s.poll_chat(out)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool no_chat(Signaling& s, int ms) {
    ChatEvent e;
    return !wait_chat(s, e, ms);
}

static void test_chat_in_call() {
    std::puts("chat in a call: to everyone, and to one member through the host");
    Signaling a, b, c;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(c.start("Third", kPortC));
    CallEvent e;
    CHECK(!a.text_all("nobody to tell"));  // not in a call

    CHECK(a.call("127.0.0.2", kPortB));
    CHECK(wait_for(b, Kind::Incoming));
    b.accept();
    CHECK(wait_for(a, Kind::Accepted));
    CHECK(a.invite("127.0.0.3", kPortC));
    CHECK(wait_for(c, Kind::Incoming));
    c.accept();
    CHECK(wait_for(a, Kind::Joined));
    CHECK(wait_for(b, Kind::Joined));  // b knows c, so b can write to c

    // a member to everyone: the host keeps it and passes it on
    ChatEvent m;
    CHECK(b.text_all("hello all = 100%"));
    CHECK(wait_chat(a, m) && m.kind == ChatEvent::Kind::Received && m.name == "Atul" &&
          m.member_id == 2 && m.to_all && m.text == "hello all = 100%");
    CHECK(wait_chat(c, m) && m.name == "Atul" && m.member_id == 2 && m.to_all &&
          m.ip == "127.0.0.2");
    CHECK(no_chat(b));  // not back to the sender

    // the host to everyone
    CHECK(a.text_all("from the host"));
    CHECK(wait_chat(b, m) && m.name == "Aric" && m.member_id == 1 && m.to_all);
    CHECK(wait_chat(c, m) && m.text == "from the host");

    // one member to another: through the host, who doesn't keep a copy
    CHECK(c.text_to("127.0.0.2", "just for you"));
    CHECK(wait_chat(b, m) && m.name == "Third" && m.member_id == 3 && !m.to_all &&
          m.text == "just for you");
    CHECK(no_chat(a));
    CHECK(no_chat(c));

    // a member to the host, and the host to one member
    CHECK(b.text_to("127.0.0.1", "to the host"));
    CHECK(wait_chat(a, m) && m.member_id == 2 && !m.to_all && m.text == "to the host");
    CHECK(a.text_to("127.0.0.3", "host to third"));
    CHECK(wait_chat(c, m) && m.member_id == 1 && !m.to_all);
    CHECK(no_chat(b));

    // control characters never reach a terminal; too long or empty isn't sent
    CHECK(b.text_all("bell\x07 and\ttab"));
    CHECK(wait_chat(a, m) && m.text == "bell  and tab");
    CHECK(wait_chat(c, m));
    CHECK(!b.text_all(std::string(Signaling::kMaxTextBytes + 1, 'x')));
    CHECK(b.text_all(std::string(Signaling::kMaxTextBytes, 'x')));
    CHECK(wait_chat(a, m) && m.text.size() == Signaling::kMaxTextBytes);
    CHECK(wait_chat(c, m));
    CHECK(!b.text_all("") && !b.text_to("127.0.0.3", ""));

    a.hang_up();
    CHECK(wait_for(b, Kind::Ended));
    CHECK(wait_for(c, Kind::Ended));
}

static void test_chat_outside_call() {
    std::puts("messages outside a call: to anyone, even someone busy in a call");
    Signaling a, b, c;
    CHECK(a.start("Aric", kPortA));
    CHECK(b.start("Atul", kPortB));
    CHECK(c.start("Third", kPortC));
    ChatEvent m;

    // two idle laptops
    CHECK(a.text_to("127.0.0.1", "are you free?", kPortB));
    CHECK(wait_chat(b, m) && m.kind == ChatEvent::Kind::Received && m.name == "Aric" &&
          m.member_id == 0 && !m.to_all && m.text == "are you free?");
    CHECK(no_chat(a));  // delivered: nothing to report back
    CHECK(a.state() == CallState::Idle && b.state() == CallState::Idle);

    // b and c in a call; a (not in it) can still write to b, and the call carries on
    CallEvent e;
    CHECK(b.call("127.0.0.1", kPortC));
    CHECK(wait_for(c, Kind::Incoming));
    c.accept();
    CHECK(wait_for(b, Kind::Accepted));
    CHECK(a.text_to("127.0.0.1", "ping me after", kPortB));
    CHECK(wait_chat(b, m) && m.name == "Aric" && !m.to_all && m.member_id == 0);
    CHECK(b.state() == CallState::InCall && c.state() == CallState::InCall);

    // an INVITE while busy is still declined
    CHECK(a.call("127.0.0.1", kPortB));
    CHECK(wait_for(a, Kind::Declined, &e) && e.reason == "busy");

    // nobody there: reported back
    b.hang_up();
    CHECK(wait_for(c, Kind::Ended));
    c.stop();
    CHECK(a.text_to("127.0.0.1", "hello?", kPortC));
    CHECK(wait_chat(a, m) && m.kind == ChatEvent::Kind::Failed && m.ip == "127.0.0.1" &&
          m.text == "hello?" && !m.reason.empty());
}

int main() {
    platform::init();  // Winsock, before the raw sockets
    test_wire();
    test_clock_math();
    test_clock_filter();
    test_full_call();
    test_quick_recall();
    test_decline_refused_cancel();
    test_room_of_two();
    test_group_call();
    test_member_dropped();
    test_encryption();
    test_silent_member();
    test_chat_in_call();
    test_chat_outside_call();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all signaling tests passed");
    return 0;
}