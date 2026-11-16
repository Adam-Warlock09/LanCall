// Unit tests for the header, packetizer and reassembler.
// No test framework on purpose: nothing to install offline. Exit code 0 = all passed.
#include <cstdio>
#include <vector>

#include "packetizer.hpp"
#include "reassembler.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using net::Datagram;

static std::vector<uint8_t> make_frame(size_t len, uint8_t seed) {
    std::vector<uint8_t> f(len);
    for (size_t i = 0; i < len; ++i) f[i] = static_cast<uint8_t>(seed + i * 7);
    return f;
}

// feeds one datagram into the reassembler the same way the receiver does
static bool feed(net::Reassembler& r, const Datagram& d, uint32_t now, net::Frame& out) {
    proto::Header h;
    if (!proto::read_header(d.data(), d.size(), h)) return false;
    return r.push(h, d.data() + proto::kHeaderSize, d.size() - proto::kHeaderSize, now, out);
}

static void test_header_roundtrip() {
    std::puts("header round trip");
    proto::Header a{proto::Type::Video, 3, 65000, 0xDEADBEEF, 4242, 5, 9};
    uint8_t buf[proto::kHeaderSize];
    proto::write_header(a, buf);

    proto::Header b;
    CHECK(proto::read_header(buf, sizeof(buf), b));
    CHECK(b.type == a.type && b.stream_id == 3 && b.seq == 65000);
    CHECK(b.timestamp_us == 0xDEADBEEF && b.frame_id == 4242);
    CHECK(b.frag_index == 5 && b.frag_count == 9);

    CHECK(!proto::read_header(buf, 11, b));  // too short
    buf[0] = 9;
    CHECK(!proto::read_header(buf, sizeof(buf), b));  // unknown type
    buf[0] = 1;
    buf[10] = 9;  // index == count
    CHECK(!proto::read_header(buf, sizeof(buf), b));
}

static void test_split_sizes() {
    std::puts("split sizes");
    net::Packetizer p(proto::Type::Video, 1);

    auto one = p.packetize(make_frame(500, 1).data(), 500, 0);
    CHECK(one.size() == 1);
    CHECK(one[0].size() == proto::kHeaderSize + 500);

    auto frame = make_frame(5000, 2);
    auto many = p.packetize(frame.data(), frame.size(), 0);
    CHECK(many.size() == 4);  // ceil(5000 / 1400)
    size_t total = 0;
    for (auto& d : many) {
        CHECK(d.size() <= proto::kHeaderSize + proto::kMaxPayload);
        total += d.size() - proto::kHeaderSize;
    }
    CHECK(total == 5000);

    std::vector<uint8_t> huge(256 * proto::kMaxPayload);
    CHECK(p.packetize(huge.data(), huge.size(), 0).empty());
}

static void test_in_order_and_reversed() {
    std::puts("reassemble in order and reversed");
    net::Packetizer p(proto::Type::Video, 1);
    auto frame = make_frame(20000, 3);
    auto dgrams = p.packetize(frame.data(), frame.size(), 1234);

    net::Reassembler r;
    net::Frame out;
    bool done = false;
    for (auto& d : dgrams) done = feed(r, d, 0, out);
    CHECK(done && out.data == frame && out.timestamp_us == 1234);

    auto frame2 = make_frame(9000, 4);
    auto d2 = p.packetize(frame2.data(), frame2.size(), 0);
    done = false;
    for (auto it = d2.rbegin(); it != d2.rend(); ++it) done = feed(r, *it, 0, out);
    CHECK(done && out.data == frame2);
    CHECK(r.stats().completed == 2 && r.pending() == 0);
}

static void test_missing_piece_times_out() {
    std::puts("missing piece times out");
    net::Packetizer p(proto::Type::Video, 1);
    auto frame = make_frame(6000, 5);
    auto dgrams = p.packetize(frame.data(), frame.size(), 0);

    net::Reassembler r(100000);
    net::Frame out;
    for (size_t i = 1; i < dgrams.size(); ++i) CHECK(!feed(r, dgrams[i], 1000, out));
    CHECK(r.pending() == 1);

    r.expire(50000);  // not yet
    CHECK(r.pending() == 1);
    r.expire(200000);
    CHECK(r.pending() == 0 && r.stats().dropped_incomplete == 1);
}

static void test_newer_frame_wins() {
    std::puts("newer frame drops older partial, late pieces ignored");
    net::Packetizer p(proto::Type::Video, 1);
    auto f1 = make_frame(5000, 6);
    auto f2 = make_frame(5000, 7);
    auto d1 = p.packetize(f1.data(), f1.size(), 0);
    auto d2 = p.packetize(f2.data(), f2.size(), 0);

    net::Reassembler r;
    net::Frame out;
    feed(r, d1[0], 0, out);  // frame 0 half-done
    bool done = false;
    for (auto& d : d2) done = feed(r, d, 0, out);  // frame 1 completes
    CHECK(done && out.frame_id == 1);
    CHECK(r.pending() == 0 && r.stats().dropped_incomplete == 1);

    CHECK(!feed(r, d1[1], 0, out));  // rest of frame 0 shows up too late
    CHECK(r.stats().late_pieces == 1);
}

static void test_duplicates() {
    std::puts("duplicate pieces ignored");
    net::Packetizer p(proto::Type::Video, 1);
    auto f = make_frame(3000, 8);
    auto d = p.packetize(f.data(), f.size(), 0);

    net::Reassembler r;
    net::Frame out;
    feed(r, d[0], 0, out);
    feed(r, d[0], 0, out);
    CHECK(r.stats().duplicate_pieces == 1);
    bool done = false;
    for (size_t i = 1; i < d.size(); ++i) done = feed(r, d[i], 0, out);
    CHECK(done && out.data == f);
}

static void test_wraparound() {
    std::puts("frame id and seq wrap past 65535");
    net::Packetizer p(proto::Type::Video, 1);
    auto f = make_frame(3000, 9);  // 3 pieces each
    // burn ids so the next frames are 65534, 65535, 0, 1
    for (int i = 0; i < 65534; ++i) p.packetize(f.data(), 10, 0);

    net::Reassembler r;
    net::Frame out;
    int completed = 0;
    for (int i = 0; i < 4; ++i) {
        auto d = p.packetize(f.data(), f.size(), 0);
        for (auto& x : d)
            if (feed(r, x, 0, out)) completed++;
    }
    CHECK(completed == 4);
    CHECK(out.frame_id == 1);
    CHECK(r.stats().late_pieces == 0);
}

int main() {
    test_header_roundtrip();
    test_split_sizes();
    test_in_order_and_reversed();
    test_missing_piece_times_out();
    test_newer_frame_wins();
    test_duplicates();
    test_wraparound();

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all net tests passed");
    return 0;
}
