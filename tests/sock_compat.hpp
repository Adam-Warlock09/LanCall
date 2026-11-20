#pragma once
// Raw sockets for the tests that drive the wire by hand (Person 1, tests/, phase 8): the few
// calls that differ between Linux and Windows. Test-only; the programs use UdpSocket and
// signaling, which have their own. Call platform::init() before the first socket.
#include "common/platform.hpp"  // the socket headers on both

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

#include <cstddef>

namespace testsock {

#if defined(_WIN32)
using Handle = SOCKET;
constexpr Handle kNone = INVALID_SOCKET;
constexpr int kNoSignal = 0;  // no SIGPIPE on Windows
inline void close(Handle s) {
    closesocket(s);
}
inline int poll(pollfd* fds, unsigned n, int timeout_ms) {
    return WSAPoll(fds, n, timeout_ms);
}
inline bool would_block() {
    return WSAGetLastError() == WSAEWOULDBLOCK;
}
#else
using Handle = int;
constexpr Handle kNone = -1;
constexpr int kNoSignal = MSG_NOSIGNAL;  // a closed peer must not kill the test
inline void close(Handle s) {
    ::close(s);
}
inline int poll(pollfd* fds, unsigned n, int timeout_ms) {
    return ::poll(fds, n, timeout_ms);
}
inline bool would_block() {
    return errno == EAGAIN || errno == EWOULDBLOCK;
}
#endif

// setsockopt wants a const char* on Windows and a const void* on Linux; this fits both
template <typename T>
inline bool set_option(Handle s, int level, int name, const T& value) {
    return setsockopt(s, level, name, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

// waits up to timeout_ms for something to read (or the other side closing)
inline bool readable(Handle s, int timeout_ms) {
    pollfd p{};
    p.fd = s;
    p.events = POLLIN;
    return testsock::poll(&p, 1, timeout_ms) > 0;
}

inline long long send_bytes(Handle s, const void* data, size_t n) {
    return ::send(s, static_cast<const char*>(data), static_cast<int>(n), kNoSignal);
}

inline long long recv_bytes(Handle s, void* data, size_t n) {
    return ::recv(s, static_cast<char*>(data), static_cast<int>(n), 0);
}

}  // namespace testsock
