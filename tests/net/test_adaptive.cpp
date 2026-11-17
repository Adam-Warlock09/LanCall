// Tests for adaptive video (Person 1, phase 3): the loss counter that feeds receiver
// reports, and the quality ladder that reacts to them. Phase 4: the merger that turns
// several receivers' reports into one for the ladder. No network, no camera; time is
// passed in, so each test replays a timed sequence of reports. Exit code 0 = all passed.
#include <cstdio>

#include "net/loss_counter.hpp"
#include "video/quality_ladder.hpp"
#include "video/report_merger.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using video::QualityLadder;

constexpr uint32_t S = 1000000;  // one second in us

// a report every 0.5 s from t0 up to (not including) t1, times in seconds
static void reports(QualityLadder& q, uint16_t loss_permille, uint32_t jitter_us, double t0,
                    double t1) {
    for (double t = t0; t < t1 - 1e-9; t += 0.5)
        q.on_report(loss_permille, jitter_us, static_cast<uint32_t>(t * S));
}

static void test_loss_counter() {
    std::puts("loss counter: clean, lossy, wrap, reorder, restart");
    net::LossCounter c;
    CHECK(c.take_interval().expected == 0);  // nothing yet

    for (int s = 0; s < 100; ++s) c.on_packet(static_cast<uint16_t>(s));
    auto iv = c.take_interval();
    CHECK(iv.expected == 100 && iv.received == 100 && iv.loss_permille() == 0);

    for (int s = 100; s < 200; ++s)
        if (s % 10 != 0) c.on_packet(static_cast<uint16_t>(s));  // every 10th lost
    iv = c.take_interval();
    CHECK(iv.expected == 100 && iv.received == 90 && iv.loss_permille() == 100);

    net::LossCounter w;
    for (int s = 65530; s < 65546; ++s) w.on_packet(static_cast<uint16_t>(s));  // wraps at 65536
    iv = w.take_interval();
    CHECK(iv.expected == 16 && iv.received == 16);
    CHECK(w.highest_seq() == 9);

    net::LossCounter r;
    r.on_packet(5);
    r.on_packet(7);
    r.on_packet(6);  // late, but it did arrive
    iv = r.take_interval();
    CHECK(iv.expected == 3 && iv.received == 3 && iv.lost() == 0);

    net::LossCounter x;
    for (int s = 0; s < 50; ++s) x.on_packet(static_cast<uint16_t>(s));
    x.take_interval();
    x.on_packet(40000);  // sender restarted: not 39950 lost packets
    x.on_packet(40001);
    iv = x.take_interval();
    CHECK(iv.expected == 2 && iv.received == 2);
}

static void test_steps() {
    std::puts("ladder steps from 15 fps / q70");
    QualityLadder q(15, 70);
    CHECK(q.levels() == 5);
    CHECK(q.step().fps == 15 && q.step().quality == 70);
    reports(q, 900, 0, 0, 10);  // terrible for a long time: bottom step, and no further
    CHECK(q.level() == 4);
    CHECK(q.step().fps == 7 && q.step().quality == 17);
}

static void test_random_loss_holds() {
    std::puts("5% random loss alone does not step down");
    QualityLadder q(15, 70);
    reports(q, 50, 300, 0, 20);  // 5% loss, normal LAN jitter
    CHECK(q.level() == 0);
}

static void test_congestion_and_recovery() {
    std::puts("congestion steps down fast, recovery steps up slowly");
    QualityLadder q(15, 70);
    q.on_report(300, 0, 0);  // 30%: two steps at once
    CHECK(q.level() == 2);
    q.on_report(120, 0, S / 2);  // 12%, but within the 1 s hold
    CHECK(q.level() == 2);
    q.on_report(120, 0, S * 3 / 2);  // hold over: one more step
    CHECK(q.level() == 3);

    // clean from t=2 s: the first step up needs 5 s of good reports
    reports(q, 0, 300, 2, 7);
    CHECK(q.level() == 3);
    q.on_report(0, 300, 7 * S);
    CHECK(q.level() == 2);
    reports(q, 0, 300, 7.5, 12.5);
    CHECK(q.level() == 1);
    reports(q, 0, 300, 12.5, 17.5);
    CHECK(q.level() == 0);
}

static void test_queue_signal() {
    std::puts("small loss with high jitter means a queue is filling");
    QualityLadder q(15, 70);
    q.on_report(30, 1000, 0);  // 3% loss, low jitter: hold
    CHECK(q.level() == 0);
    q.on_report(30, 20000, S);  // 3% loss, 20 ms jitter: step down
    CHECK(q.level() == 1);
}

