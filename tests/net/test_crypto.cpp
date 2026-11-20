// Tests for media encryption (Person 1, phase 6): sealing and opening datagrams, what an
// attacker can't do to them, and UdpSocket doing it for every datagram over loopback.
// Exit code 0 = all passed. Uses UDP ports 25030-25031.
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/media_crypto.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "common/udp_socket.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using proto::MediaCrypto;

static std::vector<uint8_t> datagram(proto::Type type, size_t body) {
    std::vector<uint8_t> d(proto::kHeaderSize + body);
    proto::Header h{type, 3, 700, 123456, 9, 0, 1};
    proto::write_header(h, d.data());
    for (size_t i = 0; i < body; ++i) d[proto::kHeaderSize + i] = static_cast<uint8_t>(i * 13);
    return d;
}

static MediaCrypto::Key key_of(uint8_t fill) {
    MediaCrypto::Key k;
    k.fill(fill);
    return k;
}

static void test_seal_open() {
    std::puts("seal and open: media body hidden, header readable, 40 bytes more");
    CHECK(MediaCrypto::available());
    MediaCrypto c(key_of(7));
    auto d = datagram(proto::Type::Video, 1400);
    std::vector<uint8_t> sealed, back;
    CHECK(c.seal(d.data(), d.size(), sealed));
    CHECK(sealed.size() == d.size() + MediaCrypto::kOverhead);
    CHECK(std::equal(d.begin(), d.begin() + proto::kHeaderSize, sealed.begin()));  // header
    CHECK(!std::equal(d.begin() + proto::kHeaderSize, d.end(),
                      sealed.begin() + proto::kHeaderSize));  // body isn't in the clear
    CHECK(c.open(sealed.data(), sealed.size(), back) && back == d);

    std::vector<uint8_t> again;
    CHECK(c.seal(d.data(), d.size(), again) && again != sealed);  // a fresh nonce every time

    std::puts("reports stay readable (the relay needs them) but are signed");
    proto::ReceiverReport r;
    r.media = proto::Type::Video;
    r.reporter = 4;
    r.loss_permille = 25;
    auto rep = proto::make_report_packet(2, 1, r);
    CHECK(c.seal(rep.data(), rep.size(), sealed));
    proto::Header h;
    proto::ReceiverReport seen;
    CHECK(proto::read_report_packet(sealed.data(), sealed.size(), h, seen));  // as the relay does
    CHECK(seen.reporter == 4 && seen.loss_permille == 25);
    CHECK(c.open(sealed.data(), sealed.size(), back) && back == rep);
    sealed[proto::kHeaderSize + 4] ^= 1;  // change the reported loss
    CHECK(!c.open(sealed.data(), sealed.size(), back));

    std::puts("an empty body works too (keepalives)");
    auto ka = datagram(proto::Type::Keepalive, 0);
    CHECK(c.seal(ka.data(), ka.size(), sealed) && c.open(sealed.data(), sealed.size(), back));
    CHECK(back == ka);
}

static void test_tampering() {
    std::puts("tampering, the wrong key, truncation and plain datagrams are all refused");
    MediaCrypto c(key_of(7)), other(key_of(8));
    auto d = datagram(proto::Type::Audio, 640);
    std::vector<uint8_t> sealed, back;
    c.seal(d.data(), d.size(), sealed);

    for (size_t at : {size_t(1), size_t(2), proto::kHeaderSize + 10, sealed.size() - 30,
                      sealed.size() - 1}) {  // stream id, seq, body, nonce, tag
        auto bad = sealed;
        bad[at] ^= 0x40;
        CHECK(!c.open(bad.data(), bad.size(), back));
    }
    auto as_report = sealed;  // relabel the media as a report, hoping it opens as readable
    as_report[0] = static_cast<uint8_t>(proto::Type::Report);
    CHECK(!c.open(as_report.data(), as_report.size(), back));
    CHECK(!other.open(sealed.data(), sealed.size(), back));
    CHECK(!c.open(sealed.data(), sealed.size() - 1, back));
    CHECK(!c.open(d.data(), d.size(), back));  // never sealed
    CHECK(!c.seal(d.data(), 5, sealed));       // not even a header
}

static void test_socket() {
    std::puts("UdpSocket seals and opens by itself, and drops what doesn't open");
    auto key = std::make_shared<const MediaCrypto>(key_of(1));
    auto wrong = std::make_shared<const MediaCrypto>(key_of(2));
    UdpSocket tx(0), rx(25030), plain_rx(25031);
    auto d = datagram(proto::Type::Video, 900);
    uint8_t buf[2048];
    std::string ip;
    uint16_t port;

    tx.set_crypto(key);
    rx.set_crypto(key);
    CHECK(tx.send_to("127.0.0.1", 25030, d.data(), d.size()));
    ssize_t n = rx.recv_from(buf, sizeof(buf), ip, port, 1000);
    CHECK(n == static_cast<ssize_t>(d.size()) && std::equal(d.begin(), d.end(), buf));

    // without the key you get the sealed bytes, 40 longer, body unreadable
    tx.send_to("127.0.0.1", 25031, d.data(), d.size());
    n = plain_rx.recv_from(buf, sizeof(buf), ip, port, 1000);
    CHECK(n == static_cast<ssize_t>(d.size() + MediaCrypto::kOverhead));

    // another call's key: dropped and counted, as if nothing came
    rx.set_crypto(wrong);
    tx.send_to("127.0.0.1", 25030, d.data(), d.size());
    CHECK(rx.recv_from(buf, sizeof(buf), ip, port, 1000) == 0 && rx.rejected() == 1);

    // someone sending in the clear to an encrypted call: dropped too
    rx.set_crypto(key);
    tx.set_crypto(nullptr);
    tx.send_to("127.0.0.1", 25030, d.data(), d.size());
    CHECK(rx.recv_from(buf, sizeof(buf), ip, port, 1000) == 0 && rx.rejected() == 2);
}

int main() {
    test_seal_open();
    test_tampering();
    test_socket();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all crypto tests passed");
    return 0;
}
