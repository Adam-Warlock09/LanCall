# video/ — Person 1

# the pipelines the lancall app uses (phase 2)
add_library(lancall_video video_pipeline.cpp)
target_include_directories(lancall_video PUBLIC ${PROJECT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(lancall_video PUBLIC lancall_net lancall_metrics ${OpenCV_LIBS} Threads::Threads)

add_executable(video_sender video_sender.cpp)
target_include_directories(video_sender PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(video_sender PRIVATE lancall_net lancall_metrics ${OpenCV_LIBS} Threads::Threads)

add_executable(video_receiver video_receiver.cpp)
target_include_directories(video_receiver PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(video_receiver PRIVATE lancall_net lancall_metrics ${OpenCV_LIBS})