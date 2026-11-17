#pragma once
// Bits shared by the video_sender and video_receiver programs (Person 1, video/).
#include <atomic>
#include <csignal>
#include <ctime>
#include <string>
#include <vector>

namespace video::app {

inline std::atomic<bool> g_stop{false};

inline void install_stop_handlers() {
    std::signal(SIGINT, [](int) { g_stop = true; });
    std::signal(SIGTERM, [](int) { g_stop = true; });
}

// logs/<prefix>_<date>_<time>.csv, so runs don't overwrite each other
inline std::string default_log(const std::string& prefix) {
    std::time_t t = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&t));
    return "logs/" + prefix + "_" + stamp + ".csv";
}

// "a,b,c" -> {"a", "b", "c"}; empty parts are dropped
inline std::vector<std::string> split(const std::string& s, char sep = ',') {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t end = s.find(sep, start);
        if (end == std::string::npos) end = s.size();
        if (end > start) out.push_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

}  // namespace video::app