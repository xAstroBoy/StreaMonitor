// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Unified Entry Point
// Default: GUI mode    |    --cli : headless CLI with interactive shell
// ─────────────────────────────────────────────────────────────────

#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <cstdio>
#endif

// Forward declarations (defined in main_gui.cpp / main_cli.cpp)
int guiMain(int argc, char **argv);
int cliMain(int argc, char **argv);

int main(int argc, char **argv)
{
    // Check if --cli flag is present anywhere in args
    bool cliMode = false;
    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--cli") == 0 ||
            std::strcmp(argv[i], "-c") == 0)
        {
            cliMode = true;
            break;
        }
    }

    if (cliMode)
    {
#ifdef _WIN32
        // We are built as a Windows GUI app (no console by default).
        // For CLI mode, attach to the parent console (cmd / powershell)
        // or allocate a new one if double-clicked.
        if (!AttachConsole(ATTACH_PARENT_PROCESS))
            AllocConsole();

        // Redirect C stdio to the new/attached console
        FILE *fp = nullptr;
        freopen_s(&fp, "CONOUT$", "w", stdout);
        freopen_s(&fp, "CONOUT$", "w", stderr);
        freopen_s(&fp, "CONIN$", "r", stdin);

        // Also fix C++ streams
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
#endif
        return cliMain(argc, argv);
    }

    // GUI mode (default)
    return guiMain(argc, argv);
}
