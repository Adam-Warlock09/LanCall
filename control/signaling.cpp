// Call setup, group calls and clock sync over TCP (Person 1, control/). The message format
// and the state machine are described in signaling.hpp.
//
// Threading: one worker thread owns every socket. The public functions (call, accept, ...)
// never touch a socket: they update the state right away, so state() is instantly right,
// queue a command for the worker and poke it awake through a pipe. The worker sends and
// receives, and reports what happened as events. Everything shared sits behind one mutex.
//
// Phase 4: a call is a room. The host holds one Peer (TCP connection) per member and one per
// invite still ringing; a member holds a single Peer, the host. The worker runs queued
// commands first every time it wakes, so by the time it looks at the network the state and
// the list of peers agree with whatever the app last asked for.
#include "control/signaling.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include "control/discovery.hpp"  // valid_name()
#include "control/signaling_detail.hpp"
#include "control/wire.hpp"

namespace control {

namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kConnectTimeout = std::chrono::seconds(5);    // TCP connect to the callee
constexpr auto kInviteTimeout = std::chrono::seconds(5);     // incoming conn must say INVITE
constexpr auto kNoAnswerTimeout = std::chrono::seconds(30);  // ringing with nobody answering
constexpr auto kPingEvery = std::chrono::seconds(2);
// In a call both sides PING every 2 s and answer every PING, so 10 s of total silence means
// the other laptop is gone without a word: cable pulled, laptop asleep, crashed hard. TCP
// alone would keep resending for ~15 minutes before giving up.
constexpr auto kSilenceTimeout = std::chrono::seconds(10);
constexpr auto kRosterEvery = std::chrono::seconds(2);  // host re-sends MEMBER with fresh off=
constexpr size_t kMaxLine = 4096;
constexpr size_t kMaxOneshots = 16;  // new connections waiting for their first line, at most
constexpr uint8_t kHostId = 1;

void set_nonblocking(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}

// Send each line right away. Without this, Nagle's algorithm holds a small write back while
// an earlier one is unacknowledged, and the other side delays its ACK by ~40 ms: a PING or
// PONG caught in that waited 40 ms, which showed up as rtt_ms ~41 on one laptop and skewed
// the clock offset (and so every e2e latency) by ~20 ms
void set_nodelay(int fd) {
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

// our lines are tiny so the send buffer is never full in practice; if it is, wait a bit.
// MSG_NOSIGNAL: writing to a socket the peer closed must not kill the whole program
bool send_line(int fd, const std::string& line) {
    if (fd < 0) return false;
    std::string s = line + "\n";
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n > 0) {
            off += static_cast<size_t>(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 100) <= 0) return false;
        } else {
            return false;
        }
    }
    return true;
}

std::string ip_string(const sockaddr_in& a) {
    char buf[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &a.sin_addr, buf, sizeof(buf));
    return buf;
}

bool is_ipv4(const std::string& s) {
    in_addr tmp{};
    return inet_pton(AF_INET, s.c_str(), &tmp) == 1;
}

std::string name_or_unknown(const std::string& n) {
    return valid_name(n) ? n : "unknown";
}

// a member id from a message, 1..kMaxMembers
bool get_id(const wire::Line& l, uint8_t& out) {
    uint32_t v;
    if (!wire::get_u32(l, "id", v) || v < 1 || v > static_cast<uint32_t>(proto::kMaxMembers))
        return false;
    out = static_cast<uint8_t>(v);
    return true;
}

}  // namespace

struct Signaling::Impl {
    enum class Conn { Connecting, AwaitInvite, Open };

    // One TCP connection
    struct Peer {
        int fd = -1;
        Conn conn = Conn::Connecting;
        bool outgoing = false;  // we connected to them (we invited them)
        bool started = false;   // outgoing: the worker has begun connecting
        bool in_room = false;   // the call is up with them
        bool dead = false;      // closed; removed at the end of the worker's round
        uint8_t id = 0;         // their member id (host: set aside when inviting)
        uint16_t port = 0;      // outgoing: their signaling port
        std::string ip, name, rxbuf;
        Clock::time_point since;  // connect started / INVITE sent / connection accepted
        Clock::time_point next_ping;
        Clock::time_point last_rx;  // anything at all arrived from them
        detail::ClockFilter clock;
    };

    // what a member knows about the others, from the host's MEMBER lines
    struct Remote {
        std::string name, ip;
        bool have_off = false;
        uint32_t off = 0;  // their clock minus the host's
    };

    // Phase 6: a connection that isn't part of a call (yet). Every incoming connection starts
    // as one until its first line says what it is (INVITE turns it into a call Peer, MSG is
    // a message); text_to() someone outside our call opens one to deliver a MSG.
    struct Oneshot {
        int fd = -1;
        bool outgoing = false;
        bool started = false;     // outgoing: the worker has begun connecting
        bool connecting = false;  // outgoing: connect() still in progress
        bool sent = false;        // outgoing: MSG written, waiting for OK
        bool dead = false;
        uint16_t port = 0;
        std::string ip, rxbuf;
        std::string text;  // outgoing: the message, for the Failed event
        Clock::time_point since;
    };

    struct Command {
        enum class Kind { Accept, Decline, HangUp, SendMedia, Chat };
        Command(Kind k, CallState w = CallState::Idle, std::string who = "", std::string ip = "")
            : kind(k), was(w), peer_name(std::move(who)), peer_ip(std::move(ip)) {}

        Kind kind;
        CallState was = CallState::Idle;  // HangUp: what we were hanging up
        std::string peer_name, peer_ip;   // HangUp: for the event, the conn may be gone already
        uint8_t to = 0;                   // Chat: member id, 0 = everyone
        std::string text;                 // Chat
    };

