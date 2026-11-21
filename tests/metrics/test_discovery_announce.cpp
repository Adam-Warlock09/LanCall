// Unit tests for the phase 7 beacon fields (control/discovery_detail.hpp): announcing a
// multicast group and a broadcast title, picking a free group, and a live check with two
// Discovery objects on this laptop (skipped when there's no network to broadcast on).
// Lives in tests/metrics/ with Person 2's other tests.

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/discovery_detail.hpp"

using control::Peer;
using control::detail::Announce;
using control::detail::clean_title;
using control::detail::first_group_choice;
using control::detail::make_beacon;
using control::detail::parse_beacon;
using control::detail::pick_group;

namespace {

Peer local_peer(uint8_t group, bool remote = false) {
    Peer p;
    p.name = "P";
    p.ip = "10.8.66.20";
    p.group = group;
    p.remote = remote;
    return p;
}

void test_plain_beacon_unchanged() {
    // nothing announced: byte for byte the phase 2 beacon, so nothing changes for anyone
    CHECK(make_beacon("3fa9c2d1", "Aric", 5011) == "LANCALL 1 id=3fa9c2d1 name=Aric sig=5011");
    CHECK(make_beacon("3fa9c2d1", "Aric", 5011, Announce{}) ==
          "LANCALL 1 id=3fa9c2d1 name=Aric sig=5011");
    // a title without a group means nothing and isn't sent
    CHECK(make_beacon("3fa9c2d1", "Aric", 5011, Announce{0, "talk"}).find("title") ==
          std::string::npos);
}

void test_broadcast_round_trip() {
    const std::string b = make_beacon("3fa9c2d1", "Atul", 5011, Announce{137, "CN lecture"});
    CHECK(b == "LANCALL 1 id=3fa9c2d1 name=Atul sig=5011 mc=137 title=CN%20lecture");
    std::string id, name;
    uint16_t sig = 0;
    Announce a;
    CHECK(parse_beacon(b, id, name, sig, a));
    CHECK(id == "3fa9c2d1" && name == "Atul" && sig == 5011);
    CHECK(a.group == 137);
    CHECK(a.title == "CN lecture");

    // odd characters survive too: '=', '%', UTF-8
    const std::string odd = "a=b 100% \xC3\xA9t\xC3\xA9";  // "a=b 100% été"
    CHECK(parse_beacon(make_beacon("1", "X", 1, Announce{9, odd}), id, name, sig, a));
    CHECK(a.title == odd);

    // a multicast call's host: a group, no title
    CHECK(parse_beacon(make_beacon("1", "X", 1, Announce{200, ""}), id, name, sig, a));
    CHECK(a.group == 200 && a.title.empty());
}

void test_old_and_new_readers() {
    // an old reader (the 4-argument parse, as the directory uses) still accepts new beacons
    std::string id, name;
    uint16_t sig = 0;
    CHECK(parse_beacon("LANCALL 1 id=ab name=Atul sig=5011 mc=7 title=x", id, name, sig));
    CHECK(name == "Atul" && sig == 5011);
    // and the new reader takes old beacons: nothing announced
    Announce a{5, "stale"};
    CHECK(parse_beacon("LANCALL 1 id=ab name=Atul sig=5011", id, name, sig, a));
    CHECK(a.group == 0 && a.title.empty());
}

void test_bad_fields_are_ignored() {
    std::string id, name;
    uint16_t sig = 0;
    Announce a;
    for (const char* mc : {"0", "255", "999", "x", ""}) {
        const std::string b = std::string("LANCALL 1 id=ab name=A sig=1 mc=") + mc + " title=t";
        CHECK(parse_beacon(b, id, name, sig, a));  // the beacon itself is fine
        CHECK(a.group == 0 && a.title.empty());
    }
    CHECK(parse_beacon("LANCALL 1 id=ab name=A sig=1 title=lonely", id, name, sig, a));
    CHECK(a.group == 0 && a.title.empty());  // a title needs a group
    CHECK(parse_beacon("LANCALL 1 id=ab name=A sig=1 mc=3 title=%zz", id, name, sig, a));
    CHECK(a.group == 3 && a.title.empty());  // broken escape: no title, group still counts
    // still rejected for the old reasons
    CHECK(!parse_beacon("LANCALL 2 id=ab name=A sig=1 mc=3", id, name, sig, a));
    CHECK(!parse_beacon("LANCALL 1 id=ab name=A sig=0 mc=3", id, name, sig, a));
}

void test_clean_title() {
    CHECK(clean_title("CN lecture") == "CN lecture");
    CHECK(clean_title("two\nlines\there") == "two lines here");  // nothing that moves the cursor
    CHECK(clean_title(std::string(60, 'a')) == std::string(48, 'a'));
    // 30 x "é" (2 bytes each) = 60 bytes: cut to 24 whole characters
    std::string e;
    for (int i = 0; i < 30; ++i) e += "\xC3\xA9";
    CHECK(clean_title(e).size() == 48);
    // 47 ASCII + "€" (3 bytes) straddles the limit: the euro sign goes entirely
    CHECK(clean_title(std::string(47, 'a') + "\xE2\x82\xAC") == std::string(47, 'a'));
    // and the cut applies on the wire too
    std::string id, name;
    uint16_t sig = 0;
    Announce a;
    CHECK(parse_beacon(make_beacon("1", "X", 1, Announce{4, std::string(80, 'z')}), id, name, sig,
                       a));
    CHECK(a.title == std::string(48, 'z'));
}

void test_pick_group() {
    const std::string id = "3fa9c2d1";
    const uint8_t first = first_group_choice(id);
    CHECK(first >= 1 && first <= 254);
    CHECK(first_group_choice(id) == first);  // the same id, the same start
    CHECK(pick_group(id, {}) == first);

    // someone on our floor already has it: the next one
    const uint8_t next = first == 254 ? 1 : first + 1;
    CHECK(pick_group(id, {local_peer(first)}) == next);
    // someone on another floor with it doesn't matter
    CHECK(pick_group(id, {local_peer(first, true)}) == first);

    // everything from `first` to 254 taken: it wraps to 1 (unless 1 is taken too, etc.)
    std::vector<Peer> busy;
    for (int g = first; g <= 254; ++g) busy.push_back(local_peer(static_cast<uint8_t>(g)));
    CHECK(pick_group(id, busy) == (first == 1 ? 0 : 1));

    // all 254 taken
    std::vector<Peer> all;
    for (int g = 1; g <= 254; ++g) all.push_back(local_peer(static_cast<uint8_t>(g)));
    CHECK(pick_group(id, all) == 0);
}

void test_remote_view_drops_old_broadcast() {
    // heard on our floor while broadcasting, then only through the directory: the broadcast
    // can't be watched from here, so it must not be listed
    control::detail::PeerTable t;
    Peer p = local_peer(42);
    p.broadcast_title = "talk";
    p.last_seen_us = 1000000;
    t.seen("a", p);
    auto now = t.alive(2000000);
    CHECK(now.size() == 1 && now[0].group == 42 && now[0].broadcast_title == "talk");
    Peer r = local_peer(0, true);
    r.last_seen_us = 7500000;
    t.seen_remote("a", r);
    now = t.alive(8000000);  // the local sighting is 7 s old: gone
    CHECK(now.size() == 1 && now[0].remote);
    CHECK(now[0].group == 0 && now[0].broadcast_title.empty());
}

void test_live_announce() {
    // two copies on this laptop share the discovery port and hear each other's broadcasts
    control::Discovery a, b;
    a.announce(42, "Test talk");  // before start(): kept
    if (!a.start("AnnA", 5191) || !b.start("AnnB", 5192)) {
        std::puts("  (skipped: discovery port busy)");
        return;
    }
    bool seen = false;
    for (int i = 0; i < 40 && !seen; ++i) {  // up to 4 s: beacons every 2 s
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (const Peer& p : b.peers())
            if (p.name == "AnnA" && p.group == 42 && p.broadcast_title == "Test talk") seen = true;
    }
    if (!seen && !a.error().empty()) {
        std::printf("  (skipped: %s)\n", a.error().c_str());
        return;
    }
    CHECK(seen);
    CHECK(!a.own_ip().empty());  // our own beacon came back
    CHECK(proto::is_multicast_ipv4(proto::lancall_group(a.pick_group())));
    CHECK(b.pick_group() != 42);  // b sees a on 42 and stays off it

    // clearing goes out at once (well before the next 2 s beacon)
    a.clear_announce();
    bool cleared = false;
    for (int i = 0; i < 15 && !cleared; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        for (const Peer& p : b.peers())
            if (p.name == "AnnA" && p.group == 0 && p.broadcast_title.empty()) cleared = true;
    }
    CHECK(cleared);
}

// own_ip from our own beacon's echoes: never another laptop's (a replayed beacon), and on a
// laptop with several cards (Windows echoes on WSL / VirtualBox adapters too) the card that
// reaches the directory or a peer
void test_choose_own_ip() {
    using control::detail::choose_own_ip;
    // one card, nothing to compare with: the echo's address
    CHECK(choose_own_ip("10.8.67.8", true, "", "") == "10.8.67.8");
    // someone replayed our beacon from their laptop: ignored
    CHECK(choose_own_ip("10.8.67.50", false, "", "10.8.67.8") == "10.8.67.8");
    CHECK(choose_own_ip("10.8.67.50", false, "", "").empty());
    // the wired card reaches the peers (want): a virtual adapter's echo doesn't replace it
    CHECK(choose_own_ip("172.29.0.1", true, "10.8.67.8", "10.8.67.8") == "10.8.67.8");
    // ... and when the virtual adapter's echo came first, the wired one takes over
    CHECK(choose_own_ip("10.8.67.8", true, "10.8.67.8", "172.29.0.1") == "10.8.67.8");
    // before any echo matches, something is better than nothing
    CHECK(choose_own_ip("192.168.56.1", true, "10.8.67.8", "") == "192.168.56.1");
}

}  // namespace

int main() {
    RUN(test_plain_beacon_unchanged);
    RUN(test_broadcast_round_trip);
    RUN(test_old_and_new_readers);
    RUN(test_choose_own_ip);
    RUN(test_bad_fields_are_ignored);
    RUN(test_clean_title);
    RUN(test_pick_group);
    RUN(test_remote_view_drops_old_broadcast);
    RUN(test_live_announce);
    return check::result();
}
