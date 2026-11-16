// log_summary — summarize metrics CSV logs (Person 2, tools/).
//
// For each file: received streams (loss, jitter, reordering, duplicates, rates, fps from
// frame_shown events, transit time), sent streams (packets, rates) and event totals. The
// numbers come from the same StreamStats code the receivers use live.
//
// Usage:
//   log_summary <file.csv> [more.csv ...]             human-readable summary per file
//   log_summary --per-second <file.csv> [more.csv ...]  one CSV line per second per received
//                                                        stream, for graphs (redirect to a file)

#include <cstdio>
#include <string>
#include <vector>

#include "metrics/log_reader.hpp"

int main(int argc, char** argv) {
    bool per_second = false;
    std::vector<std::string> files;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--per-second") {
            per_second = true;
        } else if (!a.empty() && a[0] == '-') {
            files.clear();
            break;
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) {
        std::fprintf(stderr, "usage: %s [--per-second] <file.csv> [more.csv ...]\n", argv[0]);
        return 2;
    }

    int status = 0;
    if (per_second) metrics::logs::LogSummary::print_interval_header(stdout);
    for (size_t i = 0; i < files.size(); ++i) {
        metrics::logs::LogSummary summary(per_second);
        std::string error;
        if (!summary.add_file(files[i], error)) {
            std::fprintf(stderr, "%s\n", error.c_str());
            status = 1;
            continue;
        }
        if (per_second) {
            summary.print_intervals(stdout, files[i]);
        } else {
            if (i > 0) std::printf("\n");
            summary.print(stdout, files[i]);
        }
        if (summary.invalid_rows() > 0) {
            std::fprintf(stderr, "warning: %s has %llu malformed lines (skipped)\n",
                         files[i].c_str(), static_cast<unsigned long long>(summary.invalid_rows()));
        }
    }
    return status;
}