    std::string name;
    int listen_fd = -1;
    int wake_rd = -1, wake_wr = -1;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    // everything below is shared with the API functions and guarded by mu
    mutable std::mutex mu;
    CallState state = CallState::Idle;
    bool host = false;  // we started this call
    uint8_t my_id = 0;  // member side: from INVITE id= / WELCOME (0 = old host, so 2)
    std::vector<std::unique_ptr<Peer>> peers;
    std::map<uint8_t, Remote> roster;   // member side: everyone except the host and us
    MediaMode media = MediaMode::Mesh;  // of the call we're in
    std::string relay;
    MediaMode pref_media = MediaMode::Mesh;  // what set_media() asked for, used when hosting
    std::string pref_relay;
    Clock::time_point next_roster;
    std::deque<Command> commands;
    std::deque<CallEvent> events;
    std::vector<std::unique_ptr<Oneshot>> oneshots;
    std::deque<ChatEvent> chats;
    std::string error;

    void wake() {
        char c = 1;
        if (write(wake_wr, &c, 1) < 0) {
        }  // a full pipe already means "wake up", so nothing to do
    }

    void emit(CallEvent::Kind kind, const std::string& who, const std::string& ip,
              const std::string& reason = "", uint8_t id = 0) {
        events.push_back(CallEvent{kind, who, ip, reason, id});
    }
    void emit(CallEvent::Kind kind, const Peer& p, const std::string& reason = "") {
        emit(kind, p.name, p.ip, reason, p.id);
    }

    // ------------------------------------------------------------ lookups (mu held)

    int room_count() const {
        int n = 0;
        for (const auto& p : peers)
            if (!p->dead && p->in_room) n++;
        return n;
    }

    Peer* find_id(uint8_t id) {
        for (auto& p : peers)
            if (!p->dead && p->id == id && (p->in_room || p->outgoing)) return p.get();
        return nullptr;
    }

    // a member's one connection: the host (or, while ringing, whoever is inviting us)
    Peer* host_peer() {
        for (auto& p : peers)
            if (!p->dead && !p->outgoing && p->conn == Conn::Open) return p.get();
        return nullptr;
    }

    // "the peer" of the phase 2 API: the host for a member, the lowest member id for the
    // host, or while Calling the person being called
    Peer* main_peer() {
        if (!host) return host_peer();
        Peer* best = nullptr;
        for (auto& p : peers)
            if (!p->dead && p->in_room && (!best || p->id < best->id)) best = p.get();
        if (best) return best;
        for (auto& p : peers)
            if (!p->dead && p->outgoing) return p.get();
        return nullptr;
    }
    const Peer* main_peer() const { return const_cast<Impl*>(this)->main_peer(); }

    uint8_t free_id() const {
        for (int id = 2; id <= proto::kMaxMembers; ++id) {
            bool used = false;
            for (const auto& p : peers)
                if (!p->dead && p->id == id) used = true;
            if (!used) return static_cast<uint8_t>(id);
        }
        return 0;
    }

    uint8_t me() const { return host ? kHostId : (my_id ? my_id : 2); }

    std::vector<Member> members_locked() const {
        std::vector<Member> out;
        if (state != CallState::InCall) return out;
        out.push_back({me(), name, "", true});
        if (host) {
            for (const auto& p : peers)
                if (!p->dead && p->in_room) out.push_back({p->id, p->name, p->ip, false});
        } else {
            if (const Peer* h = main_peer()) out.push_back({kHostId, h->name, h->ip, false});
            for (const auto& [id, r] : roster)
                if (id != me()) out.push_back({id, r.name, r.ip, false});
        }
        std::sort(out.begin(), out.end(),
                  [](const Member& a, const Member& b) { return a.id < b.id; });
        return out;
    }

    // ------------------------------------------------------------ chat (mu held)

