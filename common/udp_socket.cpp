#include "udp_socket.hpp"

#include "platform.hpp"  // the socket headers, for Linux and Windows

#if !defined(_WIN32)
#include <poll.h>
#include <unistd.h>
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <vector>

#include "media_crypto.hpp"

// Phase 8: the few places Linux and Windows sockets differ, kept together here.
namespace {

using Handle = UdpSocket::Handle;

#if defined(_WIN32)

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

constexpr Handle kInvalid = INVALID_SOCKET;

void close_handle(Handle fd) {
    closesocket(fd);
}

std::string last_error() {
    return "socket error " + std::to_string(WSAGetLastError());
}

int wait_readable(Handle fd, int timeout_ms) {
    WSAPOLLFD p{};
    p.fd = fd;
    p.events = POLLRDNORM;
    return WSAPoll(&p, 1, timeout_ms);
}

// Windows reports some failures on the NEXT receive instead of dropping them quietly
bool harmless_receive_error() {
    const int e = WSAGetLastError();
    return e == WSAEMSGSIZE        // a datagram bigger than the buffer: Linux would cut it
           || e == WSAECONNRESET;  // an ICMP "port unreachable" for something we sent earlier
}

#else

constexpr Handle kInvalid = -1;

void close_handle(Handle fd) {
    close(fd);
}

std::string last_error() {
    return std::strerror(errno);
}

int wait_readable(Handle fd, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    return r < 0 && errno == EINTR ? 0 : r;  // a signal (Ctrl+C) isn't an error
}

bool harmless_receive_error() {
    return false;
}

#endif

// setsockopt wants a const char* on Windows and a const void* on Linux; this fits both
template <typename T>
bool set_option(Handle fd, int level, int name, const T& value) {
    return setsockopt(fd, level, name, reinterpret_cast<const char*>(&value), sizeof(value)) == 0;
}

}  // namespace

UdpSocket::UdpSocket(uint16_t bind_port) {
    platform::init();  // Winsock must be started before the first socket
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ == kInvalid) throw std::runtime_error("socket: " + last_error());

    // lets us restart a receiver right away without "address already in use", and lets two
    // sockets on one laptop both receive a multicast group
    set_option(fd_, SOL_SOCKET, SO_REUSEADDR, 1);

#if defined(_WIN32)
    // otherwise an ICMP "port unreachable" (we sent to someone who has left) makes the next
    // receive fail with WSAECONNRESET
    BOOL report = FALSE;
    DWORD unused = 0;
    WSAIoctl(fd_, SIO_UDP_CONNRESET, &report, sizeof(report), nullptr, 0, &unused, nullptr,
             nullptr);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bind_port);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::string err = last_error();
        close_handle(fd_);
        throw std::runtime_error("bind port " + std::to_string(bind_port) + ": " + err);
    }
}

UdpSocket::~UdpSocket() {
    if (fd_ != kInvalid) close_handle(fd_);
}

bool UdpSocket::send_to(const std::string& ip, uint16_t port, const uint8_t* data, size_t len) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) return false;

    if (auto c = crypto()) {
        thread_local std::vector<uint8_t> sealed;
        if (!c->seal(data, len, sealed)) return false;
        data = sealed.data();
        len = sealed.size();
    }
    const auto n = sendto(fd_, reinterpret_cast<const char*>(data), static_cast<int>(len), 0,
                          reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    return n >= 0 && static_cast<size_t>(n) == len;
}

ssize_t UdpSocket::recv_from(uint8_t* buf, size_t cap, std::string& from_ip, uint16_t& from_port,
                             int timeout_ms) {
    // wait first so the caller's loop can wake up and check for ctrl+c etc
    const int r = wait_readable(fd_, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return -1;

    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    auto c = crypto();
    thread_local std::vector<uint8_t> raw(65536);
    uint8_t* into = c ? raw.data() : buf;
    const size_t room = c ? raw.size() : cap;
    ssize_t n = recvfrom(fd_, reinterpret_cast<char*>(into), static_cast<int>(room), 0,
                         reinterpret_cast<sockaddr*>(&src), &slen);
    if (n < 0) return harmless_receive_error() ? 0 : -1;
    if (c) {
        thread_local std::vector<uint8_t> opened;
        if (!c->open(raw.data(), static_cast<size_t>(n), opened) || opened.size() > cap) {
            std::lock_guard<std::mutex> lock(crypto_mu_);
            rejected_++;
            return 0;  // forged, damaged, from another call, or not sealed at all
        }
        std::memcpy(buf, opened.data(), opened.size());
        n = static_cast<ssize_t>(opened.size());
    }

    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
    from_ip = ipbuf;
    from_port = ntohs(src.sin_port);
    return n;
}

void UdpSocket::set_recv_buffer(int bytes) {
    set_option(fd_, SOL_SOCKET, SO_RCVBUF, bytes);
}

void UdpSocket::enable_broadcast() {
    set_option(fd_, SOL_SOCKET, SO_BROADCAST, 1);
}

namespace {
bool group_request(Handle fd, int opt, const std::string& group, const std::string& iface_ip) {
    ip_mreq m{};
    if (inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr) != 1) return false;
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!iface_ip.empty() && inet_pton(AF_INET, iface_ip.c_str(), &m.imr_interface) != 1)
        return false;
    return set_option(fd, IPPROTO_IP, opt, m);
}
}  // namespace

bool UdpSocket::join_multicast(const std::string& group, const std::string& iface_ip) {
    return group_request(fd_, IP_ADD_MEMBERSHIP, group, iface_ip);
}

bool UdpSocket::leave_multicast(const std::string& group, const std::string& iface_ip) {
    return group_request(fd_, IP_DROP_MEMBERSHIP, group, iface_ip);
}

bool UdpSocket::set_multicast_interface(const std::string& iface_ip) {
    in_addr a{};
    if (inet_pton(AF_INET, iface_ip.c_str(), &a) != 1) return false;
    return set_option(fd_, IPPROTO_IP, IP_MULTICAST_IF, a);
}

// Loop and TTL as an int: Linux takes an int or a byte, Windows wants 4 bytes. On Windows the
// loop setting acts on the receiving side instead (a socket with it off doesn't get this
// laptop's own group packets); receivers filter by source anyway, so either way works.
void UdpSocket::set_multicast_loop(bool on) {
    set_option(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, on ? 1 : 0);
}

void UdpSocket::set_multicast_ttl(int ttl) {
    set_option(fd_, IPPROTO_IP, IP_MULTICAST_TTL, ttl < 0 ? 0 : ttl > 255 ? 255 : ttl);
}

void UdpSocket::set_crypto(std::shared_ptr<const proto::MediaCrypto> crypto) {
    std::lock_guard<std::mutex> lock(crypto_mu_);
    crypto_ = std::move(crypto);
}

std::shared_ptr<const proto::MediaCrypto> UdpSocket::crypto() const {
    std::lock_guard<std::mutex> lock(crypto_mu_);
    return crypto_;
}

uint64_t UdpSocket::rejected() const {
    std::lock_guard<std::mutex> lock(crypto_mu_);
    return rejected_;
}

uint16_t UdpSocket::local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}
