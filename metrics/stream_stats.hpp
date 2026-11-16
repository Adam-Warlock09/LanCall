#pragma once
// StreamStats — receive-side statistics for one media stream (Person 2, metrics/).
//
// Used by audio_receiver, video_receiver and the log summarizer, so all three report
// identical numbers. Feed it every received datagram (every video piece, every audio
// packet) and, for video, every frame shown.
//
//   loss %   RFC 3550 A.1/A.3: expected = extended highest seq - base seq + 1,
//            lost = expected - unique packets received. Handles seq wrap past 65535,
//            reordering and duplicates. A jump of more than 3000 seqs is treated like
//            RFC 3550: the packet is ignored unless the next one continues from it, in
//            which case the sender is assumed to have restarted and counting starts over.
//   jitter   RFC 3550 A.8: transit = arrival - timestamp, D = transit - previous transit,
//            J += (|D| - J) / 16. Sender and receiver clocks need not agree, only differences
//            are used.
//   rates    packets/s and kbit/s over all datagrams that arrived (headers included,
//            whatever byte count the caller passes); fps over on_frame() calls.
//
// Times are proto::now_us() values (uint32 microseconds). Differences are taken modulo
// 2^32, so any single window must be shorter than about 71 minutes.
// All methods are thread-safe.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace metrics {

class StreamStats {
public:
    explicit StreamStats(std::string label);

    // One received datagram. arrival_us = proto::now_us() when it was received.
    void on_packet(uint16_t seq, uint32_t timestamp_us, size_t bytes, uint32_t arrival_us);

    // One complete video frame shown (or audio chunk played, if the caller wants that rate).
    // Ignored until the first packet has arrived.
    void on_frame(uint32_t shown_us);

    struct Snapshot {
        uint64_t expected = 0;    // packets the sender sent, judging by sequence numbers
        uint64_t received = 0;    // unique packets that arrived
        uint64_t lost = 0;        // expected - received, never negative
        uint64_t reordered = 0;   // arrived after a packet with a higher seq
        uint64_t duplicates = 0;  // same seq seen twice (within the last 1024 seqs)
        uint64_t restarts = 0;    // sender restarts detected (big seq jump), total only
        uint64_t frames = 0;      // on_frame() calls
        double loss_pct = 0.0;
        double jitter_ms = 0.0;  // current RFC 3550 jitter estimate
        double window_s = 0.0;   // time span the rates are computed over
        double pps = 0.0;        // datagrams per second (duplicates included)
        double kbps = 0.0;       // kilobits per second
        double fps = 0.0;        // frames per second
    };

    // Everything since the first packet (or since the last sender restart).
    // The window runs from the first packet to the latest packet or frame, and rates are
    // count / window, so N evenly spaced events read as N/(N-1) times the true rate: +3% for
    // a 2 s run, +0.3% for 5 minutes. take_interval() windows have a fixed length and no bias.
    Snapshot total() const;

    // Everything since the previous take_interval() call (or the first packet), with the
    // window ending at now_us; then starts a new interval. Use for the once-a-second line.
    Snapshot take_interval(uint32_t now_us);

    // One line, e.g. "audio  loss 1.0% (10/1000)  jitter 0.85 ms  reord 0  dup 0  50.0 pkt/s  260.8
    // kbps"
    static std::string format(const std::string& label, const Snapshot& s);
    std::string format(const Snapshot& s) const { return format(label_, s); }
    const std::string& label() const { return label_; }

private:
    static constexpr uint32_t kMaxDropout = 3000;  // RFC 3550 A.1
    static constexpr uint32_t kMaxMisorder = 100;  // RFC 3550 A.1
    static constexpr size_t kDupWindow = 1024;     // recent seqs remembered for duplicates

    struct Counters {
        uint64_t received = 0, reordered = 0, duplicates = 0;
        uint64_t packets = 0, bytes = 0, frames = 0;
    };

    void restart_locked(uint16_t seq, uint32_t arrival_us);
    int64_t expected_locked() const;
    Snapshot make_snapshot_locked(const Counters& c, int64_t expected, uint32_t window_us) const;
    void touch_time_locked(uint32_t t);

    std::string label_;
    mutable std::mutex mu_;

    bool started_ = false;
    uint16_t max_seq_ = 0;
    int64_t cycles_ = 0;          // 65536 * number of seq wraps
    int64_t base_ext_ = 0;        // extended seq of the first packet
    uint32_t bad_seq_ = 0x10000;  // RFC 3550: seq that would confirm a restart (none yet)
    uint64_t restarts_ = 0;

    std::vector<int64_t> seen_;  // ring of recent extended seqs, for duplicate detection

    bool have_transit_ = false;
    uint32_t last_transit_ = 0;  // arrival - timestamp, modulo 2^32
    double jitter_us_ = 0.0;

    Counters totals_;          // since start
    Counters interval_prior_;  // totals_ at the start of the current interval
    uint64_t interval_expected_prior_ = 0;

    uint32_t first_time_ = 0;  // first packet arrival
    uint32_t last_time_ = 0;   // latest packet arrival or frame time
    uint32_t interval_start_ = 0;
};

}  // namespace metrics
