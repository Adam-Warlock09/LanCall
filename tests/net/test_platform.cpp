// Tests for the platform layer (Person 1, common/platform.hpp, phase 8) on Linux and Windows.
// Exit code 0 = all passed. Binds nothing that stays open.
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "common/platform.hpp"

#if !defined(_WIN32)
#include <csignal>  // raise(): Linux-only part below
#endif

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

static int interrupts = 0;

int main() {
    std::puts("init, twice");
    CHECK(platform::init());
    CHECK(platform::init());

    std::puts("our own addresses");
    CHECK(platform::is_local_address("127.0.0.1"));
    CHECK(platform::is_local_address("127.0.0.3"));   // all of 127/8 is this laptop
    CHECK(!platform::is_local_address("192.0.2.1"));  // TEST-NET-1, nobody's address
    CHECK(!platform::is_local_address("not an ip"));
    CHECK(platform::local_address_towards("127.0.0.1", 5011) == "127.0.0.1");
    CHECK(platform::local_address_towards("not an ip", 5011).empty());

    std::puts("local time, process id");
    std::tm tm{};
    CHECK(platform::local_time(1000000000, tm) && tm.tm_year == 101);  // Sep 2001, any zone
    CHECK(platform::process_id() > 0);

    std::puts("Ctrl+C handler");
    platform::on_interrupt([] { interrupts++; });
#if !defined(_WIN32)
    std::raise(SIGINT);
    std::raise(SIGTERM);
    CHECK(interrupts == 2);
#endif

    std::puts("stderr silencer");
    {
        platform::StderrSilencer quiet;
        std::fprintf(stderr, "(this line should not appear)\n");
    }

    std::puts("line reader starts and stops without waiting for input");
    auto t0 = std::chrono::steady_clock::now();
    {
        platform::LineReader r;
        std::string line;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        while (r.next(line)) {
        }
    }
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));

    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all platform tests passed");
    return 0;
}
