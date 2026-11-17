// AudioSender / AudioReceiver: the audio half of a call as reusable objects (Person 2, audio/).
// The logic that was in audio_sender.cpp / audio_receiver.cpp in phase 1 lives here now; those
// programs are thin wrappers around these classes.
//
// Rules the lancall app relies on:
//   - start() never throws: false + error() for a bad address, busy port, missing device.
//   - The classes never print and never call metrics::init(); they only log rows through
//     metrics::packet / metrics::event, so whoever owns the program owns the log file.
//   - stats() is the latest once-a-second line while running, and a whole-run summary after
//     stop().
//
// Wire format (unchanged from phase 1): 12-byte header + 320 int16 samples in network byte
// order = 652 bytes per 20 ms. The header timestamp is the chunk's capture time on
// proto::now_us(), the same clock the video sender stamps frames with.
//
// Phase 3: the receiver sends a receiver report (common/report.hpp) every 500 ms back to the
// address the audio comes from; the sender reads them and shows them in its stats line.
// Audio doesn't adapt (it's ~260 kbps, nothing to gain), the reports are for visibility.
//
// Phase 4, sending: every chunk goes to each destination in the list (all other members in
// mesh mode, just the relay in relay mode), one S log row per datagram, so the log shows the
// real upload. An empty list mutes us on the network while the mic keeps running. Reports are
// taken from any destination's address and kept per receiver (audio/send_targets.hpp).
#include "audio/audio_pipeline.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>

#include "audio/pa_audio.hpp"
#include "audio/playout_buffer.hpp"
#include "audio/send_targets.hpp"
#include "common/metrics.hpp"
#include "common/protocol.hpp"
#include "common/report.hpp"
#include "common/udp_socket.hpp"
#include "metrics/stream_stats.hpp"
#include "net/loss_counter.hpp"

static_assert(audio::kSampleRate == proto::kAudioRate, "audio and protocol sample rates differ");
static_assert(audio::kChunkSamples == proto::kAudioChunk, "audio and protocol chunk sizes differ");

namespace audio {

namespace {

constexpr size_t kPayloadBytes = sizeof(int16_t) * proto::kAudioChunk;  // 640
constexpr size_t kPacketBytes = proto::kHeaderSize + kPayloadBytes;     // 652
static_assert(kPayloadBytes <= proto::kMaxPayload, "audio chunk does not fit one packet");
constexpr int kRecvBufferBytes = 1 << 20;
constexpr int kHeadroomChunks = 5;  // trim the playout buffer above prebuffer + 100 ms

using Clock = std::chrono::steady_clock;

bool valid_ipv4(const std::string& ip) {
    in_addr addr{};
    return inet_pton(AF_INET, ip.c_str(), &addr) == 1;
}

// Logs the increase of a counter as one event row; returns the increase.
uint64_t log_delta(const char* name, uint64_t now, uint64_t& prev) {
    const uint64_t d = now > prev ? now - prev : 0;
    if (d > 0) metrics::event(name, static_cast<double>(d));
    prev = now;
    return d;
}

// A string shared between the worker thread and callers.
class SharedText {
public:
    void set(std::string s) {
        std::lock_guard<std::mutex> lock(mu_);
        text_ = std::move(s);
    }
    std::string get() const {
        std::lock_guard<std::mutex> lock(mu_);
        return text_;
    }

private:
    mutable std::mutex mu_;
    std::string text_;
};

std::string fmt(const char* format, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* format, ...) {
    char buf[512];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buf, sizeof buf, format, args);
    va_end(args);
    return std::string(buf, n > 0 ? std::min<size_t>(n, sizeof buf - 1) : 0);
}

unsigned long long ull(uint64_t v) {
    return static_cast<unsigned long long>(v);
}

}  // namespace

// ================================================================ AudioSender

struct AudioSender::Impl {
    std::unique_ptr<PaSystem> pa;
    std::unique_ptr<Capture> mic;
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    SharedText error;
    SharedText stats;

    SenderOptions opt;

    // Destinations: set_destinations() (any thread) leaves a new list here and raises the
    // flag; the worker takes it before its next chunk.
    std::mutex dest_mu;
    std::vector<proto::Endpoint> pending_dests;
    std::atomic<bool> dests_changed{false};

