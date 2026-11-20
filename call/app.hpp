#pragma once
// App — everything lancall does, for any front end (Person 2, call/, phase 9).
//
// Until phase 9 all of this lived inside lancall's main(). Now the terminal lancall
// (call/main.cpp) and the window app lancall_gui (gui/) both drive one App:
//
//   App app(options);
//   app.start();                         // control plane (or direct mode's media)
//   loop:
//       app.call("Aric"), app.text(...)  // whatever the person asked for
//       app.tick();                      // events, chat, clock sync, received frames
//       for (auto& n : app.take_notices()) show n.text
//       draw app.screen()
//   app.shutdown();
//
// Every action checks the same things and says the same things as lancall always has: the
// texts in the notices are exactly the lines terminal lancall prints (the experiment
// scripts read that output). Lip sync, clock sync and every log row live in here, so both
// front ends measure the same way.
//
// Use from one thread only (the front end's main loop). Nothing here blocks for long.

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "call/broadcast.hpp"
#include "call/call_logic.hpp"
#include "call/commands.hpp"
#include "call/lip_sync.hpp"
#include "call/media_session.hpp"
#include "call/ui_logic.hpp"
#include "call/view.hpp"
#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/signaling.hpp"

class UdpSocket;
namespace net {
class Relay;
}

namespace call {

// What lancall's command line sets (call/main.cpp parses it).
struct AppOptions {
    std::string name;       // required: letters, digits, '-' and '_'
    std::string peer;       // direct mode: media to this address at start, no call setup
    std::string directory;  // lancall_directory's laptop, for peers on other floors
    std::string relay;      // calls we start: "" = mesh, "me" or a relay's IPv4 address
    std::string log_path;   // shown to the person; metrics::init() is the front end's job
    MediaOptions media;
    int port = proto::kSignalPort;  // our signaling port (a second copy on one laptop)
    bool mc_loop = false;           // hear our own multicast (one-laptop tests)
};

// One message for the person. text is what terminal lancall prints (without the newline);
// a text can span several lines.
struct Notice {
    enum class Kind {
        Info,      // everything else
        Chat,      // "[chat] ..." lines: messages in and out, and ones not delivered
        Incoming,  // someone is calling (the terminal rings the bell)
    };
    Kind kind = Kind::Info;
    std::string text;
};

// What the window shows right now. Two layouts: one big picture with an optional small
// self-view in the corner (two people, a broadcast, watching one), or a grid of named
// tiles (a group call of 3+).
struct Screen {
    std::vector<std::string> status;  // the status line(s) at the top
    bool grid = false;
    cv::Mat main;             // !grid: the big picture (empty = "waiting for video")
    cv::Mat corner;           // !grid: our camera in the corner (empty = none)
    std::vector<Tile> tiles;  // grid: one per member in id order, ours labelled "(you)"
    // phase 9: which picture is our own camera (the GUI refreshes those at the camera's
    // rate; the corner is always ours)
    bool main_is_self = false;  // main is our camera (we broadcast)
    int self_tile = -1;         // grid: index of our tile, -1 = none
};

class App {
public:
    explicit App(AppOptions opt);
    ~App();  // shutdown()
    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Starts discovery and signaling, or in direct mode the media to opt.peer. What fails
    // is said in notices and isn't fatal: whatever did start still works.
    void start();
    // Leaves cleanly: declines or hangs up, stops media, broadcast, watching, the relay,
    // then the control plane. Safe to call twice.
    void shutdown();

    // One pass of the main loop: call events, chat, the relay keepalive, whether the
    // broadcast we watch is still on, clock sync, and received frames through lip sync.
    // Call it often (at least every ~10-20 ms): frames are shown when it picks them.
    void tick();

    // ---- actions (the typed commands; each says in a notice what happened or why not) ----
    void call(const std::string& who);  // name or IP
    void add(const std::string& who);   // the host rings one more person in
    void accept();
    void decline();
    void hang_up();
    void relay(const std::string& arg);  // an IP or "me"; when idle: for calls we start
    void mesh();
    void multicast();
    void text(const std::string& to, const std::string& message);  // to = "all", name or IP
    void broadcast(const std::string& title);  // again while on: changes the title
    void unbroadcast();
    void watch(const std::string& who);
    void unwatch();
    void bridge(const std::string& ip);
    void unbridge(const std::string& ip);
    // Phase 9: call or add exactly this peer (its IP and signaling port), for a list the
    // person clicks in. A name can't tell apart two copies on one laptop with the same name.
    void call_peer(const control::Peer& p);
    void add_peer(const control::Peer& p);

    // A typed command (the terminal). Also does help, list, stats and members, which only
    // print. Quit is the front end's to handle; here it does nothing.
    void run(const Command& cmd);

    // Everything said since the last call, oldest first.
    std::vector<Notice> take_notices();

    // ---- what the front end shows ----------------------------------------------------
    const AppOptions& options() const { return opt_; }
    bool direct() const { return direct_; }
    control::CallState state() const;
    bool is_host() const;
    std::vector<control::Peer> peers() const;      // empty without discovery
    std::vector<control::Member> members() const;  // empty outside a group-capable call
    control::MediaMode media_mode() const;
    std::string media_address() const;  // the relay's IP or the multicast group, else ""
    std::string safety_code(uint8_t member_id) const;
    bool in_call() const { return media_.active(); }
    bool encrypted() const { return media_.encrypted(); }
    bool broadcasting() const { return bcast_.active(); }
    bool watching() const { return watch_.active(); }
    std::vector<std::string> stats() const;  // what `stats` prints, one line each
    Screen screen() const;

