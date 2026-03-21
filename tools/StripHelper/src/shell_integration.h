#pragma once
// ─────────────────────────────────────────────────────────────────
// StripHelper — Windows Shell Integration
// Register/unregister right-click context menu for folders.
// Creates: HKCU\Software\Classes\Directory\shell\StripHelper
// ─────────────────────────────────────────────────────────────────

#include <string>

namespace sh
{
    // Check if the context menu entry is currently registered
    bool isShellMenuInstalled();

    // Install: adds "Process with StripHelper" to folder right-click menu.
    // Uses HKCU (no admin required).
    // Returns true on success.
    bool installShellMenu();

    // Uninstall: removes the context menu entry.
    bool uninstallShellMenu();

    // Get the path to the running executable
    std::string getExePath();

} // namespace sh
