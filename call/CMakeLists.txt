# call/ — Person 2 (phase 2): the lancall app

# Testable pieces (tests/metrics): typed-command parsing, call targets and status text, and
# the window image.
add_library(lancall_call commands.cpp call_logic.cpp view.cpp)
target_include_directories(lancall_call PUBLIC ${PROJECT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
# lancall_common: proto::write_header for the relay keepalive (phase 4)
target_link_libraries(lancall_call PUBLIC ${OpenCV_LIBS} lancall_common)

# Phase 9: everything lancall does (call::App, app.hpp), shared by the terminal lancall and
# lancall_gui (gui/). broadcast.cpp: phase 7, broadcasting to the floor and watching.
add_library(lancall_app app.cpp media_session.cpp broadcast.cpp)
# lancall_net: phase 4, the relay runs inside lancall when the call's relay is this laptop
target_link_libraries(lancall_app PUBLIC lancall_call lancall_control lancall_video
                                         lancall_audio_pipeline lancall_metrics lancall_net
                                         Threads::Threads)

add_executable(lancall main.cpp)
target_link_libraries(lancall PRIVATE lancall_app)
