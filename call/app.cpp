// App — everything lancall does, for any front end (Person 2, call/, phase 9). See app.hpp.
//
// Moved here from lancall's main() as it was in phase 7: the same checks, the same texts
// (printf became say(), with the same format strings), the same log rows in the same order.
#include "call/app.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <exception>
#include <utility>

#include "audio/app_util.hpp"
#include "common/metrics.hpp"
#include "common/platform.hpp"
#include "common/udp_socket.hpp"
#include "control/discovery_detail.hpp"
#include "metrics/latency.hpp"
#include "net/bridge.hpp"
#include "net/relay.hpp"

namespace call {

namespace {

constexpr uint32_t kAudioE2eEveryUs = 100000;     // audio_e2e_ms: 10 rows per second per person
constexpr uint32_t kClockPollEveryUs = 1000000;   // read clock offsets + log rtt_ms once a second
constexpr uint32_t kWatchCheckEveryUs = 1000000;  // is the broadcast we watch still on?

std::string vformat(const char* fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    const int n = std::vsnprintf(nullptr, 0, fmt, copy);
    va_end(copy);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n) + 1, '\0');
    std::vsnprintf(out.data(), out.size(), fmt, args);
    out.resize(static_cast<size_t>(n));
    return out;
}

std::string format(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::string out = vformat(fmt, args);
    va_end(args);
    return out;
}

unsigned long long ull(uint64_t v) {
    return static_cast<unsigned long long>(v);
}

// "Aric, Rishika" for a list of members
std::string names_of(const std::vector<control::Member>& members) {
    std::string names;
    for (const auto& m : members) names += (names.empty() ? "" : ", ") + member_label(m);
    return names;
}

// the exact peer as a call target (as resolve_target makes it for a known peer)
Target peer_target(const control::Peer& p) {
    Target t;
    t.name = p.name;
    t.ip = p.ip;
    t.port = p.signal_port != 0 ? p.signal_port : proto::kSignalPort;
    return t;
}

}  // namespace

App::App(AppOptions opt) : opt_(std::move(opt)) {
    direct_ = !opt_.peer.empty();
    self_call_ = opt_.peer == "127.0.0.1";
    peer_ip_ = opt_.peer;
    relay_choice_ = opt_.relay;
    const uint32_t now = proto::now_us();
    next_clock_poll_ = now;
    next_keepalive_ = now;
    next_watch_check_ = now;
}

App::~App() {
    shutdown();
}

// ---- messages --------------------------------------------------------------------------

void App::say(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    notices_.push_back({Notice::Kind::Info, vformat(fmt, args)});
    va_end(args);
}

void App::say_as(Notice::Kind kind, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    notices_.push_back({kind, vformat(fmt, args)});
    va_end(args);
}

void App::say_lines(const std::string& block) {
    size_t start = 0;
    while (start < block.size()) {
        size_t nl = block.find('\n', start);
        if (nl == std::string::npos) nl = block.size();
        notices_.push_back({Notice::Kind::Info, block.substr(start, nl - start)});
        start = nl + 1;
    }
}

std::vector<Notice> App::take_notices() {
    std::vector<Notice> out;
    out.swap(notices_);
    return out;
}

// ---- start and stop --------------------------------------------------------------------

void App::start() {
    if (started_) return;
    started_ = true;
    // Control plane (not in direct mode). Failing to start it is not fatal: direct mode and
    // calls by IP still work as far as the parts that did start allow.
    if (!direct_) {
        signaling_ok_ = signaling_.start(opt_.name, static_cast<uint16_t>(opt_.port));
        if (!signaling_ok_) say("  call setup unavailable: %s", signaling_.error().c_str());
        discovery_ok_ =
            discovery_.start(opt_.name, static_cast<uint16_t>(opt_.port), opt_.directory);
        if (!discovery_ok_) say("  discovery unavailable: %s", discovery_.error().c_str());
        if (discovery_ok_ && !opt_.directory.empty())
            say("  directory %s: peers on other floors show up in list", opt_.directory.c_str());
        if (!opt_.relay.empty())
            say("  calls you start use the relay %s",
                opt_.relay == "me" ? "on this laptop" : opt_.relay.c_str());
    } else {
        start_media(opt_.peer);
    }
}

void App::shutdown() {
    if (!started_) return;
    started_ = false;
    // Leave cleanly: tell the others, stop media, then the control plane.
    if (!direct_) {
        const control::CallState s = signaling_.state();
        if (s == control::CallState::Ringing) signaling_.decline();
        if (s == control::CallState::Calling || s == control::CallState::InCall) {
            signaling_.hang_up();
        }
    }
    stop_media();
    stop_broadcast();  // phase 7: clears our beacon's announcement too
    stop_watch("");
    stop_relay();
    if (!direct_) {
        discovery_.stop();
        signaling_.stop();
    }
}

// ---- clocks ----------------------------------------------------------------------------

// Clock offsets to the other laptops (theirs minus ours, mod 2^32) for the *_e2e_ms events,
// per stream_id. A self-call shares one clock; otherwise they come from Signaling's clock
// sync (stream 0 = a two-person peer without a member id).
std::optional<uint32_t> App::offset_for(uint8_t id) const {
    if (self_call_ || same_clock_watch_) return 0u;
    const auto it = offsets_.find(id);
    if (it == offsets_.end()) return std::nullopt;
    return it->second;
}

// The audio clock to lip-sync a stream's video with: that stream's own, or, while only one
// person is heard, the overall one (also covers peers and video receivers that don't set
// stream ids yet, where video arrives as stream 0). src: the call's MediaSession, or the
// WatchSession of a broadcast (phase 7); both offer the same calls.
template <class Source>
std::optional<uint32_t> App::audio_clock_for(const Source& src, uint8_t id) const {
    uint32_t a = 0;
    if (src.audio_clock(id, a)) return a;
    if (src.audio_streams().size() <= 1 && src.audio_clock(a)) return a;
    return std::nullopt;
}

