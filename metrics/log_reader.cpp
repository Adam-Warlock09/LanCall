#include "metrics/log_reader.hpp"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace metrics::logs {

namespace {

constexpr uint32_t kSecondUs = 1000000;

int32_t time_diff(uint32_t a, uint32_t b) {
    return static_cast<int32_t>(a - b);
}

std::vector<std::string> split_commas(const std::string& line) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t comma = line.find(',', start);
        if (comma == std::string::npos) {
            out.push_back(line.substr(start));
            return out;
        }
        out.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
}

// Whole unsigned decimal number no larger than max.
bool parse_uint(const std::string& s, uint64_t max, uint64_t& out) {
    if (s.empty() || s.size() > 20) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    errno = 0;
    const unsigned long long v = std::strtoull(s.c_str(), nullptr, 10);
    if (errno != 0 || v > max) return false;
    out = v;
    return true;
}

bool parse_double(const std::string& s, double& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno != 0 || end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

}  // namespace

RowKind parse_row(const std::string& raw, PacketRow& packet, EventRow& event) {
    std::string line = raw;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') return RowKind::Skip;

    const auto f = split_commas(line);
    uint64_t v = 0;
    if (f.size() < 2 || !parse_uint(f[0], UINT32_MAX, v)) return RowKind::Invalid;
    const uint32_t local_us = static_cast<uint32_t>(v);

    if (f.size() == 4 && f[1] == "E") {
        if (f[2].empty() || !parse_double(f[3], event.value)) return RowKind::Invalid;
        event.local_us = local_us;
        event.name = f[2];
        return RowKind::Event;
    }
    if (f.size() == 9 && (f[1] == "S" || f[1] == "R")) {
        PacketRow p;
        p.local_us = local_us;
        p.dir = f[1][0];
        uint64_t type, stream, seq, frame, frag, bytes, ts;
        if (!parse_uint(f[2], 255, type) || !parse_uint(f[3], 255, stream) ||
            !parse_uint(f[4], 65535, seq) || !parse_uint(f[5], 65535, frame) ||
            !parse_uint(f[6], 255, frag) || !parse_uint(f[7], 1u << 20, bytes) ||
            !parse_uint(f[8], UINT32_MAX, ts)) {
            return RowKind::Invalid;
        }
        p.type = static_cast<unsigned>(type);
        p.stream = static_cast<unsigned>(stream);
        p.seq = static_cast<unsigned>(seq);
        p.frame = static_cast<unsigned>(frame);
        p.frag = static_cast<unsigned>(frag);
        p.bytes = bytes;
        p.timestamp_us = static_cast<uint32_t>(ts);
        packet = p;
        return RowKind::Packet;
    }
    return RowKind::Invalid;
}

std::string type_name(unsigned type) {
    switch (type) {
        case 1:
            return "video";
        case 2:
            return "audio";
        case 3:
            return "report";
        case 4:
            return "keepalive";
        default:
            return "type" + std::to_string(type);
    }
}

void Transit::add(int64_t us) {
    if (count == 0) {
        min_us = max_us = us;
    } else {
        min_us = std::min(min_us, us);
        max_us = std::max(max_us, us);
    }
    sum_us += static_cast<double>(us);
    ++count;
}

double SentStream::seconds() const {
    return packets > 1 ? static_cast<uint32_t>(last_us - first_us) / 1e6 : 0.0;
}

std::string LogSummary::stream_label(const StreamKey& k) {
    return type_name(k.type) + "/" + std::to_string(k.stream);
}

double LogSummary::span_s() const {
    return have_time_ ? static_cast<uint32_t>(last_us_ - first_us_) / 1e6 : 0.0;
}

