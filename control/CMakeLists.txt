# control/ — Person 1: discovery, signaling (phase 2) and the directory (phase 3)

add_library(lancall_control discovery.cpp signaling.cpp directory.cpp secure_line.cpp)
target_include_directories(lancall_control PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(lancall_control PUBLIC lancall_common Threads::Threads)

# run on one laptop so other floors can find each other: lancall_directory [--port 5012]
add_executable(lancall_directory lancall_directory.cpp)
target_link_libraries(lancall_directory PRIVATE lancall_control)