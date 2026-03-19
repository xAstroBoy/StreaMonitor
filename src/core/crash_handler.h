#pragma once
// ═══════════════════════════════════════════════════════════════════
//  StreaMonitor — Universal Crash Handler
//
//  Registers OS-level crash handlers (SEH on Windows, signals on
//  Unix) and writes a detailed stack trace + context to a file
//  in the "crashes/" directory whenever an unhandled crash occurs.
//
//  Call installCrashHandler() once at the very start of main().
// ═══════════════════════════════════════════════════════════════════

#include <string>

namespace sm
{
    /// Install the universal crash handler.  Call once at program start.
    /// @param crashDir  Directory for crash logs (default: "crashes")
    void installCrashHandler(const std::string &crashDir = "crashes");

} // namespace sm
