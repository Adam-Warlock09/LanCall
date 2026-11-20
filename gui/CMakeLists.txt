# gui/ — Person 2 (phase 9): lancall_gui, lancall in a window.
#
# Dear ImGui is copied into third_party/imgui (third_party/README.md). Until it's there,
# lancall_gui is an empty GLFW window, which at least proves GLFW and OpenGL work here.

add_executable(lancall_gui main.cpp)
target_include_directories(lancall_gui PRIVATE ${PROJECT_SOURCE_DIR} ${GLFW_INCLUDE_DIRS})
target_link_libraries(lancall_gui PRIVATE ${GLFW_LINK_LIBRARIES} OpenGL::GL lancall_common)

set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/third_party/imgui)
if(EXISTS ${IMGUI_DIR}/imgui.cpp)
    add_library(imgui STATIC
        ${IMGUI_DIR}/imgui.cpp
        ${IMGUI_DIR}/imgui_draw.cpp
        ${IMGUI_DIR}/imgui_tables.cpp
        ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/imgui_demo.cpp
        ${IMGUI_DIR}/backends/imgui_impl_glfw.cpp
        ${IMGUI_DIR}/backends/imgui_impl_opengl3.cpp)
    target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends
                                            ${GLFW_INCLUDE_DIRS})
    target_link_libraries(imgui PUBLIC ${GLFW_LINK_LIBRARIES} OpenGL::GL)
    target_compile_options(imgui PRIVATE -w)  # third-party code: its warnings aren't ours
    target_link_libraries(lancall_gui PRIVATE imgui)
    target_compile_definitions(lancall_gui PRIVATE LANCALL_HAVE_IMGUI=1)
endif()