    void push_chat(const std::string& who, const std::string& ip, uint8_t id, bool all,
                   std::string text) {
        // a message is printed straight to someone's terminal: no control characters
        for (char& c : text)
            if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) c = ' ';
        ChatEvent e;
        e.kind = ChatEvent::Kind::Received;
        e.name = who;
        e.ip = ip;
        e.member_id = id;
        e.to_all = all;
        e.text = std::move(text);
        chats.push_back(std::move(e));
    }

    // the text= field of a CHAT or MSG line, if it's a valid message
    static bool read_text(const wire::Line& l, std::string& out) {
        return wire::unescape(wire::get(l, "text"), out) && !out.empty() &&
               out.size() <= Signaling::kMaxTextBytes;
    }

    // ours, into the call: to = member id, 0 = everyone
    void send_chat(uint8_t to, const std::string& text) {
        if (state != CallState::InCall) return;
        const std::string to_s = to ? std::to_string(to) : std::string("all");
        const std::string esc = wire::escape(text);
        if (host) {
            const std::string line = "CHAT from=1 name=" + name + " to=" + to_s + " text=" + esc;
            for (auto& q : peers)
                if (!q->dead && q->in_room && (to == 0 || q->id == to)) send_line(q->fd, line);
        } else if (Peer* h = host_peer()) {
            send_line(h->fd, "CHAT to=" + to_s + " text=" + esc);
        }
    }

    // a CHAT line from a peer in our call
    void handle_chat(Peer& p, const wire::Line& l) {
        std::string text;
        if (!read_text(l, text)) return;
        const bool all = wire::get(l, "to") == "all";
        uint32_t to_id = 0;
        if (!all && (!wire::get_u32(l, "to", to_id) || to_id < 1 ||
                     to_id > static_cast<uint32_t>(proto::kMaxMembers)))
            return;

        if (host) {
            // from member p: keep it, pass it on, or both
            const std::string fwd = "CHAT from=" + std::to_string(p.id) + " name=" + p.name +
                                    " to=" + (all ? std::string("all") : std::to_string(to_id)) +
                                    " text=" + wire::escape(text);
            if (all || to_id == kHostId) push_chat(p.name, p.ip, p.id, all, text);
            for (auto& q : peers) {
                if (q->dead || !q->in_room || q.get() == &p) continue;
                if (all || q->id == to_id) send_line(q->fd, fwd);
            }
            return;
        }
        // from the host, written by someone in the call
        uint32_t from = 0;
        if (!wire::get_u32(l, "from", from) || from < 1 ||
            from > static_cast<uint32_t>(proto::kMaxMembers))
            return;
        if (!all && to_id != me()) return;
        std::string ip;
        if (from == kHostId) {
            ip = p.ip;
        } else {
            auto it = roster.find(static_cast<uint8_t>(from));
            if (it != roster.end()) ip = it->second.ip;
        }
        push_chat(name_or_unknown(wire::get(l, "name")), ip, static_cast<uint8_t>(from), all, text);
    }

    // ------------------------------------------------------------ one-shot connections

    void close_oneshot(Oneshot& o) {
        if (o.fd >= 0) close(o.fd);
        o.fd = -1;
        o.dead = true;
    }

    // our message couldn't be delivered
    void oneshot_failed(Oneshot& o, const std::string& reason) {
        if (o.dead) return;
        ChatEvent e;
        e.kind = ChatEvent::Kind::Failed;
        e.ip = o.ip;
        e.text = o.text;
        e.reason = reason;
        chats.push_back(std::move(e));
        close_oneshot(o);
    }

    void start_oneshot(Oneshot& o) {
        o.started = true;
        o.since = Clock::now();
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(o.port);
        inet_pton(AF_INET, o.ip.c_str(), &a.sin_addr);
        o.fd = socket(AF_INET, SOCK_STREAM, 0);
        if (o.fd < 0) {
            oneshot_failed(o, std::strerror(errno));
            return;
        }
        set_nonblocking(o.fd);
        set_nodelay(o.fd);
        if (connect(o.fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            send_msg(o);
        } else if (errno == EINPROGRESS) {
            o.connecting = true;
        } else {
            oneshot_failed(o, std::strerror(errno));
        }
    }

    void send_msg(Oneshot& o) {
        o.connecting = false;
        if (!send_line(o.fd, "MSG name=" + name + " text=" + wire::escape(o.text))) {
            oneshot_failed(o, "connection closed");
            return;
        }
        o.sent = true;
    }

    // the first line on a new incoming connection decides what it is
    void first_line(Oneshot& o, const std::string& text, uint32_t arrival_us) {
        wire::Line l;
        if (!wire::parse(text, l)) {
            close_oneshot(o);
            return;
        }
        if (l.verb == "MSG") {
            std::string msg;
            if (read_text(l, msg)) {
                push_chat(name_or_unknown(wire::get(l, "name")), o.ip, 0, false, msg);
                send_line(o.fd, "OK");
            }
            close_oneshot(o);
        } else if (l.verb == "INVITE") {
            // a call: hand the connection over to the call code, which rings or says busy
            auto p = std::make_unique<Peer>();
            p->fd = o.fd;
            p->conn = Conn::AwaitInvite;
            p->since = o.since;
            p->last_rx = Clock::now();
            p->ip = o.ip;
            p->rxbuf = o.rxbuf;  // anything that came after the INVITE
            o.fd = -1;
            o.dead = true;
            Peer& ref = *p;
            peers.push_back(std::move(p));
            handle_line(ref, text, arrival_us);
        } else {
            close_oneshot(o);
        }
    }

    void on_oneshot_ready(Oneshot& o, short revents, uint32_t arrival_us) {
        if (o.connecting) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(o.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == ECONNREFUSED)
                oneshot_failed(o, "refused (is lancall running there?)");
            else if (err != 0)
                oneshot_failed(o, std::strerror(err));
            else if (revents & POLLOUT)
                send_msg(o);
            return;
        }
        char buf[1024];
        ssize_t n = recv(o.fd, buf, sizeof(buf), 0);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (n <= 0) {
            if (o.outgoing)
                oneshot_failed(o, "closed without an answer (an older lancall?)");
            else
                close_oneshot(o);
            return;
        }
        o.rxbuf.append(buf, static_cast<size_t>(n));
        size_t pos = o.rxbuf.find('\n');
        if (pos == std::string::npos) {
            if (o.rxbuf.size() > kMaxLine) {
                if (o.outgoing)
                    oneshot_failed(o, "garbage from the other side");
                else
                    close_oneshot(o);
            }
            return;
        }
        std::string line = o.rxbuf.substr(0, pos);
        o.rxbuf.erase(0, pos + 1);
        if (!o.outgoing) {
            first_line(o, line, arrival_us);
            return;
        }
        wire::Line l;
        if (wire::parse(line, l) && l.verb == "OK")
            close_oneshot(o);  // delivered
        else
            oneshot_failed(o, "not accepted");
    }

    void oneshot_timers() {
        auto now = Clock::now();
        for (auto& o : oneshots) {
            if (o->dead) continue;
            if (!o->outgoing && now - o->since > kInviteTimeout) {
                close_oneshot(*o);  // connected and said nothing, e.g. a port scan
            } else if (o->outgoing && o->started && now - o->since > kConnectTimeout) {
                oneshot_failed(*o, o->connecting ? "unreachable" : "no answer (an older lancall?)");
            }
        }
    }

    // ------------------------------------------------------------ teardown (mu held)

    void close_peer(Peer& p) {
        if (p.fd >= 0) close(p.fd);
        p.fd = -1;
        p.dead = true;
    }

    void reap() {
        peers.erase(std::remove_if(peers.begin(), peers.end(),
                                   [](const std::unique_ptr<Peer>& p) { return p->dead; }),
                    peers.end());
        oneshots.erase(std::remove_if(oneshots.begin(), oneshots.end(),
                                      [](const std::unique_ptr<Oneshot>& o) { return o->dead; }),
                       oneshots.end());
    }

    // back to Idle, every connection closed (whoever needed a BYE has had it)
    void reset_call() {
        for (auto& p : peers) close_peer(*p);
        state = CallState::Idle;
        host = false;
        my_id = 0;
        roster.clear();
        media = MediaMode::Mesh;
        relay.clear();
    }

    // host: cancel invites still ringing, e.g. when the call ends under them
    void cancel_invites() {
        for (auto& p : peers) {
            if (p->dead || !p->outgoing || p->in_room) continue;
            if (p->conn == Conn::Open) send_line(p->fd, "BYE");
            emit(CallEvent::Kind::Failed, *p, "cancelled");
            close_peer(*p);
        }
    }

    // host: a member is gone. reason_word goes on the wire, reason to the app
    void member_left(Peer& p, const std::string& reason, const std::string& reason_word) {
        const std::string who = p.name, ip = p.ip;
        const uint8_t id = p.id;
        close_peer(p);

        if (room_count() == 0) {
            // the last one left: nothing to host any more
            cancel_invites();
            emit(CallEvent::Kind::Ended, who, ip, reason, id);
            reset_call();
            return;
        }
        emit(CallEvent::Kind::Left, who, ip, reason, id);
        for (auto& q : peers)
            if (!q->dead && q->in_room)
                send_line(q->fd, "LEFT id=" + std::to_string(id) + " reason=" + reason_word);
    }

    // a connection died or timed out; tell the app in terms of what it was
    void lost(Peer& p, const std::string& reason) {
        if (p.dead) return;
        if (p.in_room) {
            if (host) {
                member_left(p, reason, "dropped");
            } else {
                emit(CallEvent::Kind::Ended, p, reason);
                reset_call();
            }
        } else if (p.outgoing) {
            emit(CallEvent::Kind::Failed, p, reason);
            close_peer(p);
            if (state == CallState::Calling) reset_call();
        } else {
            if (state == CallState::Ringing) {
                emit(CallEvent::Kind::Ended, p, "caller gone");
                reset_call();
            } else {
                close_peer(p);  // never said INVITE
            }
        }
    }

    // ------------------------------------------------------------ room (host, mu held)

    std::string media_line() const {
        if (media == MediaMode::Relay) return "MEDIA mode=relay relay=" + relay;
        if (media == MediaMode::Multicast) return "MEDIA mode=multicast group=" + relay;
        return "MEDIA mode=mesh";
    }

    std::string member_line(const Peer& q) const {
        std::string l = "MEMBER id=" + std::to_string(q.id) + " name=" + q.name + " ip=" + q.ip;
        detail::ClockSample s;
        if (q.clock.best(s)) l += " off=" + std::to_string(s.offset_us);
        return l;
    }

    // everyone in the room except `to`, as MEMBER lines, the host first
    void send_roster(Peer& to) {
        send_line(to.fd, "MEMBER id=1 name=" + name + " ip=- off=0");
        for (const auto& q : peers)
            if (!q->dead && q->in_room && q.get() != &to) send_line(to.fd, member_line(*q));
    }

    // the invite was accepted
    void joined(Peer& p) {
        p.in_room = true;
        p.clock.reset();
        p.next_ping = Clock::now();  // first clock sample right away
        p.last_rx = Clock::now();
        if (state == CallState::Calling) {
            state = CallState::InCall;
            media = pref_media;
            relay = pref_relay;
            next_roster = Clock::now() + kRosterEvery;
            emit(CallEvent::Kind::Accepted, p);
        } else {
            emit(CallEvent::Kind::Joined, p);
        }
        send_line(p.fd, "WELCOME id=" + std::to_string(p.id));
        send_roster(p);
        send_line(p.fd, media_line());
        for (auto& q : peers)
            if (!q->dead && q->in_room && q.get() != &p) send_line(q->fd, member_line(p));
    }

    // ------------------------------------------------------------ commands (worker thread)

    void start_connect(Peer& p) {
        p.started = true;
        p.since = Clock::now();
        // an incoming connection whose INVITE comes in after this gets DECLINE busy

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(p.port);
        inet_pton(AF_INET, p.ip.c_str(), &a.sin_addr);

        p.fd = socket(AF_INET, SOCK_STREAM, 0);
        if (p.fd < 0) {
            lost(p, std::strerror(errno));
            return;
        }
        set_nonblocking(p.fd);
        set_nodelay(p.fd);
        // non-blocking connect: usually EINPROGRESS, and the worker's poll() tells us when
        // it's done. a blocking connect to a sleeping laptop could hang for two minutes
        if (connect(p.fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            send_invite(p);
        } else if (errno == EINPROGRESS) {
            p.conn = Conn::Connecting;
        } else {
            lost(p, std::strerror(errno));
        }
    }

    void send_invite(Peer& p) {
        p.conn = Conn::Open;
        p.since = Clock::now();  // the no-answer timer starts now
        // id= is the member id set aside for them; a phase 3 build ignores it
        if (!send_line(p.fd, "INVITE name=" + name + " id=" + std::to_string(p.id)))
            lost(p, "connection closed");
    }

    void run(const Command& c) {
        switch (c.kind) {
            case Command::Kind::Accept: {
                // if the caller already gave up, that path has told the app; nothing to add
                Peer* p = host_peer();
                if (!p) break;
                if (send_line(p->fd, "ACCEPT name=" + name)) {
                    p->in_room = true;
                    p->id = kHostId;
                    p->clock.reset();
                    p->next_ping = Clock::now();
                    p->last_rx = Clock::now();
                    emit(CallEvent::Kind::Accepted, *p);
                } else {
                    lost(*p, "connection lost");
                }
                break;
            }
            case Command::Kind::Decline:
                if (Peer* p = host_peer()) send_line(p->fd, "DECLINE reason=declined");
                reset_call();
                break;
            case Command::Kind::HangUp:
                for (auto& p : peers)
                    if (!p->dead && p->conn == Conn::Open) send_line(p->fd, "BYE");
                reset_call();
                // always report it, using the names saved when hang_up() was called: if the
                // peer hung up at the same moment, the connection may already be gone
                emit(c.was == CallState::InCall ? CallEvent::Kind::Ended : CallEvent::Kind::Failed,
                     c.peer_name, c.peer_ip, c.was == CallState::InCall ? "hung up" : "cancelled");
                break;
            case Command::Kind::SendMedia:
                for (auto& p : peers)
                    if (!p->dead && p->in_room) send_line(p->fd, media_line());
                break;
            case Command::Kind::Chat:
                send_chat(c.to, c.text);
                break;
        }
    }

    void run_commands() {
        while (!commands.empty()) {
            Command c = commands.front();
            commands.pop_front();
            run(c);
        }
        // call() and invite() only add a Peer; connecting is done here. Index loop: a
        // failed connect can end the call, but never adds to `peers`
        for (size_t i = 0; i < peers.size(); ++i)
            if (!peers[i]->dead && peers[i]->outgoing && !peers[i]->started)
                start_connect(*peers[i]);
        // text_to() someone outside our call only adds a Oneshot; same here
        for (auto& o : oneshots)
            if (!o->dead && o->outgoing && !o->started) start_oneshot(*o);
    }

    // ------------------------------------------------------------ network (worker thread)

    void accept_new() {
        for (;;) {
            sockaddr_in a{};
            socklen_t len = sizeof(a);
            // ::accept is the system call; plain accept would be Signaling::accept()
            int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
            if (fd < 0) return;  // EAGAIN: nobody else waiting
            // phase 6: everyone gets in, in a call or not, and waits as a Oneshot until its
            // first line says what it wants (a busy INVITE is declined then)
            size_t waiting = 0;
            for (const auto& o : oneshots)
                if (!o->dead && !o->outgoing) waiting++;
            if (waiting >= kMaxOneshots) {
                close(fd);
                continue;
            }
            set_nonblocking(fd);
            set_nodelay(fd);
            auto o = std::make_unique<Oneshot>();
            o->fd = fd;
            o->since = Clock::now();
            o->ip = ip_string(a);
            oneshots.push_back(std::move(o));
        }
    }

    // from the host to us, a member
    void handle_room_line(Peer& p, const wire::Line& l) {
        if (l.verb == "WELCOME") {
            uint8_t id;
            if (get_id(l, id) && id != kHostId) my_id = id;
        } else if (l.verb == "MEMBER") {
            uint8_t id;
            std::string who = wire::get(l, "name"), ip = wire::get(l, "ip");
            if (!get_id(l, id) || id == kHostId || id == me() || !valid_name(who) || !is_ipv4(ip))
                return;
            Remote r;
            r.name = who;
            r.ip = ip;
            r.have_off = wire::get_u32(l, "off", r.off);
            bool fresh = roster.count(id) == 0;
            roster[id] = r;
            if (fresh) emit(CallEvent::Kind::Joined, who, ip, "", id);
        } else if (l.verb == "LEFT") {
            uint8_t id;
            if (!get_id(l, id)) return;
            auto it = roster.find(id);
            if (it == roster.end()) return;
            std::string why = wire::get(l, "reason") == "dropped" ? "connection lost" : "hung up";
            emit(CallEvent::Kind::Left, it->second.name, it->second.ip, why, id);
            roster.erase(it);
        } else if (l.verb == "MEDIA") {
            std::string mode = wire::get(l, "mode"), rip = wire::get(l, "relay");
            std::string group = wire::get(l, "group");
            MediaMode m;
            if (mode == "mesh") {
                m = MediaMode::Mesh;
                rip.clear();
            } else if (mode == "relay" && is_ipv4(rip)) {
                m = MediaMode::Relay;
            } else if (mode == "multicast" && proto::is_multicast_ipv4(group)) {
                m = MediaMode::Multicast;
                rip = group;
            } else {
                return;
            }
            if (m == media && rip == relay) return;
            media = m;
            relay = rip;
            emit(CallEvent::Kind::MediaChanged, p);
        }
    }

    void handle_line(Peer& p, const std::string& text, uint32_t arrival_us) {
        wire::Line l;
        if (!wire::parse(text, l)) return;

        if (l.verb == "INVITE") {
            if (p.conn != Conn::AwaitInvite) return;
            if (state != CallState::Idle) {  // we started a call of our own meanwhile
                send_line(p.fd, "DECLINE reason=busy");
                close_peer(p);
                return;
            }
            p.conn = Conn::Open;
            p.name = name_or_unknown(wire::get(l, "name"));
            p.id = kHostId;
            uint8_t id;
            my_id = get_id(l, id) && id != kHostId ? id : 0;  // no id: a phase 3 caller
            host = false;
            state = CallState::Ringing;
            emit(CallEvent::Kind::Incoming, p);
        } else if (l.verb == "ACCEPT") {
            if (!p.outgoing || p.in_room || p.conn != Conn::Open) return;
            if (state != CallState::Calling && !(state == CallState::InCall && host)) return;
            p.name = name_or_unknown(wire::get(l, "name"));
            joined(p);
        } else if (l.verb == "DECLINE") {
            if (!p.outgoing || p.in_room) return;
            std::string reason = wire::get(l, "reason");
            emit(CallEvent::Kind::Declined, p, reason.empty() ? "declined" : reason);
            close_peer(p);
            if (state == CallState::Calling) reset_call();
        } else if (l.verb == "BYE") {
            if (p.in_room && host) {
                member_left(p, "peer hung up", "hung-up");
            } else if (p.in_room) {
                emit(CallEvent::Kind::Ended, p, "peer hung up");
                reset_call();
            } else if (p.outgoing) {
                lost(p, "peer hung up");
            } else if (state == CallState::Ringing) {
                emit(CallEvent::Kind::Ended, p, "caller hung up");
                reset_call();
            } else {
                close_peer(p);
            }
        } else if (l.verb == "PING") {
            uint32_t t1;
            if (p.conn == Conn::Open && wire::get_u32(l, "t1", t1))
                send_line(p.fd, "PONG t1=" + std::to_string(t1) +
                                    " t2=" + std::to_string(arrival_us) +
                                    " t3=" + std::to_string(proto::now_us()));
        } else if (l.verb == "PONG") {
            uint32_t t1, t2, t3;
            if (wire::get_u32(l, "t1", t1) && wire::get_u32(l, "t2", t2) &&
                wire::get_u32(l, "t3", t3))
                p.clock.add(detail::clock_sample(t1, t2, t3, arrival_us));
        } else if (l.verb == "CHAT") {
            if (p.in_room && state == CallState::InCall) handle_chat(p, l);
        } else if (!host && p.in_room && state == CallState::InCall) {
            handle_room_line(p, l);
        }
        // anything else is ignored, so newer versions can add messages
    }

    void on_ready(Peer& p, short revents, uint32_t arrival_us) {
        if (p.conn == Conn::Connecting) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(p.fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == ECONNREFUSED) {
                lost(p, "refused (is lancall running there?)");
            } else if (err != 0) {
                lost(p, std::strerror(err));
            } else if (revents & POLLOUT) {
                send_invite(p);
            }
            return;
        }

        char buf[1024];
        ssize_t n = recv(p.fd, buf, sizeof(buf), 0);
        if (n == 0) {
            lost(p, "connection lost");
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                lost(p, "connection lost");
            return;
        }
        p.rxbuf.append(buf, static_cast<size_t>(n));
        p.last_rx = Clock::now();

        size_t pos;
        while (!p.dead && (pos = p.rxbuf.find('\n')) != std::string::npos) {
            std::string line = p.rxbuf.substr(0, pos);
            p.rxbuf.erase(0, pos + 1);
            handle_line(p, line, arrival_us);
        }
        if (!p.dead && p.rxbuf.size() > kMaxLine) lost(p, "garbage from the other side");
    }

    void timers() {
        auto now = Clock::now();
        // index loop: lost() never adds peers, it only marks them dead
        for (size_t i = 0; i < peers.size(); ++i) {
            Peer& p = *peers[i];
            if (p.dead) continue;
            if (p.conn == Conn::Connecting && p.started && now - p.since > kConnectTimeout) {
                lost(p, "unreachable");
            } else if (p.conn == Conn::AwaitInvite && now - p.since > kInviteTimeout) {
                close_peer(p);  // someone connected and said nothing, e.g. a port scan
            } else if (p.outgoing && !p.in_room && p.conn == Conn::Open &&
                       now - p.since > kNoAnswerTimeout) {
                send_line(p.fd, "BYE");
                lost(p, "no answer");
            } else if (p.in_room && now - p.last_rx > kSilenceTimeout) {
                lost(p, "no response");
            } else if (p.in_room && now >= p.next_ping) {
                send_line(p.fd, "PING t1=" + std::to_string(proto::now_us()));
                p.next_ping = now + kPingEvery;
            }
        }
        // the host keeps everyone's off= fresh, so members can chain clock offsets
        if (host && state == CallState::InCall && now >= next_roster) {
            for (auto& p : peers)
                if (!p->dead && p->in_room) send_roster(*p);
            next_roster = now + kRosterEvery;
        }
    }

    void loop() {
        std::vector<pollfd> fds;
        std::vector<Peer*> polled;          // polled[i] goes with fds[i + 2]
        std::vector<Oneshot*> polled_once;  // then these, after the peers
        while (!stop) {
            fds.clear();
            polled.clear();
            polled_once.clear();
            {
                std::lock_guard<std::mutex> lock(mu);
                run_commands();
                reap();
                fds.push_back({wake_rd, POLLIN, 0});
                fds.push_back({listen_fd, POLLIN, 0});
                for (auto& p : peers) {
                    if (p->fd < 0) continue;
                    short ev = p->conn == Conn::Connecting ? POLLOUT : POLLIN;
                    fds.push_back({p->fd, ev, 0});
                    polled.push_back(p.get());
                }
                for (auto& o : oneshots) {
                    if (o->fd < 0) continue;
                    short ev = o->connecting ? POLLOUT : POLLIN;
                    fds.push_back({o->fd, ev, 0});
                    polled_once.push_back(o.get());
                }
            }

            // 100 ms cap so timers and pings run even when nothing happens
            poll(fds.data(), fds.size(), 100);
            uint32_t arrival_us = proto::now_us();  // t2/t4 for clock sync

            if (fds[0].revents & POLLIN) {
                char junk[64];
                while (read(wake_rd, junk, sizeof(junk)) > 0) {
                }
            }

            std::lock_guard<std::mutex> lock(mu);
            // the app may have hung up while we were in poll(): do that first, so nothing
            // below works on a call that's already over. Peers are only freed by reap(),
            // so every pointer in `polled` is still valid; closed ones are marked dead
            run_commands();
            if (fds[1].revents & POLLIN) accept_new();
            for (size_t i = 0; i < polled.size(); ++i) {
                Peer& p = *polled[i];
                if (!p.dead && p.fd == fds[i + 2].fd && fds[i + 2].revents)
                    on_ready(p, fds[i + 2].revents, arrival_us);
            }
            const size_t first_once = polled.size() + 2;
            for (size_t i = 0; i < polled_once.size(); ++i) {
                Oneshot& o = *polled_once[i];
                const pollfd& f = fds[first_once + i];
                if (!o.dead && o.fd == f.fd && f.revents)
                    on_oneshot_ready(o, f.revents, arrival_us);
            }
            timers();
            oneshot_timers();
        }
    }
};

