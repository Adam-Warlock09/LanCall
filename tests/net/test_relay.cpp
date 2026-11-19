// Tests for the group-call relay (Person 1, phase 4). Three pretend members on one laptop,
// each with its own loopback address (127.0.0.2, .3, .4: on Linux all of 127/8 is this
// laptop), talk to a real Relay over UDP. Takes about two seconds. Exit code 0 = all passed.
// Uses UDP ports 25004, 25006 and 25014.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/nack.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "net/relay.hpp"

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
    int fd = -1;
    Sock(const std::string& ip, uint16_t port) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        int yes = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
            std::printf("  can't bind %s:%u\n", ip.c_str(), port);
            failures++;
        }
    }
    ~Sock() { close(fd); }

    void send(const std::vector<uint8_t>& d, uint16_t port = kRelay) const {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
        sendto(fd, d.data(), d.size(), 0, reinterpret_cast<sockaddr*>(&a), sizeof(a));
    }

    // next datagram, or empty after timeout_ms
    std::vector<uint8_t> recv(int timeout_ms = 300) const {
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, timeout_ms) <= 0) return {};
        std::vector<uint8_t> buf(2048);
        ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
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

int main() {
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

    std::puts("NACKs: answered from the relay's copy, the rest asked of the sender");
    proto::Nack nk;
    nk.media = proto::Type::Video;
    nk.reporter = 4;
    nk.seqs = {7, 8};  // 7 went through the relay, 8 never reached it
    tx4.send(proto::make_nack_packet(2, 0, nk));
    CHECK(v4.recv() == v);        // 7, straight from the copy, to 4 only
    CHECK(v3.recv(100).empty());  // 3 didn't ask
    auto asked = tx2.recv();      // 8 is passed on to the sender
    proto::Header nh;
    proto::Nack back;
    CHECK(proto::read_nack_packet(asked.data(), asked.size(), nh, back));
    CHECK(nh.stream_id == 2 && back.reporter == 4 && back.seqs == std::vector<uint16_t>{8});

    std::puts("the sender's resend goes to everyone, a second resend to no one");
    auto v8 = packet(proto::Type::Video, 2, 8);
    tx2.send(v8);
    CHECK(v3.recv() == v8);
    CHECK(v4.recv() == v8);
    tx2.send(v8);
    CHECK(v3.recv(100).empty() && v4.recv(100).empty());

    relay.stop();
    CHECK(!relay.running() && relay.members().empty());
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all relay tests passed");
    return 0;
}
