// Tests for net/bridge.hpp (phase 7): a broadcast passed on to another floor's multicast group.
// Everything runs on this laptop: a "presenter" socket sends to the bridge over loopback, and
// sockets joined to the group (multicast loop on) play the viewers. UDP ports 25041-25045.
// If this laptop has no route for multicast at all (no network up), the forwarding checks
// are skipped, not failed. Lives in tests/metrics/ with Person 2's other tests.

#include "net/bridge.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "common/nack.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "common/udp_socket.hpp"

namespace {

constexpr uint16_t kIn = 25041, kVideo = 25042, kAudio = 25043, kIn2 = 25044;
const std::string kGroup = proto::lancall_group(251);

std::vector<uint8_t> datagram(proto::Type type, uint16_t seq, size_t body) {
    proto::Header h{};
    h.type = type;
    h.stream_id = 1;
    h.seq = seq;
    h.timestamp_us = 123456;
    h.frag_count = 1;
    std::vector<uint8_t> d(proto::kHeaderSize + body);
    proto::write_header(h, d.data());
    for (size_t i = 0; i < body; ++i) d[proto::kHeaderSize + i] = static_cast<uint8_t>(i * 7);
    return d;
}

// what arrives on s within timeout_ms (empty = nothing)
std::vector<uint8_t> receive(UdpSocket& s, int timeout_ms) {
    std::vector<uint8_t> buf(2048);
    std::string ip;
    uint16_t port = 0;
    const ssize_t n = s.recv_from(buf.data(), buf.size(), ip, port, timeout_ms);
    buf.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return buf;
}

void send(UdpSocket& s, uint16_t port, const std::vector<uint8_t>& d) {
    s.send_to("127.0.0.1", port, d.data(), d.size());
}

net::BridgeOptions options(uint16_t port, const std::string& from) {
    net::BridgeOptions o;
    o.port = port;
    o.from_ip = from;
    o.group = kGroup;
    o.loop = true;  // the "viewers" are on this laptop
    o.video_port = kVideo;
    o.audio_port = kAudio;
    o.quiet_ms = 300;
    return o;
}

void test_bad_options() {
    net::Bridge b;
    net::BridgeOptions o = options(kIn, "127.0.0.1");
    o.group = "10.8.66.20";  // not a group
    CHECK(!b.start(o));
    CHECK(b.error().find("not a multicast group") != std::string::npos);
    o = options(kIn, "");
    CHECK(!b.start(o));
    CHECK(!b.running());
}

void test_forwarding() {
    UdpSocket viewer_video(kVideo), viewer_audio(kAudio);
    CHECK(viewer_video.join_multicast(kGroup));
    CHECK(viewer_audio.join_multicast(kGroup));

    net::Bridge b;
    CHECK(b.start(options(kIn, "127.0.0.1")));
    CHECK(b.running());
    CHECK(!b.live());  // nothing from the presenter yet

    UdpSocket presenter(0);
    const auto video = datagram(proto::Type::Video, 1, 900);
    send(presenter, kIn, video);
    const auto got = receive(viewer_video, 1000);
    if (got.empty()) {
        std::puts("  (skipped: no route for multicast on this laptop)");
        return;
    }
    CHECK(got == video);  // passed on byte for byte
    CHECK(b.live());

    const auto audio = datagram(proto::Type::Audio, 2, 640);
    send(presenter, kIn, audio);
    CHECK(receive(viewer_audio, 1000) == audio);  // audio goes to the audio port
    const auto parity = datagram(proto::Type::VideoFec, 3, 500);
    send(presenter, kIn, parity);
    CHECK(receive(viewer_video, 1000) == parity);  // FEC goes where video goes
    CHECK(receive(viewer_audio, 100).empty());     // and nothing strays

    // a viewer here reports on the audio: it reaches the presenter's (audio) socket
    UdpSocket viewer(0);
    proto::ReceiverReport r;
    r.media = proto::Type::Audio;
    r.loss_permille = 12;
    const auto report = proto::make_report_packet(1, 0, r);
    send(viewer, kIn, report);
    CHECK(receive(presenter, 1000) == report);

    // NACKs (the presenter doesn't resend for broadcasts) and junk stop here
    proto::Nack nk;
    nk.seqs = {1, 2};
    send(viewer, kIn, proto::make_nack_packet(1, 0, nk));
    const std::vector<uint8_t> junk = {1, 2, 3};
    send(viewer, kIn, junk);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(receive(presenter, 100).empty());
    const net::BridgeCounters c = b.counters();
    CHECK(c.forwarded == 3);
    CHECK(c.reports_back == 1);
    CHECK(c.dropped_other == 1);
    CHECK(c.dropped_bad == 1);

    // quiet for longer than quiet_ms: no longer live (lancall_bridge stops announcing)
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    CHECK(!b.live());
    b.stop();
    CHECK(!b.running());
}

void test_only_the_presenter() {
    UdpSocket viewer_video(kVideo);
    CHECK(viewer_video.join_multicast(kGroup));
    net::Bridge b;
    CHECK(b.start(options(kIn2, "10.9.9.9")));  // the presenter is somewhere else
    UdpSocket someone(0);
    send(someone, kIn2, datagram(proto::Type::Video, 1, 100));
    CHECK(receive(viewer_video, 300).empty());  // not passed on to the floor
    CHECK(!b.live());
    CHECK(b.counters().dropped_other == 1);
    CHECK(b.counters().forwarded == 0);
}

}  // namespace

int main() {
    RUN(test_bad_options);
    RUN(test_forwarding);
    RUN(test_only_the_presenter);
    return check::result();
}
