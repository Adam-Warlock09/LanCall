# bridge.cpp: phase 7 (Person 2), a broadcast passed on to another floor
add_library(lancall_net packetizer.cpp reassembler.cpp relay.cpp bridge.cpp)
target_include_directories(lancall_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR})
target_link_libraries(lancall_net PUBLIC lancall_common Threads::Threads)

# phase 4: forward group-call media from any laptop: lancall_relay [--port 5014]
add_executable(lancall_relay lancall_relay.cpp)
target_link_libraries(lancall_relay PRIVATE lancall_net)

# phase 7 (Person 2): show a broadcast on another floor:
# lancall_bridge --from <presenter_ip> --name <presenter_name> [--title T] [--port 5016]
add_executable(lancall_bridge lancall_bridge.cpp)
target_link_libraries(lancall_bridge PRIVATE lancall_net lancall_control)
