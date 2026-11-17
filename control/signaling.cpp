// Call setup and clock sync over TCP (Person 1, control/). The message format and the
// state machine are described in signaling.hpp.
//
// Threading: one worker thread owns every socket. The public functions (call, accept, ...)
// never touch a socket: they update the state right away, so state() is instantly right,
// queue a command for the worker and poke it awake through a pipe. The worker sends and
// receives, and reports what happened as events. Everything shared sits behind one mutex.
#include "control/signaling.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
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
constexpr size_t kMaxLine = 4096;

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

std::string name_or_unknown(const std::string& n) {
    return valid_name(n) ? n : "unknown";
}

}  // namespace

struct Signaling::Impl {
    enum class Conn { None, Connecting, AwaitInvite, Open };

    struct Command {
        enum class Kind { Call, Accept, Decline, HangUp } kind;
        std::string ip;
        uint16_t port = 0;
        CallState was = CallState::Idle;  // for HangUp: what we were hanging up
        std::string peer_name, peer_ip;   // for HangUp's event, in case the conn is already gone
    };

    std::string name;
    int listen_fd = -1;
    int conn_fd = -1;
    int wake_rd = -1, wake_wr = -1;
    std::thread worker;
    std::atomic<bool> stop{false};
    std::atomic<bool> running{false};

    // everything below is shared with the API functions and guarded by mu
    mutable std::mutex mu;
    CallState state = CallState::Idle;
    Conn conn = Conn::None;
    std::string peer_name, peer_ip;
    Clock::time_point conn_since, call_since, next_ping;
    std::string rxbuf;
    std::deque<Command> commands;
    std::deque<CallEvent> events;
    detail::ClockFilter clock;
    std::string error;

    void wake() {
        char c = 1;
        if (write(wake_wr, &c, 1) < 0) {
        }  // a full pipe already means "wake up", so nothing to do
    }

    void emit(CallEvent::Kind kind, const std::string& reason = "") {
        events.push_back(CallEvent{kind, peer_name, peer_ip, reason});
    }

    // close the socket but leave the call state alone
    void drop_conn() {
        if (conn_fd >= 0) close(conn_fd);
        conn_fd = -1;
        conn = Conn::None;
        rxbuf.clear();
    }

    // close the socket and go back to Idle
    void close_conn() {
        drop_conn();
        clock.reset();
        state = CallState::Idle;
        peer_name.clear();
        peer_ip.clear();
    }

    // the connection died or timed out; tell the app in terms of where we were
    void lost(const std::string& reason) {
        if (state == CallState::InCall)
            emit(CallEvent::Kind::Ended, reason);
        else if (state == CallState::Ringing)
            emit(CallEvent::Kind::Ended, "caller gone");
        else if (state == CallState::Calling)
            emit(CallEvent::Kind::Failed, reason);
        close_conn();
    }

    void start_call() {
        clock.reset();
        next_ping = Clock::now();  // first clock sample right away
    }

    // ------------------------------------------------------------ commands (worker thread)

    void start_connect(const std::string& ip, uint16_t port) {
        // an incoming connection that hasn't said INVITE yet loses to our own call
        if (conn == Conn::AwaitInvite) {
            send_line(conn_fd, "DECLINE reason=busy");
            drop_conn();
        }
        if (conn != Conn::None) {  // shouldn't happen: commands run in order
            emit(CallEvent::Kind::Failed, "busy");
            return;
        }
        state = CallState::Calling;
        peer_ip = ip;
        peer_name.clear();
        call_since = conn_since = Clock::now();

        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);

        conn_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (conn_fd < 0) {
            lost(std::strerror(errno));
            return;
        }
        set_nonblocking(conn_fd);
        set_nodelay(conn_fd);
        // non-blocking connect: usually EINPROGRESS, and the worker's poll() tells us when
        // it's done. a blocking connect to a sleeping laptop could hang for two minutes
        if (connect(conn_fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0) {
            conn = Conn::Open;
            if (!send_line(conn_fd, "INVITE name=" + name)) lost("connection closed");
        } else if (errno == EINPROGRESS) {
            conn = Conn::Connecting;
        } else {
            lost(std::strerror(errno));
        }
    }

