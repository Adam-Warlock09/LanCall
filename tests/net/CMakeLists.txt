add_executable(test_net test_net.cpp)
target_link_libraries(test_net PRIVATE lancall_net)
add_test(NAME net_unit COMMAND test_net)

add_executable(net_loopback net_loopback.cpp)
target_link_libraries(net_loopback PRIVATE lancall_net Threads::Threads)
add_test(NAME net_loopback COMMAND net_loopback --frames 200)

# phase 3: receiver report format (common/report.hpp)
add_executable(test_report test_report.cpp)
target_link_libraries(test_report PRIVATE lancall_common)
add_test(NAME net_report COMMAND test_report)

# phase 3: loss counter (net/) and quality ladder (video/), both header-only
add_executable(test_adaptive test_adaptive.cpp)
target_include_directories(test_adaptive PRIVATE ${PROJECT_SOURCE_DIR})
add_test(NAME net_adaptive COMMAND test_adaptive)
# phase 4: the group-call relay, with pretend members on 127.0.0.2-4
add_executable(test_relay test_relay.cpp)
target_link_libraries(test_relay PRIVATE lancall_net)
add_test(NAME net_relay COMMAND test_relay)

# phase 5: NACK format, nack tracker, retransmit buffer, independent reassembler
add_executable(test_repair test_repair.cpp)
target_include_directories(test_repair PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(test_repair PRIVATE lancall_net)
add_test(NAME net_repair COMMAND test_repair)
