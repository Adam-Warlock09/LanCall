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
//
// Phase 4, receiving: one playout buffer per stream_id, mixed in the speaker callback
// (audio/mixer.hpp), with an audio clock per stream for lip sync. Packets are accepted from
// the allowed ips (members, or just the relay; empty = anyone), and each stream gets its own
// receiver report, sent to where that stream comes from, with our member id as reporter.
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

#include "audio/mixer.hpp"
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
    if (!opt.multicast_if.empty() && !m.sock->set_multicast_interface(opt.multicast_if)) {
        m.error.set("audio tx: can't send multicast from " + opt.multicast_if);
        m.sock.reset();
        return false;
    }
    m.sock->set_multicast_loop(opt.multicast_loop);  // only matters when sending to a group
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
            if (!accepts_reports_from(dests, rip) ||
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

namespace {

// What the receive thread keeps for each stream (phase 4: one per sender in the room).
struct StreamRx {
    std::unique_ptr<metrics::StreamStats> stats;  // created at the stream's first packet
    net::LossCounter loss;
    std::string ip;  // where its packets come from: the sender itself, or the relay
    uint16_t port = 0;
    Clock::time_point last_packet{};
    uint16_t report_seq = 0;
    size_t report_bytes = 0;
    PlayoutBuffer::Counters prev{};         // for the event rows
    PlayoutBuffer::Counters second_prev{};  // for the once-a-second line
};

// "audio rx" outside group calls (stream 0), "audio rx s3" for member 3
std::string stream_label(int id) {
    return id == 0 ? "audio rx" : "audio rx s" + std::to_string(id);
}

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty()) out += sep;
        out += p;
    }
    return out;
}

}  // namespace

struct AudioReceiver::Impl {
    std::unique_ptr<PaSystem> pa;
    std::unique_ptr<StreamMixer> mixer;
    std::unique_ptr<Playback> spk;
    std::unique_ptr<UdpSocket> sock;
    std::thread worker;
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> running{false};
    SharedText error;
    SharedText stats;
    // guards `spk` and `mixer` themselves, for the clock queries from other threads during stop()
    mutable std::mutex spk_mu;

    uint16_t port = 0;
    ReceiverOptions opt;

    std::mutex group_mu;  // phase 7: the multicast group joined, if any
    std::string group, group_iface;

    // Allowed source ips: set_allowed_sources() (any thread) leaves a list here and raises the
    // flag; the worker takes it before its next packet. Empty = anyone.
    std::mutex src_mu;
    std::vector<std::string> pending_sources;
    std::atomic<bool> sources_changed{false};

