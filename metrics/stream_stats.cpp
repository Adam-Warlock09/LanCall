#include "metrics/stream_stats.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <utility>

namespace metrics {

namespace {

// a - b for two uint32 microsecond times, correct across the 2^32 wrap.
int32_t time_diff(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}

size_t ring_slot(int64_t ext, size_t size) {
    const int64_t n = static_cast<int64_t>(size);
    return static_cast<size_t>(((ext % n) + n) % n);
}

}  // namespace

StreamStats::StreamStats(std::string label) : label_(std::move(label)), seen_(kDupWindow, -1) {}

void StreamStats::restart_locked(uint16_t seq, uint32_t arrival_us) {
    started_ = true;
    max_seq_ = seq;
    cycles_ = 0;
    base_ext_ = seq;
    bad_seq_ = 0x10000;
    std::fill(seen_.begin(), seen_.end(), -1);
    have_transit_ = false;
    jitter_us_ = 0.0;
    totals_ = Counters{};
    interval_prior_ = Counters{};
    interval_expected_prior_ = 0;
    first_time_ = last_time_ = interval_start_ = last_packet_ = arrival_us;
}

int64_t StreamStats::expected_locked() const {
    return started_ ? cycles_ + max_seq_ - base_ext_ + 1 : 0;
}

void StreamStats::touch_time_locked(uint32_t t) {
    if (time_diff(t, last_time_) > 0) last_time_ = t;
}

void StreamStats::on_packet(uint16_t seq, uint32_t timestamp_us, size_t bytes,
                            uint32_t arrival_us) {
    std::lock_guard<std::mutex> lock(mu_);

    int64_t ext = 0;  // extended sequence number (counts wraps past 65535)
    bool late = false;
    if (!started_) {
        restart_locked(seq, arrival_us);
        ext = base_ext_;
    } else {
        const uint16_t udelta = static_cast<uint16_t>(seq - max_seq_);
        const bool quiet = time_diff(arrival_us, last_packet_) > kRestartQuietUs;
        if (quiet && seq <= kFreshSeq && udelta != 1) {
            // a sender that started over at 0 after a pause: a new call in the same log
            finished_.push_back(make_snapshot_locked(
                totals_, expected_locked(), static_cast<uint32_t>(last_time_ - first_time_)));
            ++restarts_;
            restart_locked(seq, arrival_us);
            ext = base_ext_;
        } else if (udelta < kMaxDropout) {
            // In order, possibly after a gap (the gap is the loss).
            if (seq < max_seq_) cycles_ += 65536;  // wrapped past 65535
            max_seq_ = seq;
            ext = cycles_ + seq;
        } else if (udelta <= 65536 - kMaxMisorder) {
            // Big jump. RFC 3550: ignore it unless the next packet continues from it,
            // which means the sender restarted.
            if (seq == bad_seq_) {
                // keep the run that just ended before restart_locked() wipes its totals
                finished_.push_back(make_snapshot_locked(
                    totals_, expected_locked(), static_cast<uint32_t>(last_time_ - first_time_)));
                ++restarts_;
                restart_locked(seq, arrival_us);
                ext = base_ext_;
            } else {
                bad_seq_ = static_cast<uint16_t>(seq + 1);
                return;
            }
        } else {
            // Up to kMaxMisorder behind the highest seq: a late (reordered) or duplicate packet.
            ext = cycles_ + seq;
            if (seq > max_seq_) ext -= 65536;  // it belongs to the cycle before the last wrap
            late = true;
        }
    }

    ++totals_.packets;
    totals_.bytes += bytes;
    touch_time_locked(arrival_us);
    if (time_diff(arrival_us, last_packet_) > 0) last_packet_ = arrival_us;

    int64_t& slot = seen_[ring_slot(ext, seen_.size())];
    if (slot == ext) {
        ++totals_.duplicates;
        return;
    }
    slot = ext;
    ++totals_.received;
    if (late) ++totals_.reordered;

    // RFC 3550 A.8 interarrival jitter, from packets in order only (see the header)
    if (late) return;
    const uint32_t transit = arrival_us - timestamp_us;
    if (have_transit_) {
        const double d = std::abs(static_cast<double>(time_diff(transit, last_transit_)));
        jitter_us_ += (d - jitter_us_) / 16.0;
    }
    last_transit_ = transit;
    have_transit_ = true;
}

void StreamStats::on_frame(uint32_t shown_us) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return;  // no packet yet: no window to count frames in
    ++totals_.frames;
    touch_time_locked(shown_us);
}

