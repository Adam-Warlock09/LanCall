// crypto_bench — experiment E9 (phase 6): what media encryption costs (Person 1, tools/).
// No camera or network needed: seals and opens typical datagrams in a loop on this laptop
// and prints time per packet, the throughput one core manages, and the bytes it adds.
//
//   crypto_bench [--seconds 2]
//
// Compare the "us per packet" with the ~1 ms a packet spends on our LAN and the 66 ms
// between video frames, and the throughput with a call's ~5 Mbps per stream.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/media_crypto.hpp"
#include "common/protocol.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct Result {
    double us_per_packet;
    double mbps;
};

// seals (and opens, if `open` is set) one datagram of `body` bytes for `seconds`
Result run(const proto::MediaCrypto& c, proto::Type type, size_t body, bool open, double seconds) {
    std::vector<uint8_t> d(proto::kHeaderSize + body, 0x5A);
    proto::Header h{type, 2, 0, 0, 0, 0, 1};
    proto::write_header(h, d.data());
    std::vector<uint8_t> sealed, back;
    size_t n = 0;
    auto start = Clock::now();
    auto end = start + std::chrono::duration<double>(seconds);
    while (Clock::now() < end) {
        for (int i = 0; i < 64; ++i) {  // check the clock every 64 packets
            c.seal(d.data(), d.size(), sealed);
            if (open && !c.open(sealed.data(), sealed.size(), back)) {
                std::fprintf(stderr, "open failed\n");
                return {0, 0};
            }
        }
        n += 64;
    }
    double s = std::chrono::duration<double>(Clock::now() - start).count();
    return {s * 1e6 / n, n * d.size() * 8 / s / 1e6};
}

}  // namespace

int main(int argc, char** argv) {
    double seconds = 2;
    if (argc == 3 && std::string(argv[1]) == "--seconds") seconds = std::stod(argv[2]);
    if (!proto::MediaCrypto::available()) {
        std::fprintf(stderr, "crypto_bench: libsodium couldn't start\n");
        return 1;
    }
    proto::MediaCrypto::Key key;
    key.fill(7);
    proto::MediaCrypto c(key);

    struct Case {
        const char* name;
        proto::Type type;
        size_t body;
    } cases[] = {
        {"video packet (1400 B)", proto::Type::Video, 1400},
        {"audio packet (640 B)", proto::Type::Audio, 640},
        {"receiver report (16 B)", proto::Type::Report, 16},
    };

    std::printf("%-24s %14s %14s %12s %10s\n", "", "seal us/pkt", "seal+open", "seal Mbps",
                "overhead");
    for (const Case& k : cases) {
        Result seal = run(c, k.type, k.body, false, seconds / 2);
        Result both = run(c, k.type, k.body, true, seconds / 2);
        double overhead = 100.0 * proto::MediaCrypto::kOverhead /
                          static_cast<double>(proto::kHeaderSize + k.body);
        std::printf("%-24s %14.2f %14.2f %12.0f %9.1f%%\n", k.name, seal.us_per_packet,
                    both.us_per_packet, seal.mbps, overhead);
    }
    std::printf(
        "\nA call sends ~300 video + 50 audio packets a second per stream: at these "
        "speeds that's well under 1%% of one core.\n");
    return 0;
}
