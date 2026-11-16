// Real implementation of the frozen metrics:: interface in common/metrics.hpp (Person 2).
// Replaces the do-nothing stub from Step 0. All work is done by one global CsvLogger
// (metrics/csv_logger.hpp); rows logged before init() are ignored, as with the stub.

#include "common/metrics.hpp"

#include <cstdio>

#include "common/protocol.hpp"
#include "metrics/csv_logger.hpp"

namespace metrics {

namespace {
// Function-local static: created on first use, flushed and closed at normal program exit.
CsvLogger& logger() {
    static CsvLogger instance;
    return instance;
}
}  // namespace

void init(const std::string& csv_path) {
    if (!logger().open(csv_path)) {
        std::fprintf(stderr, "metrics: cannot open log file '%s'; logging disabled\n",
                     csv_path.c_str());
    }
}

void packet(char dir, const proto::Header& h, size_t bytes) {
    logger().packet_row(proto::now_us(), dir, static_cast<unsigned>(h.type), h.stream_id, h.seq,
                        h.frame_id, h.frag_index, bytes, h.timestamp_us);
}

void event(const char* name, double value) {
    logger().event_row(proto::now_us(), name, value);
}

}  // namespace metrics