Signaling::Signaling() : impl_(std::make_unique<Impl>()) {}
Signaling::~Signaling() {
    stop();
}

bool Signaling::start(const std::string& my_name, uint16_t port) {
    if (impl_->running) {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->error = "already running";
        return false;
    }
    impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;

    if (!valid_name(my_name)) {
        d.error = "bad name '" + my_name + "' (letters, digits, - and _, at most 32)";
        return false;
    }

    int p[2];
    if (pipe(p) < 0) {
        d.error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    d.wake_rd = p[0];
    d.wake_wr = p[1];
    set_nonblocking(d.wake_rd);
    set_nonblocking(d.wake_wr);

    d.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(d.listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (d.listen_fd < 0 || bind(d.listen_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0 ||
        listen(d.listen_fd, 8) < 0) {
        d.error = "can't listen on TCP port " + std::to_string(port) + ": " + std::strerror(errno);
        if (d.listen_fd >= 0) close(d.listen_fd);
        close(d.wake_rd);
        close(d.wake_wr);
        d.listen_fd = d.wake_rd = d.wake_wr = -1;
        return false;
    }
    set_nonblocking(d.listen_fd);

    d.name = my_name;
    d.running = true;
    d.worker = std::thread([&d] { d.loop(); });
    return true;
}

void Signaling::stop() {
    Impl& d = *impl_;
    if (!d.running) return;
    d.stop = true;
    d.wake();
    if (d.worker.joinable()) d.worker.join();

    // say goodbye so nobody sits there until a timeout
    std::lock_guard<std::mutex> lock(d.mu);
    for (auto& p : d.peers)
        if (!p->dead && p->conn == Impl::Conn::Open) send_line(p->fd, "BYE");
    d.reset_call();
    for (auto& o : d.oneshots) d.close_oneshot(*o);
    d.reap();
    close(d.listen_fd);
    close(d.wake_rd);
    close(d.wake_wr);
    d.listen_fd = d.wake_rd = d.wake_wr = -1;
    d.running = false;
}

bool Signaling::call(const std::string& ip, uint16_t port) {
    Impl& d = *impl_;
    if (!d.running || !is_ipv4(ip)) return false;

    std::lock_guard<std::mutex> lock(d.mu);
    if (d.state != CallState::Idle) return false;
    d.state = CallState::Calling;
    d.host = true;
    d.my_id = kHostId;
    auto p = std::make_unique<Impl::Peer>();
    p->outgoing = true;
    p->ip = ip;
    p->port = port;
    p->id = d.free_id();
    d.peers.push_back(std::move(p));
    d.wake();
    return true;
}

bool Signaling::invite(const std::string& ip, uint16_t port) {
    Impl& d = *impl_;
    if (!d.running || !is_ipv4(ip)) return false;

    std::unique_lock<std::mutex> lock(d.mu);
    if (d.state == CallState::Idle) {
        lock.unlock();
        return call(ip, port);
    }
    if (d.state != CallState::InCall || !d.host) return false;
    for (const auto& q : d.peers)
        if (!q->dead && q->ip == ip) return false;  // already in the room, or ringing
    uint8_t id = d.free_id();
    if (id == 0) return false;  // room full, counting invites still ringing

    auto p = std::make_unique<Impl::Peer>();
    p->outgoing = true;
    p->ip = ip;
    p->port = port;
    p->id = id;
    d.peers.push_back(std::move(p));
    d.wake();
    return true;
}

void Signaling::accept() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.running || d.state != CallState::Ringing) return;
    d.state = CallState::InCall;
    d.commands.push_back({Impl::Command::Kind::Accept, CallState::Idle, "", ""});
    d.wake();
}

void Signaling::decline() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.running || d.state != CallState::Ringing) return;
    d.state = CallState::Idle;
    d.commands.push_back({Impl::Command::Kind::Decline, CallState::Idle, "", ""});
    d.wake();
}

