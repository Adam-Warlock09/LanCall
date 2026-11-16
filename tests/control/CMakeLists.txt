# tests/control — Person 1 (phases 2 and 3)

add_executable(test_discovery test_discovery.cpp)
target_link_libraries(test_discovery PRIVATE lancall_control)
add_test(NAME control_discovery COMMAND test_discovery)

add_executable(test_signaling test_signaling.cpp)
target_link_libraries(test_signaling PRIVATE lancall_control)
add_test(NAME control_signaling COMMAND test_signaling)

add_executable(test_directory test_directory.cpp)
target_link_libraries(test_directory PRIVATE lancall_control)
add_test(NAME control_directory COMMAND test_directory)

# manual tool: discovery + signaling between two laptops, no media. not run by ctest
add_executable(control_demo control_demo.cpp)
target_link_libraries(control_demo PRIVATE lancall_control)