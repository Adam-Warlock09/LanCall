add_executable(video_sender video_sender.cpp)
target_include_directories(video_sender PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(video_sender PRIVATE lancall_net lancall_metrics ${OpenCV_LIBS} Threads::Threads)

add_executable(video_receiver video_receiver.cpp)
target_include_directories(video_receiver PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(video_receiver PRIVATE lancall_net lancall_metrics ${OpenCV_LIBS})
