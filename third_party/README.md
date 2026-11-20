# third_party/

Code we didn't write, copied in so everything builds offline. Each library keeps its own
licence file next to it. Note the version you copied in this file.

## Dear ImGui (phase 9, for lancall_gui)

Version: **v1.91.8** (the plain release, not the `docking` branch: the panels have fixed
places). From https://github.com/ocornut/imgui, copy into `third_party/imgui/`:

- `imgui.cpp`, `imgui_draw.cpp`, `imgui_tables.cpp`, `imgui_widgets.cpp`, `imgui_demo.cpp`
- `imgui.h`, `imgui_internal.h`, `imconfig.h`, `imstb_rectpack.h`, `imstb_textedit.h`,
  `imstb_truetype.h`, `LICENSE.txt`
- `backends/imgui_impl_glfw.cpp` / `.h`, `backends/imgui_impl_opengl3.cpp` / `.h`,
  `backends/imgui_impl_opengl3_loader.h`

`gui/CMakeLists.txt` picks them up as soon as `third_party/imgui/imgui.cpp` exists (compiled
without warnings, since they aren't ours). GLFW itself is not copied here: it comes from
`sudo apt install libglfw3-dev` (MSYS2 on Windows: `mingw-w64-ucrt-x86_64-glfw`).

Optional later: ImPlot (https://github.com/epezent/implot) into `third_party/implot/` for
live graphs in the stats panel.
