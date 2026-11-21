// lancall_directory — run on one laptop so people on other floors can find each other
// (Person 1, control/, phase 3).
//
//   lancall_directory [--port 5012]
//
// Then start lancall with --directory <this laptop's IP>. Prints the table every 5 s.
// The laptop running it can join calls too: its own lancall just uses its own IP.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "common/platform.hpp"
#include "common/protocol.hpp"
#include "control/directory.hpp"

static std::atomic<int> g_stop{0};

// "--port N" with N in 1024..65535, or nothing. false (with a message) for anything else:
// std::stoi used to abort on a non-number and turned 70000 into 4464
static bool parse_port_option(int argc, char** argv, uint16_t& port) {
    if (argc == 1) return true;
    if (argc == 3 && std::string(argv[1]) == "--port") {
        char* end = nullptr;
        const long v = std::strtol(argv[2], &end, 10);
        if (end != argv[2] && *end == '\0' && v >= 1024 && v <= 65535) {
            port = static_cast<uint16_t>(v);
            return true;
        }
    }
    std::fprintf(stderr, "usage: %s [--port N]   (N: 1024-65535)\n", argv[0]);
    return false;
}

int main(int argc, char** argv) {
    uint16_t port = proto::kDirectoryPort;
    if (!parse_port_option(argc, argv, port)) return 2;
    platform::init();
    platform::on_interrupt([] { g_stop = 1; });

    control::DirectoryServer dir;
    if (!dir.start(port)) {
        std::fprintf(stderr, "lancall_directory: %s\n", dir.error().c_str());
        return 1;
    }
    std::printf("directory listening on UDP %u\n", port);
    while (!g_stop) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::printf("%zu peer(s)\n%s", dir.peer_count(), dir.table().c_str());
        std::fflush(stdout);
    }
    return 0;
}