#include "audio/pa_audio.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

namespace audio {

namespace {

// Hides library chatter on stderr while PortAudio probes ALSA devices (dozens of harmless
// "ALSA lib ..." lines). Errors are still reported through PortAudio's return codes.
class StderrSilencer {
public:
    StderrSilencer() {
        std::fflush(stderr);
        saved_ = dup(STDERR_FILENO);
        const int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
    }
    ~StderrSilencer() {
        if (saved_ >= 0) {
            std::fflush(stderr);
            dup2(saved_, STDERR_FILENO);
            ::close(saved_);
        }
    }
    StderrSilencer(const StderrSilencer&) = delete;
    StderrSilencer& operator=(const StderrSilencer&) = delete;

private:
    int saved_ = -1;
};

// Opens a 16 kHz mono int16 stream on one device with its low-latency setting.
bool open_stream(bool input, int device, PaStreamCallback* cb, void* user, PaStream** stream,
                 std::string& error, std::string& name) {
    if (*stream != nullptr) {
        error = "stream already open";
        return false;
    }
    PaDeviceIndex dev = device;
    if (device < 0) dev = input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice();
    if (dev == paNoDevice) {
        error = input ? "no default input device" : "no default output device";
        return false;
    }
    if (dev >= Pa_GetDeviceCount()) {
        error = "no device with index " + std::to_string(dev) + " (run with --list)";
        return false;
    }
    const PaDeviceInfo* info = Pa_GetDeviceInfo(dev);
    if (info == nullptr || (input ? info->maxInputChannels : info->maxOutputChannels) < 1) {
        error = "device " + std::to_string(dev) + (input ? " has no input" : " has no output");
        return false;
    }
    name = info->name;

    PaStreamParameters p{};
    p.device = dev;
    p.channelCount = 1;
    p.sampleFormat = paInt16;
    p.suggestedLatency = input ? info->defaultLowInputLatency : info->defaultLowOutputLatency;
    p.hostApiSpecificStreamInfo = nullptr;

    PaError err;
    {
        StderrSilencer quiet;
        err = Pa_OpenStream(stream, input ? &p : nullptr, input ? nullptr : &p, kSampleRate,
                            kChunkSamples, paClipOff, cb, user);
    }
    if (err != paNoError) {
        *stream = nullptr;
        error = std::string("cannot open ") + (input ? "input" : "output") + " device '" + name +
                "' at 16 kHz mono int16: " + Pa_GetErrorText(err) +
                " (try the default device, or another one from --list)";
        return false;
    }
    return true;
}

bool start_stream(PaStream* stream, std::string& error) {
    if (stream == nullptr) {
        error = "stream not open";
        return false;
    }
    const PaError err = Pa_StartStream(stream);
    if (err != paNoError) {
        error = std::string("cannot start stream: ") + Pa_GetErrorText(err);
        return false;
    }
    return true;
}

void stop_stream(PaStream* stream) {
    if (stream != nullptr && Pa_IsStreamStopped(stream) == 0) Pa_StopStream(stream);
}

void close_stream(PaStream*& stream) {
    if (stream == nullptr) return;
    stop_stream(stream);
    Pa_CloseStream(stream);
    stream = nullptr;
}

}  // namespace

uint32_t steady_now_us() {
    using namespace std::chrono;
    return static_cast<uint32_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------- PaSystem

PaSystem::PaSystem() {
    PaError err;
    {
        StderrSilencer quiet;
        err = Pa_Initialize();
    }
    ok_ = (err == paNoError);
    if (!ok_) error_ = std::string("Pa_Initialize failed: ") + Pa_GetErrorText(err);
}

PaSystem::~PaSystem() {
    if (ok_) Pa_Terminate();
}

void PaSystem::list_devices(std::FILE* out) {
    const int n = Pa_GetDeviceCount();
    if (n < 0) {
        std::fprintf(out, "Pa_GetDeviceCount failed: %s\n", Pa_GetErrorText(n));
        return;
    }
    const PaDeviceIndex def_in = Pa_GetDefaultInputDevice();
    const PaDeviceIndex def_out = Pa_GetDefaultOutputDevice();
    for (int i = 0; i < n; ++i) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(i);
        if (info == nullptr) continue;
        const PaHostApiInfo* api = Pa_GetHostApiInfo(info->hostApi);
        std::fprintf(out, "[%2d] %-36.36s in:%-2d out:%-2d %6.0f Hz  %s%s%s\n", i, info->name,
                     info->maxInputChannels, info->maxOutputChannels, info->defaultSampleRate,
                     api != nullptr ? api->name : "?", i == def_in ? "  <- default input" : "",
                     i == def_out ? "  <- default output" : "");
    }
}

// ---------------------------------------------------------------- Capture

Capture::Capture(ClockFn clock) : clock_(clock != nullptr ? clock : steady_now_us) {}

Capture::~Capture() {
    close();
}

bool Capture::open(int device) {
    partial_fill_ = 0;
    return open_stream(true, device, &Capture::callback, this, &stream_, error_, device_name_);
}

bool Capture::start() {
    partial_fill_ = 0;
    return start_stream(stream_, error_);
}

void Capture::stop() {
    stop_stream(stream_);
}

void Capture::close() {
    close_stream(stream_);
}

bool Capture::pop_wait(AudioChunk& out, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!queue_.pop(out)) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

double Capture::latency_ms() const {
    const PaStreamInfo* info = stream_ != nullptr ? Pa_GetStreamInfo(stream_) : nullptr;
    return info != nullptr ? info->inputLatency * 1000.0 : 0.0;
}

int Capture::callback(const void* input, void* /*output*/, unsigned long frames,
                      const PaStreamCallbackTimeInfo* /*time*/, PaStreamCallbackFlags flags,
                      void* user) {
    static_cast<Capture*>(user)->on_input(static_cast<const int16_t*>(input), frames, flags);
    return paContinue;
}

void Capture::on_input(const int16_t* in, unsigned long frames, PaStreamCallbackFlags flags) {
    if (flags & paInputOverflow) driver_overflows_.fetch_add(1, std::memory_order_relaxed);

    unsigned long done = 0;
    while (done < frames) {
        if (partial_fill_ == 0) partial_.capture_us = clock_();
        const unsigned long room = static_cast<unsigned long>(kChunkSamples - partial_fill_);
        const unsigned long n = std::min(room, frames - done);
        int16_t* dst = partial_.samples.data() + partial_fill_;
        if (in != nullptr) {
            std::memcpy(dst, in + done, n * sizeof(int16_t));
        } else {
            std::memset(dst, 0, n * sizeof(int16_t));
        }
        partial_fill_ += static_cast<int>(n);
        done += n;

        if (partial_fill_ == kChunkSamples) {
            chunks_.fetch_add(1, std::memory_order_relaxed);
            if (!queue_.push(partial_)) overflows_.fetch_add(1, std::memory_order_relaxed);
            partial_fill_ = 0;
        }
    }
}

// ---------------------------------------------------------------- RingSource

bool RingSource::next_chunk(int16_t* out) {
    AudioChunk c;
    if (!ring_.pop(c)) return false;
    std::memcpy(out, c.samples.data(), sizeof(int16_t) * kChunkSamples);
    return true;
}

// ---------------------------------------------------------------- Playback

Playback::Playback(ChunkSource& source) : source_(source) {}

Playback::~Playback() {
    close();
}

bool Playback::open(int device) {
    current_pos_ = kChunkSamples;
    return open_stream(false, device, &Playback::callback, this, &stream_, error_, device_name_);
}

bool Playback::start() {
    current_pos_ = kChunkSamples;
    return start_stream(stream_, error_);
}

void Playback::stop() {
    stop_stream(stream_);
}

void Playback::close() {
    close_stream(stream_);
}

double Playback::latency_ms() const {
    const PaStreamInfo* info = stream_ != nullptr ? Pa_GetStreamInfo(stream_) : nullptr;
    return info != nullptr ? info->outputLatency * 1000.0 : 0.0;
}

int Playback::callback(const void* /*input*/, void* output, unsigned long frames,
                       const PaStreamCallbackTimeInfo* /*time*/, PaStreamCallbackFlags flags,
                       void* user) {
    static_cast<Playback*>(user)->on_output(static_cast<int16_t*>(output), frames, flags);
    return paContinue;
}

void Playback::on_output(int16_t* out, unsigned long frames, PaStreamCallbackFlags flags) {
    if (flags & paOutputUnderflow) driver_underflows_.fetch_add(1, std::memory_order_relaxed);

    unsigned long done = 0;
    while (done < frames) {
        if (current_pos_ == kChunkSamples) {
            if (!source_.next_chunk(current_.data())) {
                current_.fill(0);
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }
            played_.fetch_add(1, std::memory_order_relaxed);
            current_pos_ = 0;
        }
        const unsigned long room = static_cast<unsigned long>(kChunkSamples - current_pos_);
        const unsigned long n = std::min(room, frames - done);
        std::memcpy(out + done, current_.data() + current_pos_, n * sizeof(int16_t));
        current_pos_ += static_cast<int>(n);
        done += n;
    }
}

}  // namespace audio
