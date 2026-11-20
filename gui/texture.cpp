// One picture on the GPU for lancall_gui (Person 2, gui/, phase 9.4). See texture.hpp.
#include "gui/texture.hpp"

#include <utility>

#include <GLFW/glfw3.h>  // brings in the system's GL header, without windows.h
#include <opencv2/imgproc.hpp>

// OpenGL 1.2: missing from Windows' gl.h, but every driver has it
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace gui {

Texture::~Texture() {
    release();
}

Texture::Texture(Texture&& other) noexcept {
    *this = std::move(other);
}

Texture& Texture::operator=(Texture&& other) noexcept {
    if (this != &other) {
        release();
        id_ = std::exchange(other.id_, 0u);
        width_ = std::exchange(other.width_, 0);
        height_ = std::exchange(other.height_, 0);
        shown_ = std::move(other.shown_);
        rgba_ = std::move(other.rgba_);
        uploaded_ = other.uploaded_;
    }
    return *this;
}

void Texture::release() {
    if (id_ != 0) {
        const GLuint name = id_;
        glDeleteTextures(1, &name);
    }
    id_ = 0;
    width_ = height_ = 0;
    shown_.release();
}

void Texture::show(const cv::Mat& image, double min_interval_s) {
    if (image.empty()) {
        shown_.release();
        return;
    }
    if (image.data == shown_.data) return;  // the same frame (see texture.hpp)
    const auto now = std::chrono::steady_clock::now();
    if (min_interval_s > 0 && !shown_.empty() &&
        std::chrono::duration<double>(now - uploaded_).count() < min_interval_s)
        return;

    if (image.channels() == 1) {
        cv::cvtColor(image, rgba_, cv::COLOR_GRAY2RGBA);
    } else if (image.channels() == 4) {
        cv::cvtColor(image, rgba_, cv::COLOR_BGRA2RGBA);
    } else {
        cv::cvtColor(image, rgba_, cv::COLOR_BGR2RGBA);
    }

    if (id_ == 0) {
        GLuint name = 0;
        glGenTextures(1, &name);
        id_ = name;
        glBindTexture(GL_TEXTURE_2D, id_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, id_);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // RGBA rows are whole multiples of 4 bytes
    if (rgba_.cols != width_ || rgba_.rows != height_) {
        width_ = rgba_.cols;
        height_ = rgba_.rows;
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width_, height_, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     rgba_.data);
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width_, height_, GL_RGBA, GL_UNSIGNED_BYTE,
                        rgba_.data);
    }
    shown_ = image;  // a reference, not a copy: keeps the buffer (and its address) ours
    uploaded_ = now;
}

}  // namespace gui