// Received frames -> lip sync per person -> the frame to show now. The same for a call's
// media and for a broadcast we watch (phase 7): src is either session.
template <class Source>
void App::play_received(Source& src, uint32_t now) {
    video::VideoFrame f;
    while (src.pop_frame(f)) remotes_[f.stream_id].sync.push(std::move(f.image), f.capture_us, now);

    for (auto& [id, rv] : remotes_) {
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
            metrics::event("video_e2e_ms", metrics::us_to_ms(metrics::e2e_us(now, *off, capture)));
        }
    }
    // audio latency, per person heard (a two-person call: the one stream)
    for (uint8_t id : src.audio_streams()) {
        uint32_t a = 0;
        const auto off = offset_for(id);
        if (!off || !src.audio_clock(id, a)) continue;
        const auto it = next_audio_e2e_.find(id);
        if (it != next_audio_e2e_.end() && metrics::time_after(it->second, now)) continue;
        metrics::event("audio_e2e_ms", metrics::us_to_ms(metrics::e2e_us(now, *off, a)));
        next_audio_e2e_[id] = now + kAudioE2eEveryUs;
    }
}

// ---- routes: mesh, relay, multicast (phase 4, phase 7) ---------------------------------

bool App::ensure_relay() {
    if (relay_ && relay_->running()) return true;
    relay_ = std::make_unique<net::Relay>();
    if (!relay_->start()) {
        say("cannot run the relay on this laptop: %s", relay_->error().c_str());
        relay_.reset();
        return false;
    }
    say("relay running on this laptop (UDP port %u)", proto::kRelayPort);
    return true;
}

void App::stop_relay() {
    if (!relay_) return;
    relay_->stop();
    say("relay stopped: %s", relay_->stats().c_str());
    relay_.reset();
}

// Where media goes right now, from the room as signaling knows it.
Route App::current_route() const {
    if (direct_) return direct_route(opt_.peer);
    Route r;
    r.audio_to = signaling_.media_destinations(proto::kAudioPort);
    r.video_to = signaling_.media_destinations(proto::kVideoPort);
    r.sources = media_sources(signaling_.members(), signaling_.media_mode(), signaling_.relay_ip());
    return r;
}

// Phase 7: while we host a call in multicast mode, our beacon says which group it uses
// (mc=, no title: not a broadcast), so others on the floor pick another one.
void App::sync_call_announce() {
    const bool want = !direct_ && signaling_.is_host() &&
                      signaling_.media_mode() == control::MediaMode::Multicast && call_group_ != 0;
    if (want && announced_group_ != call_group_) {
        discovery_.announce(call_group_, "");
        announced_group_ = call_group_;
    } else if (!want && announced_group_ != 0) {
        discovery_.clear_announce();
        announced_group_ = 0;
    }
}

// After every room change. The relay runs here when its address is ours. Phase 7: in
// multicast mode the receivers join the call's group before the senders switch to it, and
// leave it only after the senders have switched away.
void App::apply_route() {
    if (direct_ || !media_.active()) return;
    const control::MediaMode mode = signaling_.media_mode();
    const bool relay_here =
        mode == control::MediaMode::Relay && platform::is_local_address(signaling_.relay_ip());
    if (relay_here) ensure_relay();
    const std::string group =
        mode == control::MediaMode::Multicast ? signaling_.multicast_group() : std::string();
    if (!group.empty() && !media_.join_group(group, opt_.media.multicast_if))
        say("can't join the call's multicast group %s: %s", group.c_str(), media_.error().c_str());
    media_.update_route(current_route());
    if (!relay_here) stop_relay();  // after the switch, so nothing in flight is cut off
    if (group.empty() && !media_.group().empty()) media_.leave_group();
    sync_call_announce();
}

// Host: send everyone's media through `arg` (an ip, or "me" = this laptop).
bool App::use_relay(const std::string& arg) {
    std::string ip = arg;
    if (arg == "me") {
        ip.clear();
        for (const control::Member& m : signaling_.members()) {
            if (!m.me && !m.ip.empty()) {
                ip = platform::local_address_towards(m.ip, proto::kSignalPort);
                break;
            }
        }
        if (ip.empty()) {
            say("cannot tell this laptop's address yet (nobody else in the call)");
            return false;
        }
    }
    if (platform::is_local_address(ip) && !ensure_relay()) return false;
    signaling_.set_media(control::MediaMode::Relay, ip);
    say("media now goes through the relay at %s%s", ip.c_str(),
        platform::is_local_address(ip) ? " (this laptop)" : "");
    metrics::event("media_relay", 1);
    apply_route();
    return true;
}

// Phase 7, host: everyone sends one copy to a multicast group. Only when discovery hears
// every member on our floor, since multicast stops at the floor router.
bool App::use_multicast() {
    if (!discovery_ok_) {
        say("multicast needs discovery, to know who's on this floor: %s",
            discovery_.error().c_str());
        return false;
    }
    const auto off_floor = off_floor_members(signaling_.members(), discovery_.peers());
    if (!off_floor.empty()) {
        say("can't use multicast: %s %s not on this floor (multicast stops at the floor "
            "router)",
            names_of(off_floor).c_str(), off_floor.size() == 1 ? "is" : "are");
        return false;
    }
    const uint8_t group = call_group_ != 0 ? call_group_ : discovery_.pick_group();
    if (group == 0) {
        say("no free multicast group on this floor (all 254 in use)");
        return false;
    }
    call_group_ = group;
    signaling_.set_media(control::MediaMode::Multicast, proto::lancall_group(group));
    say("media now goes to multicast group %s (one copy each, this floor only)",
        proto::lancall_group(group).c_str());
    metrics::event("media_multicast", group);
    apply_route();
    return true;
}

