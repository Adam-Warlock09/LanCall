# audio/ — Person 2: PortAudio wrapper and playout buffer (W3, W5), audio_sender and
# audio_receiver (W5). PORTAUDIO_* comes from pkg_check_modules in the top-level CMakeLists.txt.

add_library(lancall_audio
    pa_audio.cpp
    playout_buffer.cpp
)
target_include_directories(lancall_audio PUBLIC ${PROJECT_SOURCE_DIR} ${PORTAUDIO_INCLUDE_DIRS})
target_link_libraries(lancall_audio PUBLIC ${PORTAUDIO_LINK_LIBRARIES} Threads::Threads)

# AudioSender / AudioReceiver objects the lancall app uses (phase 2 kickoff).
add_library(lancall_audio_pipeline audio_pipeline.cpp)
target_link_libraries(lancall_audio_pipeline PUBLIC lancall_audio lancall_metrics lancall_common)

# Local mic -> speaker check of the wrapper, no network (use headphones).
add_executable(audio_loopback audio_loopback.cpp)
target_link_libraries(audio_loopback PRIVATE lancall_audio)

# Microphone -> UDP, and UDP -> speaker. They use common/ (protocol, UdpSocket) and metrics::.
add_executable(audio_sender audio_sender.cpp)
target_link_libraries(audio_sender PRIVATE lancall_audio lancall_metrics lancall_common)

add_executable(audio_receiver audio_receiver.cpp)
target_link_libraries(audio_receiver PRIVATE lancall_audio lancall_metrics lancall_common)