void Signaling::hang_up() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.running || (d.state != CallState::Calling && d.state != CallState::InCall)) return;
    std::string who, ip;
    if (const Impl::Peer* p = d.main_peer()) {
        who = p->name;
        ip = p->ip;
    }
    d.commands.push_back({Impl::Command::Kind::HangUp, d.state, who, ip});
    d.state = CallState::Idle;
    d.wake();
}

void Signaling::set_media(MediaMode mode, const std::string& address) {
    Impl& d = *impl_;
    // relay mode needs somewhere to relay through, multicast a group; bad ones are ignored
    if (mode == MediaMode::Relay && !is_ipv4(address)) return;
    if (mode == MediaMode::Multicast && !proto::is_multicast_ipv4(address)) return;
    // `relay` holds the relay's ip or the group, whichever the mode needs
    std::string rip = mode == MediaMode::Mesh ? std::string() : address;

    std::lock_guard<std::mutex> lock(d.mu);
    d.pref_media = mode;
    d.pref_relay = rip;
    // only the host decides for a call in progress
    if (!d.running || d.state != CallState::InCall || !d.host) return;
    if (d.media == mode && d.relay == rip) return;
    d.media = mode;
    d.relay = rip;
    d.emit(CallEvent::Kind::MediaChanged, d.name, "", "", kHostId);
    d.commands.push_back({Impl::Command::Kind::SendMedia, CallState::Idle, "", ""});
    d.wake();
}

