// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Win32 System Tray implementation
// ─────────────────────────────────────────────────────────────────

#ifdef _WIN32

#include "gui/system_tray.h"
#include <shellapi.h>
#include <spdlog/spdlog.h>
#include <filesystem>

// For glfwGetWin32Window
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

// Resource ID from app.rc  (IDI_ICON1 = first icon in resources)
#ifndef IDI_ICON1
#define IDI_ICON1 101
#endif

namespace sm
{

    // ── Registry key for Windows auto-start ─────────────────────────
    static const wchar_t *kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    static const wchar_t *kAppName = L"StreaMonitor";

    // ── Static instance pointer for WndProc dispatch ────────────────
    static SystemTray *g_trayInstance = nullptr;

    // ─────────────────────────────────────────────────────────────────
    SystemTray::SystemTray() = default;

    SystemTray::~SystemTray()
    {
        shutdown();
    }

    // ─────────────────────────────────────────────────────────────────
    bool SystemTray::init(GLFWwindow *glfwWindow, const wchar_t *tooltip)
    {
        glfwWindow_ = glfwWindow;
        g_trayInstance = this;

        // ── Create a hidden message-only window for tray messages ────
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = wndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SM_TrayMsgWnd";
        RegisterClassExW(&wc);

        hwnd_ = CreateWindowExW(0, L"SM_TrayMsgWnd", L"",
                                0, 0, 0, 0, 0,
                                HWND_MESSAGE, nullptr,
                                GetModuleHandleW(nullptr), nullptr);
        if (!hwnd_)
        {
            spdlog::error("SystemTray: Failed to create message window");
            return false;
        }

        // ── Load the icon from exe resources ────────────────────────
        hIcon_ = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_ICON1));
        if (!hIcon_)
        {
            // Fallback: default application icon
            hIcon_ = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
        }

        // ── Add tray icon ───────────────────────────────────────────
        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = IDI_TRAY;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
        nid.uCallbackMessage = WM_TRAYICON;
        nid.hIcon = hIcon_;
        wcsncpy_s(nid.szTip, tooltip, _TRUNCATE);

        if (!Shell_NotifyIconW(NIM_ADD, &nid))
        {
            spdlog::error("SystemTray: Shell_NotifyIconW failed");
            return false;
        }

        iconAdded_ = true;
        spdlog::info("System tray icon initialized");
        return true;
    }

    // ─────────────────────────────────────────────────────────────────
    void SystemTray::shutdown()
    {
        if (iconAdded_)
        {
            NOTIFYICONDATAW nid = {};
            nid.cbSize = sizeof(nid);
            nid.hWnd = hwnd_;
            nid.uID = IDI_TRAY;
            Shell_NotifyIconW(NIM_DELETE, &nid);
            iconAdded_ = false;
        }

        if (hwnd_)
        {
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        if (g_trayInstance == this)
            g_trayInstance = nullptr;
    }

    // ─────────────────────────────────────────────────────────────────
    void SystemTray::show()
    {
        if (!iconAdded_ || !hwnd_)
            return;

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = IDI_TRAY;
        nid.uFlags = NIF_STATE;
        nid.dwState = 0; // visible
        nid.dwStateMask = NIS_HIDDEN;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    void SystemTray::hide()
    {
        if (!iconAdded_ || !hwnd_)
            return;

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = IDI_TRAY;
        nid.uFlags = NIF_STATE;
        nid.dwState = NIS_HIDDEN;
        nid.dwStateMask = NIS_HIDDEN;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    // ─────────────────────────────────────────────────────────────────
    void SystemTray::setTooltip(const std::wstring &text)
    {
        if (!iconAdded_ || !hwnd_)
            return;

        NOTIFYICONDATAW nid = {};
        nid.cbSize = sizeof(nid);
        nid.hWnd = hwnd_;
        nid.uID = IDI_TRAY;
        nid.uFlags = NIF_TIP;
        wcsncpy_s(nid.szTip, text.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    // ─────────────────────────────────────────────────────────────────
    void SystemTray::setMenuItems(const std::vector<TrayMenuItem> &items, MenuCallback cb)
    {
        menuItems_ = items;
        menuCb_ = std::move(cb);
    }

    // ─────────────────────────────────────────────────────────────────
    void SystemTray::pollEvents()
    {
        // Process any pending Win32 messages for our hidden window
        MSG msg;
        while (PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    // ─────────────────────────────────────────────────────────────────
    void SystemTray::showContextMenu()
    {
        if (menuItems_.empty())
            return;

        HMENU hMenu = CreatePopupMenu();
        if (!hMenu)
            return;

        for (int i = 0; i < (int)menuItems_.size(); i++)
        {
            if (menuItems_[i].separator)
            {
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            }
            else
            {
                UINT flags = MF_STRING;
                if (!menuItems_[i].enabled)
                    flags |= MF_GRAYED;

                // Convert label to wide string
                std::wstring wlabel(menuItems_[i].label.begin(), menuItems_[i].label.end());
                AppendMenuW(hMenu, flags, (UINT_PTR)(i + 1), wlabel.c_str());
            }
        }

        // Required: SetForegroundWindow before TrackPopupMenu
        SetForegroundWindow(hwnd_);

        POINT pt;
        GetCursorPos(&pt);
        int cmd = (int)TrackPopupMenu(hMenu,
                                      TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                      pt.x, pt.y, 0, hwnd_, nullptr);
        DestroyMenu(hMenu);

        if (cmd > 0 && menuCb_)
            menuCb_(cmd - 1); // convert back to 0-based index
    }

    // ─────────────────────────────────────────────────────────────────
    LRESULT CALLBACK SystemTray::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        if (msg == WM_TRAYICON && g_trayInstance)
        {
            switch (LOWORD(lp))
            {
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                g_trayInstance->wantsRestore_ = true;
                break;

            case WM_RBUTTONUP:
                g_trayInstance->showContextMenu();
                break;
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    // ─────────────────────────────────────────────────────────────────
    // Auto-start: HKCU\Software\Microsoft\Windows\CurrentVersion\Run
    // ─────────────────────────────────────────────────────────────────
    bool SystemTray::isAutoStartEnabled()
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS)
            return false;

        wchar_t buf[MAX_PATH] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        LSTATUS status = RegQueryValueExW(key, kAppName, nullptr, &type,
                                          reinterpret_cast<BYTE *>(buf), &size);
        RegCloseKey(key);

        if (status != ERROR_SUCCESS || type != REG_SZ)
            return false;

        // Verify the registered path still matches our exe
        std::wstring regPath(buf);
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);

        // Case-insensitive compare (strip surrounding quotes if present)
        std::wstring regLower = regPath;
        std::wstring exeLower = exePath;
        for (auto &c : regLower)
            c = towlower(c);
        for (auto &c : exeLower)
            c = towlower(c);

        // Remove quotes from registry path for comparison
        if (regLower.size() >= 2 && regLower.front() == L'"' && regLower.back() == L'"')
            regLower = regLower.substr(1, regLower.size() - 2);

        return regLower == exeLower;
    }

    void SystemTray::setAutoStart(bool enable)
    {
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        {
            spdlog::error("Failed to open Run registry key");
            return;
        }

        if (enable)
        {
            wchar_t exePath[MAX_PATH] = {};
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);

            // Wrap in quotes to handle spaces in path
            std::wstring value = L"\"" + std::wstring(exePath) + L"\"";
            RegSetValueExW(key, kAppName, 0, REG_SZ,
                           reinterpret_cast<const BYTE *>(value.c_str()),
                           (DWORD)((value.size() + 1) * sizeof(wchar_t)));
            spdlog::info("Auto-start enabled");
        }
        else
        {
            RegDeleteValueW(key, kAppName);
            spdlog::info("Auto-start disabled");
        }

        RegCloseKey(key);
    }

} // namespace sm

#endif // _WIN32
