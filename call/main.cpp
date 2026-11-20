// lancall — the call app (Person 2, call/). Phase 2 two-person calls, phase 4 group calls.
//
//   lancall --name Atul                    find peers, call by name, answer calls
//   lancall --name Atul --peer 10.8.66.20  direct mode: media to that address at start-up,
//                                          no discovery or call setup (checkpoint 1)
//   lancall --name Atul --directory 10.8.64.5  also find peers on other floors, through the
//                                          lancall_directory running on that laptop
//   lancall --name Atul --relay me         calls we start send media through a relay running
//                                          inside this lancall (or --relay IP: another laptop)
//
// Type commands in the terminal (help, list, call <name|ip>, accept, decline, hangup, stats,
// quit; for group calls add <name|ip>, members, relay <ip|me>, mesh); an OpenCV window shows
// the other person with a small self-view, or in a group call a grid with one named tile per
// person. Lip sync: each received frame is shown once the audio being heard from the same
// person has reached its capture time (call/lip_sync.hpp), separately for every person.
//
// Group calls (phase 4): whoever starts the call is the host (member id 1) and adds people
// with `add`. Every room change (someone joined or left, mesh <-> relay) re-points where our
// media goes and whose media we accept (MediaSession::update_route). In relay mode, the
// laptop whose address is the relay runs it inside lancall (net::Relay), whether it chose
// `relay me` itself or the host named it. In relay mode every member also sends a 12-byte
// keepalive to the relay every 2 s, so a member with no working mic or camera is still
// known to it and still gets everyone else's media.
//
// Text chat (phase 6): `text all <msg>` in a call, `text <name|ip> <msg>` to one person in or
// out of a call. Incoming messages print as "[chat] Atul: ..." ("(to you)" when private).
//
// Broadcasts (phase 7, call/broadcast.hpp): `broadcast <title>` sends our camera and mic once
// to a multicast group on this floor and announces it in our discovery beacon; anyone on the
// floor sees it in `list` and types `watch <name>` (no call setup, viewers send nothing).
// `unbroadcast` / `unwatch` end it. Broadcasting, watching and calls exclude each other: one
// camera, and the receive ports 5004 / 5006.
//
// Self-call: --peer 127.0.0.1 sends to and receives from this same program. Both ends then
// share one clock (clock offset exactly 0), so video_e2e_ms and audio_e2e_ms are exact on one
// laptop. Use headphones, or the speaker feeds back into the mic. Between two laptops the
// clock offset comes from call setup's clock sync (Signaling::clock_offset), per member in a
// group call.
//
// Log rows (one CSV per run; metrics::init happens here and nowhere else). In a group call
// the timing rows of all members share one name, so log_summary pools them:
//   frame_shown  value = capture time of the frame shown
//   av_sync_ms   per shown frame: video capture time minus the audio being heard (+ = video ahead)
//   video_e2e_ms per shown frame: capture -> shown (needs the clock offset)
//   audio_e2e_ms 10 per second per person heard: capture -> heard (needs the clock offset)
//   rtt_ms       once per second in a call: round trip of the clock-sync sample in use
//   call_started / call_ended / call_incoming, plus everything the media objects log.
//   phase 4: member_joined / member_left (value = member id), members (people in the call),
//            media_relay (1 = relay, 0 = mesh)
//   phase 7: broadcast_started / watch_started (value = group number), broadcast_ended /
//            watch_ended. A viewer logs av_sync_ms, and the *_e2e_ms rows only when the
//            presenter is this same laptop (one shared clock; there's no clock sync without a call)
//
// Options:
//   --name NAME      required: letters, digits, '-' and '_', at most 32 characters
//   --peer IP        direct mode (see above); 127.0.0.1 for a self-call
//   --directory IP   laptop running lancall_directory, for peers on other floors (phase 3)
//   --relay IP|me    calls we start (as host) use this relay; me = run it here (phase 4)
//   --log PATH       CSV log (default logs/lancall_<name>_<date>_<time>.csv)
//   --in N / --out N PortAudio input / output device (default: system default)
//   --camera N       camera index (default 0)
//   --buffer-ms MS   audio playout buffer, 20-600 (default 60)
//   --fps N          video frame rate, 1-30 (default 15)
//   --quality N      JPEG quality, 1-100 (default 70)
//   --fec            phase 5: send a parity packet after every video strip (rebuilds one lost
//                    packet per strip without asking; costs ~1 packet in 3 extra). NACK
//                    retransmission is always on
//   --port N         phase 7: our signaling port (default 5011): a second lancall on the same
//                    laptop needs another one (one-laptop tests)
//   --mc-loop        phase 7: hear our own multicast (a broadcast watched on this same laptop)

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include "audio/app_util.hpp"
#include "call/broadcast.hpp"
#include "call/call_logic.hpp"
#include "call/commands.hpp"
#include "call/lip_sync.hpp"
#include "call/media_session.hpp"
#include "call/view.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/udp_socket.hpp"
#include "control/discovery.hpp"
#include "control/discovery_detail.hpp"
#include "control/signaling.hpp"
#include "metrics/latency.hpp"
#include "net/relay.hpp"

namespace {

constexpr uint32_t kAudioE2eEveryUs = 100000;    // audio_e2e_ms: 10 rows per second per person
constexpr uint32_t kClockPollEveryUs = 1000000;  // read clock offsets + log rtt_ms once a second
constexpr int kLoopWaitMs = 10;                  // cv::waitKey per pass of the main loop

// Reads typed lines from the terminal on its own thread. poll() with a timeout, so the
// destructor never waits on a blocked read.
class CommandReader {
public:
    CommandReader() : thread_([this] { run(); }) {}
    ~CommandReader() {
        stop_ = true;
        thread_.join();
    }
    CommandReader(const CommandReader&) = delete;
    CommandReader& operator=(const CommandReader&) = delete;

    bool next(std::string& line) {
        std::lock_guard<std::mutex> lock(mu_);
        if (lines_.empty()) return false;
        line = std::move(lines_.front());
        lines_.pop_front();
        return true;
    }

private:
    void run() {
        std::string partial;
        char buf[256];
        while (!stop_) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) return;  // stdin closed (e.g. started in the background): stop reading
            partial.append(buf, static_cast<size_t>(n));
            for (size_t nl; (nl = partial.find('\n')) != std::string::npos;) {
                std::lock_guard<std::mutex> lock(mu_);
                lines_.push_back(partial.substr(0, nl));
                partial.erase(0, nl + 1);
            }
        }
    }

    std::atomic<bool> stop_{false};
    std::mutex mu_;
    std::deque<std::string> lines_;
    std::thread thread_;
};

