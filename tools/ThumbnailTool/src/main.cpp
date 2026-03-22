// ThumbnailTool — GLFW + OpenGL 3 + Dear ImGui entry point
// Polished dark theme matching StreaMonitor exactly.
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
#include <cmath>
#include <string>
#include <filesystem>
#include <algorithm>

// ── Custom dark theme (matches StreaMonitor exactly) ────────────────────────

static void SetupStyle()
{
    ImGuiStyle &s = ImGui::GetStyle();
    ImVec4 *c = s.Colors;

    // Background
    c[ImGuiCol_WindowBg] = {0.06f, 0.06f, 0.08f, 1.0f};
    c[ImGuiCol_ChildBg] = {0.10f, 0.10f, 0.12f, 1.0f};
    c[ImGuiCol_PopupBg] = {0.08f, 0.08f, 0.10f, 0.96f};
    c[ImGuiCol_Border] = {0.18f, 0.18f, 0.20f, 0.50f};

    // Frame (inputs, checkboxes)
    c[ImGuiCol_FrameBg] = {0.14f, 0.14f, 0.16f, 1.0f};
    c[ImGuiCol_FrameBgHovered] = {0.20f, 0.20f, 0.24f, 1.0f};
    c[ImGuiCol_FrameBgActive] = {0.26f, 0.26f, 0.30f, 1.0f};

    // Title bar
    c[ImGuiCol_TitleBg] = {0.08f, 0.08f, 0.10f, 1.0f};
    c[ImGuiCol_TitleBgActive] = {0.12f, 0.12f, 0.14f, 1.0f};
    c[ImGuiCol_MenuBarBg] = {0.08f, 0.08f, 0.10f, 1.0f};

    // Headers
    c[ImGuiCol_Header] = {0.18f, 0.18f, 0.22f, 1.0f};
    c[ImGuiCol_HeaderHovered] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_HeaderActive] = {0.45f, 0.65f, 1.00f, 1.0f};

    // Buttons (blue accent)
    c[ImGuiCol_Button] = {0.18f, 0.18f, 0.22f, 1.0f};
    c[ImGuiCol_ButtonHovered] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_ButtonActive] = {0.45f, 0.65f, 1.00f, 1.0f};

    // Tabs
    c[ImGuiCol_Tab] = {0.12f, 0.12f, 0.14f, 1.0f};
    c[ImGuiCol_TabHovered] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_TabSelected] = {0.22f, 0.35f, 0.60f, 1.0f};

    // Scrollbar
    c[ImGuiCol_ScrollbarBg] = {0.06f, 0.06f, 0.08f, 0.5f};
    c[ImGuiCol_ScrollbarGrab] = {0.28f, 0.28f, 0.32f, 1.0f};

    // Table
    c[ImGuiCol_TableHeaderBg] = {0.12f, 0.12f, 0.14f, 1.0f};
    c[ImGuiCol_TableBorderStrong] = {0.18f, 0.18f, 0.20f, 1.0f};
    c[ImGuiCol_TableBorderLight] = {0.14f, 0.14f, 0.16f, 1.0f};
    c[ImGuiCol_TableRowBg] = {0.00f, 0.00f, 0.00f, 0.00f};
    c[ImGuiCol_TableRowBgAlt] = {0.08f, 0.08f, 0.10f, 0.40f};

    // Text
    c[ImGuiCol_Text] = {0.92f, 0.92f, 0.94f, 1.0f};
    c[ImGuiCol_TextDisabled] = {0.50f, 0.50f, 0.55f, 1.0f};

    // Misc
    c[ImGuiCol_CheckMark] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_SliderGrab] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_PlotHistogram] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_SeparatorHovered] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_SeparatorActive] = {0.45f, 0.65f, 1.00f, 1.0f};
    c[ImGuiCol_ResizeGrip] = {0.20f, 0.20f, 0.24f, 0.5f};
    c[ImGuiCol_ResizeGripHovered] = {0.35f, 0.55f, 1.00f, 1.0f};
    c[ImGuiCol_ResizeGripActive] = {0.45f, 0.65f, 1.00f, 1.0f};

    // Geometry — rounded, spacious, modern
    s.WindowRounding = 6.0f;
    s.FrameRounding = 4.0f;
    s.GrabRounding = 4.0f;
    s.TabRounding = 4.0f;
    s.ScrollbarRounding = 4.0f;
    s.ChildRounding = 4.0f;
    s.PopupRounding = 4.0f;
    s.WindowPadding = {10, 10};
    s.FramePadding = {8, 5};
    s.ItemSpacing = {8, 6};
    s.ScrollbarSize = 14.0f;
    s.GrabMinSize = 12.0f;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 0.0f;
    s.TabBorderSize = 0.0f;
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
#ifdef _WIN32
    // Declare per-monitor DPI awareness
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    // OpenGL 3.3 Core
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // start hidden, show after first frame

    GLFWwindow *window = glfwCreateWindow(1400, 900,
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
    // Dark title bar + custom colors on Windows 10/11
    {
        HWND hwnd = glfwGetWin32Window(window);
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        COLORREF borderColor = RGB(25, 25, 35);
        DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR */, &borderColor, sizeof(borderColor));
        COLORREF captionColor = RGB(15, 15, 20);
        DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &captionColor, sizeof(captionColor));
        COLORREF textColor = RGB(180, 180, 200);
        DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &textColor, sizeof(textColor));
    }

    // Set window icon from embedded resource
    {
        HWND hwnd = glfwGetWin32Window(window);
        HICON icon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101));
        if (icon)
        {
            SendMessageW(hwnd, WM_SETICON, ICON_BIG, (LPARAM)icon);
            SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)icon);
        }
    }
