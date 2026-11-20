#pragma once
// One picture on the GPU for lancall_gui (Person 2, gui/, phase 9.4): an OpenCV image (BGR or
// grey, any size) turned into an RGBA OpenGL texture that ImGui can draw.
//
// Uploading costs ~1.2 MB per 640x480 picture, so it happens only for a new frame:
//   - a received frame is a fresh buffer each time lip sync picks one (call::App), and we
//     keep a reference to the one we uploaded, so that buffer can't be reused for another
//     frame meanwhile: same data pointer = same frame, nothing to do;
//   - our own camera (VideoSender::self_view) is a fresh copy every tick even when the
//     camera hasn't moved on, so callers pass min_interval_s = 1/fps for it.
//
// OpenGL 1.1 calls only (Windows' opengl32.dll has nothing newer without a loader). Main
// thread only, with the window's GL context current. Call release() before the context goes.

#include <chrono>
#include <cstdint>

#include <opencv2/core.hpp>

namespace gui {

class Texture {
public:
    Texture() = default;
    ~Texture();  // release(); the GL context must still be there
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;

    // Shows image from now on (uploads it if it's new; see the top). An empty image clears
    // the texture (empty() then says so).
    void show(const cv::Mat& image, double min_interval_s = 0);
    void release();  // frees the GL texture

    bool empty() const { return id_ == 0 || shown_.empty(); }
    uint64_t id() const { return id_; }  // for ImGui (ImTextureID)
    int width() const { return width_; }
    int height() const { return height_; }

private:
    unsigned int id_ = 0;  // GL texture name, 0 = none yet
    int width_ = 0, height_ = 0;
    cv::Mat shown_;  // the image uploaded last (kept: see the top)
    cv::Mat rgba_;   // conversion buffer, reused
    std::chrono::steady_clock::time_point uploaded_{};
};

}  // namespace gui
