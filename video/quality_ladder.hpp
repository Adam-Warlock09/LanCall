#pragma once
// The video sender's quality ladder (Person 1, video/, phase 3). It turns receiver reports
// into a step: fps and JPEG quality. Header-only and given the time from outside, so the
// tests can replay any sequence of reports.
//
// Steps, from the sender's top fps F and quality Q (15 and 70 by default):
//
//   0: F,      Q          15 fps  q70
//   1: F,      3/4 Q      15 fps  q52
//   2: 4/5 F,  1/2 Q      12 fps  q35
//   3: 2/3 F,  2/5 Q      10 fps  q28
//   4: 1/2 F,  1/4 Q       7 fps  q17
//
// When to step down. Lowering the bitrate only helps when loss comes from congestion (a
// bandwidth cap, a full queue). Random loss stays the same whatever we send, and the strips
// already cope with it, so 5% random loss alone must NOT step down. So:
//   - loss above 10%                         one step down
//   - loss above 25%                         two steps down
//   - loss above 2% and jitter above 10 ms   one step down (a queue is filling: congestion)
//   - no report for 2 s (after at least one) one step down (the reports are getting lost too)
//   after a step down, wait 1 s before the next one, so one bad burst costs one step.
//
// When to step up: 5 s of reports with loss under 1% and jitter under 5 ms, one step at a
// time. If a step up is followed by a step down within 3 s, the probe failed, so the next
// wait doubles (10 s, 20 s, up to 40 s). It goes back to 5 s once we reach step 0 again.
// That stops the sender bouncing off a bandwidth cap every 5 s.
#include <algorithm>
#include <cstdint>
#include <vector>

namespace video {

class QualityLadder {
public:
    struct Step {
        int fps;
        int quality;
    };

    static constexpr uint16_t kDownLoss = 100;         // per mille, 10%
    static constexpr uint16_t kBigLoss = 250;          // 25%
    static constexpr uint16_t kQueueLoss = 20;         // 2%, together with high jitter
    static constexpr uint32_t kQueueJitterUs = 10000;  // 10 ms
    static constexpr uint16_t kGoodLoss = 10;          // 1%
    static constexpr uint32_t kGoodJitterUs = 5000;    // 5 ms
    static constexpr uint32_t kDownHoldUs = 1000000;   // 1 s between step downs
    static constexpr uint32_t kSilenceUs = 2000000;    // 2 s without reports
    static constexpr uint32_t kUpWaitUs = 5000000;     // 5 s of good reports to step up
    static constexpr uint32_t kMaxUpWaitUs = 40000000;
    static constexpr uint32_t kFailedProbeUs = 3000000;  // step down this soon after an up

    QualityLadder(int top_fps, int top_quality) {
        auto at_least = [](int v, int lo) { return std::max(v, lo); };
        steps_ = {
            {top_fps, top_quality},
            {top_fps, at_least(top_quality * 3 / 4, 10)},
            {at_least(top_fps * 4 / 5, 1), at_least(top_quality / 2, 10)},
            {at_least(top_fps * 2 / 3, 1), at_least(top_quality * 2 / 5, 10)},
            {at_least(top_fps / 2, 1), at_least(top_quality / 4, 10)},
        };
    }

    // a receiver report arrived
    void on_report(uint16_t loss_permille, uint32_t jitter_us, uint32_t now_us) {
        have_report_ = true;
        last_report_us_ = now_us;

        if (loss_permille > kBigLoss) {
            down(2, now_us);
        } else if (loss_permille > kDownLoss ||
                   (loss_permille > kQueueLoss && jitter_us > kQueueJitterUs)) {
            down(1, now_us);
        } else if (loss_permille < kGoodLoss && jitter_us < kGoodJitterUs) {
            if (!good_run_) {
                good_run_ = true;
                good_since_us_ = now_us;
            } else if (level_ > 0 && now_us - good_since_us_ >= up_wait_us_) {
                level_--;
                last_up_us_ = now_us;
                stepped_up_ = true;
                good_since_us_ = now_us;  // the next step up needs its own wait
                if (level_ == 0) up_wait_us_ = kUpWaitUs;
            }
        } else {
            good_run_ = false;  // in between: hold where we are
        }
    }

    // call regularly (every frame is fine): handles reports going missing
    void tick(uint32_t now_us) {
        if (have_report_ && now_us - last_report_us_ > kSilenceUs) {
            down(1, now_us);
            last_report_us_ = now_us;  // another step only after another 2 s of silence
        }
    }

    // we sent nothing (no destinations, phase 4), so no reports are owed: restart the
    // silence clock instead of stepping down for it. call it as often as you like
    void idle(uint32_t now_us) { last_report_us_ = now_us; }

    int level() const { return level_; }
    int levels() const { return static_cast<int>(steps_.size()); }
    Step step() const { return steps_[level_]; }
    uint32_t up_wait_us() const { return up_wait_us_; }

private:
    void down(int n, uint32_t now_us) {
        good_run_ = false;
        if (stepped_down_ && now_us - last_down_us_ < kDownHoldUs) return;
        if (stepped_up_ && now_us - last_up_us_ < kFailedProbeUs)
            up_wait_us_ = std::min(up_wait_us_ * 2, kMaxUpWaitUs);
        stepped_up_ = false;
        level_ = std::min(level_ + n, levels() - 1);
        last_down_us_ = now_us;
        stepped_down_ = true;
    }

    std::vector<Step> steps_;
    int level_ = 0;

    bool have_report_ = false;
    uint32_t last_report_us_ = 0;
    bool stepped_down_ = false;
    uint32_t last_down_us_ = 0;
    bool stepped_up_ = false;
    uint32_t last_up_us_ = 0;
    bool good_run_ = false;
    uint32_t good_since_us_ = 0;
    uint32_t up_wait_us_ = kUpWaitUs;
};

}  // namespace video