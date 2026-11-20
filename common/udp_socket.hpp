#pragma once
#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <string>

class UdpSocket {
public:
    // 0 = let the OS pick a free port. throws std::runtime_error if it can't bind
    explicit UdpSocket(uint16_t bind_port = 0);
    ~UdpSocket();
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    bool send_to(const std::string& ip, uint16_t port, const uint8_t* data, size_t len);

    // bytes received, 0 on timeout, -1 on error. timeout_ms < 0 waits forever
    ssize_t recv_from(uint8_t* buf, size_t cap, std::string& from_ip, uint16_t& from_port,
                      int timeout_ms);

    // the kernel may cap this, see net.core.rmem_max
    void set_recv_buffer(int bytes);
    void enable_broadcast();  // needed for discovery in phase 2
    uint16_t local_port() const;

    // Phase 7, multicast. A socket bound to a port receives a group's packets for that port
    // once it has joined the group. iface_ip picks the network card (our wired address);
    // empty lets the kernel choose, which may be Wi-Fi. false + errno set on failure.
    bool join_multicast(const std::string& group, const std::string& iface_ip = "");
    bool leave_multicast(const std::string& group, const std::string& iface_ip = "");
    // for sending to groups: which card they leave by, whether we hear our own (on by
    // default: needed for one-laptop tests), and how many routers they may cross (default 1)
    bool set_multicast_interface(const std::string& iface_ip);
    void set_multicast_loop(bool on);
    void set_multicast_ttl(int ttl);

private:
    int fd_ = -1;
};
