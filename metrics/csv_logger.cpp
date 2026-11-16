#include "metrics/csv_logger.hpp"

#include <chrono>
#include <cinttypes>
#include <filesystem>
#include <system_error>

namespace metrics {

CsvLogger::~CsvLogger() {
    close();
}

bool CsvLogger::open(const std::string& path) {
    close();

    const std::filesystem::path p(path);
    if (p.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(p.parent_path(), ec);  // fopen reports failure
    }
    std::FILE* f = std::fopen(path.c_str(), "w");
    if (f == nullptr) return false;

    {
        std::lock_guard<std::mutex> lock(mu_);
        file_ = f;
        pending_.clear();
        pending_.reserve(kWakeBytes * 2);
        appended_seq_ = written_seq_ = 0;
        stop_ = false;
        flush_requested_ = false;
    }
    writer_ = std::thread(&CsvLogger::writer_loop, this);
    open_.store(true, std::memory_order_release);
    return true;
}

void CsvLogger::close() {
    if (!writer_.joinable()) return;
    open_.store(false, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mu_);
        stop_ = true;
    }
    wake_.notify_one();
    writer_.join();  // the writer drains pending_ before exiting
    {
        std::lock_guard<std::mutex> lock(mu_);
        std::fclose(file_);
        file_ = nullptr;
    }
    flushed_.notify_all();
}

void CsvLogger::append(const char* row, size_t len) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stop_ || file_ == nullptr) return;
        if (pending_.size() + len > kMaxPendingBytes) {
            rows_dropped_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pending_.append(row, len);
        ++appended_seq_;
        wake = pending_.size() >= kWakeBytes;
    }
    rows_logged_.fetch_add(1, std::memory_order_relaxed);
    if (wake) wake_.notify_one();
}

void CsvLogger::packet_row(uint32_t local_us, char dir, unsigned type, unsigned stream,
                           unsigned seq, unsigned frame, unsigned frag, size_t bytes,
                           uint32_t timestamp_us) {
    if (!is_open()) return;
    char row[128];
    const int n = std::snprintf(row, sizeof row, "%" PRIu32 ",%c,%u,%u,%u,%u,%u,%zu,%" PRIu32 "\n",
                                local_us, dir, type, stream, seq, frame, frag, bytes, timestamp_us);
    if (n > 0 && static_cast<size_t>(n) < sizeof row) append(row, static_cast<size_t>(n));
}

void CsvLogger::event_row(uint32_t local_us, const char* name, double value) {
    if (!is_open()) return;
    char row[160];
    int n = std::snprintf(row, sizeof row, "%" PRIu32 ",E,", local_us);
    if (n <= 0) return;
    // Copy the name, keeping the CSV well formed: no commas, quotes or line breaks.
    size_t pos = static_cast<size_t>(n);
    const size_t name_start = pos;
    for (const char* c = (name != nullptr ? name : ""); *c != '\0' && pos < 100; ++c) {
        const char ch = *c;
        row[pos++] = (ch == ',' || ch == '\n' || ch == '\r' || ch == '"') ? '_' : ch;
    }
    if (pos == name_start) row[pos++] = '?';
    n = std::snprintf(row + pos, sizeof row - pos, ",%.10g\n", value);
    if (n <= 0 || static_cast<size_t>(n) >= sizeof row - pos) return;
    append(row, pos + static_cast<size_t>(n));
}

void CsvLogger::flush() {
    std::unique_lock<std::mutex> lock(mu_);
    if (file_ == nullptr || stop_) return;
    const uint64_t target = appended_seq_;
    if (written_seq_ >= target) return;
    flush_requested_ = true;
    wake_.notify_one();
    flushed_.wait(lock, [&] { return written_seq_ >= target || stop_ || file_ == nullptr; });
}

void CsvLogger::write_out(std::string& data) {
    if (!data.empty()) {
        std::fwrite(data.data(), 1, data.size(), file_);
        data.clear();
    }
    std::fflush(file_);
}

void CsvLogger::writer_loop() {
    std::string batch;
    batch.reserve(kWakeBytes * 2);
    std::unique_lock<std::mutex> lock(mu_);
    for (;;) {
        wake_.wait_for(lock, std::chrono::milliseconds(kFlushIntervalMs),
                       [&] { return stop_ || flush_requested_ || pending_.size() >= kWakeBytes; });
        const bool stopping = stop_;
        flush_requested_ = false;
        batch.swap(pending_);  // take everything pending; callers keep appending meanwhile
        const uint64_t batch_seq = appended_seq_;

        lock.unlock();
        write_out(batch);
        lock.lock();

        written_seq_ = batch_seq;
        flushed_.notify_all();
        if (stopping && pending_.empty()) return;
    }
}

}  // namespace metrics
