# call/ — Person 2 (phase 2): the lancall app

add_executable(lancall main.cpp)
target_link_libraries(lancall PRIVATE lancall_control lancall_video lancall_audio_pipeline lancall_metrics)