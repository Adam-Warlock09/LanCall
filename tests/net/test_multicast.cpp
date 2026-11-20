// Tests for the multicast helpers (Person 1, phase 7 kickoff): group addresses, and a real
// send/receive through a group on this laptop (multicast loop on). Exit code 0 = all passed.
// Uses UDP port 25020. If this laptop has no route for multicast at all (no network up),
// the send/receive part is skipped, not failed.
#include <cstdio>
#include <string>
#include <vector>

#include "common/protocol.hpp"
#include "common/udp_socket.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

int main() {
    std::puts("group addresses");
    CHECK(proto::lancall_group(9) == "239.255.77.9");
    CHECK(proto::lancall_group(0).empty() && proto::lancall_group(255).empty());
    CHECK(proto::is_multicast_ipv4("239.255.77.9"));
    CHECK(proto::is_multicast_ipv4("224.0.0.1") && proto::is_multicast_ipv4("239.255.255.255"));
    CHECK(!proto::is_multicast_ipv4("10.8.64.5") && !proto::is_multicast_ipv4("240.0.0.1"));
    CHECK(!proto::is_multicast_ipv4("not an ip"));

    std::puts("send to a group, receive it after joining, not after leaving");
    const std::string group = proto::lancall_group(42);
    UdpSocket rx(25020), tx(0);
    CHECK(rx.join_multicast(group));
    CHECK(!rx.join_multicast("10.8.64.5"));  // not a group
    tx.set_multicast_loop(true);
    tx.set_multicast_ttl(1);

    std::vector<uint8_t> msg{'h', 'i'};
    if (!tx.send_to(group, 25020, msg.data(), msg.size())) {
        std::puts("  (skipped: no route for multicast on this laptop)");
    } else {
        uint8_t buf[64];
        std::string ip;
        uint16_t port = 0;
        ssize_t n = rx.recv_from(buf, sizeof(buf), ip, port, 1000);
        CHECK(n == 2 && buf[0] == 'h' && port == tx.local_port());

        CHECK(rx.leave_multicast(group));
        tx.send_to(group, 25020, msg.data(), msg.size());
        CHECK(rx.recv_from(buf, sizeof(buf), ip, port, 300) == 0);  // nothing any more
    }

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all multicast tests passed");
    return 0;
}
