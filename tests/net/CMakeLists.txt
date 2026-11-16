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