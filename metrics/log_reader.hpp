#pragma once
// Log reader (Person 2, metrics/): parses the frozen metrics CSV and replays it through
// StreamStats, so tools/log_summary reports exactly the numbers the receivers print live.
//
//   packet: local_us,dir,type,stream,seq,frame,frag,bytes,timestamp_us
//   event:  local_us,E,name,value
//
// One LogSummary summarizes one file (one program's log). Received packets (R) are grouped
// by (type, stream) and fed to StreamStats with local_us as the arrival time; "frame_shown"
// events count as frames for the received video stream. Sent packets (S) are counted, events
// are totalled per name, and optional per-second intervals are recorded for graphs.

#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "metrics/stream_stats.hpp"

namespace metrics::logs {

struct PacketRow {
    uint32_t local_us = 0;
    char dir = 'R';  // 'S' or 'R'
    unsigned type = 0, stream = 0, seq = 0, frame = 0, frag = 0;
    uint64_t bytes = 0;
    uint32_t timestamp_us = 0;
};

struct EventRow {
    uint32_t local_us = 0;
    std::string name;
    double value = 0.0;
};

enum class RowKind { Packet, Event, Skip, Invalid };  // Skip: empty line or '#' comment

RowKind parse_row(const std::string& line, PacketRow& packet, EventRow& event);

// "video", "audio", "report", "keepalive" (proto::Type values 1-4), or "type<N>".
std::string type_name(unsigned type);

struct StreamKey {
    char dir;
    unsigned type;
    unsigned stream;
    bool operator<(const StreamKey& o) const {
        if (dir != o.dir) return dir < o.dir;
        if (type != o.type) return type < o.type;
        return stream < o.stream;
    }
};

// arrival - timestamp. Only a real one-way latency when sender and receiver share a clock
// (both programs on the same laptop), until Phase 2 adds clock-offset estimation.
struct Transit {
    uint64_t count = 0;
    int64_t min_us = 0, max_us = 0;
    double sum_us = 0.0;
    void add(int64_t us);
    double avg_us() const { return count ? sum_us / count : 0.0; }
};

struct ReceivedStream {
    explicit ReceivedStream(const std::string& label) : stats(label) {}
    StreamStats stats;
    Transit transit;
};

struct SentStream {
    uint64_t packets = 0, bytes = 0;
    uint32_t first_us = 0, last_us = 0;
    double seconds() const;
};

struct EventTotal {
    uint64_t count = 0;
    double sum = 0.0;
};

// Spread of a set of values, e.g. every video_e2e_ms event in a log. Percentiles use the
// nearest-rank method: p95 is the smallest value with at least 95% of values at or below it.
struct Distribution {
    size_t count = 0;
    double min = 0, mean = 0, p50 = 0, p95 = 0, max = 0;
};
Distribution summarize(std::vector<double> values);

// Events whose name ends in "_ms" (video_e2e_ms, audio_e2e_ms, av_sync_ms, buffer_ms, ...)
// are timings: log_summary reports their distribution instead of a total.
bool is_timing_event(const std::string& name);

struct IntervalRow {
    double t_s;  // end of the 1 s interval, seconds since the file's first row
    std::string stream;
    StreamStats::Snapshot snap;
};

class LogSummary {
public:
    explicit LogSummary(bool per_second = false) : per_second_(per_second) {}

    // Reads a whole file. False (and error set) if it cannot be opened.
    bool add_file(const std::string& path, std::string& error);
    void add_line(const std::string& line);

    const std::map<StreamKey, std::unique_ptr<ReceivedStream>>& received() const {
        return received_;
    }
    const std::map<StreamKey, SentStream>& sent() const { return sent_; }
    const std::map<std::string, EventTotal>& events() const { return events_; }
    // Every value of each timing event (see is_timing_event), in log order.
    const std::map<std::string, std::vector<double>>& timings() const { return timings_; }
    const std::vector<IntervalRow>& intervals() const { return intervals_; }
    uint64_t packet_rows() const { return packet_rows_; }
    uint64_t event_rows() const { return event_rows_; }
    uint64_t invalid_rows() const { return invalid_rows_; }
    double span_s() const;

    void print(std::FILE* out, const std::string& title) const;
    static void print_interval_header(std::FILE* out);
    void print_intervals(std::FILE* out, const std::string& file) const;

    static std::string stream_label(const StreamKey& k);

private:
    void advance_time(uint32_t t);
    ReceivedStream* video_stream();

    bool per_second_;
    bool have_time_ = false;
    uint32_t first_us_ = 0, last_us_ = 0, next_boundary_ = 0;

    std::map<StreamKey, std::unique_ptr<ReceivedStream>> received_;
    std::map<StreamKey, SentStream> sent_;
    std::map<std::string, EventTotal> events_;
    std::map<std::string, std::vector<double>> timings_;
    std::vector<IntervalRow> intervals_;
    uint64_t packet_rows_ = 0, event_rows_ = 0, invalid_rows_ = 0;
};

}  // namespace metrics::logs
