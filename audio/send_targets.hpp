#pragma once
// Phase 4 helpers for audio::AudioSender (Person 2, audio/): the list of places every chunk
// goes to, and what the receivers say about the stream. Header-only and free of PortAudio, so
// tests/metrics can check them without a microphone.
//
// In mesh mode the list is every other member (N-1 copies of each chunk); in relay mode it is
// just the relay. Either way each receiver sends its reports back to the address the audio
// came from, so in a group call one sender hears from several receivers. Through the relay
// they all arrive from the relay's address, so they are told apart by the report's reporter
// byte (the receiver's member id); reporter 0 (two-person calls, older builds) falls back to
// the address the report came from.

#include <arpa/inet.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "common/protocol.hpp"

namespace audio {

inline std::string endpoint_text(const proto::Endpoint& e) {
    return e.ip + ":" + std::to_string(e.port);
}

// The usable destinations from `in`, in their original order: valid IPv4, port not 0, each
// ip:port once. If `rejected` is given it gets the dropped ones, comma separated.
inline std::vector<proto::Endpoint> clean_destinations(const std::vector<proto::Endpoint>& in,
                                                       std::string* rejected = nullptr) {
    std::vector<proto::Endpoint> out;
    std::string bad;
    for (const proto::Endpoint& e : in) {
        in_addr addr{};
        const bool valid = e.port != 0 && inet_pton(AF_INET, e.ip.c_str(), &addr) == 1;
        bool repeat = false;
        for (const proto::Endpoint& o : out) repeat = repeat || (o.ip == e.ip && o.port == e.port);
        if (repeat) continue;  // harmless, just not sent twice
        if (!valid) {
            if (!bad.empty()) bad += ", ";
            bad += endpoint_text(e);
            continue;
        }
        out.push_back(e);
    }
    if (rejected) *rejected = bad;
    return out;
}

// For the stats line: "10.8.66.20:5006", "3 destinations", "nobody (muted)".
inline std::string describe_destinations(const std::vector<proto::Endpoint>& to) {
    if (to.empty()) return "nobody (muted)";
    if (to.size() == 1) return endpoint_text(to[0]);
    return std::to_string(to.size()) + " destinations";
}

// True if `ip` is one of the destinations: reports are only taken from places we send to.
inline bool is_destination_ip(const std::vector<proto::Endpoint>& to, const std::string& ip) {
    for (const proto::Endpoint& e : to) {
        if (e.ip == ip) return true;
    }
    return false;
}

// The latest receiver report from each receiver of our stream.
class ReportBoard {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kFresh{2000};  // older = that receiver went quiet

    void on_report(uint8_t reporter, const std::string& from_ip, uint16_t loss_permille,
                   uint32_t jitter_us, Clock::time_point now) {
        const std::string key = reporter != 0 ? "id " + std::to_string(reporter) : from_ip;
        entries_[key] = Entry{loss_permille, jitter_us, now};
    }

    // after the destinations change: whoever left must not linger as "stopped"
    void clear() { entries_.clear(); }

    struct Summary {
        int heard = 0;                     // receivers that ever reported
        int fresh = 0;                     // of them, reported within kFresh
        uint16_t worst_loss_permille = 0;  // over the fresh ones
        uint32_t worst_jitter_us = 0;      // over the fresh ones (may be another receiver)
        std::string worst_loss_key;        // "id 3" or an ip
    };

    Summary summary(Clock::time_point now) const {
        Summary s;
        for (const auto& [key, e] : entries_) {
            ++s.heard;
            if (now - e.at > kFresh) continue;
            if (s.fresh == 0 || e.loss_permille > s.worst_loss_permille) {
                s.worst_loss_permille = e.loss_permille;
                s.worst_loss_key = key;
            }
            if (e.jitter_us > s.worst_jitter_us) s.worst_jitter_us = e.jitter_us;
            ++s.fresh;
        }
        return s;
    }

    // The receivers' part of the sender's stats line.
    std::string describe(Clock::time_point now) const {
        const Summary s = summary(now);
        if (s.heard == 0) return "no reports yet";
        if (s.fresh == 0) return "reports stopped";
        char buf[160];
        if (s.heard == 1) {
            std::snprintf(buf, sizeof buf, "peer loss %.1f%% jitter %.1f ms",
                          s.worst_loss_permille / 10.0, s.worst_jitter_us / 1000.0);
        } else {
            std::snprintf(buf, sizeof buf, "%d peers: worst loss %.1f%% (%s) worst jitter %.1f ms",
                          s.fresh, s.worst_loss_permille / 10.0, s.worst_loss_key.c_str(),
                          s.worst_jitter_us / 1000.0);
        }
        std::string out = buf;
        if (s.heard > s.fresh) out += " (" + std::to_string(s.heard - s.fresh) + " stopped)";
        return out;
    }

private:
    struct Entry {
        uint16_t loss_permille;
        uint32_t jitter_us;
        Clock::time_point at;
    };
    std::map<std::string, Entry> entries_;
};

}  // namespace audio