    void run();
    void release() {  // after the worker has finished
        mic.reset();  // closes the stream before PortAudio is terminated
        pa.reset();
        sock.reset();
    }
};

AudioSender::AudioSender() : impl_(std::make_unique<Impl>()) {}

AudioSender::~AudioSender() {
    stop();
}

bool AudioSender::start(const std::string& dest_ip, uint16_t port, const SenderOptions& opt) {
    Impl& m = *impl_;
    if (m.worker.joinable()) {
        m.error.set("audio tx: already running");
        return false;
    }
    if (!valid_ipv4(dest_ip)) {
        m.error.set("audio tx: '" + dest_ip + "' is not an IPv4 address");
        return false;
    }
    try {
        m.sock = std::make_unique<UdpSocket>();  // any free local port
    } catch (const std::exception& e) {
        m.error.set(std::string("audio tx socket: ") + e.what());
        return false;
    }
    m.pa = std::make_unique<PaSystem>();
    if (!m.pa->ok()) {
        m.error.set("audio tx: " + m.pa->error());
        m.release();
        return false;
    }
    m.mic = std::make_unique<Capture>(&proto::now_us);
    if (!m.mic->open(opt.in_device) || !m.mic->start()) {
        m.error.set("audio tx mic: " + m.mic->error());
        m.release();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m.dest_mu);
        m.pending_dests = {proto::Endpoint{dest_ip, port}};
        m.dests_changed = true;  // the worker picks the list of one up before its first chunk
    }
    m.opt = opt;
    m.error.set("");
    m.stats.set(fmt("audio tx -> %s:%u starting (mic '%s')", dest_ip.c_str(), port,
                    m.mic->device_name().c_str()));
    m.stop_requested = false;
    m.running = true;
    m.worker = std::thread([&m] { m.run(); });
    return true;
}

void AudioSender::stop() {
    Impl& m = *impl_;
    if (!m.worker.joinable()) return;
    m.stop_requested = true;
    m.worker.join();
    m.release();
    m.running = false;
}

bool AudioSender::running() const {
    return impl_->running;
}

std::string AudioSender::stats() const {
    return impl_->stats.get();
}

std::string AudioSender::error() const {
    return impl_->error.get();
}