// Phase 7, host: someone joined a multicast call from where multicast can't reach them.
// Switch the call to a relay on this laptop (or to mesh if that can't run here).
void App::keep_everyone_reachable() {
    if (!signaling_.is_host() || signaling_.media_mode() != control::MediaMode::Multicast) return;
    const auto off_floor = off_floor_members(signaling_.members(), discovery_.peers());
    if (off_floor.empty()) return;
    say("%s %s not on this floor, so multicast can't reach them: switching to a relay on this "
        "laptop",
        names_of(off_floor).c_str(), off_floor.size() == 1 ? "is" : "are");
    if (use_relay("me")) return;
    signaling_.set_media(control::MediaMode::Mesh, "");
    say("no relay here either: switching to mesh");
    metrics::event("media_relay", 0);
    apply_route();
}

// ---- media of a call -------------------------------------------------------------------

void App::start_media(const std::string& ip) {
    remotes_.clear();
    next_audio_e2e_.clear();
    offsets_.clear();
    clock_reported_ = false;
    next_keepalive_ = proto::now_us();  // the first one goes out as soon as relay mode is on
    const uint8_t my_id = direct_ ? 0 : signaling_.my_id();
    if (!direct_ && signaling_.media_mode() == control::MediaMode::Relay &&
        platform::is_local_address(signaling_.relay_ip()))
        ensure_relay();
    // phase 7: the card group packets leave by is fixed now, even if the call goes multicast
    // later
    opt_.media.multicast_if = discovery_ok_ ? discovery_.own_ip() : std::string();
    opt_.media.multicast_loop = opt_.mc_loop;
    const auto failed = media_.start(current_route(), opt_.media, my_id);
    for (const auto& f : failed) say("  not started: %s", f.c_str());
    // phase 6: everything goes out encrypted with the key the host made for this call
    std::array<uint8_t, control::kMediaKeyBytes> key{};
    if (!direct_ && signaling_.media_key(key)) {
        proto::MediaCrypto::Key k;
        std::copy(key.begin(), key.end(), k.begin());
        media_.set_crypto(std::make_shared<const proto::MediaCrypto>(k));
        key.fill(0);
        k.fill(0);
    }
    const std::string who = peer_name_.empty() ? ip : peer_name_;
    say("in call with %s (%s%s)%s", who.c_str(), media_.parts().c_str(),
        media_.encrypted() ? ", encrypted" : ", NOT encrypted",
        self_call_ ? ", same clock: latency is measured" : "");
    if (!direct_) {
        // the host has a code per member, a member one with the host
        const uint8_t other = signaling_.is_host() ? 2 : 1;
        const std::string code = signaling_.safety_code(other);
        if (!code.empty())
            say("  safety code with %s: %s (compare it with them: different = someone in "
                "between)",
                who.c_str(), code.c_str());
    }
    metrics::event("call_started", 0);
    apply_route();  // phase 7: joins the group if the call starts in multicast mode
}

void App::stop_media() {
    if (!media_.active()) return;
    media_.stop();
    remotes_.clear();
    next_audio_e2e_.clear();
    offsets_.clear();
    stop_relay();
    call_group_ = 0;
    sync_call_announce();  // phase 7: our beacon no longer names the call's group
    metrics::event("call_ended", 0);
    say("call ended");
    for (const auto& line : media_.summary()) say("  %s", line.c_str());
    rtt_ms_ = 0;
}

// ---- phase 7: broadcasts (call/broadcast.hpp) ------------------------------------------

// A call can't start (or be answered) while we broadcast or watch: says why and says so.
bool App::busy_with_broadcast() {
    if (bcast_.active()) {
        say("you're broadcasting: unbroadcast first");
        return true;
    }
    if (watch_.active()) {
        say("you're watching %s: unwatch first", watch_.name().c_str());
        return true;
    }
    return false;
}

bool App::stop_broadcast() {
    if (!bcast_.active()) return false;
    discovery_.clear_announce();
    bcast_.stop();
    metrics::event("broadcast_ended", 0);
    say("broadcast ended");
    for (const auto& line : bcast_.summary()) say("  %s", line.c_str());
    return true;
}

void App::start_watch(const BroadcastTarget& t) {
    remotes_.clear();
    next_audio_e2e_.clear();
    WatchOptions wo;
    wo.out_device = opt_.media.out_device;
    wo.buffer_ms = opt_.media.buffer_ms;
    wo.iface_ip = discovery_.own_ip();
    for (const auto& f : watch_.start(t.name, t.title, t.ip, t.group, wo))
        say("  not started: %s", f.c_str());
    if (!watch_.active()) {
        say("could not watch %s", t.name.c_str());
        return;
    }
    // the presenter on this same laptop (one-laptop test): one clock, so latency is exact
    same_clock_watch_ = platform::is_local_address(t.ip);
    say("watching %s: \"%s\" (%s, %s)%s", t.name.c_str(), t.title.c_str(), watch_.parts().c_str(),
        proto::lancall_group(t.group).c_str(),
        same_clock_watch_ ? ", same laptop: latency is measured" : "");
    metrics::event("watch_started", t.group);
    next_watch_check_ = proto::now_us() + kWatchCheckEveryUs;
}

void App::stop_watch(const std::string& why) {
    if (!watch_.active()) return;
    const std::string who = watch_.name();
    watch_.stop();
    remotes_.clear();
    next_audio_e2e_.clear();
    same_clock_watch_ = false;
    metrics::event("watch_ended", 0);
    say("stopped watching %s%s", who.c_str(), why.c_str());
    for (const auto& line : watch_.summary()) say("  %s", line.c_str());
}

// ---- actions -----------------------------------------------------------------------------

void App::call(const std::string& who) {
    call_to(who, nullptr);
}

void App::call_peer(const control::Peer& p) {
    call_to(p.name, &p);
}

