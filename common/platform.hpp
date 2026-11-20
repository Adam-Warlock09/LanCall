#pragma once
// The platform layer (Person 1, common/, phase 8 kickoff): everything that differs between
// Linux and Windows, so the rest of the code (and the phase 9 GUI) never includes a POSIX or
// Windows header itself.
//
// FROZEN after the phase 8/9 kickoff: change only by agreement.
//
// The rule from phase 8 on: no <unistd.h>, <poll.h>, <sys/...>, <arpa/inet.h>,
// <netinet/in.h>, <csignal> or <windows.h> outside common/platform.*, common/udp_socket.cpp,
// control/signaling.cpp and tools/devcheck.cpp. Include this header instead: it brings in
// sockaddr_in, inet_pton / inet_ntop and htons on both (winsock2.h + ws2tcpip.h on Windows).
//
// Linux: what the programs did before, moved here. Windows: phase 8 (Aric) fills in
// platform.cpp; the declarations below don't change.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace platform {

// Call at the top of main(), before any socket. Windows: starts Winsock and asks for 1 ms
// timers (otherwise every sleep and timeout rounds up to 15.6 ms, which breaks audio pacing
// and NACK timing). Linux: nothing. Safe to call more than once, from any thread.
bool init();

// Ctrl+C (and SIGTERM on Linux; closing the console on Windows) calls fn. It runs in a
// signal handler or on another thread, so fn may only set an atomic flag. One fn per
// program; a second call replaces the first.
void on_interrupt(void (*fn)());

// localtime_r / localtime_s. false if t can't be converted
bool local_time(std::time_t t, std::tm& out);

// for unique temp file names in tests
int process_id();

// True if ip is one of this laptop's own addresses (binding a socket to it works).
bool is_local_address(const std::string& ip);

// Our address as peer_ip sees it: the source address the system picks to reach it (a UDP
// connect, which sends nothing). Empty if there's no route or peer_ip isn't an address.
std::string local_address_towards(const std::string& peer_ip, uint16_t port);

// Added in phase 8: the broadcast address of every IPv4 network we're on (e.g. 10.8.67.255
// for 10.8.66.20/23), loopback left out. Discovery sends there too on Windows, where
// 255.255.255.255 leaves by one network card only (maybe Wi-Fi or a virtual adapter).
std::vector<std::string> broadcast_addresses();

// Lines typed on the terminal, read on a thread of its own so the main loop never blocks.
// Lines come without the newline (and without a '\r' before it).
class LineReader {
public:
    LineReader();   // starts reading at once
    ~LineReader();  // never waits on a read that's blocked
    LineReader(const LineReader&) = delete;
    LineReader& operator=(const LineReader&) = delete;

    bool next(std::string& line);  // oldest unread line; false if there's none
    bool closed() const;           // the input ended (e.g. started in the background)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// While one exists, stderr goes nowhere. PortAudio's ALSA probing prints dozens of harmless
// "ALSA lib ..." lines on Linux; on Windows there's nothing to hide and this does nothing.
class StderrSilencer {
public:
    StderrSilencer();
    ~StderrSilencer();
    StderrSilencer(const StderrSilencer&) = delete;
    StderrSilencer& operator=(const StderrSilencer&) = delete;

private:
    int saved_ = -1;
};

}  // namespace platform
