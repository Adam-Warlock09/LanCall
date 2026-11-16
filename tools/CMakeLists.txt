# tools/ — Person 2: netem.sh, devcheck, log summarizer.
# OpenCV_* and PORTAUDIO_* come from find_package / pkg_check_modules in the top-level CMakeLists.txt.

# devcheck: camera, microphone, speaker and network adapter check for one laptop.
add_executable(devcheck devcheck.cpp)
target_include_directories(devcheck PRIVATE ${OpenCV_INCLUDE_DIRS} ${PORTAUDIO_INCLUDE_DIRS})
target_link_libraries(devcheck PRIVATE ${OpenCV_LIBS} ${PORTAUDIO_LINK_LIBRARIES})

# netem.sh is a plain script; run it from the source tree: tools/netem.sh on 5

# log_summary: loss, jitter, rates, fps and event totals from metrics CSV logs.
add_executable(log_summary log_summary.cpp)
target_link_libraries(log_summary PRIVATE lancall_metrics)