// who: what was typed; peer: the exact peer instead (a click in the GUI's list)
void App::call_to(const std::string& who, const control::Peer* peer) {
    if (direct_) {
        say("direct mode (--peer): the call is set up at start-up");
        return;
    }
    if (busy_with_broadcast()) return;  // phase 7
    if (!signaling_ok_) {
        say("call setup unavailable: %s", signaling_.error().c_str());
        return;
    }
    const control::CallState s = state();
    if (s != control::CallState::Idle) {
        say("busy: hang up (or decline) first%s",
            s == control::CallState::InCall ? "; to bring someone into this call, use add" : "");
        return;
    }
    const Target t = peer != nullptr
                         ? peer_target(*peer)
                         : resolve_target(who, discovery_ok_ ? discovery_.peers()
                                                             : std::vector<control::Peer>{});
    if (!t.ok()) {
        say("%s", t.error.c_str());
        return;
    }
    if (!signaling_.call(t.ip, t.port)) {
        say("cannot call %s: %s", t.ip.c_str(), signaling_.error().c_str());
        return;
    }
    peer_name_ = t.name;
    peer_ip_ = t.ip;
    say("calling %s (%s)...", (t.name.empty() ? t.ip : t.name).c_str(), t.ip.c_str());
}

void App::add(const std::string& who) {
    add_to(who, nullptr);
}

void App::add_peer(const control::Peer& p) {
    add_to(p.name, &p);
}

void App::add_to(const std::string& who, const control::Peer* peer) {
    if (direct_) {
        say("direct mode (--peer): no group calls");
        return;
    }
    if (busy_with_broadcast()) return;  // phase 7
    if (!signaling_ok_) {
        say("call setup unavailable: %s", signaling_.error().c_str());
        return;
    }
    const control::CallState s = state();
    if (s == control::CallState::Calling || s == control::CallState::Ringing) {
        say("busy: wait until this call is set up");
        return;
    }
    if (s == control::CallState::InCall && !signaling_.is_host()) {
        say("only the host (who started the call) can add people");
        return;
    }
    if (s == control::CallState::InCall &&
        signaling_.members().size() >= static_cast<size_t>(proto::kMaxMembers)) {
        say("the call is full (%d people)", proto::kMaxMembers);
        return;
    }
    const Target t = peer != nullptr
                         ? peer_target(*peer)
                         : resolve_target(who, discovery_ok_ ? discovery_.peers()
                                                             : std::vector<control::Peer>{});
    if (!t.ok()) {
        say("%s", t.error.c_str());
        return;
    }
    const std::string name = t.name.empty() ? t.ip : t.name;
    if (!signaling_.invite(t.ip, t.port)) {
        const std::string why = signaling_.error();
        say("cannot add %s: %s", name.c_str(),
            why.empty() ? "already in the call, or not possible yet" : why.c_str());
        return;
    }
    if (s == control::CallState::Idle) {  // same as call: we become the host
        peer_name_ = t.name;
        peer_ip_ = t.ip;
        say("calling %s (%s)...", name.c_str(), t.ip.c_str());
    } else {
        say("ringing %s (%s) into the call...", name.c_str(), t.ip.c_str());
    }
}

void App::accept() {
    if (direct_ || state() != control::CallState::Ringing) {
        say("nobody is calling");
        return;
    }
    if (bcast_.active()) {  // phase 7: the camera is the broadcast's
        say("you're broadcasting: unbroadcast first, or decline");
        return;
    }
    stop_watch(" to take the call");  // phase 7: frees ports 5004/5006
    signaling_.accept();
}

void App::decline() {
    if (direct_ || state() != control::CallState::Ringing) {
        say("nobody is calling");
        return;
    }
    signaling_.decline();
    say("declined");
}

void App::hang_up() {
    if (direct_) {
        if (media_.active()) {
            stop_media();
        } else {
            say("no call");
        }
        return;
    }
    const control::CallState s = state();
    if (s == control::CallState::Ringing) {
        say("someone is calling: type decline");
    } else if (s == control::CallState::Idle) {
        say("no call");
    } else {
        signaling_.hang_up();
        stop_media();  // right away; the Ended event that follows is harmless
    }
}

void App::relay(const std::string& arg) {
    const control::CallState s = state();
    if (direct_) {
        say("direct mode (--peer): no relay");
    } else if (arg != "me" && !audio::app::is_ipv4(arg)) {
        say("usage: relay <ip|me>");
    } else if (s == control::CallState::Idle) {
        relay_choice_ = arg;
        multicast_choice_ = false;
        say("calls you start will use the relay %s", arg == "me" ? "on this laptop" : arg.c_str());
    } else if (s != control::CallState::InCall) {
        say("busy: wait until this call is set up");
    } else if (!signaling_.is_host()) {
        say("only the host (who started the call) can change this");
    } else {
        use_relay(arg);
    }
}

void App::mesh() {
    const control::CallState s = state();
    if (direct_) {
        say("direct mode (--peer): always direct");
    } else if (s == control::CallState::Idle) {
        relay_choice_.clear();
        multicast_choice_ = false;
        say("calls you start will use mesh (everyone to everyone)");
    } else if (s != control::CallState::InCall) {
        say("busy: wait until this call is set up");
    } else if (!signaling_.is_host()) {
        say("only the host (who started the call) can change this");
    } else {
        signaling_.set_media(control::MediaMode::Mesh, "");
        say("media now goes directly between everyone (mesh)");
        metrics::event("media_relay", 0);
        apply_route();
    }
}

void App::multicast() {
    const control::CallState s = state();
    if (direct_) {
        say("direct mode (--peer): always direct");
    } else if (s == control::CallState::Idle) {
        multicast_choice_ = true;
        relay_choice_.clear();
        say("calls you start will use multicast, if everyone is on this floor (else a relay on "
            "this laptop)");
    } else if (s != control::CallState::InCall) {
        say("busy: wait until this call is set up");
    } else if (!signaling_.is_host()) {
        say("only the host (who started the call) can change this");
    } else {
        use_multicast();
    }
}

