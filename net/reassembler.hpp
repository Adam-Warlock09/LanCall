#pragma once
#include <cstdint>
#include <map>
#include <vector>

#include "protocol.hpp"

namespace net {

struct Frame {
    uint16_t frame_id = 0;
    uint32_t timestamp_us = 0;  // sender's capture time
    std::vector<uint8_t> data;
};

// Collects pieces of frames and hands back a frame once every piece has arrived.
//
// Rules:
//  - a frame that isn't complete within `timeout_us` of its first piece is dropped
//  - once frame N is shown, anything older than N is pointless, so it's dropped too
//  - pieces for frames older than the last completed one are counted as late and ignored
//
// "now" is passed in rather than read from the clock so the tests can control time.
class Reassembler {
public:
    struct Stats {
        uint64_t completed = 0;
        uint64_t dropped_incomplete = 0;  // timed out or overtaken by a newer frame
        uint64_t late_pieces = 0;         // arrived after a newer frame was already done
        uint64_t duplicate_pieces = 0;
        uint64_t bad_pieces = 0;  // piece count disagreed with earlier pieces
    };

    explicit Reassembler(uint32_t timeout_us = 100000) : timeout_us_(timeout_us) {}

    // returns true and fills `out` when this piece completes a frame
    bool push(const proto::Header& h, const uint8_t* payload, size_t len, uint32_t now_us,
              Frame& out);

    // drops frames that have waited too long. call this regularly, even when idle
    void expire(uint32_t now_us);

    const Stats& stats() const { return stats_; }
    size_t pending() const { return pending_.size(); }

private:
    struct Partial {
        uint32_t timestamp_us = 0;
        uint32_t first_seen_us = 0;
        uint8_t count = 0;
        uint8_t received = 0;
        size_t bytes = 0;
        std::vector<std::vector<uint8_t>> pieces;
        std::vector<bool> have;
    };

    // frame ids wrap at 65535, so "newer" means "less than half the ring ahead"
    static bool newer(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

    uint32_t timeout_us_;
    std::map<uint16_t, Partial> pending_;
    bool have_last_ = false;
    uint16_t last_done_ = 0;
    Stats stats_;
};

}  // namespace net