void AudioSender::Impl::run() {
    uint8_t pkt[kPacketBytes];
    uint16_t seq = 0;
    uint64_t sent = 0, send_errors = 0, sent_at_report = 0;
    uint64_t prev_overflows = 0, prev_driver = 0, prev_errors = 0;

    const auto t_start = Clock::now();
    auto next_report = t_start + std::chrono::seconds(1);
    auto last_chunk = t_start;

    std::vector<proto::Endpoint> dests;  // where every chunk goes right now

    // receiver reports arrive on the socket we send from, one stream of them per receiver
    uint8_t rbuf[128];
    std::string rip;
    uint16_t rport = 0;
    ReportBoard reports;

    while (!stop_requested) {
        if (dests_changed.exchange(false)) {
            std::lock_guard<std::mutex> lock(dest_mu);
            dests = pending_dests;
            reports.clear();
            metrics::event("audio_destinations", static_cast<double>(dests.size()));
        }

        AudioChunk c;
        if (mic->pop_wait(c, 100)) {
            last_chunk = Clock::now();

            proto::Header h{};
            h.type = proto::Type::Audio;
            h.stream_id = opt.stream_id;
            h.seq = seq++;
            h.timestamp_us = c.capture_us;
            h.frame_id = 0;
            h.frag_index = 0;
            h.frag_count = 1;
            proto::write_header(h, pkt);

            uint8_t* payload = pkt + proto::kHeaderSize;
            for (int i = 0; i < proto::kAudioChunk; ++i) {
                const uint16_t be = htons(static_cast<uint16_t>(c.samples[i]));
                std::memcpy(payload + 2 * i, &be, sizeof be);
            }

            for (const proto::Endpoint& to : dests) {  // empty list: muted on the network
                if (sock->send_to(to.ip, to.port, pkt, kPacketBytes)) {
                    ++sent;
                    metrics::packet('S', h, kPacketBytes);
                } else {
                    ++send_errors;
                }
            }
        }

        // a couple of reports per receiver per second; don't spin on a flood
        const int max_reads = 8 + 2 * static_cast<int>(dests.size());
        for (int i = 0; i < max_reads; ++i) {
            const ssize_t rn = sock->recv_from(rbuf, sizeof rbuf, rip, rport, 0);
            if (rn <= 0) break;
            proto::Header rh{};
            proto::ReceiverReport rr;
            if (!is_destination_ip(dests, rip) ||
                !proto::read_report_packet(rbuf, static_cast<size_t>(rn), rh, rr) ||
                rr.media != proto::Type::Audio || rh.stream_id != opt.stream_id)
                continue;
            reports.on_report(rr.reporter, rip, rr.loss_permille, rr.jitter_us, Clock::now());
            metrics::event("audio_peer_loss_pct", rr.loss_permille / 10.0);
        }

        log_delta("capture_overflow", mic->overflows(), prev_overflows);
        log_delta("driver_input_overflow", mic->driver_overflows(), prev_driver);
        log_delta("send_error", send_errors, prev_errors);

        const auto now = Clock::now();
        if (now >= next_report) {
            const uint64_t n = sent - sent_at_report;
            const bool silent_mic = now - last_chunk > std::chrono::seconds(1);
            stats.set(fmt(
                "audio tx -> %s  %llu pkt/s  %.1f kbps  mic overflows %llu  "
                "send errors %llu%s  | %s",
                describe_destinations(dests).c_str(), ull(n), n * kPacketBytes * 8 / 1000.0,
                ull(mic->overflows()), ull(send_errors), silent_mic ? "  NO AUDIO FROM MIC" : "",
                dests.empty() ? "muted" : reports.describe(now).c_str()));
            sent_at_report = sent;
            next_report += std::chrono::seconds(1);
        }
    }

    mic->stop();
    const double secs = std::chrono::duration<double>(Clock::now() - t_start).count();
    stats.set(fmt(
        "audio tx total: %llu packets in %.1f s (%.1f/s), mic overflows %llu, "
        "send errors %llu",
        ull(sent), secs, secs > 0 ? sent / secs : 0.0, ull(mic->overflows()), ull(send_errors)));
}

// ================================================================ AudioReceiver

struct AudioReceiver::Impl {
    std::unique_ptr<PaSystem> pa;
    std::unique_ptr<PlayoutBuffer> playout;
    std::unique_ptr<Playback> spk;
    std::unique_ptr<UdpSocket> sock;
    std::unique_ptr<metrics::StreamStats> rx_stats;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    SharedText error;
    SharedText stats;
    mutable std::mutex spk_mu;  // guards `spk` itself for playing_capture_us() during stop()

    uint16_t port = 0;
    std::string only_from;

    void run();
    void release() {  // after the worker has finished
        {
            std::lock_guard<std::mutex> lock(spk_mu);
            spk.reset();  // stops the callback before the buffer it reads goes away
        }
        playout.reset();
        pa.reset();
        sock.reset();
        rx_stats.reset();
    }
};

AudioReceiver::AudioReceiver() : impl_(std::make_unique<Impl>()) {}

AudioReceiver::~AudioReceiver() {
    stop();
}

bool AudioReceiver::start(uint16_t port, const std::string& only_from_ip,
                          const ReceiverOptions& opt) {
    Impl& m = *impl_;
    if (m.worker.joinable()) {
        m.error.set("audio rx: already running");
        return false;
    }
    if (!only_from_ip.empty() && !valid_ipv4(only_from_ip)) {
        m.error.set("audio rx: '" + only_from_ip + "' is not an IPv4 address");
        return false;
    }
    try {
        m.sock = std::make_unique<UdpSocket>(port);
    } catch (const std::exception& e) {
        m.error.set(fmt("audio rx socket: %s (is UDP port %u free?)", e.what(), port));
        return false;
    }
    m.sock->set_recv_buffer(kRecvBufferBytes);

    m.pa = std::make_unique<PaSystem>();
    if (!m.pa->ok()) {
        m.error.set("audio rx: " + m.pa->error());
        m.release();
        return false;
    }
    const int prebuffer = std::clamp((opt.buffer_ms + 19) / 20, 1, PlayoutBuffer::kSlots / 2);
    m.playout = std::make_unique<PlayoutBuffer>(prebuffer, prebuffer + kHeadroomChunks);
    {
        std::lock_guard<std::mutex> lock(m.spk_mu);
        m.spk = std::make_unique<Playback>(*m.playout, &proto::now_us);
    }
    if (!m.spk->open(opt.out_device) || !m.spk->start()) {
        m.error.set("audio rx speaker: " + m.spk->error());
        m.release();
        return false;
    }

    m.port = port;
    m.only_from = only_from_ip;
    m.rx_stats = std::make_unique<metrics::StreamStats>("audio rx");
    m.error.set("");
    m.stats.set(fmt("audio rx waiting on UDP port %u (buffer %d ms, speaker '%s')", port,
                    prebuffer * 20, m.spk->device_name().c_str()));
    m.stop_requested = false;
    m.running = true;
    m.worker = std::thread([&m] { m.run(); });
    return true;
}