bool LogSummary::add_file(const std::string& path, std::string& error) {
    std::ifstream f(path);
    if (!f) {
        error = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    std::string line;
    while (std::getline(f, line)) add_line(line);
    return true;
}

// Moves the clock to t, closing every 1 s interval that ended before it.
void LogSummary::advance_time(uint32_t t) {
    if (!have_time_) {
        have_time_ = true;
        first_us_ = last_us_ = t;
        next_boundary_ = t + kSecondUs;
        return;
    }
    if (time_diff(t, last_us_) > 0) last_us_ = t;
    if (!per_second_) return;
    while (time_diff(t, next_boundary_) >= 0) {
        const double t_s = static_cast<uint32_t>(next_boundary_ - first_us_) / 1e6;
        for (auto& [key, rs] : received_) {
            intervals_.push_back({t_s, stream_label(key), rs->stats.take_interval(next_boundary_)});
        }
        next_boundary_ += kSecondUs;
    }
}

ReceivedStream* LogSummary::video_stream() {
    for (auto& [key, rs] : received_) {
        if (key.dir == 'R' && key.type == 1) return rs.get();  // lowest stream id first
    }
    return nullptr;
}

void LogSummary::add_line(const std::string& line) {
    PacketRow p;
    EventRow e;
    switch (parse_row(line, p, e)) {
        case RowKind::Skip:
            return;
        case RowKind::Invalid:
            ++invalid_rows_;
            return;
        case RowKind::Event: {
            ++event_rows_;
            advance_time(e.local_us);
            EventTotal& t = events_[e.name];
            ++t.count;
            t.sum += e.value;
            if (e.name == "frame_shown") {
                if (ReceivedStream* v = video_stream()) v->stats.on_frame(e.local_us);
            }
            return;
        }
        case RowKind::Packet:
            break;
    }

    ++packet_rows_;
    advance_time(p.local_us);
    const StreamKey key{p.dir, p.type, p.stream};
    if (p.dir == 'S') {
        SentStream& s = sent_[key];
        if (s.packets == 0) s.first_us = p.local_us;
        s.last_us = p.local_us;
        ++s.packets;
        s.bytes += p.bytes;
        return;
    }
    auto& slot = received_[key];
    if (!slot) slot = std::make_unique<ReceivedStream>(stream_label(key));
    slot->stats.on_packet(static_cast<uint16_t>(p.seq), p.timestamp_us,
                          static_cast<size_t>(p.bytes), p.local_us);
    slot->transit.add(time_diff(p.local_us, p.timestamp_us));
}

void LogSummary::print(std::FILE* out, const std::string& title) const {
    std::fprintf(out, "== %s ==\n", title.c_str());
    std::fprintf(
        out, "rows: %" PRIu64 " packet, %" PRIu64 " event, %" PRIu64 " malformed; span %.1f s\n",
        packet_rows_, event_rows_, invalid_rows_, span_s());

    if (!received_.empty()) {
        std::fprintf(out, "received:\n");
        for (const auto& [key, rs] : received_) {
            const auto snap = rs->stats.total();
            std::fprintf(out, "  %s  (%.1f s)\n", rs->stats.format(snap).c_str(), snap.window_s);
            const Transit& tr = rs->transit;
            std::fprintf(out,
                         "    transit (arrival - timestamp): min %.2f ms  avg %.2f ms"
                         "  max %.2f ms  [a latency only if both ends share a clock]\n",
                         tr.min_us / 1000.0, tr.avg_us() / 1000.0, tr.max_us / 1000.0);
        }
    }
    if (!sent_.empty()) {
        std::fprintf(out, "sent:\n");
        for (const auto& [key, s] : sent_) {
            const double secs = s.seconds();
            std::fprintf(out, "  %-8s %8" PRIu64 " packets  %6.1f pkt/s  %7.1f kbps  (%.1f s)\n",
                         stream_label(key).c_str(), s.packets, secs > 0 ? s.packets / secs : 0.0,
                         secs > 0 ? s.bytes * 8.0 / secs / 1000.0 : 0.0, secs);
        }
    }
    if (!events_.empty()) {
        std::fprintf(out, "events:\n");
        for (const auto& [name, t] : events_) {
            std::fprintf(out, "  %-24s count %8" PRIu64 "  total %.10g\n", name.c_str(), t.count,
                         t.sum);
        }
    }
}

void LogSummary::print_interval_header(std::FILE* out) {
    std::fprintf(out, "file,t_s,stream,expected,lost,loss_pct,jitter_ms,pps,kbps,fps\n");
}

void LogSummary::print_intervals(std::FILE* out, const std::string& file) const {
    for (const auto& r : intervals_) {
        std::fprintf(out, "%s,%.0f,%s,%" PRIu64 ",%" PRIu64 ",%.3f,%.3f,%.1f,%.1f,%.1f\n",
                     file.c_str(), r.t_s, r.stream.c_str(), r.snap.expected, r.snap.lost,
                     r.snap.loss_pct, r.snap.jitter_ms, r.snap.pps, r.snap.kbps, r.snap.fps);
    }
}

}  // namespace metrics::logs
