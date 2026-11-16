// devcheck — Phase 1 device check for one laptop (Person 2, tools/).
//
// Reports, in order:
//   Network  wired interface, IPv4 address, link speed, MTU, and warnings for Wi-Fi or NAT
//   Cameras  each /dev/video* node: delivered resolution and measured fps at 640x480
//   Audio    PortAudio devices, 1 s mic recording level at 16 kHz mono int16,
//            and a 1 s 440 Hz test tone on the default speaker
//
// Usage: devcheck [--no-camera] [--no-audio] [--no-tone]
// Exit code: 0 if nothing FAILed, 1 otherwise, 2 on bad arguments.

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <portaudio.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/core/utils/logger.hpp>
#include <opencv2/videoio.hpp>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

// Same values as proto::kAudioRate / proto::kAudioChunk in common/protocol.hpp;
// repeated here so devcheck does not depend on the shared files.
constexpr int kRate = 16000;
constexpr int kChunk = 320;  // 20 ms
constexpr double kTargetFps = 15.0;
constexpr int kWidth = 640;
constexpr int kHeight = 480;
constexpr double kPi = 3.14159265358979323846;

enum class Result { Skipped = 0, Pass = 1, Warn = 2, Fail = 3 };

const char* to_string(Result r) {
    switch (r) {
        case Result::Pass:
            return "PASS";
        case Result::Warn:
            return "WARN";
        case Result::Fail:
            return "FAIL";
        default:
            return "skipped";
    }
}

// Worst of two results (Fail > Warn > Pass > Skipped).
Result worst(Result a, Result b) {
    return a > b ? a : b;
}

void section(const char* title) {
    std::printf("\n== %s ==\n", title);
}

std::string read_first_line(const std::string& path) {
    std::ifstream f(path);
    std::string s;
    std::getline(f, s);
    return s;
}

// Hides library chatter on stderr (ALSA prints many harmless messages when PortAudio
// probes devices). Errors are still reported through return codes.
class StderrSilencer {
public:
    StderrSilencer() {
        std::fflush(stderr);
        saved_ = dup(STDERR_FILENO);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
    }
    ~StderrSilencer() {
        if (saved_ >= 0) {
            std::fflush(stderr);
            dup2(saved_, STDERR_FILENO);
            close(saved_);
        }
    }
    StderrSilencer(const StderrSilencer&) = delete;
    StderrSilencer& operator=(const StderrSilencer&) = delete;

private:
    int saved_ = -1;
};

// ---------------------------------------------------------------- Network

std::string default_route_iface() {
    std::ifstream f("/proc/net/route");
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string iface, dest;
        if (ss >> iface >> dest && dest == "00000000") return iface;
    }
    return "";
}

struct Ipv4If {
    std::string name;
    std::string ip;
    bool physical;  // has /sys/class/net/<if>/device (not docker0, virbr0, vxlan, ...)
};

std::vector<Ipv4If> ipv4_interfaces() {
    std::vector<Ipv4If> out;
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (ifaddrs* a = list; a != nullptr; a = a->ifa_next) {
        if (a->ifa_addr == nullptr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK)) continue;
        char buf[INET_ADDRSTRLEN] = {};
        const auto* sin = reinterpret_cast<const sockaddr_in*>(a->ifa_addr);
        inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof buf);
        std::string name = a->ifa_name;
        out.push_back({name, buf, fs::exists("/sys/class/net/" + name + "/device")});
    }
    freeifaddrs(list);
    return out;
}

