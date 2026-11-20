# platform.cpp: phase 8 kickoff, what differs between Linux and Windows (platform.hpp)
add_library(lancall_common protocol.cpp udp_socket.cpp report.cpp nack.cpp media_crypto.cpp
                           platform.cpp)
target_include_directories(lancall_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# phase 6: libsodium for media encryption (and signaling's key exchange, which links this)
target_include_directories(lancall_common PUBLIC ${SODIUM_INCLUDE_DIRS})
target_link_libraries(lancall_common PUBLIC ${SODIUM_LINK_LIBRARIES})