    void run();
    void release() {  // after the worker has finished
        {
            std::lock_guard<std::mutex> lock(spk_mu);
            spk.reset();  // stops the callback before the buffers it reads go away
            mixer.reset();
        }
        pa.reset();
        sock.reset();
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
    {
        std::lock_guard<std::mutex> lock(m.spk_mu);
        m.mixer = std::make_unique<StreamMixer>(prebuffer, prebuffer + kHeadroomChunks);
        m.spk = std::make_unique<Playback>(*m.mixer, &proto::now_us);
    }
    if (!m.spk->open(opt.out_device) || !m.spk->start()) {
        m.error.set("audio rx speaker: " + m.spk->error());
        m.release();
        return false;
    }

    m.port = port;
    m.opt = opt;
    {
        std::lock_guard<std::mutex> lock(m.src_mu);
        m.pending_sources.clear();
        if (!only_from_ip.empty()) m.pending_sources.push_back(only_from_ip);
        m.sources_changed = true;
    }
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
    leave_group();
    m.release();
    m.running = false;
}

bool AudioReceiver::running() const {
    return impl_->running;
}

bool AudioReceiver::playing_capture_us(uint32_t& out) const {
    // Playback's own clock follows the lowest stream id with a timed chunk (StreamMixer),
    // which is the one stream of a two-person call
    std::lock_guard<std::mutex> lock(impl_->spk_mu);
    return impl_->spk != nullptr && impl_->spk->playing_capture_us(out);
}

bool AudioReceiver::playing_capture_us(uint8_t stream_id, uint32_t& out) const {
    std::lock_guard<std::mutex> lock(impl_->spk_mu);
    return impl_->mixer != nullptr &&
           impl_->mixer->playing_capture_us(stream_id, proto::now_us(), out);
}

std::vector<uint8_t> AudioReceiver::active_streams() const {
    std::lock_guard<std::mutex> lock(impl_->spk_mu);
    if (impl_->mixer == nullptr) return {};
    return impl_->mixer->active_streams(proto::now_us());
}

void AudioReceiver::set_allowed_sources(const std::vector<std::string>& ips) {
    Impl& m = *impl_;
    std::vector<std::string> clean;
    for (const std::string& ip : ips) {
        if (valid_ipv4(ip)) {
            clean.push_back(ip);
        } else {
            m.error.set("audio rx: ignored bad source '" + ip + "'");
        }
    }
    if (!ips.empty() && clean.empty()) return;  // all bad: keep the old list, never "anyone"
    std::lock_guard<std::mutex> lock(m.src_mu);
    m.pending_sources = std::move(clean);
    m.sources_changed = true;
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
    std::string from_ip;
    uint16_t from_port = 0;
    std::vector<std::string> allowed;  // empty = anyone

    std::array<StreamRx, StreamMixer::kStreams> streams;
    auto rr_last = Clock::now();
    auto rr_at = rr_last + std::chrono::milliseconds(proto::kReportEveryMs);
    uint64_t bad = 0, other = 0, bad_stream = 0, rx_errors = 0;
    uint64_t prev_bad = 0, prev_other = 0, prev_bad_stream = 0, prev_rx_err = 0;

    auto next_report = Clock::now() + std::chrono::seconds(1);

    while (!stop_requested) {
        if (sources_changed.exchange(false)) {
            std::lock_guard<std::mutex> lock(src_mu);
            allowed = pending_sources;
        }

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
            } else if (!allowed.empty() &&
                       std::find(allowed.begin(), allowed.end(), from_ip) == allowed.end()) {
                ++other;  // not someone we accept audio from (not in the call, or not the relay)
            } else if (!StreamMixer::valid_stream(h.stream_id)) {
                ++bad_stream;  // member ids only go up to proto::kMaxMembers
            } else {
                StreamRx& s = streams[h.stream_id];
                if (!s.stats)
                    s.stats = std::make_unique<metrics::StreamStats>(stream_label(h.stream_id));
                if (s.port != 0 && (from_port != s.port || from_ip != s.ip)) {
                    metrics::event("sender_port_changed", from_port);  // sender restarted
                }
                s.ip = from_ip;
                s.port = from_port;
                s.last_packet = Clock::now();

                metrics::packet('R', h, static_cast<size_t>(n));
                s.stats->on_packet(h.seq, h.timestamp_us, static_cast<size_t>(n), arrival);
                s.loss.on_packet(h.seq);
                s.report_bytes += static_cast<size_t>(n);

                const uint8_t* payload = pkt + proto::kHeaderSize;
                for (int i = 0; i < proto::kAudioChunk; ++i) {
                    uint16_t be;
                    std::memcpy(&be, payload + 2 * i, sizeof be);
                    samples[i] = static_cast<int16_t>(ntohs(be));
                }
                mixer->insert(h.stream_id, h.seq, h.timestamp_us, samples);
            }
        }

        // Every change of a counter becomes an event row (phase 1: "log every underrun").
        // Same names for every stream: the stream column of the R rows tells them apart.
        for (int i = 0; i < StreamMixer::kStreams; ++i) {
            StreamRx& s = streams[i];
            if (!s.stats) continue;
            const PlayoutBuffer::Counters c = mixer->counters(static_cast<uint8_t>(i));
            log_delta("underrun", c.underruns, s.prev.underruns);
            log_delta("concealed", c.concealed, s.prev.concealed);
            log_delta("late_drop", c.late, s.prev.late);
            log_delta("duplicate_drop", c.duplicates, s.prev.duplicates);
            log_delta("out_of_range", c.out_of_range, s.prev.out_of_range);
            log_delta("trimmed", c.trimmed, s.prev.trimmed);
            log_delta("resync", c.resyncs, s.prev.resyncs);
            log_delta("drift_trim", c.drift_trims, s.prev.drift_trims);
        }
        log_delta("bad_packet", bad, prev_bad);
        log_delta("other_sender", other, prev_other);
        log_delta("bad_stream_id", bad_stream, prev_bad_stream);
        log_delta("recv_error", rx_errors, prev_rx_err);