Result check_network() {
    section("Network");
    const auto ifs = ipv4_interfaces();
    std::string iface = default_route_iface();
    if (iface.empty()) {
        for (const auto& i : ifs) {
            if (i.physical) {
                iface = i.name;
                break;
            }
        }
    }
    if (iface.empty()) {
        std::puts("  no active physical IPv4 interface (cable unplugged?)");
        return Result::Fail;
    }

    std::string ip;
    for (const auto& i : ifs) {
        if (i.name == iface) ip = i.ip;
    }
    const std::string base = "/sys/class/net/" + iface;
    const std::string speed = read_first_line(base + "/speed");
    const std::string mtu = read_first_line(base + "/mtu");
    const std::string state = read_first_line(base + "/operstate");
    const bool wireless = fs::exists(base + "/wireless");

    std::printf("  interface : %s%s\n", iface.c_str(), wireless ? " (Wi-Fi)" : " (wired)");
    std::printf("  IPv4      : %s\n", ip.empty() ? "none" : ip.c_str());
    std::printf("  state     : %s\n", state.empty() ? "unknown" : state.c_str());
    std::printf("  speed     : %s\n",
                (speed.empty() || speed[0] == '-') ? "unknown" : (speed + " Mbps").c_str());
    std::printf("  MTU       : %s\n", mtu.empty() ? "unknown" : mtu.c_str());
    for (const auto& i : ifs) {
        if (i.name != iface) {
            std::printf("  also up   : %s %s%s\n", i.name.c_str(), i.ip.c_str(),
                        i.physical ? "" : " (virtual)");
        }
    }

    Result r = Result::Pass;
    if (ip.empty()) {
        std::puts("  FAIL: interface has no IPv4 address");
        r = Result::Fail;
    }
    if (wireless) {
        std::puts("  WARN: tests and demos must use the wired wall port");
        r = worst(r, Result::Warn);
    }
    if (ip.rfind("192.168.", 0) == 0) {
        std::puts(
            "  WARN: 192.168.x.x address, probably behind a personal router (NAT);"
            " connect to the wall port");
        r = worst(r, Result::Warn);
    }
    if (mtu != "1500" && !mtu.empty()) {
        std::puts("  WARN: MTU is not 1500; our 1400-byte payloads assume a 1500 MTU");
        r = worst(r, Result::Warn);
    }
    if (speed == "100") {
        std::puts("  note: 100 Mbps adapter; record this next to every result");
    }
    return r;
}

// ---------------------------------------------------------------- Cameras

std::vector<int> video_node_ids() {
    std::vector<int> ids;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator("/dev", ec)) {
        const std::string n = e.path().filename().string();
        if (n.size() > 5 && n.rfind("video", 0) == 0 &&
            std::all_of(n.begin() + 5, n.end(), [](char c) { return c >= '0' && c <= '9'; })) {
            ids.push_back(std::stoi(n.substr(5)));
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

std::string fourcc_string(double v) {
    const int f = static_cast<int>(v);
    std::string s;
    for (int i = 0; i < 4; ++i) {
        const char c = static_cast<char>((f >> (8 * i)) & 0xff);
        s += (c >= 32 && c < 127) ? c : '?';
    }
    return s;
}

Result check_cameras() {
    section("Cameras");
    const auto ids = video_node_ids();
    if (ids.empty()) {
        std::puts("  no /dev/video* devices found");
        return Result::Fail;
    }

    Result best = Result::Fail;  // the best camera decides the section result
    for (int id : ids) {
        const std::string dev = "/dev/video" + std::to_string(id);
        std::string card =
            read_first_line("/sys/class/video4linux/video" + std::to_string(id) + "/name");
        if (card.empty()) card = "unknown";

        cv::VideoCapture cap(id, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            std::printf("  %s (%s): cannot open as a capture device\n", dev.c_str(), card.c_str());
            continue;
        }
        cap.set(cv::CAP_PROP_FRAME_WIDTH, kWidth);
        cap.set(cv::CAP_PROP_FRAME_HEIGHT, kHeight);

        // Warm-up: the first frames are slow while auto-exposure settles.
        cv::Mat frame;
        for (int i = 0; i < 5; ++i) cap.read(frame);
        if (frame.empty()) {
            std::printf("  %s (%s): opened but gave no frames (often a metadata node; normal)\n",
                        dev.c_str(), card.c_str());
            continue;
        }

        using Clock = std::chrono::steady_clock;
        int frames = 0;
        double elapsed = 0.0;
        const auto t0 = Clock::now();
        while (elapsed < 2.0) {
            if (!cap.read(frame) || frame.empty()) break;
            ++frames;
            elapsed = std::chrono::duration<double>(Clock::now() - t0).count();
        }
        const double fps = elapsed > 0.0 ? frames / elapsed : 0.0;
        const bool size_ok = frame.cols == kWidth && frame.rows == kHeight;
        const Result r = (size_ok && fps >= kTargetFps) ? Result::Pass : Result::Warn;

        std::printf("  %s (%s): %dx%d, format %s, %.1f fps over %.1f s -> %s\n", dev.c_str(),
                    card.c_str(), frame.cols, frame.rows,
                    fourcc_string(cap.get(cv::CAP_PROP_FOURCC)).c_str(), fps, elapsed,
                    to_string(r));
        if (!size_ok) std::printf("    camera did not deliver %dx%d\n", kWidth, kHeight);
        if (fps < kTargetFps) {
            std::printf(
                "    below %.0f fps; webcams often lower their rate in dim light,"
                " so retry with better lighting\n",
                kTargetFps);
        }
        best = (best == Result::Fail) ? r : std::min(best, r);
    }
    if (best == Result::Fail) std::puts("  no camera delivered frames");
    return best;
}

// ---------------------------------------------------------------- Audio

void list_audio_devices() {
    const int n = Pa_GetDeviceCount();
    if (n < 0) {
        std::printf("  Pa_GetDeviceCount failed: %s\n", Pa_GetErrorText(n));
        return;
    }
    const PaDeviceIndex def_in = Pa_GetDefaultInputDevice();
    const PaDeviceIndex def_out = Pa_GetDefaultOutputDevice();
    std::printf("  %d devices (in/out = max channels):\n", n);
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr) continue;
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        std::printf("  [%2d] %-36.36s in:%-2d out:%-2d %6.0f Hz  %s%s%s\n", i, info->name,
                    info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate,
                    api != nullptr ? api->name : "?", i == def_in ? "  <- default input" : "",
                    i == def_out ? "  <- default output" : "");
    }
}

