#include "audio/playout_buffer.hpp"

#include <algorithm>
#include <cstring>

namespace audio {

PlayoutBuffer::PlayoutBuffer(int prebuffer, int max_chunks)
    : prebuffer_(std::clamp(prebuffer, 1, kSlots / 2)),
      max_chunks_(std::clamp(max_chunks, prebuffer_ + 1, kSlots - 1)) {}

PlayoutBuffer::Insert PlayoutBuffer::insert(uint16_t seq, uint32_t capture_us,
                                            const int16_t* samples) {
    if (!started_.load(std::memory_order_relaxed)) {
        // First packet: the consumer is still waiting and does not touch head_pub_ yet.
        head_pub_.store(seq, std::memory_order_relaxed);
        newest_.store(seq, std::memory_order_relaxed);
        started_.store(true, std::memory_order_release);
    }

    const uint16_t head = head_pub_.load(std::memory_order_acquire);
    const int ahead = seq_diff(seq, head);
    if (ahead >= kSlots || ahead < -kSlots) {
        // Far outside the buffer: a long outage, or the sender restarted with new seqs.
        // It cannot be stored; ask the consumer to jump to it.
        newest_.store(seq, std::memory_order_relaxed);
        resync_.store(true, std::memory_order_release);
        bump(out_of_range_);
        return Insert::OutOfRange;
    }
    if (ahead < 0) {
        bump(late_);
        return Insert::Late;
    }

    Slot& slot = slots_[seq % kSlots];
    if (slot.tag.load(std::memory_order_acquire) == seq) {
        bump(duplicates_);
        return Insert::Duplicate;
    }
    // Safe to overwrite: the consumer only reads a slot whose tag equals its play position,
    // and any seq sharing this slot with an unplayed chunk would be 64+ ahead (rejected above).
    std::memcpy(slot.samples.data(), samples, sizeof(int16_t) * kChunkSamples);
    slot.capture_us = capture_us;
    slot.tag.store(seq, std::memory_order_release);
    if (seq_diff(seq, newest_.load(std::memory_order_relaxed)) > 0) {
        newest_.store(seq, std::memory_order_release);
    }
    return Insert::Stored;
}

void PlayoutBuffer::skip_to(uint16_t new_head) {
    // Forget every queued chunk outside the new window [new_head, new_head + kSlots):
    // skipped ones and, after a sender restart, leftovers from the old sequence space.
    // Compare-exchange so a chunk the producer has just stored is not wiped by mistake.
    for (Slot& slot : slots_) {
        const uint32_t tag = slot.tag.load(std::memory_order_acquire);
        if (tag == kEmpty) continue;
        const int pos = seq_diff(static_cast<uint16_t>(tag), new_head);
        if (pos < 0 || pos >= kSlots) {
            uint32_t expected = tag;
            slot.tag.compare_exchange_strong(expected, kEmpty, std::memory_order_acq_rel);
        }
    }
    head_ = new_head;
    head_pub_.store(head_, std::memory_order_release);
}

bool PlayoutBuffer::next_chunk(int16_t* out, ChunkTime& time) {
    auto silence = [out] { std::memset(out, 0, sizeof(int16_t) * kChunkSamples); };
    time = ChunkTime{};

    if (state_ == State::Waiting) {
        if (!started_.load(std::memory_order_acquire)) {
            silence();
            bump(idle_);
            return true;
        }
        head_ = head_pub_.load(std::memory_order_relaxed);
        state_ = State::Buffering;
    }

    if (resync_.exchange(false, std::memory_order_acq_rel)) {
        // Start over just after the out-of-range packet (which could not be stored): the
        // packets that follow it fill the buffer, and playback resumes after `prebuffer`.
        const uint16_t target = newest_.load(std::memory_order_acquire);
        skip_to(static_cast<uint16_t>(target + 1));
        bump(resyncs_);
        state_ = State::Buffering;
        have_ref_ = false;  // a restarted sender has a new sequence space and timeline
        high_run_ = 0;
    }

    const uint16_t newest = newest_.load(std::memory_order_acquire);
    int depth = seq_diff(newest, head_) + 1;  // chunks from head_ to newest, some maybe missing

    if (depth > max_chunks_) {
        // Too much queued (clock drift, a burst, recovery after an outage): keep only
        // `prebuffer` chunks so the added delay stays small.
        const uint16_t new_head = static_cast<uint16_t>(newest - (prebuffer_ - 1));
        bump(trimmed_, static_cast<uint64_t>(seq_diff(new_head, head_)));
        skip_to(new_head);
        depth = prebuffer_;
        state_ = State::Playing;
        high_run_ = 0;
    }

    if (state_ == State::Buffering) {
        if (depth < prebuffer_) {
            silence();
            bump(buffering_);
            return true;
        }
        state_ = State::Playing;
    }

    // Delay creep (issue #8): a buffer that has stayed deeper than it needs to for a whole
    // second gives back one chunk (20 ms). One drop per second, so the step is hard to hear,
    // and a deep buffer still drains back to normal within a few seconds.
    if (depth > prebuffer_ + kDriftHeadroom) {
        if (++high_run_ >= kDriftWindow) {
            skip_to(static_cast<uint16_t>(head_ + 1));
            --depth;
            high_run_ = 0;
            bump(drift_trims_);
        }
    } else {
        high_run_ = 0;
    }

    Slot& slot = slots_[head_ % kSlots];
    if (slot.tag.load(std::memory_order_acquire) == head_) {
        std::memcpy(out, slot.samples.data(), sizeof(int16_t) * kChunkSamples);
        time.valid = true;
        time.capture_us = slot.capture_us;
        slot.tag.store(kEmpty, std::memory_order_release);
        have_ref_ = true;
        ref_seq_ = head_;
        ref_capture_ = time.capture_us;
        bump(played_);
    } else if (depth > 1) {
        // This chunk never arrived (or is very late) but newer ones are queued: conceal.
        // Its capture time follows from the last played chunk, 20 ms per sequence number.
        silence();
        if (have_ref_) {
            time.valid = true;
            time.capture_us = ref_capture_ + static_cast<uint32_t>(seq_diff(head_, ref_seq_) *
                                                                   static_cast<int32_t>(kChunkUs));
        }
        bump(concealed_);
    } else {
        // Nothing queued at or after the play position: re-buffer. head_ stays, so packets
        // that are merely delayed still play when they arrive.
        silence();
        bump(underruns_);
        state_ = State::Buffering;
        return true;
    }
    ++head_;
    head_pub_.store(head_, std::memory_order_release);
    return true;
}

PlayoutBuffer::Counters PlayoutBuffer::counters() const {
    Counters c;
    c.played = played_.load(std::memory_order_relaxed);
    c.concealed = concealed_.load(std::memory_order_relaxed);
    c.underruns = underruns_.load(std::memory_order_relaxed);
    c.buffering = buffering_.load(std::memory_order_relaxed);
    c.idle = idle_.load(std::memory_order_relaxed);
    c.late = late_.load(std::memory_order_relaxed);
    c.duplicates = duplicates_.load(std::memory_order_relaxed);
    c.out_of_range = out_of_range_.load(std::memory_order_relaxed);
    c.trimmed = trimmed_.load(std::memory_order_relaxed);
    c.resyncs = resyncs_.load(std::memory_order_relaxed);
    c.drift_trims = drift_trims_.load(std::memory_order_relaxed);
    return c;
}

int PlayoutBuffer::depth_chunks() const {
    if (!started_.load(std::memory_order_acquire)) return 0;
    const int d = seq_diff(newest_.load(std::memory_order_acquire),
                           head_pub_.load(std::memory_order_acquire)) +
                  1;
    return std::max(d, 0);
}

}  // namespace audio
