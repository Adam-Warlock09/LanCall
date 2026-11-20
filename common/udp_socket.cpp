#include "udp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

UdpSocket::UdpSocket(uint16_t bind_port) {
    fd_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));

    // lets us restart a receiver right away without "address already in use"
    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(bind_port);
    if (bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::string err = std::strerror(errno);
        close(fd_);
        throw std::runtime_error("bind port " + std::to_string(bind_port) + ": " + err);
    }
}

UdpSocket::~UdpSocket() {
    if (fd_ >= 0) close(fd_);
}

bool UdpSocket::send_to(const std::string& ip, uint16_t port, const uint8_t* data, size_t len) {
    sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &dst.sin_addr) != 1) return false;

    ssize_t n = sendto(fd_, data, len, 0, reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
    return n == static_cast<ssize_t>(len);
}

ssize_t UdpSocket::recv_from(uint8_t* buf, size_t cap, std::string& from_ip, uint16_t& from_port,
                             int timeout_ms) {
    // poll first so the caller's loop can wake up and check for ctrl+c etc
    pollfd p{fd_, POLLIN, 0};
    int r = poll(&p, 1, timeout_ms);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;

    sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t n = recvfrom(fd_, buf, cap, 0, reinterpret_cast<sockaddr*>(&src), &slen);
    if (n < 0) return -1;

    char ipbuf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
    from_ip = ipbuf;
    from_port = ntohs(src.sin_port);
    return n;
}

void UdpSocket::set_recv_buffer(int bytes) {
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &bytes, sizeof(bytes));
}

void UdpSocket::enable_broadcast() {
    int yes = 1;
    setsockopt(fd_, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
}

namespace {
bool group_request(int fd, int opt, const std::string& group, const std::string& iface_ip) {
    ip_mreq m{};
    if (inet_pton(AF_INET, group.c_str(), &m.imr_multiaddr) != 1) return false;
    m.imr_interface.s_addr = htonl(INADDR_ANY);
    if (!iface_ip.empty() && inet_pton(AF_INET, iface_ip.c_str(), &m.imr_interface) != 1)
        return false;
    return setsockopt(fd, IPPROTO_IP, opt, &m, sizeof(m)) == 0;
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
    return setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &a, sizeof(a)) == 0;
}

void UdpSocket::set_multicast_loop(bool on) {
    unsigned char v = on ? 1 : 0;
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_LOOP, &v, sizeof(v));
}

void UdpSocket::set_multicast_ttl(int ttl) {
    unsigned char v = static_cast<unsigned char>(ttl < 0 ? 0 : ttl > 255 ? 255 : ttl);
    setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &v, sizeof(v));
}

uint16_t UdpSocket::local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    return ntohs(addr.sin_port);
}
