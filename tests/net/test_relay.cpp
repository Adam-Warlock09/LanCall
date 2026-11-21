// Tests for the group-call relay (Person 1, phase 4). Three pretend members on one laptop,
// each with its own loopback address (127.0.0.2, .3, .4: on Linux all of 127/8 is this
// laptop, on Windows too), talk to a real Relay over UDP. Takes about two seconds. Exit code
// 0 = all passed.
// Uses UDP ports 25004, 25006 and 25014.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/nack.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "net/relay.hpp"
#include "tests/sock_compat.hpp"  // raw sockets on Linux and Windows (phase 8)

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

constexpr uint16_t kRelay = 25014, kVideo = 25004, kAudio = 25006;

// a UDP socket bound to one address, so several "laptops" fit on one machine
struct Sock {
    testsock::Handle fd = testsock::kNone;
    Sock(const std::string& ip, uint16_t port) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        testsock::set_option(fd, SOL_SOCKET, SO_REUSEADDR, 1);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
            std::printf("  can't bind %s:%u\n", ip.c_str(), port);
            failures++;
        }
    }
    ~Sock() { testsock::close(fd); }

    void send(const std::vector<uint8_t>& d, uint16_t port = kRelay) const {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        sendto(fd, reinterpret_cast<const char*>(d.data()), static_cast<int>(d.size()), 0,
               reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }

    // next datagram, or empty after timeout_ms
    std::vector<uint8_t> recv(int timeout_ms = 300) const {
        if (!testsock::readable(fd, timeout_ms)) return {};
        std::vector<uint8_t> buf(2048);
        const long long n = testsock::recv_bytes(fd, buf.data(), buf.size());
        buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
        return buf;
    }
};

static std::vector<uint8_t> packet(proto::Type type, uint8_t stream, uint16_t seq) {
    std::vector<uint8_t> d(proto::kHeaderSize + 20, 0xAB);
    proto::Header h{type, stream, seq, 123456, 0, 0, 1};
    proto::write_header(h, d.data());
    return d;
}

