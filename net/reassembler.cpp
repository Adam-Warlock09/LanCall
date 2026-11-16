#include "reassembler.hpp"

namespace net {

bool Reassembler::push(const proto::Header& h, const uint8_t* payload, size_t len, uint32_t now_us,
                       Frame& out) {
    if (have_last_ && !newer(h.frame_id, last_done_)) {
        stats_.late_pieces++;
        return false;
    }

    auto it = pending_.find(h.frame_id);
    if (it == pending_.end()) {
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

    last_done_ = h.frame_id;
    have_last_ = true;
    stats_.completed++;
    pending_.erase(it);

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
