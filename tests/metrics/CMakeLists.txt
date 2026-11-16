# tests/metrics/ — Person 2: unit tests for StreamStats (W2) and the logger (W4).
# Plain test programs using check.hpp; each is registered with CTest.

add_executable(test_stream_stats test_stream_stats.cpp)
target_include_directories(test_stream_stats PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_stream_stats PRIVATE lancall_metrics)
add_test(NAME metrics.stream_stats COMMAND test_stream_stats)

add_executable(test_csv_logger test_csv_logger.cpp)
target_include_directories(test_csv_logger PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_csv_logger PRIVATE lancall_metrics)
add_test(NAME metrics.csv_logger COMMAND test_csv_logger)

add_executable(test_log_reader test_log_reader.cpp)
target_include_directories(test_log_reader PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_log_reader PRIVATE lancall_metrics)
add_test(NAME metrics.log_reader COMMAND test_log_reader)

# Audio building blocks are tested here because tests/audio/ is not in the shared layout.
add_executable(test_playout_buffer test_playout_buffer.cpp)
target_include_directories(test_playout_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_playout_buffer PRIVATE lancall_audio)
add_test(NAME metrics.playout_buffer COMMAND test_playout_buffer)

# audio::SpscRing is header-only; its test lives here because tests/audio/ is not in the shared layout.
add_executable(test_spsc_ring test_spsc_ring.cpp)
target_include_directories(test_spsc_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_link_libraries(test_spsc_ring PRIVATE Threads::Threads)
add_test(NAME metrics.spsc_ring COMMAND test_spsc_ring)
