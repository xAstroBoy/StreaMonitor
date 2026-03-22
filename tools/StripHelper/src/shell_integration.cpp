// ─────────────────────────────────────────────────────────────────
// StripHelper — Windows Shell Integration implementation
// Uses HKCU\Software\Classes to avoid requiring admin rights.
// ─────────────────────────────────────────────────────────────────

#include "shell_integration.h"

#ifdef _WIN32
#include <windows.h>
#include <shlwapi.h>
#endif

namespace sh
{
#ifdef _WIN32

    // Registry paths for the shell extension (HKCU — no admin needed)
    // Two entries: one for right-clicking ON a folder, one for inside a folder.
    static const char *REG_SHELL_KEY = "Software\\Classes\\Directory\\shell\\StripHelper";
    static const char *REG_COMMAND_KEY = "Software\\Classes\\Directory\\shell\\StripHelper\\command";
    static const char *REG_BG_SHELL_KEY = "Software\\Classes\\Directory\\Background\\shell\\StripHelper";
    static const char *REG_BG_COMMAND_KEY = "Software\\Classes\\Directory\\Background\\shell\\StripHelper\\command";

    std::string getExePath()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return buf;
    }

    // Helper: create a shell key with display text, icon, and command
    static bool createShellEntry(const char *shellKey, const char *commandKey,
                                 const char *menuText, const std::string &exePath,
                                 const char *commandFmt)
    {
        HKEY hKey = nullptr;
        LONG result = RegCreateKeyExA(HKEY_CURRENT_USER, shellKey, 0, nullptr,
                                      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
        if (result != ERROR_SUCCESS)
            return false;
        RegSetValueExA(hKey, nullptr, 0, REG_SZ,
                       (const BYTE *)menuText, (DWORD)(strlen(menuText) + 1));
        RegSetValueExA(hKey, "Icon", 0, REG_SZ,
                       (const BYTE *)exePath.c_str(), (DWORD)(exePath.size() + 1));
        RegCloseKey(hKey);

        result = RegCreateKeyExA(HKEY_CURRENT_USER, commandKey, 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
        if (result != ERROR_SUCCESS)
            return false;
        std::string command = "\"" + exePath + "\" " + commandFmt;
        RegSetValueExA(hKey, nullptr, 0, REG_SZ,
                       (const BYTE *)command.c_str(), (DWORD)(command.size() + 1));
        RegCloseKey(hKey);
        return true;
    }

    static bool keyExists(const char *key)
    {
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExA(HKEY_CURRENT_USER, key, 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return true;
        }
        return false;
    }

    bool isShellMenuInstalled()
    {
        // Installed if either entry exists
        return keyExists(REG_SHELL_KEY) || keyExists(REG_BG_SHELL_KEY);
    }

    bool installShellMenu()
    {
        std::string exePath = getExePath();
        if (exePath.empty())
            return false;

        const char *menuText = "Process with StripHelper (Symlinks)";

        // 1) Directory\shell → right-click ON a folder → uses %1
        bool ok1 = createShellEntry(REG_SHELL_KEY, REG_COMMAND_KEY,
                                    menuText, exePath, "--symlinks \"%1\"");

        // 2) Directory\Background\shell → right-click INSIDE a folder → uses %V
        //    (same behavior as the old Python MergeAllFilesSymlink script)
        bool ok2 = createShellEntry(REG_BG_SHELL_KEY, REG_BG_COMMAND_KEY,
                                    menuText, exePath, "--symlinks \"%V\"");

        return ok1 || ok2;
    }

    bool uninstallShellMenu()
    {
        bool ok = false;
        // Remove Directory\shell entry
        RegDeleteKeyA(HKEY_CURRENT_USER, REG_COMMAND_KEY);
        if (RegDeleteKeyA(HKEY_CURRENT_USER, REG_SHELL_KEY) == ERROR_SUCCESS)
            ok = true;
        // Remove Directory\Background\shell entry
        RegDeleteKeyA(HKEY_CURRENT_USER, REG_BG_COMMAND_KEY);
        if (RegDeleteKeyA(HKEY_CURRENT_USER, REG_BG_SHELL_KEY) == ERROR_SUCCESS)
            ok = true;
        return ok;
    }

    bool isLegacyMenuInstalled()
    {
        HKEY hKey = nullptr;
        // Old Python script registered under HKCR\Directory\Background\shell\MergeAllFilesSymlink
        LONG result = RegOpenKeyExA(HKEY_CLASSES_ROOT,
                                    "Directory\\Background\\shell\\MergeAllFilesSymlink", 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return true;
        }
        // Also check under Directory\shell\MergeAllFilesSymlink
        result = RegOpenKeyExA(HKEY_CLASSES_ROOT,
                               "Directory\\shell\\MergeAllFilesSymlink", 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return true;
        }
        return false;
    }

    bool uninstallLegacyMenu()
    {
        bool ok = false;
        // Remove old Python script entries (HKCR — may need admin)
        RegDeleteKeyA(HKEY_CLASSES_ROOT, "Directory\\Background\\shell\\MergeAllFilesSymlink\\command");
        if (RegDeleteKeyA(HKEY_CLASSES_ROOT, "Directory\\Background\\shell\\MergeAllFilesSymlink") == ERROR_SUCCESS)
            ok = true;
        RegDeleteKeyA(HKEY_CLASSES_ROOT, "Directory\\shell\\MergeAllFilesSymlink\\command");
        if (RegDeleteKeyA(HKEY_CLASSES_ROOT, "Directory\\shell\\MergeAllFilesSymlink") == ERROR_SUCCESS)
            ok = true;
        return ok;
    }

#else
    // Non-Windows stubs
    std::string getExePath() { return ""; }
    bool isShellMenuInstalled() { return false; }
    bool installShellMenu() { return false; }
    bool uninstallShellMenu() { return false; }
    bool isLegacyMenuInstalled() { return false; }
    bool uninstallLegacyMenu() { return false; }
#endif

} // namespace sh