MediaMode Signaling::media_mode() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state == CallState::InCall ? impl_->media : impl_->pref_media;
}

std::string Signaling::relay_ip() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const Impl& d = *impl_;
    bool in_call = d.state == CallState::InCall;
    if ((in_call ? d.media : d.pref_media) != MediaMode::Relay) return "";
    return in_call ? d.relay : d.pref_relay;
}

std::string Signaling::multicast_group() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const Impl& d = *impl_;
    bool in_call = d.state == CallState::InCall;
    if ((in_call ? d.media : d.pref_media) != MediaMode::Multicast) return "";
    return in_call ? d.relay : d.pref_relay;
}

CallState Signaling::state() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state;
}

std::string Signaling::peer_name() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->state == CallState::Idle) return "";
    const Impl::Peer* p = impl_->main_peer();
    return p ? p->name : std::string();
}

std::string Signaling::peer_ip() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->state == CallState::Idle) return "";
    const Impl::Peer* p = impl_->main_peer();
    return p ? p->ip : std::string();
}

std::vector<Member> Signaling::members() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->members_locked();
}

uint8_t Signaling::my_id() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state == CallState::InCall ? impl_->me() : 0;
}

bool Signaling::is_host() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state == CallState::InCall && impl_->host;
}

std::vector<proto::Endpoint> Signaling::media_destinations(uint16_t mesh_port) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    const Impl& d = *impl_;
    if (d.state != CallState::InCall) return {};
    if (d.media == MediaMode::Relay && !d.relay.empty()) return {{d.relay, proto::kRelayPort}};
    if (d.media == MediaMode::Multicast && !d.relay.empty()) return {{d.relay, mesh_port}};
    std::vector<proto::Endpoint> out;
    for (const auto& m : d.members_locked())
        if (!m.me) out.push_back({m.ip, mesh_port});
    return out;
}

