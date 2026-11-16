#pragma once
// Small helpers shared by audio_sender and audio_receiver (Person 2, audio/).

#include <arpa/inet.h>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <ctime>
#include <string>

namespace audio::app {

// Set by Ctrl+C / SIGTERM so the programs exit normally and the CSV log is fully written.
inline std::atomic<bool> g_stop{false};

inline void on_stop_signal(int) {
    g_stop.store(true);
}

inline void install_stop_handlers() {
    std::signal(SIGINT, on_stop_signal);
    std::signal(SIGTERM, on_stop_signal);
}

// Parses a whole decimal integer in [lo, hi].
inline bool parse_int(const char* s, int lo, int hi, int& out) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

inline bool is_ipv4(const std::string& s) {
    in_addr addr{};
    return inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

// "logs/<prefix>_YYYYmmdd_HHMMSS.csv" in local time, so runs never overwrite each other.
inline std::string default_log_path(const std::string& prefix) {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
    return "logs/" + prefix + "_" + stamp + ".csv";
}

}  // namespace audio::app
