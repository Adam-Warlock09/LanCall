// audio_loopback — local check of the PortAudio wrapper (Person 2, audio/, W3).
// Plays the microphone straight to the speaker on ONE laptop, with no network, and prints
// chunk rates, queue depth, overflows and underruns once a second.
//
// USE HEADPHONES: a laptop speaker feeding its own mic will howl.
//
// Usage: audio_loopback [seconds] [--in N] [--out N] [--list]
//   seconds   how long to run (default 10); Ctrl+C stops early
//   --in N    PortAudio input device index (default: system default)
//   --out N   PortAudio output device index (default: system default)
//   --list    list devices and exit

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "audio/pa_audio.hpp"

namespace {

std::atomic<bool> g_stop{false};
void on_sigint(int) {
    g_stop = true;
}

constexpr size_t kPrebufferChunks = 2;  // 40 ms queued before playback starts
constexpr size_t kMaxQueueChunks = 10;  // keep added delay under 200 ms
constexpr double kStartupGraceS = 0.5;  // underruns while the output buffer first fills

void usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [seconds] [--in N] [--out N] [--list]\n", prog);
}

bool parse_int(const char* s, int& out) {
    char* end = nullptr;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

// Moves every captured chunk into the playback queue, dropping chunks that would push the
// added delay over kMaxQueueChunks. Returns how many were dropped.
uint64_t pump(audio::Capture& mic, audio::RingSource& src) {
    uint64_t dropped = 0;
    audio::AudioChunk c;
    while (mic.pop(c)) {
        if (src.size() >= kMaxQueueChunks || !src.push(c)) ++dropped;
    }
    return dropped;
}

}  // namespace

int main(int argc, char** argv) {
    int seconds = 10, in_dev = -1, out_dev = -1;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--list") {
            list = true;
        } else if ((a == "--in" || a == "--out") && i + 1 < argc) {
            if (!parse_int(argv[++i], a == "--in" ? in_dev : out_dev)) {
                usage(argv[0]);
                return 2;
            }
        } else if (!parse_int(argv[i], seconds) || seconds <= 0) {
            usage(argv[0]);
            return 2;
        }
    }

    audio::PaSystem pa;
    if (!pa.ok()) {
        std::fprintf(stderr, "%s\n", pa.error().c_str());
        return 1;
    }
    if (list) {
        audio::PaSystem::list_devices(stdout);
        return 0;
    }

    audio::RingSource src;
    audio::Capture mic;
    audio::Playback spk(src);
    if (!mic.open(in_dev)) {
        std::fprintf(stderr, "mic: %s\n", mic.error().c_str());
        return 1;
    }
    if (!spk.open(out_dev)) {
        std::fprintf(stderr, "speaker: %s\n", spk.error().c_str());
        return 1;
    }
    std::signal(SIGINT, on_sigint);

    std::printf("audio_loopback: %d s, 16 kHz mono int16, 20 ms chunks. USE HEADPHONES.\n",
                seconds);
    std::printf("  input : %s\n  output: %s\n", mic.device_name().c_str(),
                spk.device_name().c_str());

    if (!mic.start()) {
        std::fprintf(stderr, "mic: %s\n", mic.error().c_str());
        return 1;
    }

    // Prebuffer before starting playback, so it does not begin by playing silence.
    using Clock = std::chrono::steady_clock;
    const auto wait_until = Clock::now() + std::chrono::seconds(2);
    uint64_t dropped = 0;
    while (src.size() < kPrebufferChunks) {
        dropped += pump(mic, src);
        if (Clock::now() > wait_until) {
            std::fprintf(stderr, "mic: no audio arrived within 2 s (muted or wrong device?)\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!spk.start()) {
        std::fprintf(stderr, "speaker: %s\n", spk.error().c_str());
        return 1;
    }
    std::printf("  latency reported by PortAudio: input %.1f ms, output %.1f ms\n\n",
                mic.latency_ms(), spk.latency_ms());

    const auto t_start = Clock::now();
    auto t_next = t_start + std::chrono::seconds(1);
    uint64_t last_cap = mic.chunks(), last_play = spk.chunks_played(), last_under = 0;
    uint64_t startup_underruns = 0;
    bool startup_done = false;
    double rate_cap_sum = 0, rate_play_sum = 0;
    int rate_samples = 0;

    while (!g_stop) {
        dropped += pump(mic, src);
        const auto now = Clock::now();
        const double elapsed = std::chrono::duration<double>(now - t_start).count();
        if (!startup_done && elapsed >= kStartupGraceS) {
            startup_underruns = spk.underruns();
            last_under = startup_underruns;
            startup_done = true;
        }
        if (now >= t_next) {
            const uint64_t cap = mic.chunks(), play = spk.chunks_played(), under = spk.underruns();
            const double cap_rate = static_cast<double>(cap - last_cap);
            const double play_rate = static_cast<double>(play - last_play);
            std::printf(
                "t=%2.0fs  captured %4.1f/s  played %4.1f/s  queue %2zu (%3.0f ms)"
                "  overflows %llu  underruns +%llu  dropped %llu  driver xruns in %llu out %llu\n",
                elapsed, cap_rate, play_rate, src.size(), src.size() * audio::kChunkMs,
                static_cast<unsigned long long>(mic.overflows()),
                static_cast<unsigned long long>(under - last_under),
                static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(mic.driver_overflows()),
                static_cast<unsigned long long>(spk.driver_underflows()));
            std::fflush(stdout);
            rate_cap_sum += cap_rate;
            rate_play_sum += play_rate;
            ++rate_samples;
            last_cap = cap;
            last_play = play;
            last_under = under;
            t_next += std::chrono::seconds(1);
            if (elapsed >= seconds - 0.001) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    spk.stop();
    mic.stop();

    const uint64_t steady_underruns = spk.underruns() - startup_underruns;
    const double avg_cap = rate_samples ? rate_cap_sum / rate_samples : 0.0;
    const double avg_play = rate_samples ? rate_play_sum / rate_samples : 0.0;
    const bool rates_ok = avg_cap > 48.0 && avg_cap < 52.0 && avg_play > 48.0 && avg_play < 52.0;
    const bool pass = rates_ok && mic.overflows() == 0 && steady_underruns <= 2;

    std::printf(
        "\nsummary: captured %.1f/s, played %.1f/s (expect 50), overflows %llu,"
        " underruns %llu after start (+%llu during start-up), dropped %llu\n",
        avg_cap, avg_play, static_cast<unsigned long long>(mic.overflows()),
        static_cast<unsigned long long>(steady_underruns),
        static_cast<unsigned long long>(startup_underruns),
        static_cast<unsigned long long>(dropped));
    std::printf("result : %s (and it should have sounded clean, without crackles)\n",
                pass ? "PASS" : "CHECK OUTPUT ABOVE");
    return pass ? 0 : 1;
}
