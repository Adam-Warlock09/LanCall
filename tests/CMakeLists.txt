# run all tests with:  ctest --test-dir build/tests --output-on-failure
enable_testing()

add_subdirectory(net)      # Person 1
add_subdirectory(control)  # Person 1 (phase 2)
add_subdirectory(metrics)  # Person 2