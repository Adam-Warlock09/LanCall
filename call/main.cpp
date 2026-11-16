// PLACEHOLDER from the phase 2 kickoff (Person 2 replaces this with the real lancall app).
// It only touches every interface once, so the kickoff commit proves that everything the
// app will need compiles and links together.
#include <cstdio>

#include "audio/audio_pipeline.hpp"
#include "common/protocol.hpp"
#include "control/discovery.hpp"
#include "control/signaling.hpp"
#include "video/video_pipeline.hpp"

int main() {
    control::Discovery discovery;
    control::Signaling signaling;
    video::VideoSender vtx;
    video::VideoReceiver vrx;
    audio::AudioSender atx;
    audio::AudioReceiver arx;

    std::printf("lancall placeholder\n");
    std::printf("  discovery: %s\n", discovery.error().c_str());
    std::printf("  signaling: %s\n", signaling.error().c_str());
    std::printf("  video:     %s / %s\n", vtx.error().c_str(), vrx.error().c_str());
    std::printf("  audio:     %s / %s\n", atx.error().c_str(), arx.error().c_str());
    return 0;
}