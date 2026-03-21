// ThumbnailTool — GLFW + OpenGL 3 + Dear ImGui entry point
// Same windowing style as StreaMonitor (external popups, dark theme)
#include "app.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#ifdef _WIN32
#define GLFW_EXPOSE_NATIVE_WIN32
#endif
#include <GLFW/glfw3.h>
#ifdef _WIN32
#include <GLFW/glfw3native.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <windows.h>
#endif

#include <cstdio>
#include <string>

// ── Custom dark theme (matches StreaMonitor) ────────────────────────────────

static void SetupStyle()
{
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 3.0f;
    s.ScrollbarRounding = 6.0f;
    s.TabRounding = 4.0f;
    s.WindowPadding = ImVec2(10, 10);
    s.FramePadding = ImVec2(6, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ScrollbarSize = 14.0f;

    auto &c = s.Colors;
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.10f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.18f, 0.22f, 0.30f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.25f, 0.32f, 0.45f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.30f, 0.40f, 0.55f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.18f, 0.25f, 0.38f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.24f, 0.35f, 0.52f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.30f, 0.42f, 0.60f, 1.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.12f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.18f, 0.18f, 0.22f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.22f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.14f, 0.16f, 0.22f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.20f, 0.22f, 0.28f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.12f, 0.14f, 0.20f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.24f, 0.30f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.40f, 0.70f, 1.00f, 1.00f);
    c[ImGuiCol_PlotHistogram] = ImVec4(0.25f, 0.55f, 0.85f, 1.00f);
}

static void glfw_error_callback(int error, const char *description)
{
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

// ── Entry point ─────────────────────────────────────────────────────────────

#ifdef _WIN32
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int)
#else
int main(int, char **)
#endif
{
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // OpenGL 3.3 Core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // start hidden, show after first frame

    GLFWwindow *window = glfwCreateWindow(1200, 800,
                                          "ThumbnailTool \xe2\x80\x94 Batch Video Thumbnail Generator",
                                          nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync

#ifdef _WIN32
    // Dark title bar on Windows 10/11
    {
        HWND hwnd = glfwGetWin32Window(window);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */,
                              &dark, sizeof(dark));
    }
#endif

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini

    // Try to load system font
#ifdef _WIN32
    {
        char windir[MAX_PATH];
        GetWindowsDirectoryA(windir, MAX_PATH);
        std::string fontPath = std::string(windir) + "\\Fonts\\segoeui.ttf";
        FILE *f = fopen(fontPath.c_str(), "rb");
        if (f)
        {
            fclose(f);
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), 16.0f);
        }
    }
#endif

    SetupStyle();

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330 core");

    tt::App app;

    // Show window after first dark frame
    {
        glClearColor(0.06f, 0.06f, 0.08f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        glfwSwapBuffers(window);
        glfwShowWindow(window);
    }

    // Main loop
    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.render();

        ImGui::Render();
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.06f, 0.06f, 0.08f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