void App::text(const std::string& to, const std::string& message) {
    if (direct_) {
        say("direct mode (--peer): no call setup, so no chat");
        return;
    }
    if (!signaling_ok_) {
        say("call setup unavailable: %s", signaling_.error().c_str());
        return;
    }
    if (message.size() > control::Signaling::kMaxTextBytes) {
        say("too long: at most %zu bytes per message", control::Signaling::kMaxTextBytes);
        return;
    }
    std::string lower = to;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "all") {
        if (state() != control::CallState::InCall) {
            say("not in a call; text <name|ip> <message> writes to one person");
        } else if (signaling_.text_all(message)) {
            say_as(Notice::Kind::Chat, "[chat] you -> everyone: %s", message.c_str());
            metrics::event("chat_sent", static_cast<double>(message.size()));
            ChatLine c;
            c.from = "you";
            c.to = "everyone";
            c.text = message;
            c.mine = true;
            add_chat(std::move(c));
        } else {
            say("message not sent");
        }
        return;
    }
    const Target t =
        resolve_text_target(to, signaling_.members(),
                            discovery_ok_ ? discovery_.peers() : std::vector<control::Peer>{});
    if (!t.ok()) {
        say("%s", t.error.c_str());
        return;
    }
    if (signaling_.text_to(t.ip, message, t.port)) {
        say_as(Notice::Kind::Chat, "[chat] you -> %s: %s", (t.name.empty() ? t.ip : t.name).c_str(),
               message.c_str());
        metrics::event("chat_sent", static_cast<double>(message.size()));
        ChatLine c;
        c.from = "you";
        c.to = t.name.empty() ? t.ip : t.name;
        c.text = message;
        c.mine = true;
        add_chat(std::move(c));
    } else {
        say("message not sent");
    }
}

void App::broadcast(const std::string& typed_title) {
    if (direct_) {
        say("direct mode (--peer): no broadcasts");
        return;
    }
    if (!discovery_ok_) {
        say("broadcasts need discovery: %s", discovery_.error().c_str());
        return;
    }
    if (signaling_.state() != control::CallState::Idle) {
        say("busy: hang up (or decline) first");
        return;
    }
    if (watch_.active()) {
        say("you're watching %s: unwatch first", watch_.name().c_str());
        return;
    }
    const std::string title = control::detail::clean_title(typed_title);
    if (bcast_.active()) {  // already on: only the title changes
        bcast_.set_title(title);
        discovery_.announce(bcast_.group(), title);
        say("broadcast title now \"%s\"", title.c_str());
        return;
    }
    const uint8_t group = discovery_.pick_group();
    if (group == 0) {
        say("no free multicast group on this floor (all 254 in use)");
        return;
    }
    BroadcastOptions bo;
    bo.in_device = opt_.media.in_device;
    bo.camera = opt_.media.camera;
    bo.fps = opt_.media.fps;
    bo.quality = opt_.media.quality;
    bo.iface_ip = discovery_.own_ip();
    bo.loop = opt_.mc_loop;
    for (const auto& f : bcast_.start(title, group, bo)) say("  not started: %s", f.c_str());
    if (!bcast_.active()) {
        say("broadcast not started");
        return;
    }
    discovery_.announce(group, title);
    say("broadcasting \"%s\" (%s) to %s: anyone on this floor can type: watch %s", title.c_str(),
        bcast_.parts().c_str(), bcast_.group_ip().c_str(), opt_.name.c_str());
    if (bo.iface_ip.empty())
        say("  (our address isn't known yet, so the kernel picks the network card)");
    metrics::event("broadcast_started", group);
}

void App::unbroadcast() {
    if (!stop_broadcast()) say("not broadcasting");
}

void App::watch(const std::string& who) {
    if (direct_) {
        say("direct mode (--peer): no broadcasts");
        return;
    }
    if (!discovery_ok_) {
        say("broadcasts need discovery: %s", discovery_.error().c_str());
        return;
    }
    if (state() != control::CallState::Idle) {
        say("busy: hang up (or decline) first");
        return;
    }
    if (bcast_.active()) {
        say("you're broadcasting: unbroadcast first");
        return;
    }
    const BroadcastTarget t = resolve_broadcast(who, discovery_.peers());
    if (!t.ok()) {
        say("%s", t.error.c_str());
        return;
    }
    if (watch_.active() && watch_.presenter_ip() == t.ip && watch_.group() == t.group) {
        say("already watching %s", t.name.c_str());
        return;
    }
    stop_watch("");
    start_watch(t);
}

void App::unwatch() {
    if (watch_.active()) {
        stop_watch("");
    } else {
        say("not watching anything");
    }
}

// phase 7: bridges to other floors (net/bridge.hpp)
void App::bridge(const std::string& ip) {
    if (!bcast_.active()) {
        say("start a broadcast first (broadcast <title>)");
    } else if (!audio::app::is_ipv4(ip) || proto::is_multicast_ipv4(ip)) {
        say("usage: bridge <ip of a laptop on the other floor>");
    } else if (!bcast_.add_bridge(ip)) {
        say("already sending to the bridge at %s", ip.c_str());
    } else {
        say("also sending the broadcast to %s:%u (one more copy). On that laptop run:\n  %s",
            ip.c_str(), net::kBridgePort, bridge_command(ip).c_str());
        metrics::event("bridge_added", static_cast<double>(bcast_.bridges().size()));
    }
}

// The line to run on the bridge's laptop. --from is our address as that laptop sees it: its
// bridge takes nothing else.
std::string App::bridge_command(const std::string& ip) const {
    const std::string me = platform::local_address_towards(ip, proto::kSignalPort);
    return format("./build/net/lancall_bridge --from %s --name %s --title \"%s\"",
                  me.empty() ? "<this laptop's ip>" : me.c_str(), opt_.name.c_str(),
                  bcast_.title().c_str());
}

