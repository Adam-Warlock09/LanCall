// Video strips from the network that the receiver must survive (phase 10 fixes).
//
// read_strip() turns away strips a receiver can't use, and a VideoReceiver given such a packet
// over loopback keeps running. Before the fix, one 20-byte packet (a 12-byte header and an
// 8-byte strip prefix with no JPEG after it) made cv::imdecode throw on the receive thread,
// which aborted the whole program. UDP port 25081, loopback only.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "common/protocol.hpp"
#include "common/udp_socket.hpp"
#include "video/strip.hpp"
#include "video/video_pipeline.hpp"

static int failures = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                   \
        }                                                                 \
    } while (0)

constexpr uint16_t kPort = 25081;

static std::vector<uint8_t> prefix(uint16_t frame_height) {
    video::StripInfo s{};
    s.index = 0;
    s.count = 1;
    s.y0 = 0;
    s.frame_height = frame_height;
    std::vector<uint8_t> out(video::kStripPrefix);
    video::write_strip(s, out.data());
    return out;
}

static void test_read_strip() {
    std::puts("read_strip: no JPEG, too tall, normal");
    video::StripInfo info{};
    std::vector<uint8_t> p = prefix(480);
    CHECK(!video::read_strip(p.data(), p.size(), info));  // the prefix alone: nothing to decode
    p.push_back(0xFF);
    CHECK(video::read_strip(p.data(), p.size(), info));  // something after it: up to the decoder
    CHECK(info.frame_height == 480);

    std::vector<uint8_t> tall = prefix(65535);
    tall.push_back(0xFF);
    CHECK(!video::read_strip(tall.data(), tall.size(), info));  // would need gigabytes
    std::vector<uint8_t> max = prefix(video::kMaxFrameHeight);
    max.push_back(0xFF);
    CHECK(video::read_strip(max.data(), max.size(), info));
}

static void test_receiver_survives() {
    std::puts("VideoReceiver: an empty strip and a garbage JPEG (loopback)");
    video::VideoReceiver rx;
    if (!rx.start(kPort)) {
        std::puts("  (skipped: couldn't bind the test port)");
        return;
    }
    UdpSocket tx(0);
    auto send = [&](const std::vector<uint8_t>& strip, uint16_t seq) {
        proto::Header h{};
        h.type = proto::Type::Video;
        h.stream_id = 1;
        h.seq = seq;
        h.timestamp_us = proto::now_us();
        h.frame_id = seq;
        h.frag_index = 0;
        h.frag_count = 1;
        std::vector<uint8_t> pkt(proto::kHeaderSize + strip.size());
        proto::write_header(h, pkt.data());
        std::copy(strip.begin(), strip.end(), pkt.begin() + proto::kHeaderSize);
        tx.send_to("127.0.0.1", kPort, pkt.data(), pkt.size());
    };
    send(prefix(480), 1);  // the 20-byte packet that used to abort the program
    std::vector<uint8_t> junk = prefix(480);
    for (int i = 0; i < 40; ++i) junk.push_back(static_cast<uint8_t>(i * 37));
    send(junk, 2);  // not a JPEG at all
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(rx.running());  // still here: the bad packets were counted, not thrown
    video::VideoFrame f;
    CHECK(!rx.pop(f));  // and nothing came out of them
    rx.stop();
}

int main() {
    test_read_strip();
    test_receiver_survives();
    if (failures) {
        std::printf("%d check(s) failed\n", failures);
        return 1;
    }
    std::puts("all video receive tests passed");
    return 0;
}
