#pragma once
// Clock-sync math for signaling (Person 1, control/), split out so the tests can check it
// without sockets. Not frozen.
#include <cstdint>
#include <deque>

namespace control::detail {

struct ClockSample {
    uint32_t offset_us = 0;  // peer clock minus ours, modulo 2^32
    int32_t rtt_us = 0;      // network round trip, our clock
};

// t1 = we sent PING (ours), t2 = it arrived (theirs), t3 = PONG sent (theirs),
// t4 = PONG arrived (ours).
//
// With O = their clock minus ours and d1, d2 the two one-way delays:
//   a = t2 - t1 = O + d1      b = t3 - t4 = O - d2      (both mod 2^32)
// so a - b = d1 + d2 is the round trip, which is small, and O = a - rtt/2 if the two
// directions take equally long. O itself can be hours (clocks count from boot), which is
// why nothing here ever converts O to a signed number.
inline ClockSample clock_sample(uint32_t t1, uint32_t t2, uint32_t t3, uint32_t t4) {
    uint32_t a = t2 - t1;
    uint32_t b = t3 - t4;
    int32_t rtt = static_cast<int32_t>(a - b);
    ClockSample s;
    s.rtt_us = rtt;
    s.offset_us = a - static_cast<uint32_t>(rtt / 2);
    return s;
}

// Keeps the last 8 samples and trusts the one with the smallest round trip: a sample that
// took longer most likely sat in a queue on one side, which skews its offset.
class ClockFilter {
public:
    static constexpr size_t kKeep = 8;

    // A round trip above this is a PING or PONG that TCP had to resend (its minimum resend
    // timer is 200 ms). Its offset can be off by hundreds of ms, and at the start of a call it
    // may be the only sample there is: under 5% loss that gave e2e latencies of -715 ms and
    // +900 ms (NOTES.md issue #7). A LAN round trip is ~1 ms, so 100 ms leaves lots of room.
    static constexpr int32_t kMaxRttUs = 100000;

    void add(const ClockSample& s) {
        if (s.rtt_us < 0) return;          // impossible, a clock jumped
        if (s.rtt_us > kMaxRttUs) return;  // resent by TCP, not trustworthy
        samples_.push_back(s);
        if (samples_.size() > kKeep) samples_.pop_front();
    }

    bool best(ClockSample& out) const {
        if (samples_.empty()) return false;
        out = samples_.front();
        for (const auto& s : samples_)
            if (s.rtt_us < out.rtt_us) out = s;
        return true;
    }

    void reset() { samples_.clear(); }

private:
    std::deque<ClockSample> samples_;
};

}  // namespace control::detail