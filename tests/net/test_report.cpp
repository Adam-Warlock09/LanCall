// Tests for the receiver report format in common/report.hpp (phase 3 kickoff).
// Exit code 0 = all passed.
#include <cstdio>

#include "report.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main() {
    std::puts("report round trip");
    proto::ReceiverReport r;
    r.media = proto::Type::Audio;
    r.highest_seq = 65000;
    r.loss_permille = 52;
    r.interval_ms = 500;
    r.jitter_us = 123456;
    r.received_kbps = 4800;

    auto pkt = proto::make_report_packet(3, 77, r);
    CHECK(pkt.size() == proto::kHeaderSize + proto::kReportBodySize);

    proto::Header h;
    proto::ReceiverReport back;
    CHECK(proto::read_report_packet(pkt.data(), pkt.size(), h, back));
    CHECK(h.type == proto::Type::Report && h.stream_id == 3 && h.seq == 77);
    CHECK(back.media == proto::Type::Audio && back.highest_seq == 65000);
    CHECK(back.loss_permille == 52 && back.interval_ms == 500);
    CHECK(back.jitter_us == 123456 && back.received_kbps == 4800);

    std::puts("bad reports rejected");
    CHECK(!proto::read_report_packet(pkt.data(), pkt.size() - 1, h, back));  // short
    auto bad = pkt;
    bad[proto::kHeaderSize] = 9;  // media type that doesn't exist
    CHECK(!proto::read_report_packet(bad.data(), bad.size(), h, back));
    bad = pkt;
    bad[0] = static_cast<uint8_t>(proto::Type::Video);  // a media packet, not a report
    CHECK(!proto::read_report_packet(bad.data(), bad.size(), h, back));

    std::puts("loss is capped at 1000 permille");
    r.loss_permille = 5000;
    pkt = proto::make_report_packet(0, 0, r);
    CHECK(proto::read_report_packet(pkt.data(), pkt.size(), h, back) && back.loss_permille == 1000);

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all report tests passed");
    return 0;
}