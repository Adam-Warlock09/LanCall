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

private:
    int fd_ = -1;
};
