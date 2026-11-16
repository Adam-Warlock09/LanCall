#pragma once
// PortAudio wrapper (Person 2, audio/): microphone capture and speaker playback in fixed
// 20 ms chunks of 16 kHz mono int16, the format of one audio packet.
//
//   audio::PaSystem pa;               // first: initialises PortAudio, must outlive the streams
//   audio::Capture mic(&proto::now_us);
//   mic.open(); mic.start();
//   audio::AudioChunk c; mic.pop_wait(c, 100);   // in the sender thread
//
//   MySource src;                     // implements audio::ChunkSource
//   audio::Playback spk(src);
//   spk.open(); spk.start();          // the callback pulls 20 ms at a time from src
//
// Real-time rule: the PortAudio callbacks only copy memory and touch atomics. They never
// lock, allocate, log or print. Counters are read from other threads.

#include <portaudio.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

#include "audio/spsc_ring.hpp"

namespace audio {

// Same values as proto::kAudioRate and proto::kAudioChunk in common/protocol.hpp
// (audio_sender/receiver static_assert that they match).
constexpr int kSampleRate = 16000;
constexpr int kChunkSamples = 320;  // 20 ms
constexpr double kChunkMs = 1000.0 * kChunkSamples / kSampleRate;

// Returns the current time in microseconds, truncated to 32 bits. Must be safe to call from
// an audio callback. The sender passes &proto::now_us so audio and video share one clock.
using ClockFn = uint32_t (*)();

// Default clock: std::chrono::steady_clock in microseconds, truncated to 32 bits.
uint32_t steady_now_us();

struct AudioChunk {
    uint32_t capture_us = 0;  // clock value when the chunk's first samples were captured
    std::array<int16_t, kChunkSamples> samples{};
};

// Initialises PortAudio for the lifetime of the object. Create it before any Capture or
// Playback and destroy it after them.
class PaSystem {
public:
    PaSystem();
    ~PaSystem();
    PaSystem(const PaSystem&) = delete;
    PaSystem& operator=(const PaSystem&) = delete;

    bool ok() const { return ok_; }
    const std::string& error() const { return error_; }

    // Prints every device with its channels, default rate and host API; marks the defaults.
    static void list_devices(std::FILE* out);

private:
    bool ok_ = false;
    std::string error_;
};

// Microphone capture. The callback cuts the input into exact 320-sample chunks, stamps each
// with the clock, and queues it; the sender thread takes chunks with pop()/pop_wait().
class Capture {
public:
    static constexpr size_t kQueueChunks = 64;  // 1.28 s of audio

    explicit Capture(ClockFn clock = steady_now_us);
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;

    // device = PortAudio device index, or -1 for the default input. False + error() on failure.
    bool open(int device = -1);
    bool start();
    void stop();
    void close();

    bool pop(AudioChunk& out) { return queue_.pop(out); }
    // Waits up to timeout_ms for a chunk (polls every millisecond). False on timeout.
    bool pop_wait(AudioChunk& out, int timeout_ms);
    size_t queued() const { return queue_.size(); }

    uint64_t chunks() const { return chunks_.load(std::memory_order_relaxed); }
    // Chunks dropped because the consumer fell behind and the queue was full.
    uint64_t overflows() const { return overflows_.load(std::memory_order_relaxed); }
    // Times the driver reported that input samples were lost before reaching us.
    uint64_t driver_overflows() const { return driver_overflows_.load(std::memory_order_relaxed); }
    double latency_ms() const;
    const std::string& error() const { return error_; }
    std::string device_name() const { return device_name_; }

private:
    static int callback(const void* input, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo* time, PaStreamCallbackFlags flags,
                        void* user);
    void on_input(const int16_t* in, unsigned long frames, PaStreamCallbackFlags flags);

    ClockFn clock_;
    PaStream* stream_ = nullptr;
    std::string error_;
    std::string device_name_;

    AudioChunk partial_;  // chunk being filled (callback thread only)
    int partial_fill_ = 0;

    SpscRing<AudioChunk, kQueueChunks> queue_;
    std::atomic<uint64_t> chunks_{0};
    std::atomic<uint64_t> overflows_{0};
    std::atomic<uint64_t> driver_overflows_{0};
};

// Where Playback gets its audio. next_chunk() runs inside the audio callback, so it must not
// block, allocate, log or print. Fill exactly kChunkSamples samples and return true, or
// return false to have silence played (counted as an underrun).
class ChunkSource {
public:
    virtual ~ChunkSource() = default;
    virtual bool next_chunk(int16_t* out) = 0;
};

// Simplest source: a FIFO of chunks filled by one other thread. Used by audio_loopback;
// audio_receiver uses its own playout buffer (ordered by sequence number) instead.
class RingSource : public ChunkSource {
public:
    static constexpr size_t kQueueChunks = 64;

    bool push(const AudioChunk& c) { return ring_.push(c); }
    size_t size() const { return ring_.size(); }
    bool next_chunk(int16_t* out) override;

private:
    SpscRing<AudioChunk, kQueueChunks> ring_;
};

// Speaker playback. The callback pulls one 20 ms chunk at a time from the source.
class Playback {
public:
    explicit Playback(ChunkSource& source);
    ~Playback();
    Playback(const Playback&) = delete;
    Playback& operator=(const Playback&) = delete;

    // device = PortAudio device index, or -1 for the default output. False + error() on failure.
    bool open(int device = -1);
    bool start();
    void stop();
    void close();

    uint64_t chunks_played() const { return played_.load(std::memory_order_relaxed); }
    // 20 ms chunks of silence played because the source had nothing ready.
    uint64_t underruns() const { return underruns_.load(std::memory_order_relaxed); }
    // Times the driver reported that it ran out of output data.
    uint64_t driver_underflows() const {
        return driver_underflows_.load(std::memory_order_relaxed);
    }
    double latency_ms() const;
    const std::string& error() const { return error_; }
    std::string device_name() const { return device_name_; }

private:
    static int callback(const void* input, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo* time, PaStreamCallbackFlags flags,
                        void* user);
    void on_output(int16_t* out, unsigned long frames, PaStreamCallbackFlags flags);

    ChunkSource& source_;
    PaStream* stream_ = nullptr;
    std::string error_;
    std::string device_name_;

    std::array<int16_t, kChunkSamples> current_{};  // chunk being played (callback thread only)
    int current_pos_ = kChunkSamples;               // == kChunkSamples: need a new chunk

    std::atomic<uint64_t> played_{0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> driver_underflows_{0};
};

}  // namespace audio
