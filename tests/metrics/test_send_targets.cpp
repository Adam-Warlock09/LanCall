// Unit tests for audio/send_targets.hpp: AudioSender's destination list (phase 4 mesh/relay)
// and the per-receiver report board behind its stats line.
// Lives in tests/metrics/ because tests/audio/ is not part of the shared test layout.

#include "audio/send_targets.hpp"

#include <chrono>
#include <string>
#include <vector>

#include "check.hpp"

using audio::clean_destinations;
using audio::describe_destinations;
using audio::ReportBoard;
using proto::Endpoint;

namespace {

using Clock = ReportBoard::Clock;
using std::chrono::milliseconds;

bool same(const std::vector<Endpoint>& a, const std::vector<Endpoint>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].ip != b[i].ip || a[i].port != b[i].port) return false;
    }
    return true;
}

void test_clean_keeps_good_ones_in_order() {
    const std::vector<Endpoint> in = {{"10.8.66.31", 5006}, {"10.8.66.20", 5006}};
    std::string rejected = "x";
    CHECK(same(clean_destinations(in, &rejected), in));
    CHECK(rejected.empty());
    CHECK(clean_destinations({}).empty());  // empty = muted, a valid list
}

void test_clean_drops_bad_and_repeated() {
    // good, bad ip, port 0, repeat of the first (dropped quietly), same ip on another port
    // (kept: two receivers on one laptop), empty ip
    const std::vector<Endpoint> in = {{"10.8.66.20", 5006}, {"not-an-ip", 5006},  {"10.8.66.31", 0},
                                      {"10.8.66.20", 5006}, {"10.8.66.20", 5106}, {"", 5006}};
    std::string rejected;
    const auto out = clean_destinations(in, &rejected);
    CHECK(same(out, {{"10.8.66.20", 5006}, {"10.8.66.20", 5106}}));
    CHECK(rejected == "not-an-ip:5006, 10.8.66.31:0, :5006");
}

void test_describe_destinations() {
    CHECK(describe_destinations({}) == "nobody (muted)");
    CHECK(describe_destinations({{"10.8.66.20", 5014}}) == "10.8.66.20:5014");
    CHECK(describe_destinations({{"1.1.1.1", 1}, {"2.2.2.2", 2}, {"3.3.3.3", 3}}) ==
          "3 destinations");
}

void test_is_destination_ip() {
    const std::vector<Endpoint> to = {{"10.8.66.20", 5006}, {"10.8.66.31", 5006}};
    CHECK(audio::is_destination_ip(to, "10.8.66.31"));
    CHECK(!audio::is_destination_ip(to, "10.8.66.99"));
    CHECK(!audio::is_destination_ip({}, "10.8.66.20"));
}

void test_board_two_person_call() {
    ReportBoard b;
    const auto t0 = Clock::now();
    CHECK(b.describe(t0) == "no reports yet");
    b.on_report(0, "10.8.66.20", 15, 2000, t0);  // reporter 0: an old build or a 2-person call
    CHECK(b.describe(t0) == "peer loss 1.5% jitter 2.0 ms");        // the phase 3 text, unchanged
    b.on_report(0, "10.8.66.20", 0, 1000, t0 + milliseconds(500));  // same receiver: replaces
    CHECK(b.summary(t0 + milliseconds(500)).heard == 1);
    CHECK(b.describe(t0 + milliseconds(500)) == "peer loss 0.0% jitter 1.0 ms");
    CHECK(b.describe(t0 + milliseconds(3000)) == "reports stopped");
}

void test_board_group_through_relay() {
    // Three receivers, all reports arriving from the relay's address: told apart by reporter.
    ReportBoard b;
    const auto t0 = Clock::now();
    b.on_report(2, "10.8.66.5", 10, 9000, t0);
    b.on_report(3, "10.8.66.5", 42, 1000, t0);
    b.on_report(4, "10.8.66.5", 0, 3000, t0);
    const auto s = b.summary(t0);
    CHECK(s.heard == 3);
    CHECK(s.fresh == 3);
    CHECK(s.worst_loss_permille == 42);
    CHECK(s.worst_loss_key == "id 3");
    CHECK(s.worst_jitter_us == 9000);  // from another receiver than the worst loss
    CHECK(b.describe(t0) == "3 peers: worst loss 4.2% (id 3) worst jitter 9.0 ms");
}

void test_board_quiet_receiver_and_clear() {
    ReportBoard b;
    const auto t0 = Clock::now();
    b.on_report(2, "10.8.66.20", 300, 1000, t0);  // bad link, then went quiet
    b.on_report(3, "10.8.66.31", 5, 1000, t0 + milliseconds(2500));
    const auto t1 = t0 + milliseconds(3000);
    const auto s = b.summary(t1);
    CHECK(s.fresh == 1);
    CHECK(s.worst_loss_permille == 5);  // the stale 30% no longer counts
    CHECK(b.describe(t1) == "1 peers: worst loss 0.5% (id 3) worst jitter 1.0 ms (1 stopped)");
    b.clear();  // destinations changed
    CHECK(b.describe(t1) == "no reports yet");
}

void test_board_reporter_zero_keyed_by_ip() {
    // Two old-build receivers in a mesh: reporter 0 from two addresses = two receivers.
    ReportBoard b;
    const auto t0 = Clock::now();
    b.on_report(0, "10.8.66.20", 7, 1000, t0);
    b.on_report(0, "10.8.66.31", 3, 1000, t0);
    CHECK(b.summary(t0).heard == 2);
    CHECK(b.summary(t0).worst_loss_key == "10.8.66.20");
}

}  // namespace

int main() {
    RUN(test_clean_keeps_good_ones_in_order);
    RUN(test_clean_drops_bad_and_repeated);
    RUN(test_describe_destinations);
    RUN(test_is_destination_ip);
    RUN(test_board_two_person_call);
    RUN(test_board_group_through_relay);
    RUN(test_board_quiet_receiver_and_clear);
    RUN(test_board_reporter_zero_keyed_by_ip);
    return check::result();
}