// Phase 10: a member is (ip, stream id). Forged ids from another address can't take over an
// entry or poison its cache, two calls on one relay (both hosts are id 1) don't clash, and
// junk can't grow the member list without bound. Its own relay and "laptops" 127.0.0.5-9.
static void test_pinned_members() {
    net::Relay relay;
    net::RelayOptions opt;
    opt.port = kRelay;
    opt.video_port = kVideo;
    opt.audio_port = kAudio;
    if (!relay.start(opt)) {
        std::printf("can't start the relay: %s\n", relay.error().c_str());
        failures++;
        return;
    }
    Sock tx5("127.0.0.5", 0), v5("127.0.0.5", kVideo);  // member 2 of call X
    Sock tx6("127.0.0.6", 0), v6("127.0.0.6", kVideo);  // member 3 of call X
    Sock tx7("127.0.0.7", 0), v7("127.0.0.7", kVideo);  // host (id 1) of call X
    Sock tx8("127.0.0.8", 0), v8("127.0.0.8", kVideo);  // host (id 1) of call Y
    Sock tx9("127.0.0.9", 0), v9("127.0.0.9", kVideo);  // someone forging ids

    tx6.send(packet(proto::Type::Keepalive, 3, 0));
    tx7.send(packet(proto::Type::Keepalive, 1, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::puts("a forged keepalive for id 3 doesn't take member 3's media away");
    tx9.send(packet(proto::Type::Keepalive, 3, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    auto from2 = packet(proto::Type::Video, 2, 10);
    tx5.send(from2);
    CHECK(v6.recv() == from2);  // the real member 3 still gets it
    CHECK(v7.recv() == from2);
    v9.recv(100);  // the forger gets a sealed copy it can't read: harmless

    std::puts("forged video for id 2 doesn't make the real packet a 'resend'");
    auto forged = packet(proto::Type::Video, 2, 50);
    forged.back() = 0x66;  // different bytes, same id and seq
    tx9.send(forged);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    while (!v6.recv(50).empty()) {
    }
    while (!v7.recv(50).empty()) {
    }
    auto real = packet(proto::Type::Video, 2, 50);
    tx5.send(real);
    CHECK(v6.recv() == real);
    CHECK(v7.recv() == real);

    std::puts("two calls on one relay: both hosts are id 1, neither loses packets");
    tx8.send(packet(proto::Type::Keepalive, 1, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    for (Sock* s : {&v5, &v6, &v7, &v8, &v9})
        while (!s->recv(30).empty()) {
        }
    auto x1 = packet(proto::Type::Video, 1, 0);
    auto y1 = packet(proto::Type::Video, 1, 0);  // same id, same seq, the other call
    y1.back() = 0x77;
    tx7.send(x1);
    tx8.send(y1);
    // call X's member 3 gets both (call Y's is rejected by its socket: another key)
    auto first = v6.recv(), second = v6.recv();
    CHECK((first == x1 && second == y1) || (first == y1 && second == x1));
    CHECK(v8.recv() == x1);  // each host gets the other's, nothing dropped as a "duplicate"
    CHECK(v7.recv() == y1);

    std::puts("ids outside 1..6 are ignored; the member list has a limit");
    const size_t before = relay.members().size();
    tx9.send(packet(proto::Type::Keepalive, 7, 0));
    tx9.send(packet(proto::Type::Video, 200, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(relay.members().size() == before);
    for (int i = 1; i <= 40; ++i) {
        Sock junk("127.0.1." + std::to_string(i), 0);
        junk.send(packet(proto::Type::Keepalive, 1, 0));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(relay.members().size() == 32);
    relay.stop();
}

int main() {
    platform::init();  // Winsock, before the raw sockets
    net::Relay relay;
    net::RelayOptions opt;
    opt.port = kRelay;
    opt.video_port = kVideo;
    opt.audio_port = kAudio;
    if (!relay.start(opt)) {
        std::printf("can't start the relay: %s\n", relay.error().c_str());
        return 1;
    }

    // each member sends from one socket and receives video and audio on two others
    Sock tx2("127.0.0.2", 0), v2("127.0.0.2", kVideo), a2("127.0.0.2", kAudio);
    Sock tx3("127.0.0.3", 0), v3("127.0.0.3", kVideo), a3("127.0.0.3", kAudio);
    Sock tx4("127.0.0.4", 0), v4("127.0.0.4", kVideo), a4("127.0.0.4", kAudio);

    std::puts("members are learned from what they send");
    tx4.send(packet(proto::Type::Keepalive, 4, 0));  // 4 has no camera or mic
    tx3.send(packet(proto::Type::Audio, 3, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    a2.recv(50);  // nobody else was known yet when 3's audio came; nothing for 2 anyway
    a4.recv(50);  // 4 was known: drain its copy

    std::puts("video from 2 goes to 3 and 4, not back to 2");
    auto v = packet(proto::Type::Video, 2, 7);
    tx2.send(v);
    CHECK(v3.recv() == v);
    CHECK(v4.recv() == v);
    CHECK(v2.recv(100).empty());

    std::puts("audio from 3 goes to 2 and 4 on the audio port");
    auto a = packet(proto::Type::Audio, 3, 8);
    tx3.send(a);
    CHECK(a2.recv() == a);
    CHECK(a4.recv() == a);
    CHECK(a3.recv(100).empty());
    CHECK(v2.recv(100).empty());

    std::puts("a report about 2's video reaches the socket 2 sends video from");
    proto::ReceiverReport r;
    r.media = proto::Type::Video;
    r.reporter = 4;
    r.loss_permille = 31;
    auto rep = proto::make_report_packet(2, 1, r);
    tx4.send(rep);
    CHECK(tx2.recv() == rep);
    CHECK(tx3.recv(100).empty());

    std::puts("stream 0 and garbage aren't forwarded");
    tx2.send(packet(proto::Type::Video, 0, 9));
    tx2.send(std::vector<uint8_t>{1, 2, 3});
    CHECK(v3.recv(100).empty());
    CHECK(v4.recv(100).empty());

    std::puts("members list");
    auto ms = relay.members();
    CHECK(ms.size() == 3);
    if (ms.size() == 3) {
        CHECK(ms[0].id == 2 && ms[0].ip == "127.0.0.2");
        CHECK(ms[1].id == 3 && ms[1].ip == "127.0.0.3");
        CHECK(ms[2].id == 4 && ms[2].ip == "127.0.0.4");
        CHECK(ms[0].packets_in == 1 && ms[0].packets_out >= 2);  // the video + the report
    }

    std::puts(
        "NACKs: answered from the relay's copy, passed on to the sender if anything's missing");
    proto::Nack nk;
    nk.media = proto::Type::Video;
    nk.reporter = 4;
    nk.seqs = {7, 8};  // 7 went through the relay, 8 never reached it
    auto nack = proto::make_nack_packet(2, 0, nk);
    nack.insert(nack.end(), 40, 0x11);  // a sealed NACK's trailer (phase 6): must pass through
    tx4.send(nack);
    CHECK(v4.recv() == v);        // 7, straight from the copy, to 4 only
    CHECK(v3.recv(100).empty());  // 3 didn't ask
    CHECK(tx2.recv() == nack);    // 8 missing: the NACK goes on to the sender unchanged

    std::puts("the sender's resend goes to everyone, a second resend to no one");
    tx2.send(v);  // the sender resends all it was asked for: 7 is a duplicate here
    CHECK(v3.recv(100).empty() && v4.recv(100).empty());
    auto v8 = packet(proto::Type::Video, 2, 8);
    tx2.send(v8);
    CHECK(v3.recv() == v8);
    CHECK(v4.recv() == v8);
    tx2.send(v8);
    CHECK(v3.recv(100).empty() && v4.recv(100).empty());

    std::puts("FEC parity goes where video goes, and isn't mistaken for a resend");
    auto fec = packet(proto::Type::VideoFec, 2, 7);  // carries the seq of piece 0: 7 again
    tx2.send(fec);
    CHECK(v3.recv() == fec);
    CHECK(v4.recv() == fec);

    relay.stop();
    CHECK(!relay.running() && relay.members().empty());

    test_pinned_members();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all relay tests passed");
    return 0;
}
