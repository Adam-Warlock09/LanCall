// Tests for loss repair (Person 1, phase 5): the NACK format, which packets the receiver asks
// for, the sender's copy of what it sent, and the reassembler mode that lets an older strip
// wait for its resent piece. No network; time is passed in. Exit code 0 = all passed.
#include <cstdio>
#include <vector>

#include "common/nack.hpp"
#include "net/nack_tracker.hpp"
#include "net/packetizer.hpp"
#include "net/reassembler.hpp"
#include "net/retransmit_buffer.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

constexpr uint32_t MS = 1000;

static void test_nack_format() {
    std::puts("NACK round trip, limits, bad packets");
    proto::Nack n;
    n.media = proto::Type::Video;
    n.reporter = 3;
    n.seqs = {7, 65535, 0, 1234};
    auto pkt = proto::make_nack_packet(2, 99, n);
    CHECK(pkt.size() == proto::kHeaderSize + proto::kNackFixedSize + 8);

    proto::Header h;
    proto::Nack back;
    CHECK(proto::read_nack_packet(pkt.data(), pkt.size(), h, back));
    CHECK(h.type == proto::Type::Nack && h.stream_id == 2 && h.seq == 99);
    CHECK(back.media == proto::Type::Video && back.reporter == 3);
    CHECK(back.seqs == n.seqs);

    CHECK(!proto::read_nack_packet(pkt.data(), pkt.size() - 1, h, back));  // cut short
    auto bad = pkt;
    bad[proto::kHeaderSize + 2] = 0;  // count 0
    CHECK(!proto::read_nack_packet(bad.data(), bad.size(), h, back));

    n.seqs.assign(100, 5);  // more than fit: the rest is left out
    pkt = proto::make_nack_packet(2, 100, n);
    CHECK(proto::read_nack_packet(pkt.data(), pkt.size(), h, back));
    CHECK(back.seqs.size() == proto::kMaxNackSeqs);
}

static void test_tracker() {
    std::puts("tracker: gaps, retries, giving up, wrap, outages");
    net::NackTracker t;
    t.on_packet(10, 0);
    t.on_packet(11, 0);
    t.on_packet(14, 1 * MS);  // 12 and 13 lost
    auto d = t.due(1 * MS);
    CHECK(d.size() == 2 && d[0] == 12 && d[1] == 13);
    CHECK(t.due(2 * MS).empty());  // just asked

    t.on_packet(12, 3 * MS);  // the resend of 12 arrives
    CHECK(t.missing() == 1 && t.stats().filled == 1);
    d = t.due(1 * MS + net::NackTracker::kRetryUs);  // 13 still missing: ask again
    CHECK(d.size() == 1 && d[0] == 13);
    d = t.due(1 * MS + 3 * net::NackTracker::kRetryUs);  // two tries is all it gets
    CHECK(d.empty());
    t.due(1 * MS + net::NackTracker::kGiveUpUs + 1);  // too late now
    CHECK(t.missing() == 0 && t.stats().given_up == 1);

    // across the 16-bit wrap
    net::NackTracker w;
    w.on_packet(65534, 0);
    w.on_packet(1, 0);  // 65535 and 0 lost
    d = w.due(0);
    CHECK(d.size() == 2 && d[0] == 65535 && d[1] == 0);

    // a jump of hundreds of packets is an outage or restart: ask for nothing
    net::NackTracker o;
    o.on_packet(100, 0);
    o.on_packet(102, 0);  // 101 lost
    o.on_packet(900, 0);
    CHECK(o.due(0).empty() && o.missing() == 0);

    // reordering: the "lost" packet turns up before anyone asked
    net::NackTracker r;
    r.on_packet(1, 0);
    r.on_packet(3, 0);
    r.on_packet(2, 0);
    CHECK(r.due(0).empty());
}

static void test_retransmit_buffer() {
    std::puts("retransmit buffer: hit, miss, overwritten, too old, resend limit");
    net::RetransmitBuffer b;
    std::vector<uint8_t> p{1, 2, 3};
    b.store(500, p.data(), p.size(), 0);
    const std::vector<uint8_t>* got = b.take(500, 10 * MS);
    CHECK(got && *got == p);
    CHECK(!b.take(501, 10 * MS));  // never sent
    CHECK(b.take(500, 20 * MS));   // second resend
    CHECK(!b.take(500, 30 * MS));  // that's the limit

    b.store(7, p.data(), p.size(), 0);
    b.store(static_cast<uint16_t>(7 + net::RetransmitBuffer::kSlots), p.data(), p.size(), 0);
    CHECK(!b.take(7, 1 * MS));  // its slot was reused

    b.store(9, p.data(), p.size(), 0);
    CHECK(!b.take(9, net::RetransmitBuffer::kMaxAgeUs + 1));  // too old to be worth it
}

static void test_independent_reassembler() {
    std::puts("independent strips: an older strip completes after a newer one");
    net::Packetizer pk(proto::Type::Video, 1);
    std::vector<uint8_t> data(3000, 0x5A);  // 3 pieces per strip
    auto s1 = pk.packetize(data.data(), data.size(), 100);
    auto s2 = pk.packetize(data.data(), data.size(), 100);

    auto feed = [](net::Reassembler& r, const net::Datagram& d, net::Frame& f) {
        proto::Header h;
        proto::read_header(d.data(), d.size(), h);
        return r.push(h, d.data() + proto::kHeaderSize, d.size() - proto::kHeaderSize, 0, f);
    };

    net::Reassembler r(100000, true);
    net::Frame f;
    CHECK(!feed(r, s1[0], f));
    CHECK(!feed(r, s1[2], f));  // s1[1] lost
    CHECK(!feed(r, s2[0], f));
    CHECK(!feed(r, s2[1], f));
    CHECK(feed(r, s2[2], f) && f.frame_id == s2[0][9]);       // strip 2 done
    CHECK(feed(r, s1[1], f) && f.data == data);               // the resend completes strip 1
    CHECK(!feed(r, s1[1], f) && r.stats().late_pieces == 1);  // a second resend is late

    // the strict mode used elsewhere still drops the older strip
    net::Reassembler strict;
    feed(strict, s1[0], f);
    feed(strict, s2[0], f);
    feed(strict, s2[1], f);
    feed(strict, s2[2], f);
    CHECK(!feed(strict, s1[1], f) && strict.stats().late_pieces == 1);

    std::puts("independent strips: a restarted sender (ids from 0 again) is followed");
    net::Packetizer later(proto::Type::Video, 1);
    for (int i = 0; i < 300; ++i) later.packetize(data.data(), 1, 100);  // ids 0..299
    auto far = later.packetize(data.data(), 1, 100);                     // id 300
    net::Reassembler ind(100000, true);
    CHECK(feed(ind, far[0], f));
    net::Packetizer fresh(proto::Type::Video, 1);
    auto first = fresh.packetize(data.data(), 1, 200);  // id 0, 300 behind
    CHECK(feed(ind, first[0], f) && f.timestamp_us == 200);
}

int main() {
    test_nack_format();
    test_tracker();
    test_retransmit_buffer();
    test_independent_reassembler();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all repair tests passed");
    return 0;
}
