// Tests for loss repair (Person 1, phase 5): the NACK format, which packets the receiver asks
// for, the sender's copy of what it sent, the reassembler mode that lets an older strip
// wait for its resent piece, and rebuilding a lost piece from the FEC parity packet. No network;
// time is passed in. Exit code 0 = all passed.
#include <algorithm>
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

// the parity datagram for a strip, as the video sender builds it
static net::Datagram parity_for(const std::vector<net::Datagram>& strip) {
    std::vector<std::vector<uint8_t>> payloads;
    for (const auto& d : strip) payloads.emplace_back(d.begin() + proto::kHeaderSize, d.end());
    auto body = net::make_parity(payloads);
    proto::Header h;
    proto::read_header(strip[0].data(), strip[0].size(), h);
    h.type = proto::Type::VideoFec;
    h.frag_index = 0;
    net::Datagram d(proto::kHeaderSize + body.size());
    proto::write_header(h, d.data());
    std::copy(body.begin(), body.end(), d.begin() + proto::kHeaderSize);
    return d;
}

static bool feed_parity(net::Reassembler& r, const net::Datagram& d, net::Frame& f) {
    proto::Header h;
    proto::read_header(d.data(), d.size(), h);
    return r.push_parity(h, d.data() + proto::kHeaderSize, d.size() - proto::kHeaderSize, 0, f);
}

static bool feed_piece(net::Reassembler& r, const net::Datagram& d, net::Frame& f) {
    proto::Header h;
    proto::read_header(d.data(), d.size(), h);
    return r.push(h, d.data() + proto::kHeaderSize, d.size() - proto::kHeaderSize, 0, f);
}

static void test_fec() {
    std::puts("FEC: any one lost piece is rebuilt, in either order");
    std::vector<uint8_t> data(2803);  // 3 pieces of 935, 934, 934 bytes
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + 3);

    for (int lost = 0; lost < 3; ++lost) {
        net::Packetizer pk(proto::Type::Video, 2);
        auto strip = pk.packetize(data.data(), data.size(), 100);
        CHECK(strip.size() == 3);
        auto par = parity_for(strip);

        // parity last
        net::Reassembler r(100000, true);
        net::Frame f;
        for (int i = 0; i < 3; ++i)
            if (i != lost) CHECK(!feed_piece(r, strip[i], f));
        CHECK(feed_parity(r, par, f) && f.recovered && f.data == data);
        CHECK(f.recovered_seq == lost);  // seqs 0, 1, 2
        CHECK(r.stats().fec_recovered == 1);

        // parity first: the piece that makes it "all but one" completes the frame
        net::Reassembler r2(100000, true);
        CHECK(!feed_parity(r2, par, f));
        int fed = 0;
        bool done = false;
        for (int i = 0; i < 3; ++i)
            if (i != lost) {
                done = feed_piece(r2, strip[i], f);
                CHECK(done == (++fed == 2));
            }
        CHECK(done && f.recovered && f.data == data && f.recovered_seq == lost);
    }

    std::puts("FEC: a 1-piece strip is just sent twice");
    net::Packetizer pk(proto::Type::Video, 2);
    std::vector<uint8_t> small(300, 9);
    auto one = pk.packetize(small.data(), small.size(), 100);
    net::Reassembler r(100000, true);
    net::Frame f;
    CHECK(feed_parity(r, parity_for(one), f) && f.recovered && f.data == small);

    std::puts("FEC: two lost is too many, until a resend brings it to one");
    auto strip = pk.packetize(data.data(), data.size(), 200);
    net::Reassembler r3(100000, true);
    CHECK(!feed_piece(r3, strip[0], f));
    CHECK(!feed_parity(r3, parity_for(strip), f));                        // pieces 1 and 2 lost
    CHECK(feed_piece(r3, strip[2], f) && f.recovered && f.data == data);  // 2 resent

    std::puts("FEC: parity after the strip is done, or not matching it, is ignored");
    auto s2 = pk.packetize(data.data(), data.size(), 300);
    net::Reassembler r4(100000, true);
    for (auto& d : s2) feed_piece(r4, d, f);
    CHECK(!feed_parity(r4, parity_for(s2), f) && r4.stats().late_pieces == 1);

    auto s3 = pk.packetize(data.data(), data.size(), 400);
    auto bad = parity_for(s3);
    bad.resize(proto::kHeaderSize + 100);  // shorter than the pieces: can't be theirs
    net::Reassembler r5(100000, true);
    feed_piece(r5, s3[0], f);
    feed_piece(r5, s3[1], f);
    CHECK(!feed_parity(r5, bad, f) && r5.stats().fec_recovered == 0);
}

int main() {
    test_nack_format();
    test_tracker();
    test_retransmit_buffer();
    test_independent_reassembler();
    test_fec();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all repair tests passed");
    return 0;
}
