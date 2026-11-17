// lancall — the phase 2 call app (Person 2, call/).
//
//   lancall --name Atul                    find peers, call by name, answer calls
//   lancall --name Atul --peer 10.8.66.20  direct mode: media to that address at start-up,
//                                          no discovery or call setup (checkpoint 1)
//   lancall --name Atul --directory 10.8.64.5  also find peers on other floors, through the
//                                          lancall_directory running on that laptop
//
// Type commands in the terminal (help, list, call <name|ip>, accept, decline, hangup, stats,
// quit); an OpenCV window shows the other person with a small self-view. Lip sync: each
// received frame is shown once the audio being heard has reached its capture time
// (call/lip_sync.hpp).
//
// Self-call: --peer 127.0.0.1 sends to and receives from this same program. Both ends then
// share one clock (clock offset exactly 0), so video_e2e_ms and audio_e2e_ms are exact on one
// laptop. Use headphones, or the speaker feeds back into the mic. Between two laptops the
// clock offset comes from call setup's clock sync (Signaling::clock_offset).
//
// Log rows (one CSV per run; metrics::init happens here and nowhere else):
//   frame_shown  value = capture time of the frame shown
//   av_sync_ms   per shown frame: video capture time minus the audio being heard (+ = video ahead)
//   video_e2e_ms per shown frame: capture -> shown (needs the clock offset)
//   audio_e2e_ms 10 per second:  capture -> heard (needs the clock offset)
//   rtt_ms       once per second in a call: round trip of the clock-sync sample in use
//   call_started / call_ended / call_incoming, plus everything the media objects log.
//
// Options:
//   --name NAME      required: letters, digits, '-' and '_', at most 32 characters
//   --peer IP        direct mode (see above); 127.0.0.1 for a self-call
//   --directory IP   laptop running lancall_directory, for peers on other floors (phase 3)
//   --log PATH       CSV log (default logs/lancall_<name>_<date>_<time>.csv)
//   --in N / --out N PortAudio input / output device (default: system default)
//   --camera N       camera index (default 0)
//   --buffer-ms MS   audio playout buffer, 20-600 (default 60)
//   --fps N          video frame rate, 1-30 (default 15)
//   --quality N      JPEG quality, 1-100 (default 70)

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include "audio/app_util.hpp"
#include "call/call_logic.hpp"
#include "call/commands.hpp"
#include "call/lip_sync.hpp"
#include "call/media_session.hpp"
#include "call/view.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/signaling.hpp"
#include "metrics/latency.hpp"

namespace {

constexpr uint32_t kAudioE2eEveryUs = 100000;    // audio_e2e_ms: 10 rows per second
constexpr uint32_t kClockPollEveryUs = 1000000;  // read clock offset + log rtt_ms once a second
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
    call::MediaOptions media;
};