        const auto now = Clock::now();
        // report on each stream every 500 ms, to the address it comes from (through the relay,
        // the relay passes it on to the sender). if nothing arrived, stay quiet: silence is
        // itself the sender's sign of trouble
        if (now >= rr_at) {
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - rr_last).count();
            for (int i = 0; i < StreamMixer::kStreams; ++i) {
                StreamRx& s = streams[i];
                if (!s.stats) continue;
                const auto iv = s.loss.take_interval();
                if (iv.expected > 0 && s.port != 0) {
                    proto::ReceiverReport r;
                    r.media = proto::Type::Audio;
                    r.reporter = opt.my_id;
                    r.highest_seq = s.loss.highest_seq();
                    r.loss_permille = iv.loss_permille();
                    r.interval_ms = static_cast<uint16_t>(ms);
                    r.jitter_us = static_cast<uint32_t>(s.stats->total().jitter_ms * 1000.0);
                    r.received_kbps =
                        ms > 0 ? static_cast<uint32_t>(s.report_bytes * 8 / static_cast<size_t>(ms))
                               : 0;
                    const auto rp =
                        proto::make_report_packet(static_cast<uint8_t>(i), s.report_seq++, r);
                    sock->send_to(s.ip, s.port, rp.data(), rp.size());
                }
                s.report_bytes = 0;
            }
            rr_last = now;
            rr_at = now + std::chrono::milliseconds(proto::kReportEveryMs);
        }
        if (now < next_report) continue;
        next_report += std::chrono::seconds(1);

        // One line. A stream is listed while its packets keep coming; every stream's interval
        // is taken anyway, so each one stays exactly a second long.
        const uint32_t now_us = proto::now_us();
        std::vector<std::string> full, brief;
        for (int i = 0; i < StreamMixer::kStreams; ++i) {
            StreamRx& s = streams[i];
            if (!s.stats) continue;
            const auto snap = s.stats->take_interval(now_us);
            const PlayoutBuffer::Counters c = mixer->counters(static_cast<uint8_t>(i));
            const PlayoutBuffer::Counters p = s.second_prev;
            s.second_prev = c;
            if (now - s.last_packet > std::chrono::seconds(2)) continue;
            const int depth_ms = mixer->depth_chunks(static_cast<uint8_t>(i)) * 20;
            metrics::event("buffer_ms", depth_ms);
            full.push_back(
                s.stats->format(snap) +
                fmt("  buffer %3d ms  concealed +%llu  underruns +%llu  late +%llu  drift +%llu"
                    "  from %s",
                    depth_ms, ull(c.concealed - p.concealed), ull(c.underruns - p.underruns),
                    ull(c.late - p.late), ull(c.drift_trims - p.drift_trims), s.ip.c_str()));
            brief.push_back(fmt("s%d loss %.1f%% jitter %.1f ms buffer %d ms", i, snap.loss_pct,
                                snap.jitter_ms, depth_ms));
        }
        if (full.empty()) {
            stats.set(fmt("audio rx waiting on UDP port %u", port) +
                      (allowed.empty() ? "" : " for " + join(allowed, ", ")));
        } else if (full.size() == 1) {
            stats.set(full[0]);  // the phase 1-3 line, unchanged
        } else {
            stats.set(fmt("audio rx %zu streams | ", brief.size()) + join(brief, " | "));
        }
    }

    // Whole-run summary: one line per stream that sent anything, then what was dropped.
    std::vector<std::string> lines;
    for (int i = 0; i < StreamMixer::kStreams; ++i) {
        const StreamRx& s = streams[i];
        if (!s.stats) continue;
        const PlayoutBuffer::Counters c = mixer->counters(static_cast<uint8_t>(i));
        lines.push_back(
            "total " + s.stats->format(s.stats->total()) +
            fmt(" | played %llu concealed %llu underruns %llu late %llu duplicates %llu "
                "out-of-range %llu trimmed %llu drift-trimmed %llu resyncs %llu",
                ull(c.played), ull(c.concealed), ull(c.underruns), ull(c.late), ull(c.duplicates),
                ull(c.out_of_range), ull(c.trimmed), ull(c.drift_trims), ull(c.resyncs)));
    }
    if (lines.empty()) lines.push_back("audio rx total: no audio received");
    lines.back() += fmt(" | dropped %llu malformed, %llu from other senders", ull(bad), ull(other));
    if (bad_stream > 0) lines.back() += fmt(", %llu with a bad stream id", ull(bad_stream));
    stats.set(join(lines, "\n"));
}

// ---- phase 7: multicast groups --------------------------------------------------------

bool AudioReceiver::join_group(const std::string& group, const std::string& iface_ip) {
    Impl& m = *impl_;
    if (!m.sock) return false;
    leave_group();
    std::lock_guard<std::mutex> lock(m.group_mu);
    if (!proto::is_multicast_ipv4(group) || !m.sock->join_multicast(group, iface_ip)) {
        m.error.set("audio rx: can't join multicast group " + group);
        return false;
    }
    m.group = group;
    m.group_iface = iface_ip;
    return true;
}

void AudioReceiver::leave_group() {
    Impl& m = *impl_;
    std::lock_guard<std::mutex> lock(m.group_mu);
    if (m.group.empty() || !m.sock) return;
    m.sock->leave_multicast(m.group, m.group_iface);
    m.group.clear();
}

// ---- phase 6: encryption --------------------------------------------------------------

void AudioSender::set_crypto(std::shared_ptr<const proto::MediaCrypto> crypto) {
    if (impl_->sock) impl_->sock->set_crypto(std::move(crypto));
}

void AudioReceiver::set_crypto(std::shared_ptr<const proto::MediaCrypto> crypto) {
    if (impl_->sock) impl_->sock->set_crypto(std::move(crypto));
}

// ---- phase 4: sender destinations ------------------------------------------------------

void AudioSender::set_destinations(const std::vector<proto::Endpoint>& to) {
    Impl& m = *impl_;
    std::string rejected;
    std::vector<proto::Endpoint> clean = clean_destinations(to, &rejected);
    if (!rejected.empty()) m.error.set("audio tx: ignored bad destination(s) " + rejected);
    std::lock_guard<std::mutex> lock(m.dest_mu);
    m.pending_dests = std::move(clean);
    m.dests_changed = true;
}

}  // namespace audio