struct Options {
    std::string name, peer, log_path, directory;
    std::string relay;  // --relay: "" = mesh, "me" or an IPv4 address
    call::MediaOptions media;
    int port = proto::kSignalPort;  // phase 7, --port: our signaling port
    bool mc_loop = false;           // phase 7, --mc-loop: hear our own multicast
};

void usage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s --name NAME [--peer IP] [--directory IP] [--relay IP|me] [--log PATH]\n"
        "          [--in N] [--out N] [--camera N] [--buffer-ms MS] [--fps N] [--quality N]\n"
        "          [--fec] [--port N] [--mc-loop]\n",
        prog);
}

bool parse_args(int argc, char** argv, Options& o) {
    using audio::app::parse_int;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if (a == "--name" && has_value) {
            o.name = argv[++i];
        } else if (a == "--peer" && has_value) {
            o.peer = argv[++i];
        } else if (a == "--directory" && has_value) {
            o.directory = argv[++i];
        } else if (a == "--relay" && has_value) {
            o.relay = argv[++i];
        } else if (a == "--log" && has_value) {
            o.log_path = argv[++i];
        } else if (a == "--in" && has_value) {
            if (!parse_int(argv[++i], 0, 1000, o.media.in_device)) return false;
        } else if (a == "--out" && has_value) {
            if (!parse_int(argv[++i], 0, 1000, o.media.out_device)) return false;
        } else if (a == "--camera" && has_value) {
            if (!parse_int(argv[++i], 0, 63, o.media.camera)) return false;
        } else if (a == "--buffer-ms" && has_value) {
            if (!parse_int(argv[++i], 20, 600, o.media.buffer_ms)) return false;
        } else if (a == "--fps" && has_value) {
            if (!parse_int(argv[++i], 1, 30, o.media.fps)) return false;
        } else if (a == "--quality" && has_value) {
            if (!parse_int(argv[++i], 1, 100, o.media.quality)) return false;
        } else if (a == "--fec") {
            o.media.fec = true;
        } else if (a == "--port" && has_value) {
            if (!parse_int(argv[++i], 1024, 65535, o.port)) return false;
        } else if (a == "--mc-loop") {
            o.mc_loop = true;
        } else {
            return false;
        }
    }
    if (!control::valid_name(o.name)) {
        std::fprintf(stderr, "--name: use 1-32 letters, digits, '-' or '_'\n");
        return false;
    }
    if (!o.peer.empty() && !audio::app::is_ipv4(o.peer)) {
        std::fprintf(stderr, "--peer: give the other laptop's IPv4 address (or 127.0.0.1)\n");
        return false;
    }
    if (!o.directory.empty() && !audio::app::is_ipv4(o.directory)) {
        std::fprintf(stderr,
                     "--directory: give the IPv4 address of the lancall_directory laptop\n");
        return false;
    }
    if (!o.relay.empty() && o.relay != "me" && !audio::app::is_ipv4(o.relay)) {
        std::fprintf(stderr, "--relay: give the relay laptop's IPv4 address, or me\n");
        return false;
    }
    if (!o.directory.empty() && !o.peer.empty()) {
        std::fprintf(stderr,
                     "--directory has no effect with --peer (direct mode has no discovery)\n");
    }
    if (!o.relay.empty() && !o.peer.empty()) {
        std::fprintf(stderr, "--relay has no effect with --peer (direct mode is always direct)\n");
    }
    return true;
}

// True if ip is one of this laptop's own addresses: binding a socket to it only works then.
bool is_local_address(const std::string& ip) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    const bool ok = bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    close(fd);
    return ok;
}

// Our address as `peer_ip` sees it: the source address the kernel picks to reach it.
// Connecting a UDP socket sends nothing. Empty if there is no route.
std::string local_address_towards(const std::string& peer_ip) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(proto::kSignalPort);
    if (inet_pton(AF_INET, peer_ip.c_str(), &a.sin_addr) != 1) return "";
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return "";
    sockaddr_in local{};
    socklen_t len = sizeof local;
    const bool ok = connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 &&
                    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0;
    close(fd);
    char buf[INET_ADDRSTRLEN] = {};
    if (!ok || inet_ntop(AF_INET, &local.sin_addr, buf, sizeof buf) == nullptr) return "";
    return buf;
}

// What we keep for each person whose video we receive (key: their stream_id = member id).
struct RemoteView {
    call::LipSync<cv::Mat> sync;
    cv::Mat shown;
};

