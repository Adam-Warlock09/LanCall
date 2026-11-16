# control/ — Person 1 (phase 2): discovery and signaling

add_library(lancall_control discovery.cpp signaling.cpp)
target_include_directories(lancall_control PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lancall_control PUBLIC lancall_common Threads::Threads)