void AudioReceiver::stop() {
    Impl& m = *impl_;
    if (!m.worker.joinable()) return;
    m.stop_requested = true;
    m.worker.join();
    m.release();
    m.running = false;
}

bool AudioReceiver::running() const {
    return impl_->running;
}

bool AudioReceiver::playing_capture_us(uint32_t& out) const {
    std::lock_guard<std::mutex> lock(impl_->spk_mu);
    return impl_->spk != nullptr && impl_->spk->playing_capture_us(out);
}

std::string AudioReceiver::stats() const {
    return impl_->stats.get();
}

std::string AudioReceiver::error() const {
    return impl_->error.get();
}

void AudioReceiver::Impl::run() {
    uint8_t pkt[2048];
    int16_t samples[proto::kAudioChunk];
    std::string from_ip, sender_ip = only_from;
    uint16_t from_port = 0, sender_port = 0;
    bool receiving = false;

    // receiver reports back to the sender (phase 3)
    net::LossCounter loss;
    uint8_t sender_stream = 0;
    uint16_t report_seq = 0;
    size_t report_bytes = 0;
    auto rr_last = Clock::now();
    auto rr_at = rr_last + std::chrono::milliseconds(proto::kReportEveryMs);
    uint64_t bad = 0, other = 0, rx_errors = 0;
    uint64_t prev_bad = 0, prev_other = 0, prev_rx_err = 0;
    PlayoutBuffer::Counters prev{}, report_prev{};

    auto next_report = Clock::now() + std::chrono::seconds(1);

    while (!stop_requested) {
        const ssize_t n = sock->recv_from(pkt, sizeof pkt, from_ip, from_port, 20);
        if (n < 0) {
            ++rx_errors;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } else if (n > 0) {
            const uint32_t arrival = proto::now_us();
            proto::Header h{};
            if (!proto::read_header(pkt, static_cast<size_t>(n), h) ||
                h.type != proto::Type::Audio || static_cast<size_t>(n) != kPacketBytes) {
                ++bad;
            } else if (!sender_ip.empty() && from_ip != sender_ip) {
                ++other;  // not the peer we accept audio from
            } else {
                if (sender_ip.empty()) sender_ip = from_ip;  // lock onto the first sender
                if (sender_port != 0 && from_port != sender_port) {
                    metrics::event("sender_port_changed", from_port);  // sender restarted
                }
                sender_port = from_port;
                receiving = true;

                metrics::packet('R', h, static_cast<size_t>(n));
                rx_stats->on_packet(h.seq, h.timestamp_us, static_cast<size_t>(n), arrival);
                loss.on_packet(h.seq);
                report_bytes += static_cast<size_t>(n);
                sender_stream = h.stream_id;

                const uint8_t* payload = pkt + proto::kHeaderSize;
                for (int i = 0; i < proto::kAudioChunk; ++i) {
                    uint16_t be;
                    std::memcpy(&be, payload + 2 * i, sizeof be);
                    samples[i] = static_cast<int16_t>(ntohs(be));
                }
                playout->insert(h.seq, h.timestamp_us, samples);
            }
        }

        // Every change of a counter becomes an event row (phase 1: "log every underrun").
        const PlayoutBuffer::Counters c = playout->counters();
        log_delta("underrun", c.underruns, prev.underruns);
        log_delta("concealed", c.concealed, prev.concealed);
        log_delta("late_drop", c.late, prev.late);
        log_delta("duplicate_drop", c.duplicates, prev.duplicates);
        log_delta("out_of_range", c.out_of_range, prev.out_of_range);
        log_delta("trimmed", c.trimmed, prev.trimmed);
        log_delta("resync", c.resyncs, prev.resyncs);
        log_delta("drift_trim", c.drift_trims, prev.drift_trims);
        log_delta("bad_packet", bad, prev_bad);
        log_delta("other_sender", other, prev_other);
        log_delta("recv_error", rx_errors, prev_rx_err);

        const auto now = Clock::now();
        // report to the sender every 500 ms, at the address the audio comes from. if nothing
        // arrived, stay quiet: silence is itself the sender's sign of trouble
        if (now >= rr_at) {
            const auto iv = loss.take_interval();
            if (iv.expected > 0 && !sender_ip.empty() && sender_port != 0) {
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(now - rr_last).count();
                proto::ReceiverReport r;
                r.media = proto::Type::Audio;
                r.highest_seq = loss.highest_seq();
                r.loss_permille = iv.loss_permille();
                r.interval_ms = static_cast<uint16_t>(ms);
                r.jitter_us = static_cast<uint32_t>(rx_stats->total().jitter_ms * 1000.0);
                r.received_kbps =
                    ms > 0 ? static_cast<uint32_t>(report_bytes * 8 / static_cast<size_t>(ms)) : 0;
                const auto rp = proto::make_report_packet(sender_stream, report_seq++, r);
                sock->send_to(sender_ip, sender_port, rp.data(), rp.size());
            }
            report_bytes = 0;
            rr_last = now;
            rr_at = now + std::chrono::milliseconds(proto::kReportEveryMs);
        }
        if (now < next_report) continue;
        next_report += std::chrono::seconds(1);
        if (!receiving) {
            stats.set(fmt("audio rx waiting on UDP port %u%s%s", port,
                          sender_ip.empty() ? "" : " for ", sender_ip.c_str()));
            continue;
        }
        const auto snap = rx_stats->take_interval(proto::now_us());
        const int depth_ms = playout->depth_chunks() * 20;
        metrics::event("buffer_ms", depth_ms);
        stats.set(rx_stats->format(snap) +
                  fmt("  buffer %3d ms  concealed +%llu  underruns +%llu  late +%llu  drift +%llu"
                      "  from %s",
                      depth_ms, ull(c.concealed - report_prev.concealed),
                      ull(c.underruns - report_prev.underruns), ull(c.late - report_prev.late),
                      ull(c.drift_trims - report_prev.drift_trims), sender_ip.c_str()));
        report_prev = c;
    }

    const PlayoutBuffer::Counters c = playout->counters();
    stats.set("total " + rx_stats->format(rx_stats->total()) +
              fmt(" | played %llu concealed %llu underruns %llu late %llu duplicates %llu "
                  "out-of-range %llu trimmed %llu drift-trimmed %llu resyncs %llu | dropped %llu "
                  "malformed, %llu from other senders",
                  ull(c.played), ull(c.concealed), ull(c.underruns), ull(c.late), ull(c.duplicates),
                  ull(c.out_of_range), ull(c.trimmed), ull(c.drift_trims), ull(c.resyncs), ull(bad),
                  ull(other)));
}

// ---- phase 4 placeholders (Person 2's phase 4 branch fills these in) -------------------

void AudioSender::set_destinations(const std::vector<proto::Endpoint>& to) {
    Impl& m = *impl_;
    std::string rejected;
    std::vector<proto::Endpoint> clean = clean_destinations(to, &rejected);
    if (!rejected.empty()) m.error.set("audio tx: ignored bad destination(s) " + rejected);
    std::lock_guard<std::mutex> lock(m.dest_mu);
    m.pending_dests = std::move(clean);
    m.dests_changed = true;
}

void AudioReceiver::set_allowed_sources(const std::vector<std::string>& ips) {
    (void)ips;  // TODO phase 4: one playout buffer per stream, mixed
}

bool AudioReceiver::playing_capture_us(uint8_t stream_id, uint32_t& out) const {
    (void)stream_id;  // TODO phase 4: per stream. for now there is only one
    return playing_capture_us(out);
}

std::vector<uint8_t> AudioReceiver::active_streams() const {
    return {};  // TODO phase 4
}

}  // namespace audio
