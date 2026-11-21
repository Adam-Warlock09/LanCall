// Unit tests for metrics::logs (CSV parsing and replay used by tools/log_summary).

#include <cstdint>
#include <filesystem>
#include <string>

#include "check.hpp"
#include "common/platform.hpp"
#include "metrics/csv_logger.hpp"
#include "metrics/log_reader.hpp"
#include "metrics/stream_stats.hpp"

namespace fs = std::filesystem;
using namespace metrics::logs;

namespace {

std::string packet_line(uint32_t local_us, char dir, unsigned type, unsigned stream, unsigned seq,
                        unsigned frame, unsigned frag, unsigned bytes, uint32_t ts) {
    return std::to_string(local_us) + "," + dir + "," + std::to_string(type) + "," +
           std::to_string(stream) + "," + std::to_string(seq) + "," + std::to_string(frame) + "," +
           std::to_string(frag) + "," + std::to_string(bytes) + "," + std::to_string(ts);
}

const StreamKey kAudioRx{'R', 2, 0};
const StreamKey kVideoRx{'R', 1, 0};

void test_parse_packet_row() {
    PacketRow p;
    EventRow e;
    CHECK(parse_row("81234567,R,2,0,17,0,0,652,81230011", p, e) == RowKind::Packet);
    CHECK(p.local_us == 81234567u);
    CHECK(p.dir == 'R');
    CHECK(p.type == 2);
    CHECK(p.stream == 0);
    CHECK(p.seq == 17);
    CHECK(p.frame == 0);
    CHECK(p.frag == 0);
    CHECK(p.bytes == 652);
    CHECK(p.timestamp_us == 81230011u);
    CHECK(parse_row("4294967295,S,1,3,65535,65535,254,1412,0\r", p, e) == RowKind::Packet);
    CHECK(p.local_us == 4294967295u);
    CHECK(p.frag == 254);
}

void test_parse_event_row() {
    PacketRow p;
    EventRow e;
    CHECK(parse_row("1000,E,frame_shown,1", p, e) == RowKind::Event);
    CHECK(e.local_us == 1000);
    CHECK(e.name == "frame_shown");
    CHECK(e.value == 1.0);
    CHECK(parse_row("2000,E,jitter_ms,-0.8125", p, e) == RowKind::Event);
    CHECK(e.value == -0.8125);
}

void test_parse_rejects_bad_rows() {
    PacketRow p;
    EventRow e;
    CHECK(parse_row("", p, e) == RowKind::Skip);
    CHECK(parse_row("# comment", p, e) == RowKind::Skip);
    CHECK(parse_row("1000,X,2,0,1,0,0,652,1", p, e) == RowKind::Invalid);      // bad dir
    CHECK(parse_row("1000,R,2,0,1,0,0,652", p, e) == RowKind::Invalid);        // 8 fields
    CHECK(parse_row("1000,R,2,0,70000,0,0,652,1", p, e) == RowKind::Invalid);  // seq > 65535
    CHECK(parse_row("1000,R,2,0,-1,0,0,652,1", p, e) == RowKind::Invalid);
    CHECK(parse_row("1000,R,2,0,1,0,0,652,1x", p, e) == RowKind::Invalid);
    CHECK(parse_row("4294967296,E,x,1", p, e) == RowKind::Invalid);  // local_us > 2^32-1
    CHECK(parse_row("abc,E,x,1", p, e) == RowKind::Invalid);
    CHECK(parse_row("1000,E,,1", p, e) == RowKind::Invalid);  // empty name
    CHECK(parse_row("1000,E,x,1.5.2", p, e) == RowKind::Invalid);
    CHECK(parse_row("1000,E,x", p, e) == RowKind::Invalid);
}

void test_replay_matches_stream_stats() {
    LogSummary sum;
    metrics::StreamStats direct("audio/0");
    for (uint32_t i = 0; i < 1000; ++i) {
        if (i % 50 == 7 || i % 333 == 1) continue;  // some loss
        const uint32_t ts = 5000000 + i * 20000;
        const uint32_t arrival = ts + 3000 + (i % 3) * 700;  // some jitter
        sum.add_line(packet_line(arrival, 'R', 2, 0, i, 0, 0, 652, ts));
        direct.on_packet(static_cast<uint16_t>(i), ts, 652, arrival);
    }
    CHECK(sum.received().count(kAudioRx) == 1);
    if (sum.received().count(kAudioRx) == 1) {
        const auto a = sum.received().at(kAudioRx)->stats.total();
        const auto b = direct.total();
        CHECK(a.expected == b.expected);
        CHECK(a.lost == b.lost);
        CHECK(a.loss_pct == b.loss_pct);
        CHECK(a.jitter_ms == b.jitter_ms);
        CHECK(a.pps == b.pps);
        CHECK(a.kbps == b.kbps);
        CHECK(a.lost > 0);
    }
    CHECK(sum.packet_rows() + 0 > 0);
    CHECK(sum.invalid_rows() == 0);
}

void test_sent_events_and_streams() {
    LogSummary sum;
    for (uint32_t i = 0; i < 51; ++i)
        sum.add_line(packet_line(i * 20000, 'S', 2, 0, i, 0, 0, 652, i * 20000));
    sum.add_line("100,E,underrun,1");
    sum.add_line("200,E,underrun,2");
    sum.add_line("300,E,concealed,1");
    sum.add_line("garbage line");
    CHECK(sum.sent().count(StreamKey{'S', 2, 0}) == 1);
    if (sum.sent().count(StreamKey{'S', 2, 0}) == 1) {
        const auto& s = sum.sent().at(StreamKey{'S', 2, 0});
        CHECK(s.packets == 51);
        CHECK_NEAR(s.seconds(), 1.0, 1e-9);
    }
    CHECK(sum.received().empty());
    CHECK(sum.events().at("underrun").count == 2);
    CHECK(sum.events().at("underrun").sum == 3.0);
    CHECK(sum.events().at("concealed").count == 1);
    CHECK(sum.invalid_rows() == 1);
    CHECK(sum.event_rows() == 3);
}

void test_frame_shown_gives_video_fps() {
    LogSummary sum;
    // 2 s of video: 15 frames/s, 3 pieces per frame; frame_shown after each frame.
    for (uint32_t f = 0; f < 31; ++f) {
        const uint32_t t = 1000000 + f * 66667;
        for (unsigned k = 0; k < 3; ++k) {
            sum.add_line(packet_line(t + k * 100, 'R', 1, 0, f * 3 + k, f, k, 1412, t - 2000));
        }
        sum.add_line(std::to_string(t + 1000) + ",E,frame_shown,1");
    }
    CHECK(sum.received().count(kVideoRx) == 1);
    if (sum.received().count(kVideoRx) == 1) {
        const auto v = sum.received().at(kVideoRx)->stats.total();
        CHECK(v.frames == 31);
        // total(): 31 frames over the span from the first packet (t=1.000000 s) to the last
        // frame_shown (t=3.001010 s); see the note on StreamStats::total().
        CHECK_NEAR(v.fps, 31 / 2.00101, 1e-6);
        CHECK(v.lost == 0);
    }
}

void test_timing_events() {
    CHECK(is_timing_event("video_e2e_ms"));
    CHECK(is_timing_event("av_sync_ms"));
    CHECK(is_timing_event("buffer_ms"));
    CHECK(!is_timing_event("_ms"));
    CHECK(!is_timing_event("ms"));
    CHECK(!is_timing_event("frame_shown"));
    CHECK(!is_timing_event("underrun"));

    LogSummary sum;
    for (int v = 100; v >= 1; --v) {  // values 1..100, logged in reverse order
        sum.add_line(std::to_string(1000 + v) + ",E,video_e2e_ms," + std::to_string(v));
    }
    sum.add_line("5000,E,audio_e2e_ms,-2.5");
    sum.add_line("6000,E,underrun,1");
    CHECK(sum.timings().size() == 2);
    CHECK(sum.timings().count("underrun") == 0);
    CHECK(sum.events().at("video_e2e_ms").count == 100);  // still counted as events too

    const Distribution v = summarize(sum.timings().at("video_e2e_ms"));
    CHECK(v.count == 100);
    CHECK(v.min == 1.0);
    CHECK(v.max == 100.0);
    CHECK_NEAR(v.mean, 50.5, 1e-12);
    CHECK(v.p50 == 50.0);  // nearest rank: ceil(0.50 * 100) = 50th value
    CHECK(v.p95 == 95.0);  // ceil(0.95 * 100) = 95th value

    const Distribution a = summarize(sum.timings().at("audio_e2e_ms"));
    CHECK(a.count == 1);
    CHECK(a.min == -2.5 && a.max == -2.5 && a.p50 == -2.5 && a.p95 == -2.5);

    const Distribution none = summarize({});
    CHECK(none.count == 0);

    const Distribution small = summarize({30, 10, 20});  // p95 of 3 values is the largest
    CHECK(small.p50 == 20.0);
    CHECK(small.p95 == 30.0);
}

void test_transit() {
    LogSummary sum;
    sum.add_line(packet_line(10000, 'R', 2, 0, 0, 0, 0, 652, 9000));   // 1.0 ms
    sum.add_line(packet_line(30500, 'R', 2, 0, 1, 0, 0, 652, 29000));  // 1.5 ms
    sum.add_line(packet_line(53000, 'R', 2, 0, 2, 0, 0, 652, 49000));  // 4.0 ms
    const Transit& t = sum.received().at(kAudioRx)->transit;
    CHECK(t.count == 3);
    CHECK(t.min_us == 1000);
    CHECK(t.max_us == 4000);
    CHECK_NEAR(t.avg_us(), 2166.6667, 0.001);
}

void test_per_second_intervals() {
    LogSummary sum(true);
    const uint32_t t0 = 0xFFFFFFFFu - 1500000;  // crosses the 32-bit time wrap
    for (uint32_t i = 0; i <= 150; ++i) {       // packet 150 sits exactly on the 3 s boundary
        if (i >= 50 && i < 100 && i % 10 == 3) continue;  // 5 lost in second 2
        sum.add_line(packet_line(t0 + i * 20000, 'R', 2, 0, i, 0, 0, 652, i * 20000));
    }
    const auto& iv = sum.intervals();
    CHECK(iv.size() == 3);
    if (iv.size() == 3) {
        CHECK_NEAR(iv[0].t_s, 1.0, 1e-9);
        CHECK(iv[0].stream == "audio/0");
        CHECK(iv[0].snap.expected == 50);
        CHECK(iv[0].snap.lost == 0);
        CHECK_NEAR(iv[0].snap.pps, 50.0, 1e-9);
        CHECK(iv[1].snap.expected == 50);
        CHECK(iv[1].snap.lost == 5);
        CHECK_NEAR(iv[1].snap.loss_pct, 10.0, 1e-9);
        CHECK(iv[2].snap.lost == 0);
        CHECK_NEAR(iv[2].t_s, 3.0, 1e-9);
    }
}

void test_round_trip_with_csv_logger() {
    const fs::path p =
        fs::temp_directory_path() /
        ("lancall_test_log_reader_" + std::to_string(platform::process_id()) + ".csv");
    {
        metrics::CsvLogger log;
        CHECK(log.open(p.string()));
        for (unsigned i = 0; i < 200; ++i) {
            if (i % 20 == 5) continue;
            log.packet_row(1000000 + i * 20000, 'R', 2, 0, i, 0, 0, 652, 999000 + i * 20000);
            log.packet_row(1000000 + i * 20000, 'S', 1, 0, i, i / 4, i % 4, 1412,
                           999000 + i * 20000);
        }
        log.event_row(5000000, "underrun", 1);
        log.event_row(5000001, "bad,name", 2);
    }
    LogSummary sum;
    std::string error;
    CHECK(sum.add_file(p.string(), error));
    CHECK(sum.invalid_rows() == 0);
    CHECK(sum.packet_rows() == 380);
    CHECK(sum.event_rows() == 2);
    const auto a = sum.received().at(kAudioRx)->stats.total();
    CHECK(a.expected == 200);
    CHECK(a.lost == 10);
    CHECK(sum.sent().at(StreamKey{'S', 1, 0}).packets == 190);
    CHECK(sum.events().count("bad_name") == 1);
    std::error_code ec;
    fs::remove(p, ec);

    LogSummary missing;
    CHECK(!missing.add_file("/nonexistent/lancall.csv", error));
    CHECK(!error.empty());
}

// Group calls: lancall logs frame_stream (whose frame) before each frame_shown, so each video
// stream gets its own frames; without it, all of them went to the lowest stream id
void test_frame_stream_per_stream_fps() {
    LogSummary sum;
    for (uint32_t f = 0; f < 30; ++f) {
        const uint32_t t = 1000000 + f * 66667;
        for (unsigned id = 2; id <= 3; ++id) {
            sum.add_line(packet_line(t + id * 100, 'R', 1, id, f, f, 0, 1412, t - 2000));
            sum.add_line(std::to_string(t + 1000 + id) + ",E,frame_stream," + std::to_string(id));
            sum.add_line(std::to_string(t + 1000 + id) + ",E,frame_shown,1");
        }
    }
    const StreamKey two{'R', 1, 2}, three{'R', 1, 3};
    CHECK(sum.received().count(two) == 1 && sum.received().count(three) == 1);
    if (sum.received().count(two) == 1 && sum.received().count(three) == 1) {
        CHECK(sum.received().at(two)->stats.total().frames == 30);
        CHECK(sum.received().at(three)->stats.total().frames == 30);
    }
}

}  // namespace

int main() {
    RUN(test_parse_packet_row);
    RUN(test_parse_event_row);
    RUN(test_parse_rejects_bad_rows);
    RUN(test_replay_matches_stream_stats);
    RUN(test_sent_events_and_streams);
    RUN(test_frame_shown_gives_video_fps);
    RUN(test_transit);
    RUN(test_per_second_intervals);
    RUN(test_round_trip_with_csv_logger);
    RUN(test_timing_events);
    RUN(test_frame_stream_per_stream_fps);
    return check::result();
}
