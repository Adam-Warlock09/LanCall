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
// camera, and the receive ports 5004 / 5006. `bridge <ip>` while broadcasting also sends a
// copy to a lancall_bridge on another floor (net/bridge.hpp), which shows it there.
//
// Self-call: --peer 127.0.0.1 sends to and receives from this same program. Both ends then
// share one clock (clock offset exactly 0), so video_e2e_ms and audio_e2e_ms are exact on one
// laptop. Use headphones, or the speaker feeds back into the mic. Between two laptops the
// clock offset comes from call setup's clock sync (Signaling::clock_offset), per member in a
// group call.
//
// Phase 9: all of the above lives in call::App (call/app.hpp), which lancall_gui drives too.
// This file is the terminal front end: options, typed commands, printing what App says,
// and the OpenCV window. Same texts, same log rows, same 10 ms loop as before.
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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>

#include "audio/app_util.hpp"
#include "call/app.hpp"
#include "call/app_args.hpp"
#include "call/commands.hpp"
#include "call/view.hpp"
#include "common/metrics.hpp"
#include "common/platform.hpp"

namespace {

constexpr int kLoopWaitMs = 10;  // cv::waitKey per pass of the main loop

void usage(const char* prog) {
    std::fprintf(
        stderr,
        "usage: %s --name NAME [--peer IP] [--directory IP] [--relay IP|me] [--log PATH]\n"
        "          [--in N] [--out N] [--camera N] [--buffer-ms MS] [--fps N] [--quality N]\n"
        "          [--fec] [--port N] [--mc-loop]\n",
        prog);
}

// What App said, as lancall has always printed it.
void print(const std::vector<call::Notice>& notices) {
    for (const call::Notice& n : notices) {
        const char* bell = n.kind == call::Notice::Kind::Incoming ? "\a" : "";
        std::printf("%s%s\n", bell, n.text.c_str());
    }
    std::fflush(stdout);
}

}  // namespace

int main(int argc, char** argv) {
    call::AppOptions opt;
    std::vector<std::string> messages;
    const bool args_ok = call::parse_app_args(argc, argv, opt, messages);
    for (const std::string& m : messages) std::fprintf(stderr, "%s\n", m.c_str());
    if (!args_ok) {
        usage(argv[0]);
        return 2;
    }
    if (opt.log_path.empty()) opt.log_path = audio::app::default_log_path("lancall_" + opt.name);

    platform::init();
    audio::app::install_stop_handlers();
    metrics::init(opt.log_path);

    // The window is optional: without a display the app still runs the call from the terminal.
    // Check for a display first: with none, OpenCV's GUI backend can abort the whole process
    // (a GLib error, not a C++ exception), so cv::namedWindow must not even be tried.
    const std::string window = "lancall - " + opt.name;
#if defined(_WIN32)
    // Windows: no DISPLAY variable, and OpenCV's window code doesn't abort without a desktop;
    // if it can't open one it throws, which the catch below handles
    bool have_window = true;
#else
    const char* x11 = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    bool have_window = (x11 != nullptr && *x11 != '\0') || (wayland != nullptr && *wayland != '\0');
#endif
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
    call::App app(opt);
    app.start();
    print(app.take_notices());
    if (!app.direct()) {
        std::printf("type list to see peers, call <name> to call, help for all commands\n");
        std::fflush(stdout);
    }

    platform::LineReader reader;
    bool quit = false;
    while (!quit && !audio::app::g_stop) {
        // 1. typed commands
        std::string line;
        while (reader.next(line)) {
            const call::Command cmd = call::parse_command(line);
            if (cmd.kind == call::CommandKind::Quit) {
                quit = true;
                break;
            }
            app.run(cmd);
            print(app.take_notices());
        }

        // 2-4. call events, chat, keepalive, watch check, clock sync, received frames
        app.tick();
        print(app.take_notices());

        // 5. window
        if (have_window) {
            const call::Screen s = app.screen();
            const cv::Mat image = s.grid ? call::compose_grid(s.tiles, s.status)
                                         : call::compose_view(s.main, s.corner, s.status);
            cv::imshow(window, image);
            const int key = cv::waitKey(kLoopWaitMs);
            if ((key & 0xFF) == 'q') quit = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(kLoopWaitMs));
        }
    }

    // Leave cleanly: tell the others, stop media, then the control plane.
    app.shutdown();
    print(app.take_notices());
    if (have_window) cv::destroyAllWindows();
    std::printf("bye. log: %s\n", opt.log_path.c_str());
    return 0;
}
