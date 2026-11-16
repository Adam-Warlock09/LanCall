# tests/control — Person 1 (phase 2)

add_executable(test_discovery test_discovery.cpp)
target_link_libraries(test_discovery PRIVATE lancall_control)
add_test(NAME control_discovery COMMAND test_discovery)