#endif

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // no imgui.ini

    // DPI-aware font loading
    float dpiScale = 1.0f;
    float lastDpiScale = 1.0f;
    bool needFontRebuild = false;
    {
        float xscale = 1.0f, yscale = 1.0f;
        GLFWmonitor *monitor = glfwGetPrimaryMonitor();
        if (monitor)
            glfwGetMonitorContentScale(monitor, &xscale, &yscale);
        dpiScale = std::max(xscale, yscale);
        if (dpiScale < 1.0f)
            dpiScale = 1.0f;
        // Fallback for high-res monitors
        if (dpiScale <= 1.05f && monitor)
        {
            const GLFWvidmode *mode = glfwGetVideoMode(monitor);
            if (mode && mode->width >= 2560)
                dpiScale = std::max(dpiScale, (float)mode->width / 1920.0f);
        }
        lastDpiScale = dpiScale;
    }
    float fontSize = 16.0f * dpiScale;

#ifdef _WIN32
    {
        char windir[MAX_PATH];
        GetWindowsDirectoryA(windir, MAX_PATH);
        std::string fontPath = std::string(windir) + "\\Fonts\\segoeui.ttf";
        if (std::filesystem::exists(fontPath))
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSize);
        else
        {
            ImFontConfig fc;
            fc.SizePixels = fontSize;
            io.Fonts->AddFontDefault(&fc);
        }
    }
#else
    {
        ImFontConfig fc;
        fc.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&fc);
    }
#endif
    io.FontGlobalScale = 1.0f;

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

        // ── Handle DPI / content scale changes (monitor switch, resolution change) ──
        {
            float xscale = 1.0f, yscale = 1.0f;
            glfwGetWindowContentScale(window, &xscale, &yscale);
            float currentScale = std::max(xscale, yscale);
            if (currentScale < 1.0f)
                currentScale = 1.0f;
            if (std::abs(currentScale - lastDpiScale) > 0.01f)
            {
                lastDpiScale = currentScale;
                dpiScale = currentScale;
                needFontRebuild = true;
            }
        }

        // ── Rebuild font atlas on DPI change ──
        if (needFontRebuild)
        {
            needFontRebuild = false;
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            io.Fonts->Clear();
            float newFontSize = 16.0f * dpiScale;
#ifdef _WIN32
            {
                char windir[MAX_PATH];
                GetWindowsDirectoryA(windir, MAX_PATH);
                std::string fontPath = std::string(windir) + "\\Fonts\\segoeui.ttf";
                if (std::filesystem::exists(fontPath))
                    io.Fonts->AddFontFromFileTTF(fontPath.c_str(), newFontSize);
                else
                {
                    ImFontConfig fc;
                    fc.SizePixels = newFontSize;
                    io.Fonts->AddFontDefault(&fc);
                }
            }
#else
            {
                ImFontConfig fc;
                fc.SizePixels = newFontSize;
                io.Fonts->AddFontDefault(&fc);
            }
#endif
            io.Fonts->Build();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }

        // ── Skip rendering when framebuffer is 0 (minimized / resolution transition) ──
        int displayW, displayH;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        if (displayW <= 0 || displayH <= 0)
        {
            // Window is minimized or in a resolution transition — wait and retry
            glfwWaitEventsTimeout(0.1);
            continue;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        app.render();

        ImGui::Render();
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
