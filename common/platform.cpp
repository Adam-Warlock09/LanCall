// The platform layer (Person 1, common/, phase 8 kickoff). See platform.hpp.
#include "platform.hpp"

#if defined(_WIN32)
// Phase 8 (Aric): the Windows half. What each function needs:
//   init               WSAStartup(MAKEWORD(2, 2)) once (std::call_once), timeBeginPeriod(1)
//                      (link winmm), SetConsoleOutputCP(CP_UTF8)
//   on_interrupt       SetConsoleCtrlHandler: CTRL_C_EVENT, CTRL_BREAK_EVENT, CTRL_CLOSE_EVENT
//   local_time         localtime_s (arguments the other way round from localtime_r)
//   process_id         _getpid (<process.h>)
//   is_local_address,
//   local_address_towards  the same code with SOCKET / INVALID_SOCKET / closesocket
//   LineReader         stdin can't be polled: a thread doing blocking std::getline, detached
//                      (or CancelSynchronousIo) so the destructor never waits on it
//   StderrSilencer     nothing to do
#error "Windows support is phase 8: fill in the Windows half of common/platform.cpp"
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
