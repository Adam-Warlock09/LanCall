// Unit tests for the discovery beacon format and the peer table (Person 1).
// No network needed. Exit code 0 = all passed.
#include <cstdio>
#include <string>

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

static void test_names() {
    std::puts("valid names");
    CHECK(valid_name("Aric"));
    CHECK(valid_name("atul-boyal_2"));
    CHECK(!valid_name(""));
    CHECK(!valid_name("two words"));
    CHECK(!valid_name("name=x"));  // would break the key=value format
    CHECK(!valid_name(std::string(33, 'a')));
}

static void test_beacon_roundtrip() {
    std::puts("beacon round trip");
    std::string b = detail::make_beacon("3fa9c2d1", "Aric", 5011);
    CHECK(b == "LANCALL 1 id=3fa9c2d1 name=Aric sig=5011");

    std::string id, name;
    uint16_t sig = 0;
    CHECK(detail::parse_beacon(b, id, name, sig));
    CHECK(id == "3fa9c2d1" && name == "Aric" && sig == 5011);
    CHECK(detail::parse_beacon(b + "\r", id, name, sig));        // stray CR is fine
    CHECK(detail::parse_beacon(b + " extra=1", id, name, sig));  // unknown fields ignored
}

static void test_beacon_rejects() {
    std::puts("bad beacons rejected");
    std::string id, name;
    uint16_t sig = 0;
    CHECK(!detail::parse_beacon("", id, name, sig));
    CHECK(!detail::parse_beacon("HELLO 1 id=a name=Aric sig=5011", id, name, sig));
    CHECK(!detail::parse_beacon("LANCALL 2 id=a name=Aric sig=5011", id, name, sig));
    CHECK(!detail::parse_beacon("LANCALL 1 name=Aric sig=5011", id, name, sig));  // no id
    CHECK(!detail::parse_beacon("LANCALL 1 id=a name=A?b sig=5011", id, name, sig));
    CHECK(!detail::parse_beacon("LANCALL 1 id=a name=Aric sig=0", id, name, sig));
    CHECK(!detail::parse_beacon("LANCALL 1 id=a name=Aric sig=70000", id, name, sig));
    CHECK(!detail::parse_beacon("LANCALL 1 id=a name=Aric sig=50x1", id, name, sig));
}

static Peer peer(const std::string& name, const std::string& ip, uint32_t seen) {
    Peer p;
    p.name = name;
    p.ip = ip;
    p.signal_port = 5011;
    p.last_seen_us = seen;
    return p;
}

static void test_table() {
    std::puts("peer table: sorting, updates, expiry");
    detail::PeerTable t;
    t.seen("id-z", peer("Zed", "10.8.66.30", 1000000));
    t.seen("id-a", peer("Atul", "10.8.66.20", 1000000));

    auto list = t.alive(2000000);
    CHECK(list.size() == 2);
    CHECK(list.size() == 2 && list[0].name == "Atul" && list[1].name == "Zed");

    // same id again with a new address replaces the entry instead of adding one
    t.seen("id-a", peer("Atul", "10.8.66.21", 3000000));
    list = t.alive(3000000);
    CHECK(list.size() == 2 && list[0].ip == "10.8.66.21");

    // Zed was last heard at 1 s, so at 7.5 s (6.5 s later) it's gone; Atul (3 s) isn't
    list = t.alive(7500000);
    CHECK(list.size() == 1 && list[0].name == "Atul");
    t.prune(7500000);
    CHECK(t.size() == 1);
}

static void test_table_clock_wrap() {
    std::puts("peer table across the 32-bit clock wrap");
    detail::PeerTable t;
    t.seen("id", peer("Aric", "10.8.64.119", 0xFFFFF000u));  // just before the wrap
    CHECK(t.alive(0x00001000u).size() == 1);                 // 8 ms later, after the wrap
}

int main() {
    test_names();
    test_beacon_roundtrip();
    test_beacon_rejects();
    test_table();
    test_table_clock_wrap();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all discovery tests passed");
    return 0;
}