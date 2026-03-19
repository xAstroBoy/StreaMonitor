// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Unified Entry Point
// Default: GUI mode    |    --cli : headless CLI with interactive shell
// ─────────────────────────────────────────────────────────────────

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/crash_handler.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <io.h>
#include <fcntl.h>
#else
#include <unistd.h>
#include <cerrno>
#include <dirent.h>
#include <climits>
#ifdef __APPLE__
#include <libproc.h>
#endif
#endif

// Forward declarations (defined in main_gui.cpp / main_cli.cpp)
int guiMain(int argc, char **argv);
int cliMain(int argc, char **argv);

// ─────────────────────────────────────────────────────────────────
// Anti-duplicate: only allow ONE instance from the same folder.
// Scans running processes for another exe in the same directory.
// No lock files, no mutexes — impossible to softlock.
// ─────────────────────────────────────────────────────────────────

#ifdef _WIN32

// Get full path of a process by PID (empty string on failure)
static std::wstring getProcessPath(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h)
        return {};
    wchar_t buf[MAX_PATH];
    DWORD sz = MAX_PATH;
    BOOL ok = QueryFullProcessImageNameW(h, 0, buf, &sz);
    CloseHandle(h);
    return ok ? std::wstring(buf, sz) : std::wstring{};
}

// Extract parent directory (case-insensitive compare later)
static std::wstring dirOf(const std::wstring &path)
{
    auto pos = path.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? path.substr(0, pos) : path;
}

static std::wstring toLowerW(std::wstring s)
{
    for (auto &c : s)
        c = towlower(c);
    return s;
}

static bool acquireSingleInstance()
{
    DWORD myPid = GetCurrentProcessId();

    // Get our own exe path
    wchar_t myExeBuf[MAX_PATH];
    DWORD myLen = GetModuleFileNameW(nullptr, myExeBuf, MAX_PATH);
    if (myLen == 0)
        return true; // Can't determine — allow running
    std::wstring myExe(myExeBuf, myLen);
    std::wstring myDir = toLowerW(dirOf(myExe));
    std::wstring myName = toLowerW(myExe.substr(myExe.find_last_of(L"\\/") + 1));

    // Snapshot all processes
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return true;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);

    if (Process32FirstW(snap, &pe))
    {
        do
        {
            if (pe.th32ProcessID == myPid)
                continue;

            // Quick name check first (cheap)
            std::wstring peName = toLowerW(pe.szExeFile);
            if (peName != myName)
                continue;

            // Expensive: get full path and compare directory
            std::wstring otherPath = getProcessPath(pe.th32ProcessID);
            if (otherPath.empty())
                continue;

            std::wstring otherDir = toLowerW(dirOf(otherPath));
            if (otherDir == myDir)
            {
                CloseHandle(snap);
                return false; // Another instance from same folder is running
            }
        } while (Process32NextW(snap, &pe));
    }

    CloseHandle(snap);
    return true; // No duplicate found
}

static void releaseSingleInstance() {} // Nothing to release

#else // Linux / macOS

static std::string getMyExePath()
{
#ifdef __APPLE__
    char buf[PROC_PIDPATHINFO_MAXSIZE];
    if (proc_pidpath(getpid(), buf, sizeof(buf)) > 0)
        return buf;
#else
    char buf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len > 0)
    {
        buf[len] = '\0';
        return buf;
    }
#endif
    return {};
}

static std::string dirOf(const std::string &path)
{
    auto pos = path.find_last_of('/');
    return (pos != std::string::npos) ? path.substr(0, pos) : path;
}

static std::string baseName(const std::string &path)
{
    auto pos = path.find_last_of('/');
    return (pos != std::string::npos) ? path.substr(pos + 1) : path;
}

static bool acquireSingleInstance()
{
    pid_t myPid = getpid();
    std::string myExe = getMyExePath();
    if (myExe.empty())
        return true; // Can't determine — allow

    std::string myDir = dirOf(myExe);
    std::string myName = baseName(myExe);

#ifdef __APPLE__
    // macOS: use proc_listpids + proc_pidpath
    int bufSize = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bufSize <= 0)
        return true;

    std::vector<pid_t> pids(bufSize / sizeof(pid_t));
    bufSize = proc_listpids(PROC_ALL_PIDS, 0, pids.data(), bufSize);
    int count = bufSize / sizeof(pid_t);

    for (int i = 0; i < count; i++)
    {
        pid_t pid = pids[i];
        if (pid == 0 || pid == myPid)
            continue;

        char pathBuf[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pid, pathBuf, sizeof(pathBuf)) <= 0)
            continue;

        std::string otherExe = pathBuf;
        if (baseName(otherExe) == myName && dirOf(otherExe) == myDir)
            return false; // Duplicate found
    }
#else
    // Linux: scan /proc/*/exe symlinks
    DIR *d = opendir("/proc");
    if (!d)
        return true;

    struct dirent *entry;
    while ((entry = readdir(d)) != nullptr)
    {
        // Only numeric directories (PIDs)
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9')
            continue;

        pid_t pid = static_cast<pid_t>(std::atol(entry->d_name));
        if (pid == 0 || pid == myPid)
            continue;

        char linkPath[64];
        std::snprintf(linkPath, sizeof(linkPath), "/proc/%ld/exe", (long)pid);

        char target[PATH_MAX];
        ssize_t len = readlink(linkPath, target, sizeof(target) - 1);
        if (len <= 0)
            continue;
        target[len] = '\0';

        std::string otherExe = target;
        if (baseName(otherExe) == myName && dirOf(otherExe) == myDir)
        {
            closedir(d);
            return false; // Duplicate found
        }
    }
    closedir(d);
#endif

    return true; // No duplicate
}

static void releaseSingleInstance() {} // Nothing to release

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