void App::unbridge(const std::string& ip) {
    if (bcast_.remove_bridge(ip)) {
        say("no longer sending to the bridge at %s", ip.c_str());
        metrics::event("bridge_removed", static_cast<double>(bcast_.bridges().size()));
    } else {
        say("no bridge at %s", ip.c_str());
    }
}

void App::run(const Command& cmd) {
    switch (cmd.kind) {
        case CommandKind::Empty:
        case CommandKind::Quit:  // the front end's
            break;
        case CommandKind::Help:
            say_lines(help_text());
            break;
        case CommandKind::Stats:
            if (!media_.active() && !bcast_.active() && !watch_.active()) say("no call");
            for (const auto& s : stats()) say("  %s", s.c_str());
            break;
        case CommandKind::List:
            if (direct_) {
                say("direct mode (--peer): no discovery");
            } else if (!discovery_ok_) {
                say("discovery unavailable: %s", discovery_.error().c_str());
            } else {
                say_lines(format_peers(discovery_.peers(), proto::now_us()));
            }
            break;
        case CommandKind::Call:
            call(cmd.arg);
            break;
        case CommandKind::Add:
            add(cmd.arg);
            break;
        case CommandKind::Members:
            if (direct_) {
                say("direct mode: you and %s", opt_.peer.c_str());
            } else {
                say_lines(
                    format_members(signaling_.members(), signaling_.media_mode(), media_address()));
                // phase 6: the codes to compare, one per connection we share
                for (const control::Member& m : signaling_.members()) {
                    const std::string code = signaling_.safety_code(m.id);
                    if (!code.empty())
                        say("  safety code with %s: %s", m.name.c_str(), code.c_str());
                }
                say("  media %s", media_.encrypted() ? "encrypted" : "NOT encrypted");
            }
            break;
        case CommandKind::Relay:
            relay(cmd.arg);
            break;
        case CommandKind::Mesh:
            mesh();
            break;
        case CommandKind::Multicast:
            multicast();
            break;
        case CommandKind::Accept:
            accept();
            break;
        case CommandKind::Decline:
            decline();
            break;
        case CommandKind::Hangup:
            hang_up();
            break;
        case CommandKind::Text:
            text(cmd.arg, cmd.text);
            break;
        case CommandKind::Broadcast:
            broadcast(cmd.text);
            break;
        case CommandKind::Unbroadcast:
            unbroadcast();
            break;
        case CommandKind::Watch:
            watch(cmd.arg);
            break;
        case CommandKind::Unwatch:
            unwatch();
            break;
        case CommandKind::Bridge:
            bridge(cmd.arg);
            break;
        case CommandKind::Unbridge:
            unbridge(cmd.arg);
            break;
        case CommandKind::Invalid:
            say("%s", cmd.error.c_str());
            break;
    }
}

// ---- the main loop's pass --------------------------------------------------------------

void App::tick() {
    handle_call_events();
    handle_chat();
    const uint32_t now = proto::now_us();
    send_keepalive(now);
    check_watch(now);
    poll_clock(now);

    // media: the frames to show, and our own camera for the self-view
    if (media_.active()) {
        play_received(media_, now);
        if (!media_.self_view(self_)) self_.release();
    } else if (watch_.active()) {
        play_received(watch_, now);
        self_.release();  // a viewer sends nothing, so there's no camera to show
    } else if (bcast_.active()) {
        if (!bcast_.self_view(self_)) self_.release();  // the presenter sees what goes out
    } else {
        self_.release();
    }
}

// call setup events
void App::handle_call_events() {
    control::CallEvent ev;
    while (!direct_ && signaling_.poll_event(ev)) {
        const std::string who = ev.peer_name.empty() ? ev.peer_ip : ev.peer_name;
        switch (ev.kind) {
            case control::CallEvent::Kind::Incoming:
                peer_name_ = ev.peer_name;
                peer_ip_ = ev.peer_ip;
                say_as(Notice::Kind::Incoming, "%s (%s) is calling: accept or decline", who.c_str(),
                       ev.peer_ip.c_str());
                if (bcast_.active())  // phase 7
                    say("  (you're broadcasting: unbroadcast before you accept)");
                else if (watch_.active())
                    say("  (accepting stops watching %s)", watch_.name().c_str());
                metrics::event("call_incoming", 0);
                break;
            case control::CallEvent::Kind::Accepted:
                peer_name_ = ev.peer_name;
                peer_ip_ = ev.peer_ip;
                // our own call: the room starts in the mode we chose (--relay, relay, mesh,
                // multicast). Multicast that can't reach everyone starts as relay instead
                if (signaling_.is_host() && multicast_choice_) {
                    if (!use_multicast()) {
                        say("starting with a relay on this laptop instead");
                        use_relay("me");
                    }
                } else if (signaling_.is_host() && !relay_choice_.empty()) {
                    use_relay(relay_choice_);
                }
                start_media(ev.peer_ip);
                next_clock_poll_ = proto::now_us();
                break;
            case control::CallEvent::Kind::Declined:
                say("%s declined%s%s", who.c_str(), ev.reason.empty() ? "" : ": ",
                    ev.reason.c_str());
                break;
            case control::CallEvent::Kind::Failed:
                say("call to %s failed%s%s", who.c_str(), ev.reason.empty() ? "" : ": ",
                    ev.reason.c_str());
                break;
            case control::CallEvent::Kind::Ended:
                if (!ev.reason.empty()) say("call ended by %s", ev.reason.c_str());
                stop_media();
                break;
            case control::CallEvent::Kind::Joined: {
                const size_t n = signaling_.members().size();
                say("%s joined (%zu in the call)", who.c_str(), n);
                const std::string code = signaling_.safety_code(ev.member_id);
                if (!code.empty()) say("  safety code with %s: %s", who.c_str(), code.c_str());
                metrics::event("member_joined", ev.member_id);
                metrics::event("members", static_cast<double>(n));
                apply_route();
                keep_everyone_reachable();  // phase 7: a multicast call, someone off-floor
                break;
            }
            case control::CallEvent::Kind::Left: {
                const size_t n = signaling_.members().size();
                say("%s left%s%s (%zu in the call)", who.c_str(), ev.reason.empty() ? "" : ": ",
                    ev.reason.c_str(), n);
                remotes_.erase(ev.member_id);
                next_audio_e2e_.erase(ev.member_id);
                offsets_.erase(ev.member_id);
                metrics::event("member_left", ev.member_id);
                metrics::event("members", static_cast<double>(n));
                apply_route();
                break;
            }
            case control::CallEvent::Kind::MediaChanged: {
                const control::MediaMode mode = signaling_.media_mode();
                const bool via_relay = mode == control::MediaMode::Relay;
                if (via_relay) {
                    say("media now goes through the relay at %s", signaling_.relay_ip().c_str());
                } else if (mode == control::MediaMode::Multicast) {  // phase 7
                    say("media now goes to multicast group %s",
                        signaling_.multicast_group().c_str());
                } else {
                    say("media now goes directly between everyone (mesh)");
                }
                metrics::event("media_relay", via_relay ? 1 : 0);
                apply_route();
                break;
            }
        }
    }
}