    void run(const Command& c) {
        switch (c.kind) {
            case Command::Kind::Call:
                start_connect(c.ip, c.port);
                break;
            case Command::Kind::Accept:
                // if the caller already gave up, that path has told the app; nothing to add
                if (conn != Conn::Open) break;
                if (send_line(conn_fd, "ACCEPT name=" + name)) {
                    start_call();
                    emit(CallEvent::Kind::Accepted);
                } else {
                    lost("connection lost");
                }
                break;
            case Command::Kind::Decline:
                if (conn == Conn::Open) send_line(conn_fd, "DECLINE reason=declined");
                if (conn != Conn::None) close_conn();
                break;
            case Command::Kind::HangUp:
                if (conn == Conn::Open) send_line(conn_fd, "BYE");
                if (conn != Conn::None) close_conn();
                // always report it, using the names saved when hang_up() was called: if the peer
                // hung up at the same moment, the connection may already be gone
                events.push_back(CallEvent{
                    c.was == CallState::InCall ? CallEvent::Kind::Ended : CallEvent::Kind::Failed,
                    c.peer_name, c.peer_ip, c.was == CallState::InCall ? "hung up" : "cancelled"});
                break;
        }
    }

    // ------------------------------------------------------------ network (worker thread)

    void accept_new() {
        for (;;) {
            sockaddr_in a{};
            socklen_t len = sizeof(a);
            // ::accept is the system call; plain accept would be Signaling::accept()
            int fd = ::accept(listen_fd, reinterpret_cast<sockaddr*>(&a), &len);
            if (fd < 0) return;  // EAGAIN: nobody else waiting
            if (state == CallState::Idle && conn == Conn::None) {
                set_nonblocking(fd);
                set_nodelay(fd);
                conn_fd = fd;
                conn = Conn::AwaitInvite;
                conn_since = Clock::now();
                peer_ip = ip_string(a);
                rxbuf.clear();
            } else {
                send_line(fd, "DECLINE reason=busy");
                close(fd);
            }
        }
    }

    void handle_line(const std::string& text, uint32_t arrival_us) {
        wire::Line l;
        if (!wire::parse(text, l)) return;

        if (l.verb == "INVITE") {
            if (conn == Conn::AwaitInvite && state == CallState::Idle) {
                conn = Conn::Open;
                peer_name = name_or_unknown(wire::get(l, "name"));
                state = CallState::Ringing;
                emit(CallEvent::Kind::Incoming);
            }
        } else if (l.verb == "ACCEPT") {
            if (state == CallState::Calling && conn == Conn::Open) {
                peer_name = name_or_unknown(wire::get(l, "name"));
                state = CallState::InCall;
                start_call();
                emit(CallEvent::Kind::Accepted);
            }
        } else if (l.verb == "DECLINE") {
            if (state == CallState::Calling) {
                std::string reason = wire::get(l, "reason");
                emit(CallEvent::Kind::Declined, reason.empty() ? "declined" : reason);
                close_conn();
            }
        } else if (l.verb == "BYE") {
            if (state == CallState::InCall)
                emit(CallEvent::Kind::Ended, "peer hung up");
            else if (state == CallState::Ringing)
                emit(CallEvent::Kind::Ended, "caller hung up");
            else if (state == CallState::Calling)
                emit(CallEvent::Kind::Failed, "peer hung up");
            close_conn();
        } else if (l.verb == "PING") {
            uint32_t t1;
            if (conn == Conn::Open && wire::get_u32(l, "t1", t1))
                send_line(conn_fd, "PONG t1=" + std::to_string(t1) +
                                       " t2=" + std::to_string(arrival_us) +
                                       " t3=" + std::to_string(proto::now_us()));
        } else if (l.verb == "PONG") {
            uint32_t t1, t2, t3;
            if (wire::get_u32(l, "t1", t1) && wire::get_u32(l, "t2", t2) &&
                wire::get_u32(l, "t3", t3))
                clock.add(detail::clock_sample(t1, t2, t3, arrival_us));
        }
        // anything else is ignored, so newer versions can add messages
    }