unsigned long long ull(uint64_t v) {
    return static_cast<unsigned long long>(v);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse_args(argc, argv, opt)) {
        usage(argv[0]);
        return 2;
    }
    if (opt.log_path.empty()) opt.log_path = audio::app::default_log_path("lancall_" + opt.name);
    const bool direct = !opt.peer.empty();

    audio::app::install_stop_handlers();
    metrics::init(opt.log_path);

    // The window is optional: without a display the app still runs the call from the terminal.
    // Check for a display first: with none, OpenCV's GUI backend can abort the whole process
    // (a GLib error, not a C++ exception), so cv::namedWindow must not even be tried.
    const std::string window = "lancall - " + opt.name;
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    bool have_window = (x11 != nullptr && *x11 != '\0') || (wayland != nullptr && *wayland != '\0');
    if (!have_window) {
        std::fprintf(stderr, "no display: running without a video window\n");
    } else {
        try {
            cv::namedWindow(window, cv::WINDOW_AUTOSIZE);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "no video window (%s); continuing without one\n", e.what());
            have_window = false;
        }
    }

    std::printf("lancall %s  log %s\n", opt.name.c_str(), opt.log_path.c_str());

    // Control plane (not in direct mode). Failing to start it is not fatal: direct mode and
    // calls by IP still work as far as the parts that did start allow.
    control::Discovery discovery;
    control::Signaling signaling;
    bool discovery_ok = false, signaling_ok = false;
    if (!direct) {
        signaling_ok = signaling.start(opt.name, static_cast<uint16_t>(opt.port));
        if (!signaling_ok) std::printf("  call setup unavailable: %s\n", signaling.error().c_str());
        discovery_ok = discovery.start(opt.name, static_cast<uint16_t>(opt.port), opt.directory);
        if (!discovery_ok) std::printf("  discovery unavailable: %s\n", discovery.error().c_str());
        if (discovery_ok && !opt.directory.empty())
            std::printf("  directory %s: peers on other floors show up in list\n",
                        opt.directory.c_str());
        if (!opt.relay.empty())
            std::printf("  calls you start use the relay %s\n",
                        opt.relay == "me" ? "on this laptop" : opt.relay.c_str());
    }

    // Clock offsets to the other laptops (theirs minus ours, mod 2^32) for the *_e2e_ms
    // events, per stream_id. A self-call shares one clock; otherwise they come from
    // Signaling's clock sync (stream 0 = a two-person peer without a member id).
    const bool self_call = opt.peer == "127.0.0.1";
    std::map<uint8_t, uint32_t> offsets;
    bool clock_reported = false;
    double rtt_ms = 0;
    bool same_clock_watch = false;  // phase 7: watching a broadcast from this same laptop
    auto offset_for = [&](uint8_t id) -> std::optional<uint32_t> {
        if (self_call || same_clock_watch) return 0u;
        const auto it = offsets.find(id);
        if (it == offsets.end()) return std::nullopt;
        return it->second;
    };

    call::MediaSession media;
    std::map<uint8_t, RemoteView> remotes;       // per person we receive video from
    std::map<uint8_t, uint32_t> next_audio_e2e;  // per person we hear
    cv::Mat self;
    std::string peer_name, peer_ip = opt.peer;
    std::string relay_choice = opt.relay;  // for calls we start; changed by relay/mesh when idle
    bool multicast_choice = false;  // phase 7: calls we start use multicast (`multicast` when idle)
    uint8_t call_group = 0;         // phase 7: the multicast group of the call we host, 0 = none
    uint8_t announced_group = 0;    // phase 7: what our beacon says about it (host only)
    std::unique_ptr<net::Relay> relay;  // when the relay is this laptop

    // Relay mode: "still here" to the relay every 2 s (call_logic.hpp, keepalive_packet).
    std::unique_ptr<UdpSocket> keepalive_sock;
    uint16_t keepalive_seq = 0;
    uint32_t next_keepalive = proto::now_us();

    // The audio clock to lip-sync a stream's video with: that stream's own, or, while only one
    // person is heard, the overall one (also covers peers and video receivers that don't set
    // stream ids yet, where video arrives as stream 0). src: the call's MediaSession, or the
    // WatchSession of a broadcast (phase 7); both offer the same calls.
    auto audio_clock_for = [&](auto& src, uint8_t id) -> std::optional<uint32_t> {
        uint32_t a = 0;
        if (src.audio_clock(id, a)) return a;
        if (src.audio_streams().size() <= 1 && src.audio_clock(a)) return a;
        return std::nullopt;
    };

    auto ensure_relay = [&]() -> bool {
        if (relay && relay->running()) return true;
        relay = std::make_unique<net::Relay>();
        if (!relay->start()) {
            std::printf("cannot run the relay on this laptop: %s\n", relay->error().c_str());
            relay.reset();
            return false;
        }
        std::printf("relay running on this laptop (UDP port %u)\n", proto::kRelayPort);
        return true;
    };
    auto stop_relay = [&] {
        if (!relay) return;
        relay->stop();
        std::printf("relay stopped: %s\n", relay->stats().c_str());
        relay.reset();
    };

    // Where media goes right now, from the room as signaling knows it.
    auto current_route = [&]() -> call::Route {
        if (direct) return call::direct_route(opt.peer);
        call::Route r;
        r.audio_to = signaling.media_destinations(proto::kAudioPort);
        r.video_to = signaling.media_destinations(proto::kVideoPort);
        r.sources =
            call::media_sources(signaling.members(), signaling.media_mode(), signaling.relay_ip());
        return r;
    };
    // Phase 7: while we host a call in multicast mode, our beacon says which group it uses
    // (mc=, no title: not a broadcast), so others on the floor pick another one.
    auto sync_call_announce = [&] {
        const bool want = !direct && signaling.is_host() &&
                          signaling.media_mode() == control::MediaMode::Multicast &&
                          call_group != 0;
        if (want && announced_group != call_group) {
            discovery.announce(call_group, "");
            announced_group = call_group;
        } else if (!want && announced_group != 0) {
            discovery.clear_announce();
            announced_group = 0;
        }
    };

    // After every room change. The relay runs here when its address is ours. Phase 7: in
    // multicast mode the receivers join the call's group before the senders switch to it,
    // and leave it only after the senders have switched away.
    auto apply_route = [&] {
        if (direct || !media.active()) return;
        const control::MediaMode mode = signaling.media_mode();
        const bool relay_here =
            mode == control::MediaMode::Relay && is_local_address(signaling.relay_ip());
        if (relay_here) ensure_relay();
        const std::string group =
            mode == control::MediaMode::Multicast ? signaling.multicast_group() : std::string();
        if (!group.empty() && !media.join_group(group, opt.media.multicast_if))
            std::printf("can't join the call's multicast group %s: %s\n", group.c_str(),
                        media.error().c_str());
        media.update_route(current_route());
        if (!relay_here) stop_relay();  // after the switch, so nothing in flight is cut off
        if (group.empty() && !media.group().empty()) media.leave_group();
        sync_call_announce();
    };

    // Host: send everyone's media through `arg` (an ip, or "me" = this laptop).
    auto use_relay = [&](const std::string& arg) -> bool {
        std::string ip = arg;
        if (arg == "me") {
            ip.clear();
            for (const control::Member& m : signaling.members()) {
                if (!m.me && !m.ip.empty()) {
                    ip = local_address_towards(m.ip);
                    break;
                }
            }
            if (ip.empty()) {
                std::printf("cannot tell this laptop's address yet (nobody else in the call)\n");
                return false;
            }
        }
        if (is_local_address(ip) && !ensure_relay()) return false;
        signaling.set_media(control::MediaMode::Relay, ip);
        std::printf("media now goes through the relay at %s%s\n", ip.c_str(),
                    is_local_address(ip) ? " (this laptop)" : "");
        metrics::event("media_relay", 1);
        apply_route();
        return true;
    };

    // Phase 7, host: everyone sends one copy to a multicast group. Only when discovery hears
    // every member on our floor, since multicast stops at the floor router.
    auto use_multicast = [&]() -> bool {
        if (!discovery_ok) {
            std::printf("multicast needs discovery, to know who's on this floor: %s\n",
                        discovery.error().c_str());
            return false;
        }
        const auto far = call::off_floor_members(signaling.members(), discovery.peers());
        if (!far.empty()) {
            std::string names;
            for (const auto& m : far) names += (names.empty() ? "" : ", ") + call::member_label(m);
            std::printf(
                "can't use multicast: %s %s not on this floor (multicast stops at the "
                "floor router)\n",
                names.c_str(), far.size() == 1 ? "is" : "are");
            return false;
        }
        const uint8_t group = call_group != 0 ? call_group : discovery.pick_group();
        if (group == 0) {
            std::printf("no free multicast group on this floor (all 254 in use)\n");
            return false;
        }
        call_group = group;
        signaling.set_media(control::MediaMode::Multicast, proto::lancall_group(group));
        std::printf("media now goes to multicast group %s (one copy each, this floor only)\n",
                    proto::lancall_group(group).c_str());
        metrics::event("media_multicast", group);
        apply_route();
        return true;
    };

    // Phase 7, host: someone joined a multicast call from where multicast can't reach them.
    // Switch the call to a relay on this laptop (or to mesh if that can't run here).
    auto keep_everyone_reachable = [&] {
        if (!signaling.is_host() || signaling.media_mode() != control::MediaMode::Multicast) return;
        const auto far = call::off_floor_members(signaling.members(), discovery.peers());
        if (far.empty()) return;
        std::string names;
        for (const auto& m : far) names += (names.empty() ? "" : ", ") + call::member_label(m);
        std::printf(
            "%s %s not on this floor, so multicast can't reach them: switching to a "
            "relay on this laptop\n",
            names.c_str(), far.size() == 1 ? "is" : "are");
        if (use_relay("me")) return;
        signaling.set_media(control::MediaMode::Mesh, "");
        std::printf("no relay here either: switching to mesh\n");
        metrics::event("media_relay", 0);
        apply_route();
    };

    auto start_media = [&](const std::string& ip) {
        remotes.clear();
        next_audio_e2e.clear();
        offsets.clear();
        clock_reported = false;
        next_keepalive = proto::now_us();  // the first one goes out as soon as relay mode is on
        const uint8_t my_id = direct ? 0 : signaling.my_id();
        if (!direct && signaling.media_mode() == control::MediaMode::Relay &&
            is_local_address(signaling.relay_ip()))
            ensure_relay();
        // phase 7: the card group packets leave by is fixed now, even if the call goes
        // multicast later
        opt.media.multicast_if = discovery_ok ? discovery.own_ip() : std::string();
        opt.media.multicast_loop = opt.mc_loop;
        const auto failed = media.start(current_route(), opt.media, my_id);
        for (const auto& f : failed) std::printf("  not started: %s\n", f.c_str());
        // phase 6: everything goes out encrypted with the key the host made for this call
        std::array<uint8_t, control::kMediaKeyBytes> key{};
        if (!direct && signaling.media_key(key)) {
            proto::MediaCrypto::Key k;
            std::copy(key.begin(), key.end(), k.begin());
            media.set_crypto(std::make_shared<const proto::MediaCrypto>(k));
            key.fill(0);
            k.fill(0);
        }
        std::printf("in call with %s (%s%s)%s\n", (peer_name.empty() ? ip : peer_name).c_str(),
                    media.parts().c_str(), media.encrypted() ? ", encrypted" : ", NOT encrypted",
                    self_call ? ", same clock: latency is measured" : "");
        if (!direct) {
            // the host has a code per member, a member one with the host
            const uint8_t other = signaling.is_host() ? 2 : 1;
            const std::string code = signaling.safety_code(other);
            if (!code.empty())
                std::printf(
                    "  safety code with %s: %s (compare it with them: different = someone in "
                    "between)\n",
                    (peer_name.empty() ? ip : peer_name).c_str(), code.c_str());
        }
        metrics::event("call_started", 0);
        apply_route();  // phase 7: joins the group if the call starts in multicast mode
    };
    auto stop_media = [&] {
        if (!media.active()) return;
        media.stop();
        remotes.clear();
        next_audio_e2e.clear();
        offsets.clear();
        stop_relay();
        call_group = 0;
        sync_call_announce();  // phase 7: our beacon no longer names the call's group
        metrics::event("call_ended", 0);
        std::printf("call ended\n");
        for (const auto& line : media.summary()) std::printf("  %s\n", line.c_str());
        rtt_ms = 0;
    };

    // ---- phase 7: broadcasts (call/broadcast.hpp) --------------------------------------------
    // At most one of a call, a broadcast of ours and a broadcast we watch at any time.
    call::BroadcastSession bcast;
    call::WatchSession watch;
    uint32_t next_watch_check = proto::now_us();

    // A call can't start (or be answered) while we broadcast or watch: prints why and says so.
    auto busy_with_broadcast = [&]() -> bool {
        if (bcast.active()) {
            std::printf("you're broadcasting: unbroadcast first\n");
            return true;
        }
        if (watch.active()) {
            std::printf("you're watching %s: unwatch first\n", watch.name().c_str());
            return true;
        }
        return false;
    };

    auto start_broadcast = [&](const std::string& typed_title) {
        if (direct) {
            std::printf("direct mode (--peer): no broadcasts\n");
            return;
        }
        if (!discovery_ok) {
            std::printf("broadcasts need discovery: %s\n", discovery.error().c_str());
            return;
        }
        if (signaling.state() != control::CallState::Idle) {
            std::printf("busy: hang up (or decline) first\n");
            return;
        }
        if (watch.active()) {
            std::printf("you're watching %s: unwatch first\n", watch.name().c_str());
            return;
        }
        const std::string title = control::detail::clean_title(typed_title);
        if (bcast.active()) {  // already on: only the title changes
            bcast.set_title(title);
            discovery.announce(bcast.group(), title);
            std::printf("broadcast title now \"%s\"\n", title.c_str());
            return;
        }
        const uint8_t group = discovery.pick_group();
        if (group == 0) {
            std::printf("no free multicast group on this floor (all 254 in use)\n");
            return;
        }
        call::BroadcastOptions bo;
        bo.in_device = opt.media.in_device;
        bo.camera = opt.media.camera;
        bo.fps = opt.media.fps;
        bo.quality = opt.media.quality;
        bo.iface_ip = discovery.own_ip();
        bo.loop = opt.mc_loop;
        for (const auto& f : bcast.start(title, group, bo))
            std::printf("  not started: %s\n", f.c_str());
        if (!bcast.active()) {
            std::printf("broadcast not started\n");
            return;
        }
        discovery.announce(group, title);
        std::printf("broadcasting \"%s\" (%s) to %s: anyone on this floor can type: watch %s\n",
                    title.c_str(), bcast.parts().c_str(), bcast.group_ip().c_str(),
                    opt.name.c_str());
        if (bo.iface_ip.empty())
            std::printf("  (our address isn't known yet, so the kernel picks the network card)\n");
        metrics::event("broadcast_started", group);
    };
    auto stop_broadcast = [&]() -> bool {
        if (!bcast.active()) return false;
        discovery.clear_announce();
        bcast.stop();
        metrics::event("broadcast_ended", 0);
        std::printf("broadcast ended\n");
        for (const auto& line : bcast.summary()) std::printf("  %s\n", line.c_str());
        return true;
    };

    auto start_watch = [&](const call::BroadcastTarget& t) {
        remotes.clear();
        next_audio_e2e.clear();
        call::WatchOptions wo;
        wo.out_device = opt.media.out_device;
        wo.buffer_ms = opt.media.buffer_ms;
        wo.iface_ip = discovery.own_ip();
        for (const auto& f : watch.start(t.name, t.title, t.ip, t.group, wo))
            std::printf("  not started: %s\n", f.c_str());
        if (!watch.active()) {
            std::printf("could not watch %s\n", t.name.c_str());
            return;
        }
        // the presenter on this same laptop (one-laptop test): one clock, so latency is exact
        same_clock_watch = is_local_address(t.ip);
        std::printf("watching %s: \"%s\" (%s, %s)%s\n", t.name.c_str(), t.title.c_str(),
                    watch.parts().c_str(), proto::lancall_group(t.group).c_str(),
                    same_clock_watch ? ", same laptop: latency is measured" : "");
        metrics::event("watch_started", t.group);
        next_watch_check = proto::now_us() + 1000000;
    };
    auto stop_watch = [&](const std::string& why) {
        if (!watch.active()) return;
        const std::string who = watch.name();
        watch.stop();
        remotes.clear();
        next_audio_e2e.clear();
        same_clock_watch = false;
        metrics::event("watch_ended", 0);
        std::printf("stopped watching %s%s\n", who.c_str(), why.c_str());
        for (const auto& line : watch.summary()) std::printf("  %s\n", line.c_str());
    };

    if (direct) {
        start_media(opt.peer);
    } else {
        std::printf("type list to see peers, call <name> to call, help for all commands\n");
    }
    std::fflush(stdout);

    CommandReader reader;
    bool quit = false;
    uint32_t next_clock_poll = proto::now_us();

    while (!quit && !audio::app::g_stop) {
        const control::CallState state =
            direct ? (media.active() ? control::CallState::InCall : control::CallState::Idle)
                   : signaling.state();

        // 1. typed commands
        std::string line;
        while (reader.next(line)) {
            const call::Command cmd = call::parse_command(line);
            switch (cmd.kind) {
                case call::CommandKind::Empty:
                    break;
                case call::CommandKind::Help:
                    std::fputs(call::help_text(), stdout);
                    break;
                case call::CommandKind::Stats: {
                    if (!media.active() && !bcast.active() && !watch.active())
                        std::printf("no call\n");
                    for (const auto& s : media.stats()) std::printf("  %s\n", s.c_str());
                    for (const auto& [id, rv] : remotes) {
                        const auto& c = rv.sync.counters();
                        const std::string which =
                            remotes.size() > 1 ? " s" + std::to_string(id) : std::string();
                        std::printf(
                            "  lip sync%s: shown %llu  skipped %llu  waited-too-long %llu  "
                            "no-audio %llu  queue-full %llu\n",
                            which.c_str(), ull(c.shown), ull(c.skipped), ull(c.forced),
                            ull(c.no_audio), ull(c.dropped_full));
                    }
                    if (rtt_ms > 0) std::printf("  clock sync: rtt %.2f ms\n", rtt_ms);
                    if (relay) std::printf("  %s\n", relay->stats().c_str());
                    for (const auto& s : bcast.stats()) std::printf("  %s\n", s.c_str());
                    for (const auto& s : watch.stats()) std::printf("  %s\n", s.c_str());
                    break;
                }
                case call::CommandKind::List:
                    if (direct) {
                        std::printf("direct mode (--peer): no discovery\n");
                    } else if (!discovery_ok) {
                        std::printf("discovery unavailable: %s\n", discovery.error().c_str());
                    } else {
                        std::fputs(call::format_peers(discovery.peers(), proto::now_us()).c_str(),
                                   stdout);
                    }
                    break;
                case call::CommandKind::Call: {
                    if (direct) {
                        std::printf("direct mode (--peer): the call is set up at start-up\n");
                        break;
                    }
                    if (busy_with_broadcast()) break;  // phase 7
                    if (!signaling_ok) {
                        std::printf("call setup unavailable: %s\n", signaling.error().c_str());
                        break;
                    }
                    if (state != control::CallState::Idle) {
                        std::printf("busy: hang up (or decline) first%s\n",
                                    state == control::CallState::InCall
                                        ? "; to bring someone into this call, use add"
                                        : "");
                        break;
                    }
                    const call::Target t = call::resolve_target(
                        cmd.arg, discovery_ok ? discovery.peers() : std::vector<control::Peer>{});
                    if (!t.ok()) {
                        std::printf("%s\n", t.error.c_str());
                        break;
                    }
                    if (!signaling.call(t.ip, t.port)) {
                        std::printf("cannot call %s: %s\n", t.ip.c_str(),
                                    signaling.error().c_str());
                        break;
                    }
                    peer_name = t.name;
                    peer_ip = t.ip;
                    std::printf("calling %s (%s)...\n", (t.name.empty() ? t.ip : t.name).c_str(),
                                t.ip.c_str());
                    break;
                }
                case call::CommandKind::Add: {
                    if (direct) {
                        std::printf("direct mode (--peer): no group calls\n");
                        break;
                    }
                    if (busy_with_broadcast()) break;  // phase 7
                    if (!signaling_ok) {
                        std::printf("call setup unavailable: %s\n", signaling.error().c_str());
                        break;
                    }
                    if (state == control::CallState::Calling ||
                        state == control::CallState::Ringing) {
                        std::printf("busy: wait until this call is set up\n");
                        break;
                    }
                    if (state == control::CallState::InCall && !signaling.is_host()) {
                        std::printf("only the host (who started the call) can add people\n");
                        break;
                    }
                    if (state == control::CallState::InCall &&
                        signaling.members().size() >= static_cast<size_t>(proto::kMaxMembers)) {
                        std::printf("the call is full (%d people)\n", proto::kMaxMembers);
                        break;
                    }
                    const call::Target t = call::resolve_target(
                        cmd.arg, discovery_ok ? discovery.peers() : std::vector<control::Peer>{});
                    if (!t.ok()) {
                        std::printf("%s\n", t.error.c_str());
                        break;
                    }
                    const std::string who = t.name.empty() ? t.ip : t.name;
                    if (!signaling.invite(t.ip, t.port)) {
                        const std::string why = signaling.error();
                        std::printf(
                            "cannot add %s: %s\n", who.c_str(),
                            why.empty() ? "already in the call, or not possible yet" : why.c_str());
                        break;
                    }
                    if (state == control::CallState::Idle) {  // same as call: we become the host
                        peer_name = t.name;
                        peer_ip = t.ip;
                        std::printf("calling %s (%s)...\n", who.c_str(), t.ip.c_str());
                    } else {
                        std::printf("ringing %s (%s) into the call...\n", who.c_str(),
                                    t.ip.c_str());
                    }
                    break;
                }
                case call::CommandKind::Members:
                    if (direct) {
                        std::printf("direct mode: you and %s\n", opt.peer.c_str());
                    } else {
                        const bool mc = signaling.media_mode() == control::MediaMode::Multicast;
                        std::fputs(call::format_members(
                                       signaling.members(), signaling.media_mode(),
                                       mc ? signaling.multicast_group() : signaling.relay_ip())
                                       .c_str(),
                                   stdout);
                        // phase 6: the codes to compare, one per connection we share
                        for (const control::Member& m : signaling.members()) {
                            const std::string code = signaling.safety_code(m.id);
                            if (!code.empty())
                                std::printf("  safety code with %s: %s\n", m.name.c_str(),
                                            code.c_str());
                        }
                        std::printf("  media %s\n",
                                    media.encrypted() ? "encrypted" : "NOT encrypted");
                    }
                    break;
                case call::CommandKind::Relay:
                    if (direct) {
                        std::printf("direct mode (--peer): no relay\n");
                    } else if (cmd.arg != "me" && !audio::app::is_ipv4(cmd.arg)) {
                        std::printf("usage: relay <ip|me>\n");
                    } else if (state == control::CallState::Idle) {
                        relay_choice = cmd.arg;
                        multicast_choice = false;
                        std::printf("calls you start will use the relay %s\n",
                                    cmd.arg == "me" ? "on this laptop" : cmd.arg.c_str());
                    } else if (state != control::CallState::InCall) {
                        std::printf("busy: wait until this call is set up\n");
                    } else if (!signaling.is_host()) {
                        std::printf("only the host (who started the call) can change this\n");
                    } else {
                        use_relay(cmd.arg);
                    }
                    break;
                case call::CommandKind::Mesh:
                    if (direct) {
                        std::printf("direct mode (--peer): always direct\n");
                    } else if (state == control::CallState::Idle) {
                        relay_choice.clear();
                        multicast_choice = false;
                        std::printf("calls you start will use mesh (everyone to everyone)\n");
                    } else if (state != control::CallState::InCall) {
                        std::printf("busy: wait until this call is set up\n");
                    } else if (!signaling.is_host()) {
                        std::printf("only the host (who started the call) can change this\n");
                    } else {
                        signaling.set_media(control::MediaMode::Mesh, "");
                        std::printf("media now goes directly between everyone (mesh)\n");
                        metrics::event("media_relay", 0);
                        apply_route();
                    }
                    break;
                case call::CommandKind::Multicast:  // phase 7
                    if (direct) {
                        std::printf("direct mode (--peer): always direct\n");
                    } else if (state == control::CallState::Idle) {
                        multicast_choice = true;
                        relay_choice.clear();
                        std::printf(
                            "calls you start will use multicast, if everyone is on this "
                            "floor (else a relay on this laptop)\n");
                    } else if (state != control::CallState::InCall) {
                        std::printf("busy: wait until this call is set up\n");
                    } else if (!signaling.is_host()) {
                        std::printf("only the host (who started the call) can change this\n");
                    } else {
                        use_multicast();
                    }
                    break;
                case call::CommandKind::Accept:
                case call::CommandKind::Decline:
                    if (direct || state != control::CallState::Ringing) {
                        std::printf("nobody is calling\n");
                    } else if (cmd.kind == call::CommandKind::Accept) {
                        if (bcast.active()) {  // phase 7: the camera is the broadcast's
                            std::printf("you're broadcasting: unbroadcast first, or decline\n");
                            break;
                        }
                        stop_watch(" to take the call");  // phase 7: frees ports 5004/5006
                        signaling.accept();
                    } else {
                        signaling.decline();
                        std::printf("declined\n");
                    }
                    break;
                case call::CommandKind::Hangup:
                    if (direct) {
                        if (media.active()) {
                            stop_media();
                        } else {
                            std::printf("no call\n");
                        }
                    } else if (state == control::CallState::Ringing) {
                        std::printf("someone is calling: type decline\n");
                    } else if (state == control::CallState::Idle) {
                        std::printf("no call\n");
                    } else {
                        signaling.hang_up();
                        stop_media();  // right away; the Ended event that follows is harmless
                    }
                    break;
                case call::CommandKind::Quit:
                    quit = true;
                    break;
                case call::CommandKind::Text: {
                    if (direct) {
                        std::printf("direct mode (--peer): no call setup, so no chat\n");
                        break;
                    }
                    if (!signaling_ok) {
                        std::printf("call setup unavailable: %s\n", signaling.error().c_str());
                        break;
                    }
                    if (cmd.text.size() > control::Signaling::kMaxTextBytes) {
                        std::printf("too long: at most %zu bytes per message\n",
                                    control::Signaling::kMaxTextBytes);
                        break;
                    }
                    std::string lower_arg = cmd.arg;
                    for (char& c : lower_arg)
                        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                    if (lower_arg == "all") {
                        if (state != control::CallState::InCall) {
                            std::printf(
                                "not in a call; text <name|ip> <message> writes to one "
                                "person\n");
                        } else if (signaling.text_all(cmd.text)) {
                            std::printf("[chat] you -> everyone: %s\n", cmd.text.c_str());
                            metrics::event("chat_sent", static_cast<double>(cmd.text.size()));
                        } else {
                            std::printf("message not sent\n");
                        }
                        break;
                    }
                    const call::Target t = call::resolve_text_target(
                        cmd.arg, signaling.members(),
                        discovery_ok ? discovery.peers() : std::vector<control::Peer>{});
                    if (!t.ok()) {
                        std::printf("%s\n", t.error.c_str());
                        break;
                    }
                    if (signaling.text_to(t.ip, cmd.text, t.port)) {
                        std::printf("[chat] you -> %s: %s\n",
                                    (t.name.empty() ? t.ip : t.name).c_str(), cmd.text.c_str());
                        metrics::event("chat_sent", static_cast<double>(cmd.text.size()));
                    } else {
                        std::printf("message not sent\n");
                    }
                    break;
                }
                // phase 7: broadcasts
                case call::CommandKind::Broadcast:
                    start_broadcast(cmd.text);
                    break;
                case call::CommandKind::Unbroadcast:
                    if (!stop_broadcast()) std::printf("not broadcasting\n");
                    break;
                case call::CommandKind::Watch: {
                    if (direct) {
                        std::printf("direct mode (--peer): no broadcasts\n");
                        break;
                    }
                    if (!discovery_ok) {
                        std::printf("broadcasts need discovery: %s\n", discovery.error().c_str());
                        break;
                    }
                    if (state != control::CallState::Idle) {
                        std::printf("busy: hang up (or decline) first\n");
                        break;
                    }
                    if (bcast.active()) {
                        std::printf("you're broadcasting: unbroadcast first\n");
                        break;
                    }
                    const call::BroadcastTarget t =
                        call::resolve_broadcast(cmd.arg, discovery.peers());
                    if (!t.ok()) {
                        std::printf("%s\n", t.error.c_str());
                        break;
                    }
                    if (watch.active() && watch.presenter_ip() == t.ip &&
                        watch.group() == t.group) {
                        std::printf("already watching %s\n", t.name.c_str());
                        break;
                    }
                    stop_watch("");
                    start_watch(t);
                    break;
                }
                case call::CommandKind::Unwatch:
                    if (watch.active()) {
                        stop_watch("");
                    } else {
                        std::printf("not watching anything\n");
                    }
                    break;
                case call::CommandKind::Invalid:
                    std::printf("%s\n", cmd.error.c_str());
                    break;
            }
            std::fflush(stdout);
        }

        // 2. call setup events
        control::CallEvent ev;
        while (!direct && signaling.poll_event(ev)) {
            const std::string who = ev.peer_name.empty() ? ev.peer_ip : ev.peer_name;
            switch (ev.kind) {
                case control::CallEvent::Kind::Incoming:
                    peer_name = ev.peer_name;
                    peer_ip = ev.peer_ip;
                    std::printf("\a%s (%s) is calling: accept or decline\n", who.c_str(),
                                ev.peer_ip.c_str());
                    if (bcast.active())  // phase 7
                        std::printf("  (you're broadcasting: unbroadcast before you accept)\n");
                    else if (watch.active())
                        std::printf("  (accepting stops watching %s)\n", watch.name().c_str());
                    metrics::event("call_incoming", 0);
                    break;
                case control::CallEvent::Kind::Accepted:
                    peer_name = ev.peer_name;
                    peer_ip = ev.peer_ip;
                    // our own call: the room starts in the mode we chose (--relay, relay, mesh,
                    // multicast). Multicast that can't reach everyone starts as relay instead
                    if (signaling.is_host() && multicast_choice) {
                        if (!use_multicast()) {
                            std::printf("starting with a relay on this laptop instead\n");
                            use_relay("me");
                        }
                    } else if (signaling.is_host() && !relay_choice.empty()) {
                        use_relay(relay_choice);
                    }
                    start_media(ev.peer_ip);
                    next_clock_poll = proto::now_us();
                    break;
                case control::CallEvent::Kind::Declined:
                    std::printf("%s declined%s%s\n", who.c_str(), ev.reason.empty() ? "" : ": ",
                                ev.reason.c_str());
                    break;
                case control::CallEvent::Kind::Failed:
                    std::printf("call to %s failed%s%s\n", who.c_str(),
                                ev.reason.empty() ? "" : ": ", ev.reason.c_str());
                    break;
                case control::CallEvent::Kind::Ended:
                    if (!ev.reason.empty()) std::printf("call ended by %s\n", ev.reason.c_str());
                    stop_media();
                    break;
                case control::CallEvent::Kind::Joined: {
                    const size_t n = signaling.members().size();
                    std::printf("%s joined (%zu in the call)\n", who.c_str(), n);
                    const std::string code = signaling.safety_code(ev.member_id);
                    if (!code.empty())
                        std::printf("  safety code with %s: %s\n", who.c_str(), code.c_str());
                    metrics::event("member_joined", ev.member_id);
                    metrics::event("members", static_cast<double>(n));
                    apply_route();
                    keep_everyone_reachable();  // phase 7: a multicast call, someone off-floor
                    break;
                }
                case control::CallEvent::Kind::Left: {
                    const size_t n = signaling.members().size();
                    std::printf("%s left%s%s (%zu in the call)\n", who.c_str(),
                                ev.reason.empty() ? "" : ": ", ev.reason.c_str(), n);
                    remotes.erase(ev.member_id);
                    next_audio_e2e.erase(ev.member_id);
                    offsets.erase(ev.member_id);
                    metrics::event("member_left", ev.member_id);
                    metrics::event("members", static_cast<double>(n));
                    apply_route();
                    break;
                }
                case control::CallEvent::Kind::MediaChanged: {
                    const control::MediaMode mode = signaling.media_mode();
                    const bool via_relay = mode == control::MediaMode::Relay;
                    if (via_relay) {
                        std::printf("media now goes through the relay at %s\n",
                                    signaling.relay_ip().c_str());
                    } else if (mode == control::MediaMode::Multicast) {  // phase 7
                        std::printf("media now goes to multicast group %s\n",
                                    signaling.multicast_group().c_str());
                    } else {
                        std::printf("media now goes directly between everyone (mesh)\n");
                    }
                    metrics::event("media_relay", via_relay ? 1 : 0);
                    apply_route();
                    break;
                }
            }
            std::fflush(stdout);
        }

        // 2a. text chat (phase 6): messages for us, and ours that didn't get through. The log
        // only gets counts, never the text
        control::ChatEvent chat;
        while (!direct && signaling.poll_chat(chat)) {
            if (chat.kind == control::ChatEvent::Kind::Received) {
                const std::string from = chat.name.empty() ? chat.ip : chat.name;
                std::printf("[chat] %s%s: %s\n", from.c_str(), chat.to_all ? "" : " (to you)",
                            chat.text.c_str());
                metrics::event("chat_received", static_cast<double>(chat.text.size()));
            } else {
                std::printf("[chat] not delivered to %s (%s): %s\n", chat.ip.c_str(),
                            chat.reason.c_str(), chat.text.c_str());
            }
            std::fflush(stdout);
        }

        const uint32_t now = proto::now_us();

        // 2b. relay mode: tell the relay we're still here, even if we send no media at all
        if (!direct && media.active() && signaling.media_mode() == control::MediaMode::Relay &&
            !metrics::time_after(next_keepalive, now)) {
            const std::string relay_ip = signaling.relay_ip();
            const uint8_t my_id = signaling.my_id();
            if (!relay_ip.empty() && my_id != 0) {  // the relay ignores stream 0
                if (!keepalive_sock) {
                    try {
                        keepalive_sock = std::make_unique<UdpSocket>();
                    } catch (const std::exception& e) {
                        std::printf("relay keepalive unavailable: %s\n", e.what());
                    }
                }
                if (keepalive_sock) {
                    // sealed like the rest of the call (the relay only reads the header)
                    keepalive_sock->set_crypto(media.crypto());
                    const auto pkt = call::keepalive_packet(my_id, keepalive_seq++, now);
                    keepalive_sock->send_to(relay_ip, proto::kRelayPort, pkt.data(), pkt.size());
                }
            }
            next_keepalive = now + call::kKeepaliveEveryUs;
        }

        // 2c. phase 7: is the broadcast we watch still on? Its presenter's beacon says so (and
        // where it is, should it have moved to another group)
        if (watch.active() && !metrics::time_after(next_watch_check, now)) {
            next_watch_check = now + 1000000;
            bool live = false;
            for (const control::Peer& p : discovery.peers()) {
                if (p.remote || p.ip != watch.presenter_ip() || p.name != watch.name()) continue;
                if (p.group == 0 || p.broadcast_title.empty()) continue;
                live = true;
                if (p.group != watch.group()) {
                    watch.move_to(p.group);
                    std::printf("%s's broadcast moved to %s\n", watch.name().c_str(),
                                proto::lancall_group(p.group).c_str());
                    std::fflush(stdout);
                }
            }
            if (!live) {
                stop_watch(" (the broadcast ended)");
                std::fflush(stdout);
            }
        }

        // 3. clock sync: offsets for the e2e events (per person), rtt for the log
        if (!direct && media.active() && !metrics::time_after(next_clock_poll, now)) {
            std::vector<uint8_t> ids = media.audio_streams();
            for (const auto& [id, rv] : remotes) ids.push_back(id);
            for (uint8_t id : ids) {
                uint32_t off = 0;
                const bool ok = id == 0 ? signaling.clock_offset(off)  // a two-person peer
                                        : signaling.clock_offset(id, off);
                if (ok) offsets[id] = off;
            }
            if (!clock_reported && !offsets.empty()) {
                std::printf("clock sync ok: latency is measured\n");
                std::fflush(stdout);
                clock_reported = true;
            }
            rtt_ms = signaling.rtt_ms();
            if (rtt_ms > 0) metrics::event("rtt_ms", rtt_ms);
            next_clock_poll = now + kClockPollEveryUs;
        }

        // 4. media: received frames -> lip sync per person -> the frame to show now. The same
        // for a call's media and for a broadcast we watch (phase 7): src is either session
        auto play_received = [&](auto& src) {
            video::VideoFrame f;
            while (src.pop_frame(f))
                remotes[f.stream_id].sync.push(std::move(f.image), f.capture_us, now);

            for (auto& [id, rv] : remotes) {
                const std::optional<uint32_t> audio_clock = audio_clock_for(src, id);
                cv::Mat frame;
                uint32_t capture = 0;
                if (!rv.sync.select(audio_clock, now, frame, capture)) continue;
                rv.shown = std::move(frame);
                metrics::event("frame_shown", capture);
                if (audio_clock) {
                    metrics::event("av_sync_ms",
                                   metrics::us_to_ms(metrics::av_offset_us(capture, *audio_clock)));
                }
                if (const auto off = offset_for(id)) {
                    metrics::event("video_e2e_ms",
                                   metrics::us_to_ms(metrics::e2e_us(now, *off, capture)));
                }
            }
            // audio latency, per person heard (a two-person call: the one stream)
            for (uint8_t id : src.audio_streams()) {
                uint32_t a = 0;
                const auto off = offset_for(id);
                if (!off || !src.audio_clock(id, a)) continue;
                const auto it = next_audio_e2e.find(id);
                if (it != next_audio_e2e.end() && metrics::time_after(it->second, now)) continue;
                metrics::event("audio_e2e_ms", metrics::us_to_ms(metrics::e2e_us(now, *off, a)));
                next_audio_e2e[id] = now + kAudioE2eEveryUs;
            }
        };
        if (media.active()) {
            play_received(media);
            if (!media.self_view(self)) self.release();
        } else if (watch.active()) {
            play_received(watch);
            self.release();  // a viewer sends nothing, so there's no camera to show
        } else if (bcast.active()) {
            if (!bcast.self_view(self)) self.release();  // the presenter sees what goes out
        } else {
            self.release();
        }

        // 5. window
        if (have_window) {
            const std::vector<control::Member> members =
                direct ? std::vector<control::Member>{} : signaling.members();
            call::StatusInfo st;
            st.my_name = opt.name;
            st.state =
                direct ? (media.active() ? control::CallState::InCall : control::CallState::Idle)
                       : signaling.state();
            st.peer_name = peer_name;
            st.peer_ip = peer_ip;
            st.media_parts = media.parts();
            st.rtt_ms = rtt_ms;
            st.people = static_cast<int>(members.size());
            if (!direct && signaling.media_mode() == control::MediaMode::Relay)
                st.relay_ip = signaling.relay_ip();
            if (!direct && signaling.media_mode() == control::MediaMode::Multicast)
                st.multicast_group = signaling.multicast_group();
            std::vector<std::string> status = {call::status_line(st)};
            if (bcast.active()) {  // phase 7
                status = {call::broadcast_status(opt.name, bcast.title(), bcast.group_ip())};
            } else if (watch.active()) {
                status = {call::watch_status(opt.name, watch.name(), watch.title())};
            }

            cv::Mat image;
            if (bcast.active()) {
                // phase 7: the presenter sees its own camera, full size: what goes out
                image = call::compose_view(self, cv::Mat(), status);
            } else if (members.size() > 2) {
                // group call: one tile per member in id order, ours shows the camera
                std::vector<call::Tile> tiles;
                for (const control::Member& m : members) {
                    call::Tile t;
                    t.label = call::member_label(m) + (m.me ? " (you)" : "");
                    if (m.me) {
                        t.image = self;
                    } else if (const auto it = remotes.find(m.id); it != remotes.end()) {
                        t.image = it->second.shown;
                    }
                    tiles.push_back(std::move(t));
                }
                image = call::compose_grid(tiles, status);
            } else {
                // two people: the other person full size (whatever stream id their video
                // carries), ourselves in the corner
                cv::Mat other;
                for (const auto& [id, rv] : remotes) {
                    if (!rv.shown.empty()) {
                        other = rv.shown;
                        break;
                    }
                }
                image = call::compose_view(other, self, status);
            }
            cv::imshow(window, image);
            const int key = cv::waitKey(kLoopWaitMs);
            if ((key & 0xFF) == 'q') quit = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(kLoopWaitMs));
        }
    }

    // Leave cleanly: tell the others, stop media, then the control plane.
    if (!direct) {
        const control::CallState s = signaling.state();
        if (s == control::CallState::Ringing) signaling.decline();
        if (s == control::CallState::Calling || s == control::CallState::InCall) {
            signaling.hang_up();
        }
    }
    stop_media();
    stop_broadcast();  // phase 7: clears our beacon's announcement too
    stop_watch("");
    stop_relay();
    if (!direct) {
        discovery.stop();
        signaling.stop();
    }
    if (have_window) cv::destroyAllWindows();
    std::printf("bye. log: %s\n", opt.log_path.c_str());
    return 0;
}