void usage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s --name NAME [--peer IP] [--directory IP] [--log PATH] [--in N] [--out N]\n"
        "          [--camera N] [--buffer-ms MS] [--fps N] [--quality N]\n",
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
    if (!o.directory.empty() && !o.peer.empty()) {
        std::fprintf(stderr,
                     "--directory has no effect with --peer (direct mode has no discovery)\n");
    }
    return true;
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
        signaling_ok = signaling.start(opt.name, proto::kSignalPort);
        if (!signaling_ok) std::printf("  call setup unavailable: %s\n", signaling.error().c_str());
        discovery_ok = discovery.start(opt.name, proto::kSignalPort, opt.directory);
        if (!discovery_ok) std::printf("  discovery unavailable: %s\n", discovery.error().c_str());
        if (discovery_ok && !opt.directory.empty())
            std::printf("  directory %s: peers on other floors show up in list\n",
                        opt.directory.c_str());
    }

    // Clock offset between the laptops (peer minus ours, mod 2^32) for the *_e2e_ms events.
    // A self-call shares one clock; otherwise it comes from Signaling's clock sync.
    const bool self_call = opt.peer == "127.0.0.1";
    std::optional<uint32_t> clock_offset;
    if (self_call) clock_offset = 0;
    double rtt_ms = 0;

    call::MediaSession media;
    call::LipSync<cv::Mat> sync;
    cv::Mat shown, self;
    std::string peer_name, peer_ip = opt.peer;

    auto start_media = [&](const std::string& ip) {
        sync.clear();
        shown.release();
        const auto failed = media.start(ip, opt.media);
        for (const auto& f : failed) std::printf("  not started: %s\n", f.c_str());
        std::printf("in call with %s (%s)%s\n", (peer_name.empty() ? ip : peer_name).c_str(),
                    media.parts().c_str(), self_call ? ", same clock: latency is measured" : "");
        metrics::event("call_started", 0);
    };
    auto stop_media = [&] {
        if (!media.active()) return;
        media.stop();
        sync.clear();
        shown.release();
        metrics::event("call_ended", 0);
        std::printf("call ended\n");
        for (const auto& line : media.summary()) std::printf("  %s\n", line.c_str());
        if (!self_call) clock_offset.reset();
        rtt_ms = 0;
    };

    if (direct) {
        start_media(opt.peer);
    } else {
        std::printf("type list to see peers, call <name> to call, help for all commands\n");
    }
    std::fflush(stdout);

    CommandReader reader;
    bool quit = false;
    uint32_t next_audio_e2e = proto::now_us();
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
                    if (!media.active()) std::printf("no call\n");
                    for (const auto& s : media.stats()) std::printf("  %s\n", s.c_str());
                    const auto& c = sync.counters();
                    std::printf(
                        "  lip sync: shown %llu  skipped %llu  waited-too-long %llu  "
                        "no-audio %llu  queue-full %llu\n",
                        static_cast<unsigned long long>(c.shown),
                        static_cast<unsigned long long>(c.skipped),
                        static_cast<unsigned long long>(c.forced),
                        static_cast<unsigned long long>(c.no_audio),
                        static_cast<unsigned long long>(c.dropped_full));
                    if (rtt_ms > 0) std::printf("  clock sync: rtt %.2f ms\n", rtt_ms);
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
                    if (!signaling_ok) {
                        std::printf("call setup unavailable: %s\n", signaling.error().c_str());
                        break;
                    }
                    if (state != control::CallState::Idle) {
                        std::printf("busy: hang up (or decline) first\n");
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
                case call::CommandKind::Accept:
                case call::CommandKind::Decline:
                    if (direct || state != control::CallState::Ringing) {
                        std::printf("nobody is calling\n");
                    } else if (cmd.kind == call::CommandKind::Accept) {
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
                    metrics::event("call_incoming", 0);
                    break;
                case control::CallEvent::Kind::Accepted:
                    peer_name = ev.peer_name;
                    peer_ip = ev.peer_ip;
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
                case control::CallEvent::Kind::Joined:
                case control::CallEvent::Kind::Left:
                case control::CallEvent::Kind::MediaChanged:
                    break;  // TODO phase 4 (Person 2): group calls
            }
            std::fflush(stdout);
        }

        const uint32_t now = proto::now_us();

        // 3. clock sync: offset for the e2e events, rtt for the log
        if (!direct && media.active() && !metrics::time_after(next_clock_poll, now)) {
            uint32_t off = 0;
            if (signaling.clock_offset(off)) {
                if (!clock_offset) std::printf("clock sync ok: latency is measured\n");
                clock_offset = off;
            }
            rtt_ms = signaling.rtt_ms();
            if (rtt_ms > 0) metrics::event("rtt_ms", rtt_ms);
            next_clock_poll = now + kClockPollEveryUs;
        }

        // 4. media: received frames -> lip sync -> the frame to show now
        if (media.active()) {
            video::VideoFrame f;
            while (media.pop_frame(f)) sync.push(std::move(f.image), f.capture_us, now);

            std::optional<uint32_t> audio_clock;
            uint32_t a = 0;
            if (media.audio_clock(a)) audio_clock = a;

            cv::Mat frame;
            uint32_t capture = 0;
            if (sync.select(audio_clock, now, frame, capture)) {
                shown = std::move(frame);
                metrics::event("frame_shown", capture);
                if (audio_clock) {
                    metrics::event("av_sync_ms",
                                   metrics::us_to_ms(metrics::av_offset_us(capture, *audio_clock)));
                }
                if (clock_offset) {
                    metrics::event("video_e2e_ms",
                                   metrics::us_to_ms(metrics::e2e_us(now, *clock_offset, capture)));
                }
            }
            if (audio_clock && clock_offset && !metrics::time_after(next_audio_e2e, now)) {
                metrics::event("audio_e2e_ms", metrics::us_to_ms(metrics::e2e_us(now, *clock_offset,
                                                                                 *audio_clock)));
                next_audio_e2e = now + kAudioE2eEveryUs;
            }
            if (!media.self_view(self)) self.release();
        } else {
            self.release();
        }

        // 5. window
        if (have_window) {
            call::StatusInfo st;
            st.my_name = opt.name;
            st.state =
                direct ? (media.active() ? control::CallState::InCall : control::CallState::Idle)
                       : signaling.state();
            st.peer_name = peer_name;
            st.peer_ip = peer_ip;
            st.media_parts = media.parts();
            st.rtt_ms = rtt_ms;
            cv::imshow(window, call::compose_view(shown, self, {call::status_line(st)}));
            const int key = cv::waitKey(kLoopWaitMs);
            if ((key & 0xFF) == 'q') quit = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(kLoopWaitMs));
        }
    }

    // Leave cleanly: tell the peer, stop media, then the control plane.
    if (!direct) {
        const control::CallState s = signaling.state();
        if (s == control::CallState::Ringing) signaling.decline();
        if (s == control::CallState::Calling || s == control::CallState::InCall) {
            signaling.hang_up();
        }
    }
    stop_media();
    if (!direct) {
        discovery.stop();
        signaling.stop();
    }
    if (have_window) cv::destroyAllWindows();
    std::printf("bye. log: %s\n", opt.log_path.c_str());
    return 0;
}