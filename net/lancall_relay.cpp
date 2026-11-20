// lancall_relay — forward group-call media on its own (Person 1, net/, phase 4).
//
//   lancall_relay [--port 5014]
//
// Then the call's host picks relay mode with this laptop's IP (lancall --relay <ip>, or the
// relay command in a call). Prints who it's forwarding for every 5 s. Ctrl+C stops it.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "common/platform.hpp"
#include "net/relay.hpp"

static std::atomic<int> g_stop{0};

int main(int argc, char** argv) {
    net::RelayOptions opt;
    if (argc == 3 && std::string(argv[1]) == "--port")
        opt.port = static_cast<uint16_t>(std::stoi(argv[2]));
    platform::init();
    platform::on_interrupt([] { g_stop = 1; });

    net::Relay relay;
    if (!relay.start(opt)) {
        std::fprintf(stderr, "lancall_relay: %s\n", relay.error().c_str());
        return 1;
    }
    std::printf("relay listening on UDP %u\n", opt.port);

    auto next = std::chrono::steady_clock::now();
    while (!g_stop) {
        if (std::chrono::steady_clock::now() >= next) {
            std::printf("%s\n", relay.stats().c_str());
            for (const auto& m : relay.members())
                std::printf("  %u  %-15s  in %llu  out %llu\n", m.id, m.ip.c_str(),
                            static_cast<unsigned long long>(m.packets_in),
                            static_cast<unsigned long long>(m.packets_out));
            std::fflush(stdout);
            next += std::chrono::seconds(5);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    relay.stop();
    return 0;
}
