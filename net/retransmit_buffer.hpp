#pragma once
// The sender's copy of what it sent recently, so it can answer NACKs (Person 1, net/,
// phase 5). Header-only, time passed in. Not frozen.
//
// kSlots datagrams in a ring indexed by sequence number: at ~300 video packets a second
// that's about 3 s, far more than a NACK can usefully be late (the receiver gives up after
// 80 ms). Each packet is resent at most kMaxResends times, so a receiver that keeps asking
// (or a NACK storm through the relay) can't make us send it over and over.
#include <cstdint>
#include <vector>

namespace net {

class RetransmitBuffer {
public:
    static constexpr size_t kSlots = 1024;
    static constexpr uint32_t kMaxAgeUs = 500000;  // older than this isn't worth resending
    static constexpr int kMaxResends = 2;

    RetransmitBuffer() : slots_(kSlots) {}

    void store(uint16_t seq, const uint8_t* data, size_t len, uint32_t now_us) {
        Slot& s = slots_[seq % kSlots];
        s.used = true;
        s.seq = seq;
        s.sent_us = now_us;
        s.resends = 0;
        s.data.assign(data, data + len);
    }

    // the datagram with this seq, if we still have it and may resend it (counts as a resend)
    const std::vector<uint8_t>* take(uint16_t seq, uint32_t now_us) {
        Slot& s = slots_[seq % kSlots];
        if (!s.used || s.seq != seq || now_us - s.sent_us > kMaxAgeUs) return nullptr;
        if (s.resends >= kMaxResends) return nullptr;
        s.resends++;
        return &s.data;
    }

private:
    struct Slot {
        bool used = false;
        uint16_t seq = 0;
        uint32_t sent_us = 0;
        int resends = 0;
        std::vector<uint8_t> data;
    };
    std::vector<Slot> slots_;
};

}  // namespace net
