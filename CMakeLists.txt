cmake_minimum_required(VERSION 3.16)
project(lancall CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2)

find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PORTAUDIO REQUIRED portaudio-2.0)
pkg_check_modules(SODIUM REQUIRED libsodium)  # phase 6: sudo apt install libsodium-dev

add_subdirectory(common)   # shared
add_subdirectory(net)      # Person 1
add_subdirectory(video)    # Person 1
add_subdirectory(control)  # Person 1 (phase 2)
add_subdirectory(audio)    # Person 2
add_subdirectory(metrics)  # Person 2
add_subdirectory(tools)    # Person 2
add_subdirectory(call)     # Person 2 (phase 2), the lancall app
add_subdirectory(tests)    # each person owns a subfolder