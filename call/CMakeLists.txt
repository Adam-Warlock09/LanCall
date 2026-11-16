# call/ — Person 2 (phase 2): the lancall app

# Testable pieces (tests/metrics): typed-command parsing, call targets and status text, and
# the window image.
add_library(lancall_call commands.cpp call_logic.cpp view.cpp)
target_include_directories(lancall_call PUBLIC ${PROJECT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(lancall_call PUBLIC ${OpenCV_LIBS})

add_executable(lancall main.cpp media_session.cpp)
target_link_libraries(lancall PRIVATE lancall_call lancall_control lancall_video
                                      lancall_audio_pipeline lancall_metrics Threads::Threads)
