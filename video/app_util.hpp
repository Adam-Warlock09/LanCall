#pragma once
// Bits shared by the video_sender and video_receiver programs (Person 1, video/).
#include <atomic>
#include <csignal>
#include <ctime>
#include <string>

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

}  // namespace video::app