// lancall_gui (Person 2, gui/, phase 9): lancall in a window. Phase 8/9 kickoff skeleton:
// opens a window and, once Dear ImGui is in third_party/imgui, its demo window, to check
// that GLFW, OpenGL and ImGui all work on this laptop. Phase 9 replaces all of this.
//
// Rules for this folder: everything runs on the main thread (GLFW and OpenGL want that), and
// nothing here includes a POSIX or Windows header: use common/platform.hpp.
#include <cstdio>

#include <GLFW/glfw3.h>

#ifdef LANCALL_HAVE_IMGUI
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#endif

#include "common/platform.hpp"

int main() {
    platform::init();
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
    glfwSwapInterval(1);  // vsync: ~60 frames a second

#ifdef LANCALL_HAVE_IMGUI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");
#else
    std::printf("lancall_gui: no Dear ImGui yet (third_party/README.md), so just a window\n");
#endif

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
            glfwSetWindowShouldClose(window, GLFW_TRUE);
#ifdef LANCALL_HAVE_IMGUI
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        ImGui::ShowDemoWindow();
        ImGui::Render();
#endif
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.10f, 0.11f, 0.13f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
#ifdef LANCALL_HAVE_IMGUI
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#endif
        glfwSwapBuffers(window);
    }

#ifdef LANCALL_HAVE_IMGUI
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
#endif
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
