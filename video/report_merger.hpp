#pragma once
// Receiver reports from several receivers at once (Person 1, video/, phase 4). Header-only,
// time passed in, so the tests can replay any sequence. Not frozen.
//
// In a group call one encoding of our video goes to everyone, so the quality ladder has to
// follow the worst receiver: it gets the highest loss and the highest jitter among the
// receivers heard from in the last kForgetUs. A receiver that stops reporting (left the
// call, or its reports are lost along with everything else) drops out after that; if every
// receiver goes quiet, the ladder's own silence rule steps down.
//
// A receiver is told apart by the address its report came from plus the reporter byte:
// through the relay every report comes from the relay's address, and the reporter (its
// member id) is what's left. Phase 3 receivers write reporter 0, which is fine in mesh mode
// because there the addresses differ.
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace video {

class ReportMerger {
public:
    static constexpr uint32_t kForgetUs = 2500000;  // a report every 0.5 s, so 5 missed

    struct Worst {
        uint16_t loss_permille = 0;
        uint32_t jitter_us = 0;
        int receivers = 0;  // how many were taken into account
    };

    // one report arrived; returns the worst over every receiver still remembered
    Worst add(const std::string& from_ip, uint8_t reporter, uint16_t loss_permille,
              uint32_t jitter_us, uint32_t now_us) {
        Entry* e = nullptr;
        for (auto& x : entries_)
            if (x.ip == from_ip && x.reporter == reporter) e = &x;
        if (!e) {
            entries_.push_back({from_ip, reporter, 0, 0, 0});
            e = &entries_.back();
        }
        e->loss_permille = loss_permille;
        e->jitter_us = jitter_us;
        e->at_us = now_us;
        return worst(now_us);
    }

    Worst worst(uint32_t now_us) {
        forget(now_us);
        Worst w;
        for (const auto& e : entries_) {
            w.loss_permille = std::max(w.loss_permille, e.loss_permille);
            w.jitter_us = std::max(w.jitter_us, e.jitter_us);
            w.receivers++;
        }
        return w;
    }

    // after the destinations change: forget receivers at addresses we no longer send to
    void keep_only(const std::vector<std::string>& ips) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& e) {
                                          return std::find(ips.begin(), ips.end(), e.ip) ==
                                                 ips.end();
                                      }),
                       entries_.end());
    }

    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::string ip;
        uint8_t reporter;
        uint16_t loss_permille;
        uint32_t jitter_us;
        uint32_t at_us;
    };

    void forget(uint32_t now_us) {
        // now - at wraps correctly with 32-bit times as long as it's under ~71 minutes
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& e) { return now_us - e.at_us > kForgetUs; }),
                       entries_.end());
    }

    std::vector<Entry> entries_;
};

}  // namespace video
