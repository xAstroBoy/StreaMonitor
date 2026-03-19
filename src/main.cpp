// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Unified Entry Point
// Default: GUI mode    |    --cli : headless CLI with interactive shell
// ─────────────────────────────────────────────────────────────────

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include "core/crash_handler.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#else
#include <sys/file.h>
#include <unistd.h>
#include <cerrno>
#endif

// Forward declarations (defined in main_gui.cpp / main_cli.cpp)
int guiMain(int argc, char **argv);
int cliMain(int argc, char **argv);

// ─────────────────────────────────────────────────────────────────
// Anti-duplicate: only allow ONE instance of StreaMonitor at a time.
// Windows: Named mutex   |   Linux/macOS: flock() on a temp file
// ─────────────────────────────────────────────────────────────────
#ifdef _WIN32
static HANDLE g_singleInstanceMutex = nullptr;

static bool acquireSingleInstance()
{
    g_singleInstanceMutex = CreateMutexW(nullptr, TRUE, L"Global\\StreaMonitor_SingleInstance");
    if (!g_singleInstanceMutex)
        return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
        return false;
    }
    return true;
}

static void releaseSingleInstance()
{
    if (g_singleInstanceMutex)
    {
        ReleaseMutex(g_singleInstanceMutex);
        CloseHandle(g_singleInstanceMutex);
        g_singleInstanceMutex = nullptr;
    }
}
#else
static int g_lockFd = -1;

static bool acquireSingleInstance()
{
    const char *tmpDir = std::getenv("TMPDIR");
    if (!tmpDir)
        tmpDir = "/tmp";
    char lockPath[512];
    std::snprintf(lockPath, sizeof(lockPath), "%s/streamonitor.lock", tmpDir);

    g_lockFd = open(lockPath, O_CREAT | O_RDWR, 0600);
    if (g_lockFd < 0)
        return true; // Can't create lock file — allow running anyway
    if (flock(g_lockFd, LOCK_EX | LOCK_NB) != 0)
    {
        close(g_lockFd);
        g_lockFd = -1;
        return false;
    }
    return true;
}

static void releaseSingleInstance()
{
    if (g_lockFd >= 0)
    {
        flock(g_lockFd, LOCK_UN);
        close(g_lockFd);
        g_lockFd = -1;
    }
}
#endif

int main(int argc, char **argv)
{
    // Install universal crash handler FIRST — before anything else.
    // Writes detailed stack traces to crashes/ on any unhandled crash.
    sm::installCrashHandler("crashes");

    // ── Anti-duplicate: prevent running two copies at once ──────
    if (!acquireSingleInstance())
    {
#ifdef _WIN32
        MessageBoxW(nullptr,
                    L"StreaMonitor is already running.\n\n"
                    L"Check the system tray or Task Manager.",
                    L"StreaMonitor", MB_OK | MB_ICONINFORMATION);
#else
        std::fprintf(stderr, "StreaMonitor is already running.\n");
#endif
        return 1;
    }

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

    int result = 0;

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
        result = cliMain(argc, argv);
    }
    else
    {
        // GUI mode (default)
        result = guiMain(argc, argv);
    }

    releaseSingleInstance();
    return result;
}