StreamStats::Snapshot StreamStats::make_snapshot_locked(const Counters& c, int64_t expected,
                                                        uint32_t window_us) const {
    Snapshot s;
    s.expected = expected > 0 ? static_cast<uint64_t>(expected) : 0;
    s.received = c.received;
    s.lost = s.expected > s.received ? s.expected - s.received : 0;
    s.reordered = c.reordered;
    s.duplicates = c.duplicates;
    s.frames = c.frames;
    s.loss_pct = s.expected > 0 ? 100.0 * static_cast<double>(s.lost) / s.expected : 0.0;
    s.jitter_ms = jitter_us_ / 1000.0;
    s.window_s = window_us / 1e6;
    if (window_us > 0) {
        s.pps = c.packets / s.window_s;
        s.kbps = c.bytes * 8.0 / s.window_s / 1000.0;
        s.fps = c.frames / s.window_s;
    }
    return s;
}

StreamStats::Snapshot StreamStats::total() const {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return Snapshot{};
    Snapshot s = make_snapshot_locked(totals_, expected_locked(),
                                      static_cast<uint32_t>(last_time_ - first_time_));
    s.restarts = restarts_;
    return s;
}

std::vector<StreamStats::Snapshot> StreamStats::segments() const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Snapshot> out = finished_;
    if (started_)
        out.push_back(make_snapshot_locked(totals_, expected_locked(),
                                           static_cast<uint32_t>(last_time_ - first_time_)));
    return out;
}

StreamStats::Snapshot StreamStats::take_interval(uint32_t now_us) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!started_) return Snapshot{};

    Counters d;
    d.received = totals_.received - interval_prior_.received;
    d.reordered = totals_.reordered - interval_prior_.reordered;
    d.duplicates = totals_.duplicates - interval_prior_.duplicates;
    d.packets = totals_.packets - interval_prior_.packets;
    d.bytes = totals_.bytes - interval_prior_.bytes;
    d.frames = totals_.frames - interval_prior_.frames;
    const int64_t expected_now = expected_locked();
    const int32_t window = time_diff(now_us, interval_start_);

    Snapshot s =
        make_snapshot_locked(d, expected_now - static_cast<int64_t>(interval_expected_prior_),
                             window > 0 ? static_cast<uint32_t>(window) : 0);

    interval_prior_ = totals_;
    interval_expected_prior_ = static_cast<uint64_t>(expected_now);
    interval_start_ = now_us;
    return s;
}

std::string StreamStats::format(const std::string& label, const Snapshot& s) {
    char buf[320];
    int n =
        std::snprintf(buf, sizeof buf,
                      "%-6s loss %5.1f%% (%" PRIu64 "/%" PRIu64 ")  jitter %6.2f ms  reord %" PRIu64
                      "  dup %" PRIu64 "  %6.1f pkt/s  %7.1f kbps",
                      label.c_str(), s.loss_pct, s.lost, s.expected, s.jitter_ms, s.reordered,
                      s.duplicates, s.pps, s.kbps);
    std::string out(buf, n > 0 ? std::min<size_t>(n, sizeof buf - 1) : 0);
    if (s.frames > 0) {
        std::snprintf(buf, sizeof buf, "  %5.1f fps", s.fps);
        out += buf;
    }
    if (s.restarts > 0) {
        std::snprintf(buf, sizeof buf, "  restarts %" PRIu64, s.restarts);
        out += buf;
    }
    return out;
}

}  // namespace metrics