Result check_mic() {
    if (Pa_GetDefaultInputDevice() == paNoDevice) {
        std::puts("  mic: no default input device");
        return Result::Fail;
    }
    PaStream* stream = nullptr;
    PaError err;
    {
        StderrSilencer quiet;
        err = Pa_OpenDefaultStream(&stream, 1, 0, paInt16, kRate, kChunk, nullptr, nullptr);
    }
    if (err != paNoError) {
        std::printf("  mic: cannot open default input at %d Hz mono int16: %s\n", kRate,
                    Pa_GetErrorText(err));
        return Result::Fail;
    }
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::printf("  mic: cannot start input stream: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        return Result::Fail;
    }

    std::puts("  mic: recording 1 s from the default input, say something...");
    std::vector<int16_t> buf(kChunk);
    int peak = 0;
    double sum_sq = 0.0;
    long samples = 0;
    int overflows = 0;
    Result r = Result::Pass;
    for (int c = 0; c < kRate / kChunk; ++c) {
        err = Pa_ReadStream(stream, buf.data(), kChunk);
        if (err == paInputOverflowed) {
            ++overflows;
        } else if (err != paNoError) {
            std::printf("  mic: read failed: %s\n", Pa_GetErrorText(err));
            r = Result::Fail;
            break;
        }
        for (int16_t v : buf) {
            peak = std::max(peak, std::abs(static_cast<int>(v)));
            sum_sq += static_cast<double>(v) * v;
            ++samples;
        }
    }
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    if (r == Result::Fail || samples == 0) return Result::Fail;

    const double rms = std::sqrt(sum_sq / samples);
    const double peak_db = 20.0 * std::log10(std::max(peak, 1) / 32768.0);
    const double rms_db = 20.0 * std::log10(std::max(rms, 1.0) / 32768.0);
    std::printf("  mic: peak %.1f dBFS, average (RMS) %.1f dBFS, %d input overflows\n", peak_db,
                rms_db, overflows);
    if (peak == 0) {
        std::puts("  mic: FAIL, all samples are zero (muted, or the wrong default device)");
        return Result::Fail;
    }
    if (peak_db < -50.0) {
        std::puts("  mic: WARN, very quiet; check the input volume and the default device");
        return Result::Warn;
    }
    if (peak_db > -0.5) {
        std::puts("  mic: WARN, clipping; lower the input gain");
        return Result::Warn;
    }
    return r;
}

