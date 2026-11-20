// lancall_gui (Person 2, gui/, phase 9): lancall in a window.
//
//   lancall_gui [--name NAME] [--directory IP] [--relay IP|me] [--log PATH] [--in N] [--out N]
//               [--camera N] [--buffer-ms MS] [--fps N] [--quality N] [--fec] [--port N]
//               [--mc-loop]
//
// The same options as the terminal lancall (call/app_args.hpp), except --peer: direct mode
// stays in the terminal app. Without --name a start screen asks for it. The calls, chat,
// broadcasts and every measurement are call::App's (call/app.hpp), the same code the
// terminal lancall runs; this file is the window around it, and gui/panels.* the panels.
//
// The loop: wait for input for at most 10 ms, App::tick(), draw. Like the terminal app it
// ticks every 10 ms, so lip sync picks frames as often and the latency rows compare. No
// vsync: on Wayland a vsynced swap can block while the window is hidden or minimized, which
// would stop the ticks (no calls answered, no chat). Minimized, it keeps ticking and skips
// the drawing.
//
// Log: logs/lancall_gui_<name>_<date>_<time>.csv (--log overrides), the same rows as lancall.
// Everything App says is also printed to the terminal it was started from.
//
// Rules for this folder: everything runs on the main thread (GLFW and OpenGL want that), and
// nothing here includes a POSIX or Windows header: use common/platform.hpp.
//
// Snap VS Code's terminal makes GTK abort this program at start (NOTES.md, Setup): run it
// from a normal terminal.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>

#include "audio/app_util.hpp"
#include "call/app.hpp"
#include "call/app_args.hpp"
#include "common/metrics.hpp"
#include "common/platform.hpp"
#include "gui/panels.hpp"
#include "gui/tabs.hpp"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

namespace {

constexpr double kLoopWaitS = 0.010;  // at most this long between ticks, as in lancall

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--name NAME] [--directory IP] [--relay IP|me] [--log PATH]\n"
                 "          [--in N] [--out N] [--camera N] [--buffer-ms MS] [--fps N] "
                 "[--quality N]\n"
                 "          [--fec] [--port N] [--mc-loop]\n",
                 prog);
}

// The fixed layout: peers and call on the left, video on the right, the tabs below
// (messages, chat, stats, settings).
void draw_panels(call::App& app, gui::Ui& ui, gui::VideoView& video, const ImVec2& size) {
    const float left = std::clamp(size.x * 0.30f, 300.0f, 460.0f);
    const float bottom = std::clamp(size.y * 0.26f, 150.0f, 300.0f);
    const float top = size.y - bottom;
    const float peers = std::floor(top * 0.42f);
    gui::draw_peers(app, ui, ImVec2(0, 0), ImVec2(left, peers));
    gui::draw_call(app, ui, ImVec2(0, peers), ImVec2(left, top - peers));
    gui::draw_video(app, video, ImVec2(left, 0), ImVec2(size.x - left, top));
    gui::draw_bottom(app, ui, ImVec2(0, top), ImVec2(size.x, bottom));
    gui::draw_incoming(app);
}

}  // namespace

int main(int argc, char** argv) {
    call::AppOptions opt;
    std::vector<std::string> messages;
    const bool args_ok = call::parse_app_args(argc, argv, opt, messages, false);
    for (const std::string& m : messages) std::fprintf(stderr, "%s\n", m.c_str());
    if (!args_ok) {
        usage(argv[0]);
        return 2;
    }
    if (!opt.peer.empty()) {
        std::fprintf(stderr, "--peer: direct mode is only in the terminal lancall\n");
        return 2;
    }

    platform::init();
    audio::app::install_stop_handlers();

    if (!glfwInit()) {
        std::fprintf(stderr, "lancall_gui: can't start GLFW (no display?)\n");
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    GLFWwindow* window = glfwCreateWindow(1280, 800, "LanCall", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "lancall_gui: can't open an OpenGL 3.0 window\n");
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(0);  // no vsync: the 10 ms wait paces the loop (see the top)

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;  // fixed layout: nothing to remember, no imgui.ini
    float scale = 1.0f;        // HiDPI screens: everything a bit bigger
    glfwGetWindowContentScale(window, &scale, nullptr);
    scale = std::max(1.0f, scale);
    ImFontConfig font;
    font.SizePixels = std::round(15.0f * scale);
    io.Fonts->AddFontDefault(&font);
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(scale);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    std::unique_ptr<call::App> app;
    gui::Ui ui;
    gui::VideoView video;  // the textures: freed below while the GL context is still there
    auto start_app = [&] {
        if (opt.log_path.empty())
            opt.log_path = audio::app::default_log_path("lancall_gui_" + opt.name);
        metrics::init(opt.log_path);
        std::printf("lancall_gui %s  log %s\n", opt.name.c_str(), opt.log_path.c_str());
        app = std::make_unique<call::App>(opt);
        app->start();
        gui::take_notices(*app, ui);
        gui::add_line(ui, gui::Line::Kind::Info,
                      "lancall " + opt.name + "  log " + opt.log_path +
                          "   (click a peer to call, or type commands below: help)");
        glfwSetWindowTitle(window, ("LanCall - " + opt.name).c_str());
    };
    if (!opt.name.empty()) start_app();

    while (!glfwWindowShouldClose(window) && !audio::app::g_stop && !ui.quit) {
        glfwWaitEventsTimeout(kLoopWaitS);
        if (app) {
            app->tick();
            if (gui::take_notices(*app, ui)) glfwRequestWindowAttention(window);  // ringing
        }
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) continue;  // keep ticking, don't draw

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        if (app) {
            draw_panels(*app, ui, video, io.DisplaySize);
        } else if (gui::draw_start(opt, io.DisplaySize)) {
            start_app();
        }
        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // Leave cleanly: hang up (the others hear it), stop media, then the control plane.
    if (app) {
        app->shutdown();
        gui::take_notices(*app, ui);
        app.reset();
    }
    video.release();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (!opt.log_path.empty()) std::printf("bye. log: %s\n", opt.log_path.c_str());
    return 0;
}
