// lancall_directory — run on one laptop so people on other floors can find each other
// (Person 1, control/, phase 3).
//
//   lancall_directory [--port 5012]
//
// Then start lancall with --directory <this laptop's IP>. Prints the table every 5 s.
// The laptop running it can join calls too: its own lancall just uses its own IP.
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

#include "common/protocol.hpp"
#include "control/directory.hpp"

static volatile std::sig_atomic_t g_stop = 0;

int main(int argc, char** argv) {
    uint16_t port = proto::kDirectoryPort;
    if (argc == 3 && std::string(argv[1]) == "--port")
        port = static_cast<uint16_t>(std::stoi(argv[2]));
    std::signal(SIGINT, [](int) { g_stop = 1; });

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