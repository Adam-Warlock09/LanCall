add_library(lancall_net packetizer.cpp reassembler.cpp)
target_include_directories(lancall_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lancall_net PUBLIC lancall_common)
