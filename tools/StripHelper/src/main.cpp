// StripHelper C++ — Win32 + DirectX 11 + Dear ImGui entry point
#include "app.h"
#include "config.h"
#include "shell_integration.h"

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <tchar.h>
#include <dwmapi.h>
#include <windows.h>
#include <shellapi.h>
#include <cstdio>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

// Forward-declare the ImGui Win32 message handler
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ── Globals ─────────────────────────────────────────────────────────────────

static ID3D11Device *g_pd3dDevice = nullptr;
static ID3D11DeviceContext *g_pd3dCtx = nullptr;
static IDXGISwapChain *g_pSwapChain = nullptr;
static ID3D11RenderTargetView *g_pRTV = nullptr;
static bool g_swapOccluded = false;
static UINT g_resizeW = 0, g_resizeH = 0;

static void CreateRenderTarget()
{
    ID3D11Texture2D *pBack = nullptr;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    if (pBack)
    {
        g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_pRTV);
        pBack->Release();
    }
}

static void CleanupRenderTarget()
{
    if (g_pRTV)
    {
        g_pRTV->Release();
        g_pRTV = nullptr;
    }
}

static bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 2, D3D11_SDK_VERSION, &sd,
        &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dCtx);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, 2, D3D11_SDK_VERSION, &sd,
            &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dCtx);
    if (FAILED(hr))
        return false;

    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)
    {
        g_pSwapChain->Release();
        g_pSwapChain = nullptr;
    }
    if (g_pd3dCtx)
    {
        g_pd3dCtx->Release();
        g_pd3dCtx = nullptr;
    }
    if (g_pd3dDevice)
    {
        g_pd3dDevice->Release();
        g_pd3dDevice = nullptr;
    }
}

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED)
            return 0;
        g_resizeW = (UINT)LOWORD(lParam);
        g_resizeH = (UINT)HIWORD(lParam);
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0; // disable Alt menu
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── Custom theme ────────────────────────────────────────────────────────────

