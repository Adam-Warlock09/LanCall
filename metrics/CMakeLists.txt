# metrics/ — Person 2: StreamStats (W2) and the logger implementation (W4).
# Code includes headers as "metrics/stream_stats.hpp", relative to the project root.

add_library(lancall_metrics
    stream_stats.cpp
    csv_logger.cpp
    log_reader.cpp
    metrics.cpp      # implements common/metrics.hpp; needs Step 0's common/ library
)
target_include_directories(lancall_metrics PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lancall_metrics PUBLIC lancall_common Threads::Threads)
