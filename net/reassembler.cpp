#include "reassembler.hpp"

namespace net {

bool Reassembler::push(const proto::Header& h, const uint8_t* payload, size_t len, uint32_t now_us,
                       Frame& out) {
    auto it = pending_.find(h.frame_id);
    if (it == pending_.end()) {
        bool late;
        if (!independent_) {
            late = have_last_ && !newer(h.frame_id, last_done_);
        } else {
            // far behind isn't a late piece (those are ~10 strips behind at most): the sender
            // restarted and counts from 0 again, so start over with it
            if (have_last_ && static_cast<int16_t>(last_done_ - h.frame_id) > kLateWindow) {
                done_.clear();
                have_last_ = false;
            }
            late = recently_done(h.frame_id);
        }
        if (late) {
            stats_.late_pieces++;
            return false;
        }

        Partial p;
        p.timestamp_us = h.timestamp_us;
        p.first_seen_us = now_us;
        p.count = h.frag_count;
        p.pieces.resize(h.frag_count);
        p.have.assign(h.frag_count, false);
        it = pending_.emplace(h.frame_id, std::move(p)).first;
    }
    Partial& p = it->second;

    if (h.frag_count != p.count) {
        stats_.bad_pieces++;
        return false;
    }
    if (p.have[h.frag_index]) {
        stats_.duplicate_pieces++;
        return false;
    }

    p.pieces[h.frag_index].assign(payload, payload + len);
    p.have[h.frag_index] = true;
    p.received++;
    p.bytes += len;

    if (p.received < p.count) return false;

    // got everything: glue the pieces back together in index order
    out.frame_id = h.frame_id;
    out.timestamp_us = p.timestamp_us;
    out.data.clear();
    out.data.reserve(p.bytes);
    for (auto& piece : p.pieces) out.data.insert(out.data.end(), piece.begin(), piece.end());

    stats_.completed++;
    pending_.erase(it);

    if (independent_) {
        done_.push_back(h.frame_id);
        if (done_.size() > kRememberDone) done_.pop_front();
        if (!have_last_ || newer(h.frame_id, last_done_)) last_done_ = h.frame_id;
        have_last_ = true;
        return true;  // older strips keep waiting for their missing pieces
    }

    last_done_ = h.frame_id;
    have_last_ = true;

    // anything still waiting that's older than this frame will never be shown
    for (auto i = pending_.begin(); i != pending_.end();) {
        if (!newer(i->first, last_done_)) {
            stats_.dropped_incomplete++;
            i = pending_.erase(i);
        } else {
            ++i;
        }
    }
    return true;
}

void Reassembler::expire(uint32_t now_us) {
    for (auto i = pending_.begin(); i != pending_.end();) {
        // unsigned subtraction handles the clock wrapping around
        if (now_us - i->second.first_seen_us > timeout_us_) {
            stats_.dropped_incomplete++;
            i = pending_.erase(i);
        } else {
            ++i;
        }
    }
}

}  // namespace net
