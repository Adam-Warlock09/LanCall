add_executable(test_net test_net.cpp)
target_link_libraries(test_net PRIVATE lancall_net)
add_test(NAME net_unit COMMAND test_net)

add_executable(net_loopback net_loopback.cpp)
target_link_libraries(net_loopback PRIVATE lancall_net Threads::Threads)
add_test(NAME net_loopback COMMAND net_loopback --frames 200)
