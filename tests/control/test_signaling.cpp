// Tests for signaling (Person 1): the text format and clock-sync math without a network,
// then real calls between Signaling objects over 127.0.0.1. Takes a few seconds.
// Exit code 0 = all passed. Uses TCP ports 25011-25013 on this laptop.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

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

int main() {
    test_wire();
    test_clock_math();
    test_clock_filter();
    test_full_call();
    test_decline_refused_cancel();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all signaling tests passed");
    return 0;
}