bool Signaling::text_all(const std::string& text) {
    Impl& d = *impl_;
    if (!d.running || text.empty() || text.size() > kMaxTextBytes) return false;
    std::lock_guard<std::mutex> lock(d.mu);
    if (d.state != CallState::InCall) return false;
    Impl::Command c{Impl::Command::Kind::Chat, CallState::Idle, "", ""};
    c.text = text;
    d.commands.push_back(std::move(c));
    d.wake();
    return true;
}

bool Signaling::text_to(const std::string& ip, const std::string& text, uint16_t port) {
    Impl& d = *impl_;
    if (!d.running || !is_ipv4(ip) || text.empty() || text.size() > kMaxTextBytes) return false;
    std::lock_guard<std::mutex> lock(d.mu);
    // someone in our call: through the call
    for (const auto& m : d.members_locked())
        if (!m.me && m.ip == ip) {
            Impl::Command c{Impl::Command::Kind::Chat, CallState::Idle, "", ""};
            c.to = m.id;
            c.text = text;
            d.commands.push_back(std::move(c));
            d.wake();
            return true;
        }
    // anyone else: a connection of its own, which the worker opens
    auto o = std::make_unique<Impl::Oneshot>();
    o->outgoing = true;
    o->ip = ip;
    o->port = port;
    o->text = text;
    d.oneshots.push_back(std::move(o));
    d.wake();
    return true;
}