    // phase 9, for the GUI's panels
    const std::string& peer_name() const { return peer_name_; }  // who we call / talk to
    const std::string& peer_ip() const { return peer_ip_; }
    double rtt_ms() const { return rtt_ms_; }  // clock-sync round trip, 0 = unknown
    std::string media_parts() const { return media_.parts(); }         // "audio+video", ...
    const std::string& relay_choice() const { return relay_choice_; }  // for calls we start
    bool multicast_choice() const { return multicast_choice_; }        // for calls we start
    // why the control plane (or part of it) isn't running; empty = all fine or direct mode
    std::string control_error() const;
    const std::string& watching_name() const { return watch_.name(); }
    const std::string& watching_title() const { return watch_.title(); }
    const std::string& broadcast_title() const { return bcast_.title(); }
    const std::vector<std::string>& bridges() const { return bcast_.bridges(); }
    // the lancall_bridge line to run on the laptop at ip (also in bridge()'s notice)
    std::string bridge_command(const std::string& ip) const;

    // phase 9.5: the chat tab, the settings tab
    static constexpr size_t kMaxChat = 300;
    const std::deque<ChatLine>& chat() const { return chat_; }  // the last kMaxChat, oldest first
    std::string own_ip() const;  // our address as discovery learned it ("" = not yet)
    // Devices, camera, buffer, fps, quality, FEC for the next call, broadcast or watch (a
    // running one keeps what it started with). Check them first: media_options_problem().
    void set_media_options(const MediaOptions& media);
    // Another directory laptop ("" = none): restarts discovery, so only when we're idle (not
    // in a call, broadcasting or watching). false + a notice if not possible.
    bool set_directory(const std::string& ip);

private:
    // what we keep for each person whose video we receive (key: their stream id)
    struct RemoteView {
        LipSync<cv::Mat> sync;
        cv::Mat shown;
    };

    AppOptions opt_;
    bool direct_ = false;
    bool self_call_ = false;
    bool started_ = false;

    control::Discovery discovery_;
    control::Signaling signaling_;
    bool discovery_ok_ = false, signaling_ok_ = false;

    // clock offsets to the other laptops (theirs minus ours), per stream id
    std::map<uint8_t, uint32_t> offsets_;
    bool clock_reported_ = false;
    double rtt_ms_ = 0;
    bool same_clock_watch_ = false;  // watching a broadcast from this same laptop
    uint32_t next_clock_poll_ = 0;

    MediaSession media_;
    std::map<uint8_t, RemoteView> remotes_;       // per person we receive video from
    std::map<uint8_t, uint32_t> next_audio_e2e_;  // per person we hear
    cv::Mat self_;
    std::string peer_name_, peer_ip_;
    std::string relay_choice_;           // for calls we start; changed by relay/mesh when idle
    bool multicast_choice_ = false;      // calls we start use multicast
    uint8_t call_group_ = 0;             // the multicast group of the call we host, 0 = none
    uint8_t announced_group_ = 0;        // what our beacon says about it (host only)
    std::unique_ptr<net::Relay> relay_;  // when the relay is this laptop

    // relay mode: "still here" to the relay every 2 s
    std::unique_ptr<UdpSocket> keepalive_sock_;
    uint16_t keepalive_seq_ = 0;
    uint32_t next_keepalive_ = 0;

    // broadcasts: at most one of a call, a broadcast of ours and one we watch at any time
    BroadcastSession bcast_;
    WatchSession watch_;
    uint32_t next_watch_check_ = 0;

    std::vector<Notice> notices_;
    std::deque<ChatLine> chat_;
    uint64_t chat_seq_ = 0;
    void add_chat(ChatLine line);

    void say(const char* fmt, ...)
#if defined(__MINGW32__)
        __attribute__((format(gnu_printf, 2, 3)))  // see audio_pipeline.cpp's fmt()
#elif defined(__GNUC__)
        __attribute__((format(printf, 2, 3)))
#endif
        ;
    void say_as(Notice::Kind kind, const char* fmt, ...)
#if defined(__MINGW32__)
        __attribute__((format(gnu_printf, 3, 4)))  // see audio_pipeline.cpp's fmt()
#elif defined(__GNUC__)
        __attribute__((format(printf, 3, 4)))
#endif
        ;
    void say_lines(const std::string& block);  // one notice per line of block

    std::optional<uint32_t> offset_for(uint8_t id) const;
    template <class Source>
    std::optional<uint32_t> audio_clock_for(const Source& src, uint8_t id) const;
    template <class Source>
    void play_received(Source& src, uint32_t now);

    bool ensure_relay();
    void stop_relay();
    Route current_route() const;
    void sync_call_announce();
    void apply_route();
    bool use_relay(const std::string& arg);
    bool use_multicast();
    void keep_everyone_reachable();
    void start_media(const std::string& ip);
    void stop_media();
    void call_to(const std::string& who, const control::Peer* peer);
    void add_to(const std::string& who, const control::Peer* peer);
    bool busy_with_broadcast();
    bool stop_broadcast();
    void start_watch(const BroadcastTarget& t);
    void stop_watch(const std::string& why);

    void handle_call_events();
    void handle_chat();
    void send_keepalive(uint32_t now);
    void check_watch(uint32_t now);
    void poll_clock(uint32_t now);
};

}  // namespace call