    void on_conn_ready(short revents, uint32_t arrival_us) {
        if (conn == Conn::Connecting) {
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(conn_fd, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err == ECONNREFUSED) {
                lost("refused (is lancall running there?)");
            } else if (err != 0) {
                lost(std::strerror(err));
            } else if (revents & POLLOUT) {
                conn = Conn::Open;
                if (!send_line(conn_fd, "INVITE name=" + name)) lost("connection closed");
            }
            return;
        }

        char buf[1024];
        ssize_t n = recv(conn_fd, buf, sizeof(buf), 0);
        if (n == 0) {
            lost("connection lost");
            return;
        }
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) lost("connection lost");
            return;
        }
        rxbuf.append(buf, static_cast<size_t>(n));

        size_t pos;
        while (conn_fd >= 0 && (pos = rxbuf.find('\n')) != std::string::npos) {
            std::string line = rxbuf.substr(0, pos);
            rxbuf.erase(0, pos + 1);
            handle_line(line, arrival_us);
        }
        if (rxbuf.size() > kMaxLine) lost("garbage from the other side");
    }

    void timers() {
        auto now = Clock::now();
        if (conn == Conn::Connecting && now - conn_since > kConnectTimeout) {
            lost("unreachable");
        } else if (conn == Conn::AwaitInvite && now - conn_since > kInviteTimeout) {
            drop_conn();  // someone connected and said nothing, e.g. a port scan
            peer_ip.clear();
        } else if (state == CallState::Calling && conn == Conn::Open &&
                   now - call_since > kNoAnswerTimeout) {
            send_line(conn_fd, "BYE");
            emit(CallEvent::Kind::Failed, "no answer");
            close_conn();
        } else if (state == CallState::InCall && conn == Conn::Open && now >= next_ping) {
            send_line(conn_fd, "PING t1=" + std::to_string(proto::now_us()));
            next_ping = now + kPingEvery;
        }
    }

    void loop() {
        while (!stop) {
            std::vector<pollfd> fds;
            int conn_idx = -1;
            {
                std::lock_guard<std::mutex> lock(mu);
                while (!commands.empty()) {
                    Command c = commands.front();
                    commands.pop_front();
                    run(c);
                }
                fds.push_back({wake_rd, POLLIN, 0});
                fds.push_back({listen_fd, POLLIN, 0});
                if (conn_fd >= 0) {
                    conn_idx = static_cast<int>(fds.size());
                    short ev = conn == Conn::Connecting ? POLLOUT : POLLIN;
                    fds.push_back({conn_fd, ev, 0});
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
            if (fds[1].revents & POLLIN) accept_new();
            // only the worker opens/closes conn_fd, so it's still the one we polled,
            // but checking costs nothing
            if (conn_idx >= 0 && conn_fd == fds[conn_idx].fd && fds[conn_idx].revents)
                on_conn_ready(fds[conn_idx].revents, arrival_us);
            timers();
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
        listen(d.listen_fd, 4) < 0) {
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

    // say goodbye so the other side doesn't sit there until a timeout
    if (d.conn == Impl::Conn::Open && d.state != CallState::Idle) send_line(d.conn_fd, "BYE");
    d.close_conn();
    close(d.listen_fd);
    close(d.wake_rd);
    close(d.wake_wr);
    d.listen_fd = d.wake_rd = d.wake_wr = -1;
    d.running = false;
}

bool Signaling::call(const std::string& ip, uint16_t port) {
    Impl& d = *impl_;
    if (!d.running) return false;
    in_addr tmp{};
    if (inet_pton(AF_INET, ip.c_str(), &tmp) != 1) return false;

    std::lock_guard<std::mutex> lock(d.mu);
    if (d.state != CallState::Idle) return false;
    d.state = CallState::Calling;
    d.peer_ip = ip;
    d.peer_name.clear();
    Impl::Command c{Impl::Command::Kind::Call, ip, port, CallState::Idle, "", ""};
    d.commands.push_back(c);
    d.wake();
    return true;
}

void Signaling::accept() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.running || d.state != CallState::Ringing) return;
    d.state = CallState::InCall;
    d.commands.push_back({Impl::Command::Kind::Accept, "", 0, CallState::Idle, "", ""});
    d.wake();
}

void Signaling::decline() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.running || d.state != CallState::Ringing) return;
    d.state = CallState::Idle;
    d.commands.push_back({Impl::Command::Kind::Decline, "", 0, CallState::Idle, "", ""});
    d.wake();
}

void Signaling::hang_up() {
    Impl& d = *impl_;
    std::lock_guard<std::mutex> lock(d.mu);
    if (!d.running || (d.state != CallState::Calling && d.state != CallState::InCall)) return;
    d.commands.push_back({Impl::Command::Kind::HangUp, "", 0, d.state, d.peer_name, d.peer_ip});
    d.state = CallState::Idle;
    d.wake();
}

CallState Signaling::state() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state;
}

std::string Signaling::peer_name() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state == CallState::Idle ? std::string() : impl_->peer_name;
}

std::string Signaling::peer_ip() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->state == CallState::Idle ? std::string() : impl_->peer_ip;
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
    detail::ClockSample s;
    if (impl_->state != CallState::InCall || !impl_->clock.best(s)) return false;
    peer_minus_mine_us = s.offset_us;
    return true;
}

double Signaling::rtt_ms() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    detail::ClockSample s;
    return impl_->clock.best(s) ? s.rtt_us / 1000.0 : 0.0;
}

std::string Signaling::error() const {
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->error;
}

}  // namespace control