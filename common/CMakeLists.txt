add_library(lancall_common protocol.cpp udp_socket.cpp)
target_include_directories(lancall_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
