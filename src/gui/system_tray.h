#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Win32 System Tray (notification area) icon
//
// Provides:
//   - Tray icon with app icon
//   - Left-click: restore window
//   - Right-click: context menu with status, errors, quit
//   - Minimize-to-tray (hide window, show tray icon)
//   - Auto-start on login (HKCU\...\Run registry key)
// ─────────────────────────────────────────────────────────────────

#ifdef _WIN32

#include <string>
#include <vector>
#include <functional>
#include <Windows.h>

struct GLFWwindow;

namespace sm
{

    // ── Menu item for the tray right-click popup ────────────────────
    struct TrayMenuItem
    {
        std::string label;
        bool enabled = true;
        bool separator = false; // if true, draw a separator line instead
    };

    // ── System tray controller ──────────────────────────────────────
    class SystemTray
    {
    public:
        SystemTray();
        ~SystemTray();

        // Initialize tray icon. Call once after window creation.
        bool init(GLFWwindow *glfwWindow, const wchar_t *tooltip = L"StreaMonitor");

        // Remove tray icon and clean up.
        void shutdown();

        // Show/hide the tray icon.
        void show();
        void hide();

        // Update the tooltip text (e.g. "Recording 3 models").
        void setTooltip(const std::wstring &text);

        // ── Popup menu ──────────────────────────────────────────────
        // Set the items that appear in the right-click menu.
        // Callback receives the index of the clicked item.
        using MenuCallback = std::function<void(int index)>;
        void setMenuItems(const std::vector<TrayMenuItem> &items, MenuCallback cb);

        // ── Auto-start (Windows startup) ────────────────────────────
        static bool isAutoStartEnabled();
        static void setAutoStart(bool enable);

        // Pump Win32 messages (call once per frame from the main loop).
        void pollEvents();

        // Was the "restore" action triggered? (left-click on tray icon)
        bool wantsRestore() const { return wantsRestore_; }
        void clearRestore() { wantsRestore_ = false; }

    private:
        static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
        void showContextMenu();

        HWND hwnd_ = nullptr;   // hidden message-only window
        HICON hIcon_ = nullptr; // loaded from exe resources
        bool iconAdded_ = false;
        bool wantsRestore_ = false;
        GLFWwindow *glfwWindow_ = nullptr;

        std::vector<TrayMenuItem> menuItems_;
        MenuCallback menuCb_;

        static constexpr UINT WM_TRAYICON = WM_USER + 1;
        static constexpr UINT IDI_TRAY = 1;
    };

} // namespace sm

#endif // _WIN32
