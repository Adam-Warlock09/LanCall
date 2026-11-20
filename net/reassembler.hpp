#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <vector>

#include "protocol.hpp"

namespace net {

struct Frame {
    uint16_t frame_id = 0;
    uint32_t timestamp_us = 0;  // sender's capture time
    std::vector<uint8_t> data;
    // phase 5: one piece was rebuilt from the parity packet; its sequence number
    bool recovered = false;
    uint16_t recovered_seq = 0;
};

// Phase 5 FEC: one parity packet per frame (per strip, for video), sent after its pieces.
//
//   header: type VideoFec, same stream_id, timestamp and frame_id as the pieces,
//           seq = seq of piece 0, frag_index 0, frag_count = number of pieces
//   body:   [length xor: 2] [xor of every piece's payload, each padded with zeros to the
//            longest]
//
// With all pieces but one, the missing one is the parity XORed with the others, and its
// length is the length xor XORed with theirs. So any one lost piece per frame is rebuilt
// without asking anyone, and a 1-piece frame (parity = the piece) is simply sent twice.
// make_parity() builds the body; Reassembler::push_parity() takes it in.
std::vector<uint8_t> make_parity(const std::vector<std::vector<uint8_t>>& pieces);

// Collects pieces of frames and hands back a frame once every piece has arrived.
//
// Rules:
//  - a frame that isn't complete within `timeout_us` of its first piece is dropped
//  - once frame N is shown, anything older than N is pointless, so it's dropped too
//  - pieces for frames older than the last completed one are counted as late and ignored
//
// Independent mode (phase 5, video strips): each "frame" is a strip, and strips don't make
// each other pointless. Strip 3 waiting for a resent packet must survive strip 4 completing,
// or NACK could never help. So in this mode the last two rules become:
//  - older frames still waiting are kept until their timeout
//  - a piece for a frame that already completed (a duplicate or a needless resend) is late;
//    a piece for an older frame that never started (all its packets lost, now resent) is fine
//
// "now" is passed in rather than read from the clock so the tests can control time.
class Reassembler {
public:
    struct Stats {
        uint64_t completed = 0;
        uint64_t dropped_incomplete = 0;  // timed out or overtaken by a newer frame
        uint64_t late_pieces = 0;         // arrived after a newer frame was already done
        uint64_t duplicate_pieces = 0;
        uint64_t bad_pieces = 0;     // piece count disagreed with earlier pieces
        uint64_t fec_recovered = 0;  // pieces rebuilt from parity
    };

    explicit Reassembler(uint32_t timeout_us = 100000, bool independent = false)
        : timeout_us_(timeout_us), independent_(independent) {}

    // returns true and fills `out` when this piece completes a frame
    bool push(const proto::Header& h, const uint8_t* payload, size_t len, uint32_t now_us,
              Frame& out);

    // the parity packet of a frame (body as built by make_parity). returns true and fills
    // `out` when it completes the frame, with out.recovered set
    bool push_parity(const proto::Header& h, const uint8_t* body, size_t len, uint32_t now_us,
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
        bool have_parity = false;
        uint16_t parity_len_xor = 0;
        uint16_t first_seq = 0;  // seq of piece 0, from the parity header
        std::vector<uint8_t> parity;
        int rebuilt_index = -1;
    };

    // finds or starts the partial for this header; nullptr if the piece is late
    Partial* partial_for(const proto::Header& h, uint32_t now_us);
    // all pieces but one and the parity are in: rebuild the missing one. false if they
    // don't add up (a corrupt or mismatched parity)
    bool rebuild(Partial& p);
    // every piece is in: glue them into `out` and do the completion bookkeeping
    void complete(uint16_t frame_id, Partial& p, Frame& out);

    // frame ids wrap at 65535, so "newer" means "less than half the ring ahead"
    static bool newer(uint16_t a, uint16_t b) { return static_cast<int16_t>(a - b) > 0; }

    // independent mode: was this frame id completed recently?
    bool recently_done(uint16_t id) const {
        for (uint16_t d : done_)
            if (d == id) return true;
        return false;
    }

    static constexpr size_t kRememberDone = 256;  // ids, ~2.5 s of 10-strip video
    static constexpr int kLateWindow = 256;       // further behind than this: a restart

    uint32_t timeout_us_;
    bool independent_;
    std::deque<uint16_t> done_;  // independent mode: the last kRememberDone completed ids
    std::map<uint16_t, Partial> pending_;
    bool have_last_ = false;
    uint16_t last_done_ = 0;
    Stats stats_;
};

}  // namespace net