static void test_failed_probe_backs_off() {
    std::puts("a failed step up doubles the wait before the next one");
    QualityLadder q(15, 70);
    q.on_report(300, 0, 0);  // to step 2
    reports(q, 0, 300, 1, 6.5);
    CHECK(q.level() == 1);       // stepped up at t=6
    q.on_report(150, 0, 7 * S);  // and it hurt within 3 s
    CHECK(q.level() == 2);
    CHECK(q.up_wait_us() == 10 * S);

    reports(q, 0, 300, 7.5, 17);
    CHECK(q.level() == 2);  // 5 s isn't enough any more
    q.on_report(0, 300, 18 * S);
    CHECK(q.level() == 1);  // 10 s of good reports is

    // back at the top, the wait resets
    reports(q, 0, 300, 18.5, 28.5);
    CHECK(q.level() == 0);
    CHECK(q.up_wait_us() == 5 * S);
}

static void test_silence() {
    std::puts("reports going missing counts as trouble, but only after the first one");
    QualityLadder fresh(15, 70);
    fresh.tick(100 * S);
    CHECK(fresh.level() == 0);  // never had a report: maybe an old receiver, don't punish

    QualityLadder q(15, 70);
    q.on_report(0, 300, 0);
    q.tick(S * 3 / 2);
    CHECK(q.level() == 0);
    q.tick(S * 5 / 2);  // 2.5 s of silence
    CHECK(q.level() == 1);
    q.tick(3 * S);
    CHECK(q.level() == 1);  // the next step needs another 2 s
    q.tick(S * 46 / 10);
    CHECK(q.level() == 2);
}

static void test_report_merger() {
    std::puts("merger: worst receiver, relay reporters, forgetting, keep_only");
    video::ReportMerger m;
    auto w = m.add("10.0.0.2", 0, 30, 1000, 0);
    CHECK(w.receivers == 1 && w.loss_permille == 30 && w.jitter_us == 1000);

    // a second receiver: loss and jitter are each the worst of the two
    w = m.add("10.0.0.3", 0, 5, 4000, S / 10);
    CHECK(w.receivers == 2 && w.loss_permille == 30 && w.jitter_us == 4000);

    // the first one gets better: its new report replaces the old one
    w = m.add("10.0.0.2", 0, 0, 500, S / 2);
    CHECK(w.receivers == 2 && w.loss_permille == 5 && w.jitter_us == 4000);

    // through a relay every report has the same address; the reporter tells them apart
    w = m.add("10.0.0.9", 2, 80, 0, S);
    w = m.add("10.0.0.9", 3, 10, 0, S);
    CHECK(w.receivers == 4 && w.loss_permille == 80);

    // 10.0.0.3 last reported at 0.1 s, so it's forgotten after 2.6 s
    w = m.worst(S / 10 + video::ReportMerger::kForgetUs + 1);
    CHECK(w.receivers == 3);

    // we stop sending straight to anyone: only the relay's reporters stay
    m.keep_only({"10.0.0.9"});
    w = m.worst(S + S / 2);
    CHECK(m.size() == 2 && w.loss_permille == 80);

    std::puts("ladder follows the worst receiver, and recovers when it leaves");
    QualityLadder q(15, 70);
    video::ReportMerger g;
    for (double t = 0; t < 3; t += 0.5) {
        uint32_t now = static_cast<uint32_t>(t * S);
        auto a = g.add("10.0.0.2", 0, 0, 500, now);  // fine
        q.on_report(a.loss_permille, a.jitter_us, now);
        auto b = g.add("10.0.0.3", 0, 150, 500, now);  // congested link
        q.on_report(b.loss_permille, b.jitter_us, now);
    }
    CHECK(q.level() >= 2);
    int low = q.level();
    g.keep_only({"10.0.0.2"});  // the congested one left the call
    for (double t = 3; t < 9.1; t += 0.5) {
        uint32_t now = static_cast<uint32_t>(t * S);
        auto a = g.add("10.0.0.2", 0, 0, 500, now);
        q.on_report(a.loss_permille, a.jitter_us, now);
    }
    CHECK(q.level() == low - 1);  // 5 s of good reports: one step back up
}

static void test_idle() {
    std::puts("no silence step-down while there's nobody to send to");
    QualityLadder q(15, 70);
    reports(q, 0, 500, 0, 2);               // a call
    for (double t = 2; t < 12; t += 0.1) {  // then 10 s with no destinations
        q.idle(static_cast<uint32_t>(t * S));
        q.tick(static_cast<uint32_t>(t * S));
    }
    CHECK(q.level() == 0);
    q.tick(static_cast<uint32_t>(13.5 * S));  // sending again, reports stop: that counts
    CHECK(q.level() == 0);                    // (only 1.5 s so far)
    q.tick(static_cast<uint32_t>(14.5 * S));
    CHECK(q.level() == 1);
}

int main() {
    test_loss_counter();
    test_steps();
    test_random_loss_holds();
    test_congestion_and_recovery();
    test_queue_signal();
    test_failed_probe_backs_off();
    test_silence();
    test_report_merger();
    test_idle();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all adaptive tests passed");
    return 0;
}