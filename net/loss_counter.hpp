#pragma once
// Counts how many packets of a stream were expected and how many arrived, interval by
// interval, from 16-bit sequence numbers (Person 1, net/). This is what goes into a receiver
// report's loss figure. Same idea as RTCP's "fraction lost" (RFC 3550 A.3):
//
//   extend seq to 32 bits by counting wraps, expected = highest - highest at last interval,
//   lost = expected - received (never below 0, since duplicates can make it negative).
//
// A jump of more than 3000 either way is taken as the sender restarting, and counting starts
// over from there instead of reporting thousands of lost packets. Not thread-safe.
#include <cstdint>

namespace net {

class LossCounter {
public:
    struct Interval {
        uint32_t expected = 0;
        uint32_t received = 0;
        uint32_t lost() const { return expected > received ? expected - received : 0; }
        // per 1000, the unit receiver reports use
        uint16_t loss_permille() const {
            return expected ? static_cast<uint16_t>(uint64_t(lost()) * 1000 / expected) : 0;
        }
    };

    void on_packet(uint16_t seq) {
        if (!started_) {
            restart(seq);
            return;
        }
        int diff = static_cast<int16_t>(seq - static_cast<uint16_t>(highest_));
        if (diff > 3000 || diff < -3000) {
            restart(seq);
            return;
        }
        if (diff > 0) highest_ += static_cast<uint32_t>(diff);
        received_++;  // late and out-of-order packets still count, they did arrive
    }

    // numbers since the previous call, then starts a new interval
    Interval take_interval() {
        Interval out;
        if (!started_) return out;
        out.expected = highest_ - interval_start_;
        out.received = received_;
        interval_start_ = highest_;
        received_ = 0;
        return out;
    }

    uint16_t highest_seq() const { return static_cast<uint16_t>(highest_); }

private:
    void restart(uint16_t seq) {
        started_ = true;
        highest_ = seq;
        interval_start_ = highest_ - 1;  // so this first packet counts as expected
        received_ = 1;
    }

    bool started_ = false;
    uint32_t highest_ = 0;         // extended highest sequence number
    uint32_t interval_start_ = 0;  // highest_ at the start of the interval
    uint32_t received_ = 0;        // packets that arrived during the interval
};

}  // namespace net