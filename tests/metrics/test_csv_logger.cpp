// Unit tests for metrics::CsvLogger (the logger behind metrics::init/packet/event).
// Files are written under the system temp directory ($TMPDIR if set) and removed afterwards.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "check.hpp"
#include "common/platform.hpp"
#include "metrics/csv_logger.hpp"

namespace fs = std::filesystem;
using metrics::CsvLogger;

namespace {

fs::path temp_dir() {
    static const fs::path dir =
        fs::temp_directory_path() /
        ("lancall_test_csv_logger_" + std::to_string(platform::process_id()));
    return dir;
}

std::vector<std::string> read_lines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream f(p);
    std::string line;
    while (std::getline(f, line)) lines.push_back(line);
    return lines;
}

std::vector<std::string> split(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string field;
    while (std::getline(ss, field, ',')) out.push_back(field);
    if (!s.empty() && s.back() == ',') out.push_back("");
    return out;
}

void test_packet_row_format() {
    const fs::path p = temp_dir() / "packet.csv";
    CsvLogger log;
    CHECK(log.open(p.string()));
    log.packet_row(81234567u, 'R', 2, 0, 17, 0, 0, 652, 81230011u);
    log.packet_row(4294967295u, 'S', 1, 3, 65535, 65535, 254, 1412, 0u);
    log.close();
    const auto lines = read_lines(p);
    CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        CHECK(lines[0] == "81234567,R,2,0,17,0,0,652,81230011");
        CHECK(lines[1] == "4294967295,S,1,3,65535,65535,254,1412,0");
    }
}

void test_event_row_format() {
    const fs::path p = temp_dir() / "event.csv";
    CsvLogger log;
    CHECK(log.open(p.string()));
    log.event_row(1000u, "frame_shown", 1);
    log.event_row(2000u, "jitter_ms", 0.8125);
    log.event_row(3000u, "big", 1234567890.0);
    log.event_row(4000u, "bad,name\nwith\"stuff", -2);
    log.event_row(5000u, "", 0);
    log.event_row(6000u, nullptr, 0);
    log.close();
    const auto lines = read_lines(p);
    CHECK(lines.size() == 6);
    if (lines.size() == 6) {
        CHECK(lines[0] == "1000,E,frame_shown,1");
        CHECK(lines[1] == "2000,E,jitter_ms,0.8125");
        CHECK(lines[2] == "3000,E,big,1234567890");
        CHECK(lines[3] == "4000,E,bad_name_with_stuff,-2");
        CHECK(lines[4] == "5000,E,?,0");
        CHECK(lines[5] == "6000,E,?,0");
    }
}

void test_rows_before_open_and_after_close_are_ignored() {
    const fs::path p = temp_dir() / "closed.csv";
    CsvLogger log;
    log.event_row(1, "before_open", 1);  // no file yet: ignored, no crash
    CHECK(log.open(p.string()));
    log.event_row(2, "kept", 1);
    log.close();
    log.event_row(3, "after_close", 1);
    log.flush();  // no-op when closed
    const auto lines = read_lines(p);
    CHECK(lines.size() == 1);
    if (!lines.empty()) CHECK(lines[0] == "2,E,kept,1");
}

void test_flush_writes_while_open() {
    const fs::path p = temp_dir() / "flush.csv";
    CsvLogger log;
    CHECK(log.open(p.string()));
    for (unsigned i = 0; i < 10; ++i) log.packet_row(i, 'S', 2, 0, i, 0, 0, 652, i);
    log.flush();
    CHECK(read_lines(p).size() == 10);  // on disk while the logger is still open
    log.close();
}

void test_background_flush() {
    const fs::path p = temp_dir() / "background.csv";
    CsvLogger log;
    CHECK(log.open(p.string()));
    log.event_row(1, "tick", 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(3 * CsvLogger::kFlushIntervalMs));
    CHECK(read_lines(p).size() == 1);  // written without calling flush()
    log.close();
}

void test_open_creates_parent_dirs_and_reopen_truncates() {
    const fs::path p = temp_dir() / "nested" / "logs" / "run.csv";
    CsvLogger log;
    CHECK(log.open(p.string()));
    log.event_row(1, "first", 1);
    log.close();
    CHECK(read_lines(p).size() == 1);
    CHECK(log.open(p.string()));  // same logger, same path: starts a fresh file
    log.event_row(2, "second", 2);
    log.close();
    const auto lines = read_lines(p);
    CHECK(lines.size() == 1);
    if (!lines.empty()) CHECK(lines[0] == "2,E,second,2");
}

void test_open_failure() {
    CsvLogger log;
    CHECK(!log.open("/proc/lancall_no_such_dir/x.csv"));
    CHECK(!log.is_open());
    log.event_row(1, "ignored", 1);  // must not crash
}

void test_many_threads() {
    const fs::path p = temp_dir() / "threads.csv";
    constexpr unsigned kThreads = 4;
    constexpr unsigned kRows = 50000;
    {
        CsvLogger log;
        CHECK(log.open(p.string()));
        std::vector<std::thread> threads;
        for (unsigned t = 0; t < kThreads; ++t) {
            threads.emplace_back([&log, t] {
                for (unsigned i = 0; i < kRows; ++i) {
                    if (i % 10 == 0) {
                        log.event_row(i, "thread_event", t);
                    } else {
                        // stream = thread id, seq = row index: lets us check per-thread order.
                        log.packet_row(i, 'R', 1, t, i, i / 10, i % 10, 1412, i);
                    }
                }
            });
        }
        for (auto& th : threads) th.join();
        CHECK(log.rows_logged() == kThreads * kRows);
        CHECK(log.rows_dropped() == 0);
    }  // destructor flushes and closes

    const auto lines = read_lines(p);
    CHECK(lines.size() == kThreads * kRows);
    std::vector<long> last_seq(kThreads, -1);
    unsigned bad_rows = 0, out_of_order = 0, events = 0;
    for (const auto& line : lines) {
        const auto f = split(line);
        if (f.size() == 4 && f[1] == "E" && f[2] == "thread_event") {
            ++events;
        } else if (f.size() == 9 && f[1] == "R") {
            const unsigned t = static_cast<unsigned>(std::stoul(f[3]));
            const long seq = std::stol(f[4]);
            if (t >= kThreads) {
                ++bad_rows;
            } else {
                if (seq <= last_seq[t]) ++out_of_order;
                last_seq[t] = seq;
            }
        } else {
            ++bad_rows;
        }
    }
    CHECK(bad_rows == 0);
    CHECK(out_of_order == 0);
    CHECK(events == kThreads * kRows / 10);
}

}  // namespace

int main() {
    RUN(test_packet_row_format);
    RUN(test_event_row_format);
    RUN(test_rows_before_open_and_after_close_are_ignored);
    RUN(test_flush_writes_while_open);
    RUN(test_background_flush);
    RUN(test_open_creates_parent_dirs_and_reopen_truncates);
    RUN(test_open_failure);
    RUN(test_many_threads);
    std::error_code ec;
    fs::remove_all(temp_dir(), ec);
    return check::result();
}