// text chat (phase 6): messages for us, and ours that didn't get through. The log only gets
// counts, never the text
void App::handle_chat() {
    control::ChatEvent chat;
    while (!direct_ && signaling_.poll_chat(chat)) {
        if (chat.kind == control::ChatEvent::Kind::Received) {
            const std::string from = chat.name.empty() ? chat.ip : chat.name;
            say_as(Notice::Kind::Chat, "[chat] %s%s: %s", from.c_str(),
                   chat.to_all ? "" : " (to you)", chat.text.c_str());
            metrics::event("chat_received", static_cast<double>(chat.text.size()));
            ChatLine c;
            c.from = from;
            c.to = chat.to_all ? "everyone" : "you";
            c.text = chat.text;
            add_chat(std::move(c));
        } else {
            say_as(Notice::Kind::Chat, "[chat] not delivered to %s (%s): %s", chat.ip.c_str(),
                   chat.reason.c_str(), chat.text.c_str());
            ChatLine c;
            c.from = "you";
            c.to = chat.ip;
            c.text = chat.text;
            c.mine = true;
            c.failed = true;
            c.reason = chat.reason;
            add_chat(std::move(c));
        }
    }
}

// relay mode: tell the relay we're still here, even if we send no media at all
void App::send_keepalive(uint32_t now) {
    if (direct_ || !media_.active() || signaling_.media_mode() != control::MediaMode::Relay ||
        metrics::time_after(next_keepalive_, now))
        return;
    const std::string relay_ip = signaling_.relay_ip();
    const uint8_t my_id = signaling_.my_id();
    if (!relay_ip.empty() && my_id != 0) {  // the relay ignores stream 0
        if (!keepalive_sock_) {
            try {
                keepalive_sock_ = std::make_unique<UdpSocket>();
            } catch (const std::exception& e) {
                say("relay keepalive unavailable: %s", e.what());
            }
        }
        if (keepalive_sock_) {
            // sealed like the rest of the call (the relay only reads the header)
            keepalive_sock_->set_crypto(media_.crypto());
            const auto pkt = keepalive_packet(my_id, keepalive_seq_++, now);
            keepalive_sock_->send_to(relay_ip, proto::kRelayPort, pkt.data(), pkt.size());
        }
    }
    next_keepalive_ = now + kKeepaliveEveryUs;
}

// phase 7: is the broadcast we watch still on? Its presenter's beacon says so (and where it
// is, should it have moved to another group)
void App::check_watch(uint32_t now) {
    if (!watch_.active() || metrics::time_after(next_watch_check_, now)) return;
    next_watch_check_ = now + kWatchCheckEveryUs;
    bool live = false;
    for (const control::Peer& p : discovery_.peers()) {
        if (p.remote || p.ip != watch_.presenter_ip() || p.name != watch_.name()) continue;
        if (p.group == 0 || p.broadcast_title.empty()) continue;
        live = true;
        if (p.group != watch_.group()) {
            watch_.move_to(p.group);
            say("%s's broadcast moved to %s", watch_.name().c_str(),
                proto::lancall_group(p.group).c_str());
        }
    }
    if (!live) stop_watch(" (the broadcast ended)");
}

// clock sync: offsets for the e2e events (per person), rtt for the log
void App::poll_clock(uint32_t now) {
    if (direct_ || !media_.active() || metrics::time_after(next_clock_poll_, now)) return;
    std::vector<uint8_t> ids = media_.audio_streams();
    for (const auto& [id, rv] : remotes_) ids.push_back(id);
    for (uint8_t id : ids) {
        uint32_t off = 0;
        const bool ok = id == 0 ? signaling_.clock_offset(off)  // a two-person peer
                                : signaling_.clock_offset(id, off);
        if (ok) offsets_[id] = off;
    }
    if (!clock_reported_ && !offsets_.empty()) {
        say("clock sync ok: latency is measured");
        clock_reported_ = true;
    }
    rtt_ms_ = signaling_.rtt_ms();
    if (rtt_ms_ > 0) metrics::event("rtt_ms", rtt_ms_);
    next_clock_poll_ = now + kClockPollEveryUs;
}

// ---- what the front end shows ----------------------------------------------------------

control::CallState App::state() const {
    if (direct_) return media_.active() ? control::CallState::InCall : control::CallState::Idle;
    return signaling_.state();
}

bool App::is_host() const {
    return !direct_ && signaling_.is_host();
}

std::vector<control::Peer> App::peers() const {
    return discovery_ok_ ? discovery_.peers() : std::vector<control::Peer>{};
}

