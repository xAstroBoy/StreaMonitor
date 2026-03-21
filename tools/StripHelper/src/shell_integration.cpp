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

    // Registry path for the shell extension (HKCU — no admin needed)
    static const char *REG_SHELL_KEY = "Software\\Classes\\Directory\\shell\\StripHelper";
    static const char *REG_COMMAND_KEY = "Software\\Classes\\Directory\\shell\\StripHelper\\command";

    std::string getExePath()
    {
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return buf;
    }

    bool isShellMenuInstalled()
    {
        HKEY hKey = nullptr;
        LONG result = RegOpenKeyExA(HKEY_CURRENT_USER, REG_SHELL_KEY, 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS)
        {
            RegCloseKey(hKey);
            return true;
        }
        return false;
    }

    bool installShellMenu()
    {
        std::string exePath = getExePath();
        if (exePath.empty())
            return false;

        // Create the shell key with display name
        HKEY hKey = nullptr;
        LONG result = RegCreateKeyExA(HKEY_CURRENT_USER, REG_SHELL_KEY, 0, nullptr,
                                      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
        if (result != ERROR_SUCCESS)
            return false;

        // Set display text
        const char *menuText = "Process with StripHelper (Symlinks)";
        RegSetValueExA(hKey, nullptr, 0, REG_SZ,
                       (const BYTE *)menuText, (DWORD)(strlen(menuText) + 1));

        // Set icon (use the exe itself)
        RegSetValueExA(hKey, "Icon", 0, REG_SZ,
                       (const BYTE *)exePath.c_str(), (DWORD)(exePath.size() + 1));

        RegCloseKey(hKey);

        // Create the command key
        result = RegCreateKeyExA(HKEY_CURRENT_USER, REG_COMMAND_KEY, 0, nullptr,
                                 REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
        if (result != ERROR_SUCCESS)
            return false;

        // Command: "C:\path\to\StripHelper.exe" --symlinks "%1"
        std::string command = "\"" + exePath + "\" --symlinks \"%1\"";
        RegSetValueExA(hKey, nullptr, 0, REG_SZ,
                       (const BYTE *)command.c_str(), (DWORD)(command.size() + 1));

        RegCloseKey(hKey);
        return true;
    }

    bool uninstallShellMenu()
    {
        // Delete the command subkey first
        RegDeleteKeyA(HKEY_CURRENT_USER, REG_COMMAND_KEY);
        // Then delete the shell key
        LONG result = RegDeleteKeyA(HKEY_CURRENT_USER, REG_SHELL_KEY);
        return result == ERROR_SUCCESS;
    }

    bool isLegacyMenuInstalled()
    {
        HKEY hKey = nullptr;
        // Old Python script registered under HKCR\Directory\Background\shell\MergeAllFilesSymlink
        LONG result = RegOpenKeyExA(HKEY_CLASSES_ROOT,
            "Directory\\Background\\shell\\MergeAllFilesSymlink", 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS) { RegCloseKey(hKey); return true; }
        // Also check under Directory\shell\MergeAllFilesSymlink
        result = RegOpenKeyExA(HKEY_CLASSES_ROOT,
            "Directory\\shell\\MergeAllFilesSymlink", 0, KEY_READ, &hKey);
        if (result == ERROR_SUCCESS) { RegCloseKey(hKey); return true; }
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
