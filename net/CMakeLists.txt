add_library(lancall_net packetizer.cpp reassembler.cpp relay.cpp)
target_include_directories(lancall_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_link_libraries(lancall_net PUBLIC lancall_common Threads::Threads)

# phase 4: forward group-call media from any laptop: lancall_relay [--port 5014]
add_executable(lancall_relay lancall_relay.cpp)
target_link_libraries(lancall_relay PRIVATE lancall_net)