Result check_speaker() {
    if (Pa_GetDefaultOutputDevice() == paNoDevice) {
        std::puts("  speaker: no default output device");
        return Result::Fail;
    }
    PaStream* stream = nullptr;
    PaError err;
    {
        StderrSilencer quiet;
        err = Pa_OpenDefaultStream(&stream, 0, 1, paInt16, kRate, kChunk, nullptr, nullptr);
    }
    if (err != paNoError) {
        std::printf("  speaker: cannot open default output at %d Hz mono int16: %s\n", kRate,
                    Pa_GetErrorText(err));
        return Result::Fail;
    }
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        std::printf("  speaker: cannot start output stream: %s\n", Pa_GetErrorText(err));
        Pa_CloseStream(stream);
        return Result::Fail;
    }

    std::puts("  speaker: playing a 1 s 440 Hz tone on the default output, listen for it...");
    const int total = kRate;       // 1 s
    const int ramp = kRate / 100;  // 10 ms fade in/out to avoid clicks
    std::vector<int16_t> buf(kChunk);
    int underflows = 0;
    Result r = Result::Pass;
    for (int start = 0; start < total; start += kChunk) {
        for (int i = 0; i < kChunk; ++i) {
            const int n = start + i;
            double gain = 0.3;
            if (n < ramp) gain *= static_cast<double>(n) / ramp;
            if (n > total - ramp) gain *= static_cast<double>(total - n) / ramp;
            buf[i] = static_cast<int16_t>(gain * 32767.0 * std::sin(2.0 * kPi * 440.0 * n / kRate));
        }
        err = Pa_WriteStream(stream, buf.data(), kChunk);
        if (err == paOutputUnderflowed) {
            ++underflows;
        } else if (err != paNoError) {
            std::printf("  speaker: write failed: %s\n", Pa_GetErrorText(err));
            r = Result::Fail;
            break;
        }
    }
    Pa_StopStream(stream);  // waits for queued audio to finish playing
    Pa_CloseStream(stream);
    if (r == Result::Pass) {
        std::printf(
            "  speaker: played without errors (%d output underflows);"
            " PASS only if you heard the tone\n",
            underflows);
    }
    return r;
}

void usage(const char* prog) {
    std::fprintf(stderr, "usage: %s [--no-camera] [--no-audio] [--no-tone]\n", prog);
}

}  // namespace

int main(int argc, char** argv) {
    bool do_camera = true, do_audio = true, do_tone = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--no-camera") {
            do_camera = false;
        } else if (a == "--no-audio") {
            do_audio = false;
        } else if (a == "--no-tone") {
            do_tone = false;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    char host[256] = {};
    gethostname(host, sizeof host - 1);
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_ERROR);
    std::printf("devcheck on %s  (OpenCV %s, %s)\n", host, CV_VERSION,
                Pa_GetVersionInfo()->versionText);

    Result net = check_network();
    Result cam = do_camera ? check_cameras() : Result::Skipped;

    Result mic = Result::Skipped, spk = Result::Skipped;
    if (do_audio) {
        section("Audio");
        PaError err;
        {
            StderrSilencer quiet;
            err = Pa_Initialize();
        }
        if (err != paNoError) {
            std::printf("  Pa_Initialize failed: %s\n", Pa_GetErrorText(err));
            mic = Result::Fail;
            spk = do_tone ? Result::Fail : Result::Skipped;
        } else {
            list_audio_devices();
            mic = check_mic();
            if (do_tone) spk = check_speaker();
            Pa_Terminate();
        }
    }

    section("Summary");
    std::printf("  network : %s\n", to_string(net));
    std::printf("  camera  : %s\n", to_string(cam));
    std::printf("  mic     : %s\n", to_string(mic));
    std::printf("  speaker : %s%s\n", to_string(spk),
                spk == Result::Pass ? " (if you heard the tone)" : "");

    const Result overall = worst(worst(net, cam), worst(mic, spk));
    return overall == Result::Fail ? 1 : 0;
}
