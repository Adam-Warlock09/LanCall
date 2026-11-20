cmake_minimum_required(VERSION 3.16)
project(lancall CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2)
if(WIN32)
    # phase 8: Windows 10 APIs (inet_pton, WSAPoll, ...); built with MSYS2 UCRT64's GCC
    add_compile_definitions(_WIN32_WINNT=0x0A00)
endif()

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

# Phase 9 (Person 2): lancall_gui, Dear ImGui on GLFW + OpenGL 3. Built only when GLFW is
# installed (sudo apt install libglfw3-dev); everything else builds the same without it.
option(LANCALL_GUI "Build lancall_gui (phase 9) when GLFW and OpenGL are found" ON)
if(LANCALL_GUI)
    pkg_check_modules(GLFW QUIET glfw3)
    set(OpenGL_GL_PREFERENCE GLVND)
    find_package(OpenGL QUIET)
    if(GLFW_FOUND AND OPENGL_FOUND)
        add_subdirectory(gui)
    else()
        message(STATUS "lancall_gui skipped: needs GLFW and OpenGL (sudo apt install libglfw3-dev)")
    endif()
endif()
add_subdirectory(tests)    # each person owns a subfolder