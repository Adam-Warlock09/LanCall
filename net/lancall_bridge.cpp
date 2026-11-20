// lancall_bridge — show a broadcast on another floor (Person 2, net/, phase 7).
//
//   lancall_bridge --from <presenter_ip> --name <presenter_name> [--title TEXT] [--port 5016]
//
// Run it on a laptop on the floor where people want to watch. The presenter (on its own
// floor, broadcasting) then types `bridge <this laptop's ip>` in lancall, which sends one
// unicast copy here; this program puts it on a multicast group on this floor (net/bridge.hpp)
// and announces it in a discovery beacon of its own as "<name>-bridge", with the title plus
// " (via bridge)". Viewers here type `watch <name>-bridge`. The announcement is only on while
// the presenter's media actually arrives. lancall's `bridge` command prints the exact line to
// run. This laptop can watch too, with its own lancall. Ctrl+C stops it.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/discovery_detail.hpp"
#include "net/bridge.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;

void on_stop(int) {
    g_stop = 1;
}

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s --from <presenter_ip> --name <presenter_name> [--title TEXT] "
                 "[--port N]\n",
                 prog);
}

constexpr const char* kSuffix = " (via bridge)";

// the title we announce: theirs, cut so " (via bridge)" still fits in a beacon's 48 bytes
std::string bridged_title(const std::string& title) {
    std::string t = control::detail::clean_title(title);
    const size_t room = control::detail::kMaxTitleBytes - std::string(kSuffix).size();
    if (t.size() > room) {
        size_t cut = room;  // don't split a UTF-8 character (see clean_title)
        while (cut > 0 && (static_cast<unsigned char>(t[cut]) & 0xC0) == 0x80) --cut;
        t.resize(cut);
    }
    return t + kSuffix;
}

}  // namespace

int main(int argc, char** argv) {
    std::string from, name, title;
    int port = net::kBridgePort;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--from" && has_value) {
            from = argv[++i];
        } else if (a == "--name" && has_value) {
            name = argv[++i];
        } else if (a == "--title" && has_value) {
            title = argv[++i];
        } else if (a == "--port" && has_value) {
            port = std::atoi(argv[++i]);
        } else {
            usage(argv[0]);
            return 2;
        }
    }
    const std::string bridge_name = name + "-bridge";
    if (from.empty() || proto::is_multicast_ipv4(from) || name.empty() ||
        !control::valid_name(bridge_name) || port < 1024 || port > 65535) {
        usage(argv[0]);
        std::fprintf(stderr,
                     "  --from: an IPv4 address; --name: letters, digits, - and _, at "
                     "most 25 characters\n");
        return 2;
    }
    if (title.empty()) title = name + "'s broadcast";
    std::signal(SIGINT, on_stop);
    std::signal(SIGTERM, on_stop);

    // our own discovery, under the bridge's name: the beacon carries the announcement. Its
    // "sig" port is the bridge's port, where nobody answers calls, so `call <name>-bridge`
    // fails at once instead of ringing whatever else runs on this laptop
    control::Discovery discovery;
    if (!discovery.start(bridge_name, static_cast<uint16_t>(port))) {
        std::fprintf(stderr, "lancall_bridge: discovery: %s\n", discovery.error().c_str());
        return 1;
    }
    std::printf("listening to this floor's beacons for 3 s, to pick a free group...\n");
    std::fflush(stdout);
    for (int i = 0; i < 30 && !g_stop; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const uint8_t group = discovery.pick_group();
    if (group == 0) {
        std::fprintf(stderr, "lancall_bridge: no free multicast group on this floor\n");
        return 1;
    }

    net::BridgeOptions opt;
    opt.port = static_cast<uint16_t>(port);
    opt.from_ip = from;
    opt.group = proto::lancall_group(group);
    opt.iface_ip = discovery.own_ip();
    net::Bridge bridge;
    if (!bridge.start(opt)) {
        std::fprintf(stderr, "lancall_bridge: %s\n", bridge.error().c_str());
        return 1;
    }
    const std::string shown = bridged_title(title);
    std::printf(
        "bridge for %s (%s) on UDP %d -> %s on this floor%s\n"
        "the presenter types: bridge <this laptop's ip>; viewers here: watch %s\n",
        name.c_str(), from.c_str(), port, opt.group.c_str(),
        opt.iface_ip.empty() ? " (network card picked by the kernel)" : "", bridge_name.c_str());
    std::fflush(stdout);

    bool announced = false;
    auto next_stats = std::chrono::steady_clock::now();
    while (!g_stop) {
        // announce exactly while there's something to watch
        const bool live = bridge.live();
        if (live && !announced) {
            discovery.announce(group, shown);
            std::printf("%s's broadcast is arriving: announced as \"%s\"\n", name.c_str(),
                        shown.c_str());
        } else if (!live && announced) {
            discovery.clear_announce();
            std::printf("nothing from %s for a while: announcement off\n", from.c_str());
        }
        announced = live;
        if (std::chrono::steady_clock::now() >= next_stats) {
            const net::BridgeCounters c = bridge.counters();
            std::printf("%s  (forwarded %llu, reports back %llu, dropped %llu)\n",
                        bridge.stats().c_str(), static_cast<unsigned long long>(c.forwarded),
                        static_cast<unsigned long long>(c.reports_back),
                        static_cast<unsigned long long>(c.dropped_other + c.dropped_bad));
            next_stats += std::chrono::seconds(5);
        }
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    discovery.clear_announce();
    bridge.stop();
    discovery.stop();
    std::printf("bridge stopped\n");
    return 0;
}