bool Signaling::poll_chat(ChatEvent& out) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->chats.empty()) return false;
    out = std::move(impl_->chats.front());
    impl_->chats.pop_front();
    return true;
}

bool Signaling::poll_event(CallEvent& out) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->events.empty()) return false;
    out = impl_->events.front();
    impl_->events.pop_front();
    return true;
}

bool Signaling::clock_offset(uint32_t& peer_minus_mine_us) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->state != CallState::InCall) return false;
    const Impl::Peer* p = impl_->main_peer();
    detail::ClockSample s;
    if (!p || !p->in_room || !p->clock.best(s)) return false;
    peer_minus_mine_us = s.offset_us;
    return true;
}

bool Signaling::clock_offset(uint8_t member_id, uint32_t& member_minus_mine_us) const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    Impl& d = *impl_;
    if (d.state != CallState::InCall) return false;
    if (member_id == d.me()) {
        member_minus_mine_us = 0;
        return true;
    }
    detail::ClockSample s;
    if (d.host) {
        Impl::Peer* p = d.find_id(member_id);
        if (!p || !p->in_room || !p->clock.best(s)) return false;
        member_minus_mine_us = s.offset_us;
        return true;
    }
    // a member: our offset to the host, plus (for anyone else) theirs to the host
    Impl::Peer* h = d.host_peer();
    if (!h || !h->clock.best(s)) return false;
    if (member_id == kHostId) {
        member_minus_mine_us = s.offset_us;
        return true;
    }
    auto it = d.roster.find(member_id);
    if (it == d.roster.end() || !it->second.have_off) return false;
    // (them - host) + (host - us), modulo 2^32 like every other clock value here
    member_minus_mine_us = it->second.off + s.offset_us;
    return true;
}

double Signaling::rtt_ms() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->state == CallState::Idle) return 0.0;
    const Impl::Peer* p = impl_->main_peer();
    detail::ClockSample s;
    return p && p->clock.best(s) ? s.rtt_us / 1000.0 : 0.0;
}

std::string Signaling::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

}  // namespace control
