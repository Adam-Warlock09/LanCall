#pragma once
// Which packets to ask for again (Person 1, net/, phase 5). The receiver feeds it every
// sequence number of one stream as it arrives; due() says which missing ones to NACK now.
// Header-only, time passed in, so the tests can replay any sequence. Not frozen.
//
// Rules:
//   - a gap is asked for right away. On our LAN packets almost never arrive out of order,
//     and a needless NACK only costs one duplicate packet, which the reassembler ignores
//   - still missing kRetryUs (10 ms) later: ask once more (the NACK or the resend got lost)
//   - after kGiveUpUs it's too late to help the frame it belonged to, so forget it
//   - a jump of more than kMaxGap packets is an outage or a restarted sender, not loss:
//     asking for all of it would just flood the sender, so nothing is asked
//   - a seq more than kRestartBack behind the highest is a restarted sender too (it begins at
//     0 again, e.g. someone who rejoined with the same id): start over from it. No real late
//     packet is that old: kRestartBack is over a second of video
#include <algorithm>
#include <cstdint>
#include <vector>

namespace net {

class NackTracker {
public:
    static constexpr int kMaxGap = 64;
    static constexpr int kRestartBack = 1000;
    static constexpr uint32_t kRetryUs = 10000;
    static constexpr int kMaxTries = 2;
    static constexpr uint32_t kGiveUpUs = 80000;
    static constexpr size_t kMaxMissing = 256;

    struct Stats {
        uint64_t asked = 0;     // sequence numbers put in NACKs (retries included)
        uint64_t filled = 0;    // missing ones that turned up later (resent, recovered, late)
        uint64_t given_up = 0;  // never arrived in time
    };

    void on_packet(uint16_t seq, uint32_t now_us) {
        if (!have_) {
            have_ = true;
            highest_ = seq;
            return;
        }
        int diff = static_cast<int16_t>(seq - highest_);
        if (diff > 0) {
            if (diff > kMaxGap) {
                missing_.clear();  // outage or restart: start over from here
            } else {
                for (uint16_t s = static_cast<uint16_t>(highest_ + 1); s != seq; ++s)
                    if (missing_.size() < kMaxMissing) missing_.push_back({s, now_us, 0, 0});
            }
            highest_ = seq;
            return;
        }
        if (diff < -kRestartBack) {  // a restarted sender: start over from here
            missing_.clear();
            highest_ = seq;
            return;
        }
        // older than the highest: fills a hole, or is a duplicate
        for (auto it = missing_.begin(); it != missing_.end(); ++it)
            if (it->seq == seq) {
                missing_.erase(it);
                stats_.filled++;
                return;
            }
    }

    // a packet we rebuilt ourselves (FEC): stop asking for it
    void recovered(uint16_t seq, uint32_t now_us) { on_packet(seq, now_us); }

    // sequence numbers to NACK now; each one counts as asked
    std::vector<uint16_t> due(uint32_t now_us) {
        std::vector<uint16_t> out;
        for (auto it = missing_.begin(); it != missing_.end();) {
            if (now_us - it->first_us > kGiveUpUs) {
                stats_.given_up++;
                it = missing_.erase(it);
                continue;
            }
            bool ask =
                it->tries == 0 || (it->tries < kMaxTries && now_us - it->asked_us >= kRetryUs);
            if (ask && out.size() < kMaxNackOut) {
                out.push_back(it->seq);
                it->tries++;
                it->asked_us = now_us;
                stats_.asked++;
            }
            ++it;
        }
        return out;
    }

    size_t missing() const { return missing_.size(); }
    const Stats& stats() const { return stats_; }

private:
    static constexpr size_t kMaxNackOut = 64;  // proto::kMaxNackSeqs

    struct Miss {
        uint16_t seq;
        uint32_t first_us;
        uint32_t asked_us;
        int tries;
    };

    bool have_ = false;
    uint16_t highest_ = 0;
    std::vector<Miss> missing_;
    Stats stats_;
};

}  // namespace net
