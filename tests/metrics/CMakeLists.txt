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

# Phase 2: E1 latency math (metrics/latency.hpp) and lip sync (call/lip_sync.hpp), both header-only.
add_executable(test_latency test_latency.cpp)
target_include_directories(test_latency PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
add_test(NAME metrics.latency COMMAND test_latency)

add_executable(test_lip_sync test_lip_sync.cpp)
target_include_directories(test_lip_sync PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
add_test(NAME metrics.lip_sync COMMAND test_lip_sync)

# Audio building blocks are tested here because tests/audio/ is not in the shared layout.
add_executable(test_playout_buffer test_playout_buffer.cpp)
target_include_directories(test_playout_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_playout_buffer PRIVATE lancall_audio)
add_test(NAME metrics.playout_buffer COMMAND test_playout_buffer)

# Phase 2: the lancall app's typed commands and window image (call/).
add_executable(test_commands test_commands.cpp)
target_include_directories(test_commands PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_commands PRIVATE lancall_call)
add_test(NAME metrics.commands COMMAND test_commands)

add_executable(test_call_logic test_call_logic.cpp)
target_include_directories(test_call_logic PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_call_logic PRIVATE lancall_call)
add_test(NAME metrics.call_logic COMMAND test_call_logic)

add_executable(test_view test_view.cpp)
target_include_directories(test_view PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_view PRIVATE lancall_call)
add_test(NAME metrics.view COMMAND test_view)

# Phase 2: the audio clock behind AudioReceiver::playing_capture_us().
add_executable(test_audio_clock test_audio_clock.cpp)
target_include_directories(test_audio_clock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_audio_clock PRIVATE lancall_audio)
add_test(NAME metrics.audio_clock COMMAND test_audio_clock)

# audio::SpscRing is header-only; its test lives here because tests/audio/ is not in the shared layout.
add_executable(test_spsc_ring test_spsc_ring.cpp)
target_include_directories(test_spsc_ring PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_link_libraries(test_spsc_ring PRIVATE Threads::Threads)
add_test(NAME metrics.spsc_ring COMMAND test_spsc_ring)

# Phase 4: AudioSender's destination list and per-receiver reports (audio/send_targets.hpp).
add_executable(test_send_targets test_send_targets.cpp)
target_include_directories(test_send_targets PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_link_libraries(test_send_targets PRIVATE lancall_common)  # phase 7: is_multicast_ipv4
add_test(NAME metrics.send_targets COMMAND test_send_targets)

# Phase 4: one playout buffer per stream, mixed, with an audio clock per stream (audio/mixer.hpp).
add_executable(test_mixer test_mixer.cpp)
target_include_directories(test_mixer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_mixer PRIVATE lancall_audio)
add_test(NAME metrics.mixer COMMAND test_mixer)

# Phase 7: broadcast / multicast-group announcements in discovery beacons (control/discovery*).
add_executable(test_discovery_announce test_discovery_announce.cpp)
target_include_directories(test_discovery_announce PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_discovery_announce PRIVATE lancall_control)
add_test(NAME metrics.discovery_announce COMMAND test_discovery_announce)

# Phase 7: a broadcast passed on to another floor's group (net/bridge.hpp).
add_executable(test_bridge test_bridge.cpp)
target_include_directories(test_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_bridge PRIVATE lancall_net)
add_test(NAME metrics.bridge COMMAND test_bridge)

# Phase 9: call::App (call/app.hpp), the actions' checks and texts, with no network or devices.
add_executable(test_app test_app.cpp)
target_include_directories(test_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(test_app PRIVATE lancall_app)
add_test(NAME metrics.app COMMAND test_app)