std::vector<control::Member> App::members() const {
    return direct_ ? std::vector<control::Member>{} : signaling_.members();
}

control::MediaMode App::media_mode() const {
    return direct_ ? control::MediaMode::Mesh : signaling_.media_mode();
}

std::string App::media_address() const {
    switch (media_mode()) {
        case control::MediaMode::Relay:
            return signaling_.relay_ip();
        case control::MediaMode::Multicast:
            return signaling_.multicast_group();
        default:
            return "";
    }
}

std::string App::safety_code(uint8_t member_id) const {
    return direct_ ? std::string() : signaling_.safety_code(member_id);
}

// ---- phase 9.5: chat history and settings -------------------------------------------------

void App::add_chat(ChatLine line) {
    line.seq = ++chat_seq_;
    std::tm tm{};
    char hhmm[8] = "";
    if (platform::local_time(std::time(nullptr), tm))
        std::strftime(hhmm, sizeof hhmm, "%H:%M", &tm);
    line.time = hhmm;
    chat_.push_back(std::move(line));
    while (chat_.size() > kMaxChat) chat_.pop_front();
}

std::string App::own_ip() const {
    return discovery_ok_ ? discovery_.own_ip() : std::string();
}

void App::set_media_options(const MediaOptions& media) {
    // the multicast card and loop are set when media starts (start_media), not here
    const std::string iface = opt_.media.multicast_if;
    const bool loop = opt_.media.multicast_loop;
    opt_.media = media;
    opt_.media.multicast_if = iface;
    opt_.media.multicast_loop = loop;
    say("settings saved: used from the next call, broadcast or watch on");
}

bool App::set_directory(const std::string& ip) {
    if (direct_) {
        say("direct mode (--peer): no discovery");
        return false;
    }
    if (state() != control::CallState::Idle || bcast_.active() || watch_.active()) {
        // a restart empties the peer list for a moment: a watch would end, a multicast call
        // would think everyone left the floor
        say("change the directory when you're not in a call, broadcasting or watching");
        return false;
    }
    if (!ip.empty() && !audio::app::is_ipv4(ip)) {
        say("directory: give the IPv4 address of the lancall_directory laptop");
        return false;
    }
    opt_.directory = ip;
    if (!started_) return true;  // used when start() runs
    discovery_.stop();
    discovery_ok_ = discovery_.start(opt_.name, static_cast<uint16_t>(opt_.port), ip);
    if (!discovery_ok_) {
        say("  discovery unavailable: %s", discovery_.error().c_str());
    } else if (!ip.empty()) {
        say("  directory %s: peers on other floors show up in list", ip.c_str());
    } else {
        say("  no directory: only peers on this floor show up in list");
    }
    return discovery_ok_;
}

std::string App::control_error() const {
    if (direct_ || !started_) return "";
    if (!signaling_ok_) return "call setup unavailable: " + signaling_.error();
    if (!discovery_ok_) return "discovery unavailable: " + discovery_.error();
    return "";
}

std::vector<std::string> App::stats() const {
    std::vector<std::string> lines = media_.stats();
    for (const auto& [id, rv] : remotes_) {
        const auto& c = rv.sync.counters();
        const std::string which = remotes_.size() > 1 ? " s" + std::to_string(id) : std::string();
        lines.push_back(
            format("lip sync%s: shown %llu  skipped %llu  waited-too-long %llu  no-audio %llu  "
                   "queue-full %llu",
                   which.c_str(), ull(c.shown), ull(c.skipped), ull(c.forced), ull(c.no_audio),
                   ull(c.dropped_full)));
    }
    if (rtt_ms_ > 0) lines.push_back(format("clock sync: rtt %.2f ms", rtt_ms_));
    if (relay_) lines.push_back(relay_->stats());
    for (const auto& s : bcast_.stats()) lines.push_back(s);
    for (const auto& b : bcast_.bridges())
        lines.push_back(format("bridge: a copy to %s:%u", b.c_str(), net::kBridgePort));
    for (const auto& s : watch_.stats()) lines.push_back(s);
    return lines;
}

Screen App::screen() const {
    const std::vector<control::Member> people = members();
    Screen out;
    if (bcast_.active()) {  // phase 7
        out.status = {broadcast_status(opt_.name, bcast_.title(), bcast_.group_ip())};
    } else if (watch_.active()) {
        out.status = {watch_status(opt_.name, watch_.name(), watch_.title())};
    } else {
        StatusInfo st;
        st.my_name = opt_.name;
        st.state = state();
        st.peer_name = peer_name_;
        st.peer_ip = peer_ip_;
        st.media_parts = media_.parts();
        st.rtt_ms = rtt_ms_;
        st.people = static_cast<int>(people.size());
        if (media_mode() == control::MediaMode::Relay) st.relay_ip = signaling_.relay_ip();
        if (media_mode() == control::MediaMode::Multicast)
            st.multicast_group = signaling_.multicast_group();
        out.status = {status_line(st)};
    }

    if (bcast_.active()) {
        // phase 7: the presenter sees its own camera, full size: what goes out
        out.main = self_;
        out.main_is_self = true;
    } else if (people.size() > 2) {
        // group call: one tile per member in id order, ours shows the camera
        out.grid = true;
        for (const control::Member& m : people) {
            Tile t;
            t.label = member_label(m) + (m.me ? " (you)" : "");
            if (m.me) {
                t.image = self_;
                out.self_tile = static_cast<int>(out.tiles.size());
            } else if (const auto it = remotes_.find(m.id); it != remotes_.end()) {
                t.image = it->second.shown;
            }
            out.tiles.push_back(std::move(t));
        }
    } else {
        // two people (or watching): the other person full size (whatever stream id their
        // video carries), ourselves in the corner
        for (const auto& [id, rv] : remotes_) {
            if (!rv.shown.empty()) {
                out.main = rv.shown;
                break;
            }
        }
        out.corner = self_;
    }
    return out;
}

}  // namespace call
