add_library(lancall_common protocol.cpp udp_socket.cpp report.cpp nack.cpp)
target_include_directories(lancall_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})