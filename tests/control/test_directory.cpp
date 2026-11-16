// Tests for the cross-floor directory (Person 1, phase 3): the PEER line format, how the peer
// table merges floor and directory sightings, and a real DirectoryServer over 127.0.0.1 with
// two fake clients. Uses UDP port 25020. Exit code 0 = all passed.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "common/udp_socket.hpp"
#include "control/directory.hpp"
#include "control/discovery_detail.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

using namespace control;

static Peer peer(const std::string& name, const std::string& ip, uint32_t seen) {
    Peer p;
    p.name = name;
    p.ip = ip;
    p.signal_port = 5011;
    p.last_seen_us = seen;
    return p;
}

static void test_peer_line() {
    std::puts("PEER line round trip");
    std::string line = detail::make_peer_line("7c01aa2e", peer("Atul", "10.8.66.20", 0), 1500);
    CHECK(line == "PEER 1 id=7c01aa2e name=Atul ip=10.8.66.20 sig=5011 age=1500");

    std::string id;
    Peer p;
    uint32_t age = 0;
    CHECK(detail::parse_peer_line(line, id, p, age));
    CHECK(id == "7c01aa2e" && p.name == "Atul" && p.ip == "10.8.66.20");
    CHECK(p.signal_port == 5011 && age == 1500);

    // age is optional
    CHECK(detail::parse_peer_line("PEER 1 id=a name=X ip=1.2.3.4 sig=5011", id, p, age));
    CHECK(age == 0);

    std::puts("bad PEER lines rejected");
    CHECK(!detail::parse_peer_line("PEER 2 id=a name=X ip=1.2.3.4 sig=5011", id, p, age));
    CHECK(!detail::parse_peer_line("PEER 1 id=a name=X sig=5011", id, p, age));  // no ip
    CHECK(!detail::parse_peer_line("PEER 1 id=a name=X ip=1.2.3.4 sig=0", id, p, age));
    CHECK(!detail::parse_peer_line("LANCALL 1 id=a name=X sig=5011", id, p, age));
}

static void test_local_and_remote() {
    std::puts("peer table: local beats remote, each side expires on its own");
    detail::PeerTable t;

    // only heard through the directory: remote
    t.seen_remote("b", peer("Bob", "10.8.72.5", 1000000));
    auto list = t.alive(1000000);
    CHECK(list.size() == 1 && list[0].remote);

    // now also heard by broadcast: local wins, and the broadcast details are kept
    t.seen("b", peer("Bob", "10.8.72.5", 2000000));
    t.seen_remote("b", peer("Bob", "9.9.9.9", 2500000));  // must not overwrite the local ip
    list = t.alive(3000000);
    CHECK(list.size() == 1 && !list[0].remote && list[0].ip == "10.8.72.5");

    // broadcasts stop (moved floors?) but the directory still knows it: becomes remote
    t.seen_remote("b", peer("Bob", "10.8.72.5", 7000000));
    list = t.alive(8500000);  // local last at 2 s, so stale; remote at 7 s, fresh
    CHECK(list.size() == 1 && list[0].remote);

    // and eventually gone both ways
    t.prune(14000000);
    CHECK(t.size() == 0);
}

static bool recv_line(UdpSocket& s, std::string& out, int timeout_ms) {
    uint8_t buf[512];
    std::string ip;
    uint16_t port;
    ssize_t n = s.recv_from(buf, sizeof(buf), ip, port, timeout_ms);
    if (n <= 0) return false;
    out.assign(reinterpret_cast<const char*>(buf), n);
    return true;
}

static void send_text(UdpSocket& s, uint16_t port, const std::string& text) {
    s.send_to("127.0.0.1", port, reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

static void test_server() {
    std::puts("directory server over loopback");
    constexpr uint16_t kPort = 25020;
    DirectoryServer dir;
    CHECK(dir.start(kPort));
    if (!dir.running()) {
        std::printf("  (couldn't start: %s)\n", dir.error().c_str());
        return;
    }

    UdpSocket a(0), b(0);
    std::string line;

    // first client alone: nobody to report, so no reply
    send_text(a, kPort, detail::make_beacon("aaaa", "Aric", 5011));
    CHECK(!recv_line(a, line, 300));

    // second client: told about the first
    send_text(b, kPort, detail::make_beacon("bbbb", "Atul", 5022));
    CHECK(recv_line(b, line, 1000));
    std::string id;
    Peer p;
    uint32_t age = 0;
    CHECK(detail::parse_peer_line(line, id, p, age));
    CHECK(id == "aaaa" && p.name == "Aric" && p.ip == "127.0.0.1" && p.signal_port == 5011);
    CHECK(age < 2000);

    // first client again: now told about the second, and only about the second
    send_text(a, kPort, detail::make_beacon("aaaa", "Aric", 5011));
    CHECK(recv_line(a, line, 1000));
    CHECK(detail::parse_peer_line(line, id, p, age) && id == "bbbb" && p.signal_port == 5022);
    CHECK(!recv_line(a, line, 200));

    CHECK(dir.peer_count() == 2);
    std::string table = dir.table();
    CHECK(table.find("Aric") != std::string::npos && table.find("Atul") != std::string::npos);

    // garbage is ignored, not answered
    send_text(a, kPort, "hello there");
    CHECK(!recv_line(a, line, 200));
    CHECK(dir.peer_count() == 2);
}

int main() {
    test_peer_line();
    test_local_and_remote();
    test_server();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all directory tests passed");
    return 0;
}