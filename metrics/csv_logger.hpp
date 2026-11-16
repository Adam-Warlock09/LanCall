#pragma once
// CsvLogger — thread-safe, buffered CSV writer behind the frozen metrics:: interface
// (Person 2, metrics/). metrics/metrics.cpp wraps one global instance as
// metrics::init / metrics::packet / metrics::event.
//
// Frozen row formats (no header row; packet and event rows share one file):
//   packet: local_us,dir,type,stream,seq,frame,frag,bytes,timestamp_us   e.g.
//   81234567,R,2,0,17,0,0,652,81230011 event:  local_us,E,name,value e.g. 81234567,E,underrun,1
// dir is S (sent) or R (received); type is the numeric proto::Type; frag is frag_index.
//
// Callers format their row and append it to an in-memory buffer under a short lock; a
// background thread writes the buffer to disk every 100 ms (sooner once it reaches 64 KB).
// If the disk stalls and more than 8 MB is pending, new rows are dropped and counted.
// Not for use inside audio callbacks (it takes a mutex).

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace metrics {

class CsvLogger {
public:
    CsvLogger() = default;
    ~CsvLogger();  // writes everything still pending, then closes
    CsvLogger(const CsvLogger&) = delete;
    CsvLogger& operator=(const CsvLogger&) = delete;

    // Opens (truncates) the file, creating parent directories. Closes any previous file first.
    // Returns false if the file cannot be created.
    bool open(const std::string& path);
    void close();
    bool is_open() const { return open_.load(std::memory_order_acquire); }

    // Rows logged while no file is open are ignored.
    void packet_row(uint32_t local_us, char dir, unsigned type, unsigned stream, unsigned seq,
                    unsigned frame, unsigned frag, size_t bytes, uint32_t timestamp_us);
    void event_row(uint32_t local_us, const char* name, double value);

    // Blocks until every row logged before this call is written to the file.
    void flush();

    uint64_t rows_logged() const { return rows_logged_.load(std::memory_order_relaxed); }
    uint64_t rows_dropped() const { return rows_dropped_.load(std::memory_order_relaxed); }

    static constexpr size_t kWakeBytes = 64 * 1024;  // wake the writer early
    static constexpr size_t kMaxPendingBytes = 8 * 1024 * 1024;
    static constexpr int kFlushIntervalMs = 100;

private:
    void append(const char* row, size_t len);
    void writer_loop();
    void write_out(std::string& data);

    std::mutex mu_;
    std::condition_variable wake_;     // writer: data or stop
    std::condition_variable flushed_;  // flush(): writer finished a batch
    std::string pending_;              // rows not yet handed to the writer
    uint64_t appended_seq_ = 0;        // batches: rows appended so far
    uint64_t written_seq_ = 0;         // rows known to be on disk (fflush'ed)
    bool stop_ = false;
    bool flush_requested_ = false;

    std::FILE* file_ = nullptr;  // used only by the writer thread while open
    std::thread writer_;
    std::atomic<bool> open_{false};
    std::atomic<uint64_t> rows_logged_{0};
    std::atomic<uint64_t> rows_dropped_{0};
};

}  // namespace metrics
