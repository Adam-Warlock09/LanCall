#include "reassembler.hpp"

#include <algorithm>

namespace net {

std::vector<uint8_t> make_parity(const std::vector<std::vector<uint8_t>>& pieces) {
    size_t longest = 0;
    for (const auto& p : pieces) longest = std::max(longest, p.size());
    std::vector<uint8_t> body(2 + longest, 0);
    uint16_t len_xor = 0;
    for (const auto& p : pieces) {
        len_xor ^= static_cast<uint16_t>(p.size());
        for (size_t i = 0; i < p.size(); ++i) body[2 + i] ^= p[i];
    }
    body[0] = static_cast<uint8_t>(len_xor >> 8);
    body[1] = static_cast<uint8_t>(len_xor);
    return body;
}

Reassembler::Partial* Reassembler::partial_for(const proto::Header& h, uint32_t now_us) {
    auto it = pending_.find(h.frame_id);
    if (it != pending_.end()) return &it->second;

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
        return nullptr;
    }

    Partial p;
    p.timestamp_us = h.timestamp_us;
    p.first_seen_us = now_us;
    p.count = h.frag_count;
    p.pieces.resize(h.frag_count);
    p.have.assign(h.frag_count, false);
    return &pending_.emplace(h.frame_id, std::move(p)).first->second;
}

bool Reassembler::push(const proto::Header& h, const uint8_t* payload, size_t len, uint32_t now_us,
                       Frame& out) {
    Partial* pp = partial_for(h, now_us);
    if (!pp) return false;
    Partial& p = *pp;

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

    out.recovered = false;
    // one short and the parity is already here: rebuild the last one now
    if (p.received + 1 == p.count && p.have_parity && rebuild(p)) out.recovered = true;
    if (p.received < p.count) return false;
    complete(h.frame_id, p, out);
    return true;
}

bool Reassembler::push_parity(const proto::Header& h, const uint8_t* body, size_t len,
                              uint32_t now_us, Frame& out) {
    if (len < 2) {
        stats_.bad_pieces++;
        return false;
    }
    Partial* pp = partial_for(h, now_us);
    if (!pp) return false;
    Partial& p = *pp;
    if (h.frag_count != p.count) {
        stats_.bad_pieces++;
        return false;
    }
    if (p.have_parity) {
        stats_.duplicate_pieces++;
        return false;
    }
    p.have_parity = true;
    p.first_seq = h.seq;
    p.parity_len_xor = static_cast<uint16_t>((body[0] << 8) | body[1]);
    p.parity.assign(body + 2, body + len);

    if (p.received + 1 != p.count || !rebuild(p)) return false;
    out.recovered = true;
    complete(h.frame_id, p, out);
    return true;
}

bool Reassembler::rebuild(Partial& p) {
    size_t missing = 0;
    while (missing < p.count && p.have[missing]) ++missing;
    if (missing == p.count) return false;

    std::vector<uint8_t> piece = p.parity;
    uint16_t len = p.parity_len_xor;
    for (size_t i = 0; i < p.count; ++i) {
        if (i == missing) continue;
        const auto& q = p.pieces[i];
        if (q.size() > piece.size()) return false;  // longer than the parity: not ours
        len ^= static_cast<uint16_t>(q.size());
        for (size_t j = 0; j < q.size(); ++j) piece[j] ^= q[j];
    }
    if (len > piece.size()) return false;
    piece.resize(len);

    p.bytes += piece.size();
    p.pieces[missing] = std::move(piece);
    p.have[missing] = true;
    p.received++;
    p.rebuilt_index = static_cast<int>(missing);
    stats_.fec_recovered++;
    return true;
}

void Reassembler::complete(uint16_t frame_id, Partial& p, Frame& out) {
    // got everything: glue the pieces back together in index order
    out.frame_id = frame_id;
    out.timestamp_us = p.timestamp_us;
    if (out.recovered) out.recovered_seq = static_cast<uint16_t>(p.first_seq + p.rebuilt_index);
    out.data.clear();
    out.data.reserve(p.bytes);
    for (auto& piece : p.pieces) out.data.insert(out.data.end(), piece.begin(), piece.end());

    stats_.completed++;
    pending_.erase(frame_id);  // p is gone after this

    if (independent_) {
        done_.push_back(frame_id);
        if (done_.size() > kRememberDone) done_.pop_front();
        if (!have_last_ || newer(frame_id, last_done_)) last_done_ = frame_id;
        have_last_ = true;
        return;  // older strips keep waiting for their missing pieces
    }

    last_done_ = frame_id;
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