static void SetupStyle()
{
    // ── Match StreaMonitor's polished dark theme exactly ────────────
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

// ── Entry point ─────────────────────────────────────────────────────────────

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR lpCmdLine, int)
{
    // Declare per-monitor DPI awareness so Windows reports true scale
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // Allow UTF-8 console output
    SetConsoleOutputCP(CP_UTF8);

    // Init env-based paths
    sh::initPaths();

    // ── Parse ALL command-line arguments ────────────────────────────────────
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    // CLI overrides (applied to settings after loading)
    struct CliOverrides
    {
        int threads = -1;
        int symlinks = -1; // 0=off 1=on
        int repairPts = -1;
        int deleteTs = -1;
        int failedTsMaxMB = -1;
        int targetFps = -1;
        int audioSampleRate = -1;
        int audioChannels = -1;
        int capMaxW = -1;
        int capMaxH = -1;
        int thumbnailEnabled = -1;
        int thumbnailWidth = -1;
        int thumbnailCols = -1;
        int thumbnailRows = -1;
        std::string folderPath;
        std::string configPath;
        std::string toProcessPath;
        bool autoStart = false;
        bool cliMode = false;
        bool showHelp = false;
    } cli;

    auto narrowArg = [](LPCWSTR w) -> std::string
    {
        char buf[2048];
        WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
        return std::string(buf);
    };

    auto nextArgStr = [&](int &i) -> std::string
    {
        if (i + 1 < argc)
            return narrowArg(argv[++i]);
        return "";
    };

    auto nextArgInt = [&](int &i) -> int
    {
        if (i + 1 < argc)
        {
            auto s = narrowArg(argv[++i]);
            try
            {
                return std::stoi(s);
            }
            catch (...)
            {
            }
        }
        return -1;
    };

    if (argv)
    {
        for (int i = 1; i < argc; i++)
        {
            std::string a = narrowArg(argv[i]);

            // Shell integration (exit after)
            if (a == "--install-context-menu")
            {
                sh::installShellMenu();
                LocalFree(argv);
                return 0;
            }
            if (a == "--uninstall-context-menu")
            {
                sh::uninstallShellMenu();
                LocalFree(argv);
                return 0;
            }

            // Help
            if (a == "--help" || a == "-h" || a == "-?")
            {
                cli.showHelp = true;
                continue;
            }

            // Mode
            if (a == "--cli")
            {
                cli.cliMode = true;
                continue;
            }

            // Worker threads
            if (a == "--threads" || a == "-t")
            {
                cli.threads = nextArgInt(i);
                continue;
            }

            // Boolean toggles
            if (a == "--symlinks")
            {
                cli.symlinks = 1;
                continue;
            }
            if (a == "--no-symlinks")
            {
                cli.symlinks = 0;
                continue;
            }
            if (a == "--repair-pts")
            {
                cli.repairPts = 1;
                continue;
            }
            if (a == "--no-repair-pts")
            {
                cli.repairPts = 0;
                continue;
            }
            if (a == "--delete-ts")
            {
                cli.deleteTs = 1;
                continue;
            }
            if (a == "--no-delete-ts")
            {
                cli.deleteTs = 0;
                continue;
            }
            if (a == "--thumbnails")
            {
                cli.thumbnailEnabled = 1;
                continue;
            }
            if (a == "--no-thumbnails")
            {
                cli.thumbnailEnabled = 0;
                continue;
            }

            // Numeric settings
            if (a == "--failed-ts-max-mb")
            {
                cli.failedTsMaxMB = nextArgInt(i);
                continue;
            }
            if (a == "--target-fps" || a == "--fps")
            {
                cli.targetFps = nextArgInt(i);
                continue;
            }
            if (a == "--audio-sr")
            {
                cli.audioSampleRate = nextArgInt(i);
                continue;
            }
            if (a == "--audio-ch")
            {
                cli.audioChannels = nextArgInt(i);
                continue;
            }
            if (a == "--max-width")
            {
                cli.capMaxW = nextArgInt(i);
                continue;
            }
            if (a == "--max-height")
            {
                cli.capMaxH = nextArgInt(i);
                continue;
            }
            if (a == "--thumb-width")
            {
                cli.thumbnailWidth = nextArgInt(i);
                continue;
            }
            if (a == "--thumb-cols")
            {
                cli.thumbnailCols = nextArgInt(i);
                continue;
            }
            if (a == "--thumb-rows")
            {
                cli.thumbnailRows = nextArgInt(i);
                continue;
            }

            // Path settings
            if (a == "--config")
            {
                cli.configPath = nextArgStr(i);
                continue;
            }
            if (a == "--to-process")
            {
                cli.toProcessPath = nextArgStr(i);
                continue;
            }

            // Positional: folder path
            if (!a.empty() && a[0] != '-')
            {
                cli.folderPath = a;
                cli.autoStart = true;
            }
        }
        LocalFree(argv);
    }

    // ── Show help ──────────────────────────────────────────────────────────
    if (cli.showHelp)
    {
        AllocConsole();
        FILE *f;
        freopen_s(&f, "CONOUT$", "w", stdout);
        printf(
            "StripHelper — Remux & Merge Pipeline\n"
            "=====================================\n\n"
            "Usage: StripHelper.exe [options] [folder]\n\n"
            "Options:\n"
            "  --help, -h                 Show this help\n"
            "  --cli                      Run in CLI mode (no GUI)\n"
            "  --threads, -t <N>          Worker thread count (1-32)\n"
            "  --symlinks / --no-symlinks Create symlinks (default: off)\n"
            "  --repair-pts / --no-repair-pts  PTS timestamp repair (default: on)\n"
            "  --delete-ts / --no-delete-ts    Delete .ts after remux (default: on)\n"
            "  --failed-ts-max-mb <N>     Auto-delete failed .ts under N MB\n"
            "  --target-fps, --fps <N>    Target FPS (default: 30)\n"
            "  --audio-sr <N>             Audio sample rate (default: 48000)\n"
            "  --audio-ch <N>             Audio channels (default: 2)\n"
            "  --max-width <N>            Max video width (default: 3840)\n"
            "  --max-height <N>           Max video height (default: 2160)\n"
            "  --thumbnails / --no-thumbnails  Generate contact sheet (default: on)\n"
            "  --thumb-width <N>          Thumbnail width (default: 1280)\n"
            "  --thumb-cols <N>           Thumbnail columns (default: 4)\n"
            "  --thumb-rows <N>           Thumbnail rows (default: 4)\n"
            "  --config <path>            Path to StreaMonitor config.json\n"
            "  --to-process <path>        Default processing folder\n"
            "  --install-context-menu     Install Explorer right-click menu\n"
            "  --uninstall-context-menu   Remove Explorer right-click menu\n"
            "\nExamples:\n"
            "  StripHelper.exe \"D:\\recordings\"\n"
            "  StripHelper.exe --threads 8 --no-repair-pts \"D:\\recordings\"\n"
            "  StripHelper.exe --cli --threads 4 \"D:\\recordings\"\n");
        printf("\nPress Enter to exit...");
        getchar();
        return 0;
    }

    // ── Apply path overrides before creating the app ────────────────────────
    if (!cli.configPath.empty())
        sh::CONFIG_PATH = cli.configPath;
    if (!cli.toProcessPath.empty())
        sh::TO_PROCESS = cli.toProcessPath;

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(101)); // IDI_ICON1 from resource.h
    wc.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(101));
    wc.lpszClassName = L"StripHelperClass";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0, wc.lpszClassName, L"StripHelper \x2014 Remux & Merge",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1600, 900,
        nullptr, nullptr, hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        return 1;
    }

    // Render one dark frame before showing, so the user never sees a white flash
    {
        const float black[4] = {0.06f, 0.06f, 0.08f, 1.00f};
        g_pd3dCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
        g_pd3dCtx->ClearRenderTargetView(g_pRTV, black);
        g_pSwapChain->Present(0, 0);
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Dark title bar (Windows 10/11)
    BOOL useDarkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDarkMode, sizeof(useDarkMode));

    // ImGui setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr; // disable imgui.ini

    // Font: load system Segoe UI or fallback (DPI-scaled)
    float dpiScale = 1.0f;
    {
        UINT dpi = GetDpiForWindow(hwnd);
        dpiScale = (float)dpi / 96.0f;
        if (dpiScale < 1.0f)
            dpiScale = 1.0f;
    }
    float fontSize = 16.0f * dpiScale;
    {
        char windir[MAX_PATH];
        GetWindowsDirectoryA(windir, MAX_PATH);
        std::string fontPath = std::string(windir) + "\\Fonts\\segoeui.ttf";
        if (GetFileAttributesA(fontPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            io.Fonts->AddFontFromFileTTF(fontPath.c_str(), fontSize);
        }
        else
        {
            ImFontConfig fc;
            fc.SizePixels = fontSize;
            io.Fonts->AddFontDefault(&fc);
        }
    }

    SetupStyle();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dCtx);

    // Application
    sh::App app;

    // Apply CLI overrides to app settings
    if (cli.threads > 0)
        app.setThreads(cli.threads);
    if (cli.symlinks >= 0)
        app.setSymlinks(cli.symlinks != 0);
    if (cli.repairPts >= 0)
        app.setRepairPts(cli.repairPts != 0);
    if (cli.deleteTs >= 0)
        app.setDeleteTs(cli.deleteTs != 0);
    if (cli.failedTsMaxMB >= 0)
        app.setFailedTsMaxMB(cli.failedTsMaxMB);
    if (cli.targetFps > 0)
        app.setTargetFps(cli.targetFps);
    if (cli.audioSampleRate > 0)
        app.setAudioSampleRate(cli.audioSampleRate);
    if (cli.audioChannels > 0)
        app.setAudioChannels(cli.audioChannels);
    if (cli.capMaxW > 0)
        app.setCapMaxW(cli.capMaxW);
    if (cli.capMaxH > 0)
        app.setCapMaxH(cli.capMaxH);
    if (cli.thumbnailEnabled >= 0)
        app.setThumbnailEnabled(cli.thumbnailEnabled != 0);
    if (cli.thumbnailWidth > 0)
        app.setThumbnailWidth(cli.thumbnailWidth);
    if (cli.thumbnailCols > 0)
        app.setThumbnailColumns(cli.thumbnailCols);
    if (cli.thumbnailRows > 0)
        app.setThumbnailRows(cli.thumbnailRows);
    if (!cli.folderPath.empty())
    {
        app.setPath(cli.folderPath);
        app.setAutoStart(true);
    }

    // Push final settings (from JSON + CLI overrides) → config.h globals
    app.syncSettingsToGlobals();

    // Main loop
    const float clearColor[4] = {0.06f, 0.06f, 0.08f, 1.00f};
    bool running = true;

    while (running)
    {
        // ── Wait strategy: event-driven when idle, throttled when busy ──
        //
        // Idle (no workers):  block until the OS delivers input (mouse / kbd)
        //                     or 500 ms passes (in case cursor blink etc.)
        //                     → CPU usage ≈ 0 %
        //
        // Processing (workers active):  poll at ~30 fps so the table/progress
        //                                updates at a reasonable rate.
        //                     → CPU usage ≈ 1-2 % from the UI thread alone
        if (!app.isProcessing())
        {
            // Sleep until any user-input message arrives, or 500ms timeout
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 500, QS_ALLINPUT);
        }
        else
        {
            Sleep(33); // ~30 fps for progress updates
        }

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                running = false;
        }
        if (!running)
            break;

        // Handle swap chain occlusion (minimized) — sleep hard
        if (g_swapOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)
        {
            Sleep(100);
            continue;
        }
        g_swapOccluded = false;

        // Handle resize
        if (g_resizeW != 0 && g_resizeH != 0)
        {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        // ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        app.render();

        ImGui::Render();
        g_pd3dCtx->OMSetRenderTargets(1, &g_pRTV, nullptr);
        g_pd3dCtx->ClearRenderTargetView(g_pRTV, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        HRESULT hr = g_pSwapChain->Present(1, 0); // VSync on
        g_swapOccluded = (hr == DXGI_STATUS_OCCLUDED);

        if (app.wantQuit())
            break;
    }

    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);

    return 0;
}
