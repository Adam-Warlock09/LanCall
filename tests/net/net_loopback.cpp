// Sends fake "frames" through a real UDP socket and checks they come back intact.
// Good first check on any laptop, and handy with netem later:
//
//   net_loopback                         127.0.0.1, 200 frames, no loss
//   net_loopback --drop 5                drop 5% of packets on purpose before sending
//   net_loopback --frames 1000 --max 60000
//
// Exit code 0 when, with no forced drops, every frame arrived byte for byte.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

#include "packetizer.hpp"
#include "reassembler.hpp"
#include "udp_socket.hpp"

// frame contents depend only on the frame id, so the receiver can check them
static void fill(std::vector<uint8_t>& f, uint16_t id) {
    for (size_t i = 0; i < f.size(); ++i) f[i] = static_cast<uint8_t>(id * 31 + i);
}

int main(int argc, char** argv) {
    int frames = 200;
    size_t max_size = 60000;
    double drop_pct = 0;
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string a = argv[i];
        if (a == "--frames")
            frames = std::atoi(argv[i + 1]);
        else if (a == "--max")
            max_size = std::strtoul(argv[i + 1], nullptr, 10);
        else if (a == "--drop")
            drop_pct = std::atof(argv[i + 1]);
    }

    UdpSocket rx(0);  // any free port
    rx.set_recv_buffer(4 << 20);
    uint16_t port = rx.local_port();

    std::atomic<bool> sender_done{false};
    int good = 0, corrupt = 0;
    net::Reassembler reasm;

    std::thread receiver([&] {
        std::vector<uint8_t> buf(2048);
        std::string ip;
        uint16_t from;
        net::Frame f;
        std::vector<uint8_t> expect;
        int idle = 0;
        // keep reading until the sender is done and nothing has arrived for a bit
        while (!(sender_done && idle > 3)) {
            ssize_t n = rx.recv_from(buf.data(), buf.size(), ip, from, 50);
            if (n <= 0) {
                idle++;
                continue;
            }
            idle = 0;
            proto::Header h;
            if (!proto::read_header(buf.data(), n, h)) continue;
            if (reasm.push(h, buf.data() + proto::kHeaderSize, n - proto::kHeaderSize,
                           proto::now_us(), f)) {
                expect.resize(f.data.size());
                fill(expect, f.frame_id);
                if (f.data == expect)
                    good++;
                else
                    corrupt++;
            }
            reasm.expire(proto::now_us());
        }
    });

    UdpSocket tx(0);
    net::Packetizer pk(proto::Type::Video, 1);
    std::mt19937 rng(42);
    std::uniform_int_distribution<size_t> size_dist(1000, max_size);
    std::uniform_real_distribution<double> coin(0, 100);
    int sent_pkts = 0, dropped_pkts = 0;

    for (int i = 0; i < frames; ++i) {
        std::vector<uint8_t> frame(size_dist(rng));
        fill(frame, static_cast<uint16_t>(i));
        for (auto& d : pk.packetize(frame.data(), frame.size(), proto::now_us())) {
            if (coin(rng) < drop_pct) {
                dropped_pkts++;
                continue;
            }
            tx.send_to("127.0.0.1", port, d.data(), d.size());
            sent_pkts++;
        }
        // roughly a frame every 2 ms. blasting with no gap can overflow the receive buffer
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sender_done = true;
    receiver.join();

    auto& s = reasm.stats();
    std::printf("frames sent %d | packets sent %d, dropped on purpose %d\n", frames, sent_pkts,
                dropped_pkts);
    std::printf("frames ok %d, corrupt %d | incomplete %llu, late pieces %llu, dups %llu\n", good,
                corrupt, (unsigned long long)s.dropped_incomplete,
                (unsigned long long)s.late_pieces, (unsigned long long)s.duplicate_pieces);

    if (corrupt) return 1;
    if (drop_pct == 0 && good != frames) return 1;
    return 0;
}
