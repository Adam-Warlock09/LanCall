#pragma once
// LipSync — decides which received video frame to show, given the audio being heard
// (Person 2, call/). Header-only and templated on the frame type, so the app uses cv::Mat
// and the unit tests use plain numbers.
//
// Both clocks involved are the SENDER's: video frames carry their capture time, and
// AudioReceiver::playing_capture_us() gives the capture time of the audio leaving the speaker
// now. Comparing the two needs no clock agreement between laptops.
//
// Rules, applied by select() on every pass of the app's display loop:
//   audio playing   show the newest queued frame captured no later than the audio clock;
//                   older queued frames are skipped. Frames newer than the audio wait.
//   no audio        (before the first packet, buffering, underrun) show the newest frame at
//                   once, so video never freezes waiting for audio.
//   waited too long if every queued frame is ahead of the audio and the oldest has waited
//                   max_wait_us (local clock), show it anyway, so a stalled audio clock cannot
//                   hold video back indefinitely.
//   queue bound     at most max_queue frames; the oldest is dropped when full.
// All timestamp comparisons are mod 2^32. Not thread-safe: use from one thread (the UI loop).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>

namespace call {

struct LipSyncConfig {
    size_t max_queue = 30;          // ~2 s at 15 fps
    uint32_t max_wait_us = 500000;  // longest a frame waits for the audio to catch up
};

struct LipSyncCounters {
    uint64_t pushed = 0;        // frames received
    uint64_t shown = 0;         // frames returned by select()
    uint64_t skipped = 0;       // frames passed over because a newer one was due
    uint64_t dropped_full = 0;  // frames dropped because the queue was full
    uint64_t forced = 0;        // shown after waiting max_wait_us for the audio
    uint64_t no_audio = 0;      // shown while no audio was playing
};

template <typename Frame>
class LipSync {
public:
    explicit LipSync(LipSyncConfig cfg = {}) : cfg_(cfg) {
        if (cfg_.max_queue == 0) cfg_.max_queue = 1;
    }

    // Queue a decoded frame. capture_us: sender's clock; arrival_local_us: our clock, now.
    void push(Frame frame, uint32_t capture_us, uint32_t arrival_local_us) {
        if (queue_.size() >= cfg_.max_queue) {
            queue_.pop_front();
            ++counters_.dropped_full;
        }
        queue_.push_back(Entry{std::move(frame), capture_us, arrival_local_us});
        ++counters_.pushed;
    }

    // Picks the frame to show now. audio_clock: sender-clock capture time of the audio being
    // heard, or empty if none is playing. Returns true and fills `out` and `out_capture_us`
    // when a new frame should be shown; false means keep showing the current one.
    bool select(std::optional<uint32_t> audio_clock, uint32_t local_now_us, Frame& out,
                uint32_t& out_capture_us) {
        if (queue_.empty()) return false;

        if (!audio_clock) {
            ++counters_.no_audio;
            return take(queue_.size() - 1, out, out_capture_us);
        }

        // Newest frame captured no later than the audio clock.
        std::optional<size_t> due;
        for (size_t i = 0; i < queue_.size(); ++i) {
            if (static_cast<int32_t>(queue_[i].capture_us - *audio_clock) <= 0) due = i;
        }
        if (due) return take(*due, out, out_capture_us);

        // Every frame is ahead of the audio. Wait, unless the oldest has waited too long.
        const uint32_t waited = local_now_us - queue_.front().arrival_local_us;
        if (static_cast<int32_t>(waited) >= static_cast<int32_t>(cfg_.max_wait_us)) {
            ++counters_.forced;
            return take(0, out, out_capture_us);
        }
        return false;
    }

    size_t queued() const { return queue_.size(); }
    const LipSyncCounters& counters() const { return counters_; }
    void clear() { queue_.clear(); }

private:
    struct Entry {
        Frame frame;
        uint32_t capture_us;
        uint32_t arrival_local_us;
    };

    // Returns queue_[index], dropping it and every older frame.
    bool take(size_t index, Frame& out, uint32_t& out_capture_us) {
        out = std::move(queue_[index].frame);
        out_capture_us = queue_[index].capture_us;
        counters_.skipped += index;
        queue_.erase(queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(index) + 1);
        ++counters_.shown;
        return true;
    }

    LipSyncConfig cfg_;
    std::deque<Entry> queue_;
    LipSyncCounters counters_;
};

}  // namespace call
