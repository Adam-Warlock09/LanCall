// The platform layer (Person 1, common/, phase 8 kickoff). See platform.hpp.
#include "platform.hpp"

#if defined(_WIN32)
// ---------------------------------------------------------------------------- Windows

// windows.h first: mmsystem.h needs its types
#include <windows.h>

#include <mmsystem.h>  // timeBeginPeriod (winmm)
#include <process.h>   // _getpid

#include <atomic>
#include <cstdio>
#include <deque>
#include <iostream>
#include <mutex>
#include <thread>

namespace platform {

namespace {

std::atomic<void (*)()> g_interrupt{nullptr};

BOOL WINAPI on_console_event(DWORD event) {
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            if (auto fn = g_interrupt.load()) fn();
            return TRUE;  // handled: don't kill the process, the program stops by itself
        case CTRL_CLOSE_EVENT:
            // the console window is closing: Windows ends the process once this returns, so
            // give the main loop a moment to stop and finish writing its log
            if (auto fn = g_interrupt.load()) fn();
            Sleep(2000);
            return TRUE;
        default:
            return FALSE;
    }
}

}  // namespace

bool init() {
    static bool ok = [] {
        WSADATA wsa;
        const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
        timeBeginPeriod(1);  // 1 ms sleeps and timeouts instead of 15.6 ms
        SetConsoleOutputCP(CP_UTF8);
        return started;
    }();  // a function-local static is initialised once, thread-safely
    return ok;
}

void on_interrupt(void (*fn)()) {
    g_interrupt.store(fn);
    static bool installed = SetConsoleCtrlHandler(on_console_event, TRUE) != 0;
    (void)installed;
}

bool local_time(std::time_t t, std::tm& out) {
    return localtime_s(&out, &t) == 0;
}

int process_id() {
    return _getpid();
}

bool is_local_address(const std::string& ip) {
    init();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return false;
    const SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) return false;
    const bool ok = bind(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0;
    closesocket(fd);
    return ok;
}

std::string local_address_towards(const std::string& peer_ip, uint16_t port) {
    init();
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, peer_ip.c_str(), &a.sin_addr) != 1) return "";
    const SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) return "";
    sockaddr_in local{};
    int len = sizeof local;
    const bool ok = connect(fd, reinterpret_cast<const sockaddr*>(&a), sizeof a) == 0 &&
                    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &len) == 0;
    closesocket(fd);
    char buf[INET_ADDRSTRLEN] = {};
    if (!ok || inet_ntop(AF_INET, &local.sin_addr, buf, sizeof buf) == nullptr) return "";
    return buf;
}

// ---- LineReader: Windows can't wait on stdin with a timeout, so a thread sits in a blocking
// getline. The destructor can't interrupt that read, so it lets the thread go (detach); the
// state it writes to is shared, so it stays valid, and the process exit ends the thread.

namespace {
struct LineState {
    std::atomic<bool> stop{false};
    std::atomic<bool> closed{false};
    std::mutex mu;
    std::deque<std::string> lines;
};
}  // namespace

struct LineReader::Impl {
    std::shared_ptr<LineState> state = std::make_shared<LineState>();
    std::thread thread;
};

LineReader::LineReader() : impl_(std::make_unique<Impl>()) {
    impl_->thread = std::thread([st = impl_->state] {
        std::string line;
        while (!st->stop && std::getline(std::cin, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::lock_guard<std::mutex> lock(st->mu);
            st->lines.push_back(line);
        }
        st->closed = true;
    });
}

LineReader::~LineReader() {
    impl_->state->stop = true;
    impl_->thread.detach();
}

bool LineReader::next(std::string& line) {
    std::lock_guard<std::mutex> lock(impl_->state->mu);
    if (impl_->state->lines.empty()) return false;
    line = std::move(impl_->state->lines.front());
    impl_->state->lines.pop_front();
    return true;
}

bool LineReader::closed() const {
    return impl_->state->closed;
}

// ---- StderrSilencer: no ALSA on Windows, nothing to hide

StderrSilencer::StderrSilencer() {
    (void)saved_;
}

StderrSilencer::~StderrSilencer() {}

}  // namespace platform

#else

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace platform {

namespace {

std::atomic<void (*)()> g_interrupt{nullptr};

void on_signal(int) {
    if (auto fn = g_interrupt.load()) fn();
}

}  // namespace

bool init() {
    return true;
}

void on_interrupt(void (*fn)()) {
    g_interrupt.store(fn);
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
}

bool local_time(std::time_t t, std::tm& out) {
    return localtime_r(&t, &out) != nullptr;
}

int process_id() {
    return static_cast<int>(getpid());
}

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

std::string local_address_towards(const std::string& peer_ip, uint16_t port) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
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

// ---- LineReader: poll() with a timeout, so the destructor never waits on a blocked read

struct LineReader::Impl {
    std::atomic<bool> stop{false};
    std::atomic<bool> closed{false};
    std::mutex mu;
    std::deque<std::string> lines;
    std::thread thread;

    void run() {
        std::string partial;
        char buf[256];
        while (!stop) {
            pollfd p{STDIN_FILENO, POLLIN, 0};
            if (poll(&p, 1, 100) <= 0) continue;
            const ssize_t n = read(STDIN_FILENO, buf, sizeof buf);
            if (n <= 0) {  // stdin closed (e.g. started in the background): stop reading
                closed = true;
                return;
            }
            partial.append(buf, static_cast<size_t>(n));
            for (size_t nl; (nl = partial.find('\n')) != std::string::npos;) {
                std::string line = partial.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                partial.erase(0, nl + 1);
                std::lock_guard<std::mutex> lock(mu);
                lines.push_back(std::move(line));
            }
        }
    }
};

LineReader::LineReader() : impl_(std::make_unique<Impl>()) {
    impl_->thread = std::thread([this] { impl_->run(); });
}

LineReader::~LineReader() {
    impl_->stop = true;
    impl_->thread.join();
}

bool LineReader::next(std::string& line) {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->lines.empty()) return false;
    line = std::move(impl_->lines.front());
    impl_->lines.pop_front();
    return true;
}

bool LineReader::closed() const {
    return impl_->closed;
}

// ---- StderrSilencer

StderrSilencer::StderrSilencer() {
    std::fflush(stderr);
    saved_ = dup(STDERR_FILENO);
    const int devnull = open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
        dup2(devnull, STDERR_FILENO);
        ::close(devnull);
    }
}

StderrSilencer::~StderrSilencer() {
    if (saved_ >= 0) {
        std::fflush(stderr);
        dup2(saved_, STDERR_FILENO);
        ::close(saved_);
    }
}

}  // namespace platform

#endif
