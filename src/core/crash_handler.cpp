// ═══════════════════════════════════════════════════════════════════
//  StreaMonitor — Universal Crash Handler  (implementation)
//
//  Windows : SetUnhandledExceptionFilter + DbgHelp StackWalk64
//  Linux   : signal handlers + backtrace() / backtrace_symbols()
//  macOS   : signal handlers + backtrace() / backtrace_symbols()
//
//  CRITICAL DESIGN RULE — HEAP-CORRUPTION SAFE:
//  The most common crash is 0xc0000374 (STATUS_HEAP_CORRUPTION).
//  When the heap is corrupted, ANY function that allocates memory
//  (malloc, new, std::string, fprintf, std::filesystem) will crash
//  inside the crash handler itself, producing a 0-byte crash file.
//
//  To survive heap corruption, this handler:
//    1. Pre-allocates a 256 KB static buffer at compile time
//    2. Formats all output via snprintf() into that buffer
//    3. Writes to disk via raw Win32 WriteFile() / POSIX write()
//    4. Uses __try/__except around DbgHelp (which allocates internally)
//    5. Phase 1 (raw addresses) is flushed to disk BEFORE Phase 2
//       (symbol resolution), so even if Phase 2 crashes, we have data
//
//  File format:  crash_YYYYMMDD_HHMMSS.txt  (+  .dmp on Windows)
// ═══════════════════════════════════════════════════════════════════

#include "crash_handler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cstdarg>
#include <chrono>
#include <mutex>

// ── Platform headers ────────────────────────────────────────────
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <csignal>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "psapi.lib")
#else
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <execinfo.h> // backtrace, backtrace_symbols
#include <cxxabi.h>   // __cxa_demangle
#include <filesystem>
#endif

namespace sm
{
    // ─── Pre-allocated static state (NO heap at crash time) ─────
    static char g_crashDir[512] = "crashes";
    static std::once_flag g_initFlag;

    // 256 KB pre-allocated buffer — enough for any crash report
    static constexpr size_t CRASH_BUF_SIZE = 256 * 1024;
    static char g_crashBuf[CRASH_BUF_SIZE];

    // ─── Heap-safe helpers ──────────────────────────────────────

    // Format timestamp into a STATIC buffer — no heap allocation
    static const char *makeTimestampSafe()
    {
        static char buf[64];
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return buf;
    }

    static const char *makeReadableTimeSafe()
    {
        static char buf[64];
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

    // Build crash file path into a STATIC buffer — no heap
    static const char *makeCrashPath(const char *suffix = "")
    {
        static char pathBuf[1024];
        snprintf(pathBuf, sizeof(pathBuf), "%s/crash_%s%s.txt",
                 g_crashDir, makeTimestampSafe(), suffix);
        return pathBuf;
    }

    // Append formatted text to buffer — returns new offset
    static size_t bufAppend(char *buf, size_t off, size_t max, const char *fmt, ...)
    {
        if (off >= max - 1)
            return off;
        va_list ap;
        va_start(ap, fmt);
        int n = vsnprintf(buf + off, max - off, fmt, ap);
        va_end(ap);
        if (n > 0)
            off += (size_t)n;
        return off;
    }

    // ═══════════════════════════════════════════════════════════
    //  WINDOWS IMPLEMENTATION
    // ═══════════════════════════════════════════════════════════
#ifdef _WIN32

    static const char *exceptionCodeToString(DWORD code)
    {
        switch (code)
        {
        case EXCEPTION_ACCESS_VIOLATION:
            return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
        case EXCEPTION_BREAKPOINT:
            return "EXCEPTION_BREAKPOINT";
        case EXCEPTION_DATATYPE_MISALIGNMENT:
            return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case EXCEPTION_FLT_DENORMAL_OPERAND:
            return "EXCEPTION_FLT_DENORMAL_OPERAND";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
            return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_FLT_INEXACT_RESULT:
            return "EXCEPTION_FLT_INEXACT_RESULT";
        case EXCEPTION_FLT_INVALID_OPERATION:
            return "EXCEPTION_FLT_INVALID_OPERATION";
        case EXCEPTION_FLT_OVERFLOW:
            return "EXCEPTION_FLT_OVERFLOW";
        case EXCEPTION_FLT_STACK_CHECK:
            return "EXCEPTION_FLT_STACK_CHECK";
        case EXCEPTION_FLT_UNDERFLOW:
            return "EXCEPTION_FLT_UNDERFLOW";
        case EXCEPTION_GUARD_PAGE:
            return "EXCEPTION_GUARD_PAGE";
        case EXCEPTION_ILLEGAL_INSTRUCTION:
            return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_IN_PAGE_ERROR:
            return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
            return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_INT_OVERFLOW:
            return "EXCEPTION_INT_OVERFLOW";
        case EXCEPTION_INVALID_DISPOSITION:
            return "EXCEPTION_INVALID_DISPOSITION";
        case EXCEPTION_INVALID_HANDLE:
            return "EXCEPTION_INVALID_HANDLE";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION:
            return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
        case EXCEPTION_PRIV_INSTRUCTION:
            return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_SINGLE_STEP:
            return "EXCEPTION_SINGLE_STEP";
        case EXCEPTION_STACK_OVERFLOW:
            return "EXCEPTION_STACK_OVERFLOW";
        case 0xc0000374:
            return "STATUS_HEAP_CORRUPTION";
        case 0xE06D7363:
            return "C++ EXCEPTION (0xE06D7363)";
        default:
            return "UNKNOWN";
        }
    }

    // Write raw bytes to a Win32 file handle — no heap, no CRT
    static void writeRaw(HANDLE h, const char *data, size_t len)
    {
        DWORD written = 0;
        while (len > 0)
        {
            DWORD chunk = (DWORD)(len > 0x7FFFFFFF ? 0x7FFFFFFF : len);
            if (!WriteFile(h, data, chunk, &written, nullptr))
                break;
            data += written;
            len -= written;
        }
    }

    static LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
    {
        // ══════════════════════════════════════════════════════════
        // PHASE 1: Basic crash info + raw stack addresses.
        // Uses ONLY static buffers and Win32 API — survives heap
        // corruption, which is our most common crash (0xc0000374).
        // ══════════════════════════════════════════════════════════

        DWORD code = ep->ExceptionRecord->ExceptionCode;
        void *addr = ep->ExceptionRecord->ExceptionAddress;

        // Ensure crash dir exists (Win32 API — no heap)
        CreateDirectoryA(g_crashDir, nullptr);

        // Build file path (static buffer)
        const char *crashPath = makeCrashPath();

        // Open via Win32 CreateFile (NOT fopen — fopen allocates a FILE buffer on heap)
        HANDLE hFile = CreateFileA(crashPath, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hFile == INVALID_HANDLE_VALUE)
        {
            // Emergency fallback
            hFile = CreateFileA("crash_emergency.txt", GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }

        // Format into pre-allocated buffer (NO heap)
        size_t off = 0;
        char *buf = g_crashBuf;
        const size_t mx = CRASH_BUF_SIZE;

        off = bufAppend(buf, off, mx, "================================================================\r\n");
        off = bufAppend(buf, off, mx, "  StreaMonitor - Crash Report\r\n");
        off = bufAppend(buf, off, mx, "================================================================\r\n\r\n");
        off = bufAppend(buf, off, mx, "  Time     : %s\r\n", makeReadableTimeSafe());
        off = bufAppend(buf, off, mx, "  Exception: 0x%08lX  (%s)\r\n", code, exceptionCodeToString(code));
        off = bufAppend(buf, off, mx, "  Address  : 0x%p\r\n", addr);
        off = bufAppend(buf, off, mx, "  Thread   : %lu\r\n", GetCurrentThreadId());
        off = bufAppend(buf, off, mx, "  Process  : %lu\r\n\r\n", GetCurrentProcessId());

        // Access violation details
        if ((code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR) &&
            ep->ExceptionRecord->NumberParameters >= 2)
        {
            ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR target = ep->ExceptionRecord->ExceptionInformation[1];
            off = bufAppend(buf, off, mx, "  AV Type  : %s at 0x%p\r\n\r\n",
                            rw == 0 ? "READ" : rw == 1 ? "WRITE"
                                           : rw == 8   ? "DEP"
                                                       : "UNKNOWN",
                            reinterpret_cast<void *>(target));
        }

        // Heap corruption note
        if (code == 0xc0000374)
        {
            off = bufAppend(buf, off, mx,
                            "  NOTE: HEAP CORRUPTION detected. The heap was damaged by a\r\n"
                            "  buffer overflow, use-after-free, or double-free BEFORE this\r\n"
                            "  point. The stack trace shows where corruption was DETECTED,\r\n"
                            "  not necessarily where it was CAUSED.\r\n"
                            "  Enable Page Heap (gflags /p /enable StreaMonitor.exe /full)\r\n"
                            "  or compile with ASAN to pinpoint the source.\r\n\r\n");
        }

        // ── Register dump ───────────────────────────────────────
        CONTEXT *ctx = ep->ContextRecord;
        off = bufAppend(buf, off, mx, "-- Registers -------------------------------------------\r\n");
#ifdef _M_X64
        off = bufAppend(buf, off, mx, "  RAX=%016llX  RBX=%016llX  RCX=%016llX\r\n", ctx->Rax, ctx->Rbx, ctx->Rcx);
        off = bufAppend(buf, off, mx, "  RDX=%016llX  RSI=%016llX  RDI=%016llX\r\n", ctx->Rdx, ctx->Rsi, ctx->Rdi);
        off = bufAppend(buf, off, mx, "  RBP=%016llX  RSP=%016llX  RIP=%016llX\r\n", ctx->Rbp, ctx->Rsp, ctx->Rip);
        off = bufAppend(buf, off, mx, "  R8 =%016llX  R9 =%016llX  R10=%016llX\r\n", ctx->R8, ctx->R9, ctx->R10);
        off = bufAppend(buf, off, mx, "  R11=%016llX  R12=%016llX  R13=%016llX\r\n", ctx->R11, ctx->R12, ctx->R13);
        off = bufAppend(buf, off, mx, "  R14=%016llX  R15=%016llX\r\n", ctx->R14, ctx->R15);
#elif defined(_M_IX86)
        off = bufAppend(buf, off, mx, "  EAX=%08lX  EBX=%08lX  ECX=%08lX  EDX=%08lX\r\n", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
        off = bufAppend(buf, off, mx, "  ESI=%08lX  EDI=%08lX  EBP=%08lX  ESP=%08lX\r\n", ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
        off = bufAppend(buf, off, mx, "  EIP=%08lX\r\n", ctx->Eip);
#elif defined(_M_ARM64)
        for (int i = 0; i < 29; i++)
            off = bufAppend(buf, off, mx, "  X%02d=%016llX%s", i, ctx->X[i], (i % 3 == 2) ? "\r\n" : "");
        off = bufAppend(buf, off, mx, "\r\n  FP=%016llX  LR=%016llX  SP=%016llX  PC=%016llX\r\n",
                        ctx->Fp, ctx->Lr, ctx->Sp, ctx->Pc);
#endif
        off = bufAppend(buf, off, mx, "\r\n");

        // ── Raw stack addresses (heap-safe via CaptureStackBackTrace) ──
        off = bufAppend(buf, off, mx, "-- Raw Stack (CaptureStackBackTrace - heap safe) ----------\r\n");
        {
            void *rawStack[128];
            USHORT frames = CaptureStackBackTrace(0, 128, rawStack, nullptr);
            for (USHORT i = 0; i < frames; i++)
            {
                char modName[MAX_PATH] = "<unknown>";
                HMODULE hMod = nullptr;
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                       (LPCSTR)rawStack[i], &hMod))
                {
                    GetModuleFileNameA(hMod, modName, MAX_PATH);
                    if (const char *sl = strrchr(modName, '\\'))
                        memmove(modName, sl + 1, strlen(sl + 1) + 1);
                }
                off = bufAppend(buf, off, mx, "  #%-3d 0x%p  %s\r\n", i, rawStack[i], modName);
            }
        }
        off = bufAppend(buf, off, mx, "\r\n");

        // ── FLUSH PHASE 1 TO DISK IMMEDIATELY ──
        // Even if Phase 2 crashes (DbgHelp allocating on corrupted heap),
        // we already have the raw addresses + registers on disk.
        if (hFile != INVALID_HANDLE_VALUE)
        {
            writeRaw(hFile, buf, off);
            FlushFileBuffers(hFile);
        }

        // ══════════════════════════════════════════════════════════
        // PHASE 2: Symbolicated stack trace via DbgHelp.
        // Wrapped in __try/__except because SymInitialize/StackWalk64
        // allocate heap memory internally and WILL crash if the heap
        // is destroyed.  Phase 1 is already safe on disk.
        // ══════════════════════════════════════════════════════════
        size_t off2 = 0;

        __try
        {
            HANDLE proc = GetCurrentProcess();
            HANDLE thread = GetCurrentThread();

            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            if (SymInitialize(proc, nullptr, TRUE))
            {
                off2 = bufAppend(buf, off2, mx, "-- Symbolicated Stack (StackWalk64) --------------------\r\n");

                STACKFRAME64 frame{};
                DWORD machineType = 0;
#ifdef _M_X64
                machineType = IMAGE_FILE_MACHINE_AMD64;
                frame.AddrPC.Offset = ctx->Rip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx->Rbp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx->Rsp;
                frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_IX86)
                machineType = IMAGE_FILE_MACHINE_I386;
                frame.AddrPC.Offset = ctx->Eip;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx->Ebp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx->Esp;
                frame.AddrStack.Mode = AddrModeFlat;
#elif defined(_M_ARM64)
                machineType = IMAGE_FILE_MACHINE_ARM64;
                frame.AddrPC.Offset = ctx->Pc;
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Offset = ctx->Fp;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Offset = ctx->Sp;
                frame.AddrStack.Mode = AddrModeFlat;
#endif
                constexpr int MAX_FRAMES = 128;
                for (int i = 0; i < MAX_FRAMES; i++)
                {
                    if (!StackWalk64(machineType, proc, thread, &frame, ctx,
                                     nullptr, SymFunctionTableAccess64,
                                     SymGetModuleBase64, nullptr))
                        break;

                    DWORD64 pc = frame.AddrPC.Offset;
                    if (pc == 0)
                        break;

                    // Module name (stack buffer — no heap)
                    char moduleName[MAX_PATH] = "<unknown>";
                    HMODULE hMod = nullptr;
                    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                           reinterpret_cast<LPCSTR>(pc), &hMod))
                    {
                        GetModuleFileNameA(hMod, moduleName, MAX_PATH);
                        if (const char *slash = strrchr(moduleName, '\\'))
                            memmove(moduleName, slash + 1, strlen(slash + 1) + 1);
                    }

                    // Symbol name (stack buffer — no heap)
                    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
                    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 511;

                    DWORD64 symDisp = 0;
                    bool hasSym = SymFromAddr(proc, pc, &symDisp, sym) == TRUE;

                    // Source file + line (stack buffer)
                    IMAGEHLP_LINE64 line{};
                    line.SizeOfStruct = sizeof(line);
                    DWORD lineDisp = 0;
                    bool hasLine = SymGetLineFromAddr64(proc, pc, &lineDisp, &line) == TRUE;

                    off2 = bufAppend(buf, off2, mx, "  #%-3d 0x%016llX  %s!", i, pc, moduleName);
                    if (hasSym)
                        off2 = bufAppend(buf, off2, mx, "%s +0x%llX", sym->Name, symDisp);
                    else
                        off2 = bufAppend(buf, off2, mx, "<unknown>");

                    if (hasLine)
                        off2 = bufAppend(buf, off2, mx, "  [%s:%lu]", line.FileName, line.LineNumber);

                    off2 = bufAppend(buf, off2, mx, "\r\n");
                }

                SymCleanup(proc);
            }
            else
            {
                off2 = bufAppend(buf, off2, mx, "-- SymInitialize FAILED (heap may be corrupted) --------\r\n");
            }

            // ── Loaded modules ──────────────────────────────────
            off2 = bufAppend(buf, off2, mx, "\r\n-- Loaded Modules --------------------------------------\r\n");
            HMODULE mods[512];
            DWORD needed = 0;
            if (EnumProcessModules(proc, mods, sizeof(mods), &needed))
            {
                DWORD count = needed / sizeof(HMODULE);
                for (DWORD m = 0; m < count && m < 512; m++)
                {
                    char name[MAX_PATH] = {};
                    GetModuleFileNameA(mods[m], name, MAX_PATH);
                    MODULEINFO mi{};
                    GetModuleInformation(proc, mods[m], &mi, sizeof(mi));
                    off2 = bufAppend(buf, off2, mx, "  0x%p  %7lu KB  %s\r\n",
                                     mi.lpBaseOfDll, mi.SizeOfImage / 1024, name);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            off2 = bufAppend(buf, off2, mx,
                             "\r\n-- PHASE 2 CRASHED (heap too damaged for DbgHelp) ------\r\n"
                             "  Symbol resolution failed because DbgHelp itself crashed.\r\n"
                             "  The raw stack addresses in Phase 1 can be resolved offline:\r\n"
                             "    WinDbg: .reload /f StreaMonitor.exe; kb\r\n"
                             "    Or: dumpbin /disasm StreaMonitor.exe with the .pdb\r\n\r\n");
        }

        off2 = bufAppend(buf, off2, mx, "\r\n================================================================\r\n");
        off2 = bufAppend(buf, off2, mx, "End of crash report.\r\n");

        // Write phase 2 and close
        if (hFile != INVALID_HANDLE_VALUE)
        {
            writeRaw(hFile, buf, off2);
            FlushFileBuffers(hFile);
            CloseHandle(hFile);
        }

        // ══════════════════════════════════════════════════════════
        // PHASE 3: MiniDump for WinDbg / Visual Studio analysis.
        // Also wrapped in __try — if the heap is destroyed, this
        // will fail, but phases 1+2 are already safe on disk.
        // ══════════════════════════════════════════════════════════
        __try
        {
            static char dmpPath[1024];
            snprintf(dmpPath, sizeof(dmpPath), "%s/crash_%s.dmp",
                     g_crashDir, makeTimestampSafe());

            HANDLE dmpFile = CreateFileA(dmpPath, GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (dmpFile != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;

                MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dmpFile,
                                  static_cast<MINIDUMP_TYPE>(
                                      MiniDumpWithDataSegs |
                                      MiniDumpWithHandleData |
                                      MiniDumpWithThreadInfo |
                                      MiniDumpWithUnloadedModules |
                                      MiniDumpWithFullMemoryInfo),
                                  &mei, nullptr, nullptr);
                CloseHandle(dmpFile);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            // MiniDump failed — phases 1+2 are on disk.
        }

        // stderr (may fail if CRT is damaged)
        {
            static char stderrBuf[512];
            int n = snprintf(stderrBuf, sizeof(stderrBuf),
                             "\n[FATAL] StreaMonitor crashed: %s (0x%08lX) at %p\n"
                             "[FATAL] Crash report: %s\n",
                             exceptionCodeToString(code), code, addr, crashPath);
            if (n > 0)
            {
                HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
                if (hErr != INVALID_HANDLE_VALUE)
                    writeRaw(hErr, stderrBuf, (size_t)n);
            }
        }

        return EXCEPTION_CONTINUE_SEARCH;
    }

    // ── Secondary handlers (pure call, invalid param, etc.) ─────

    static void purecallHandler()
    {
        CreateDirectoryA(g_crashDir, nullptr);
        static char path[1024];
        snprintf(path, sizeof(path), "%s/crash_%s_purecall.txt",
                 g_crashDir, makeTimestampSafe());

        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            static char msg[512];
            int n = snprintf(msg, sizeof(msg),
                             "StreaMonitor - Pure Virtual Function Call\r\n"
                             "Time: %s\r\n"
                             "A pure virtual function was called.\r\n",
                             makeReadableTimeSafe());
            if (n > 0)
                writeRaw(h, msg, (size_t)n);
            CloseHandle(h);
        }
        _exit(3);
    }

    static void invalidParameterHandler(const wchar_t *expr, const wchar_t *func,
                                        const wchar_t *file, unsigned int line,
                                        uintptr_t)
    {
        CreateDirectoryA(g_crashDir, nullptr);
        static char path[1024];
        snprintf(path, sizeof(path), "%s/crash_%s_invalidparam.txt",
                 g_crashDir, makeTimestampSafe());

        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            size_t off = 0;
            char *buf = g_crashBuf;
            const size_t mx = CRASH_BUF_SIZE;
            off = bufAppend(buf, off, mx, "StreaMonitor - Invalid Parameter\r\n");
            off = bufAppend(buf, off, mx, "Time: %s\r\n", makeReadableTimeSafe());
            if (expr)
                off = bufAppend(buf, off, mx, "Expression: %ls\r\n", expr);
            if (func)
                off = bufAppend(buf, off, mx, "Function  : %ls\r\n", func);
            if (file)
                off = bufAppend(buf, off, mx, "File      : %ls  Line: %u\r\n", file, line);
            writeRaw(h, buf, off);
            CloseHandle(h);
        }
        _exit(3);
    }

    // Helper: extract current exception info into buffer (uses C++ try/catch,
    // so it MUST be in a separate function from any __try/__except code).
    static size_t grabExceptionInfo(char *buf, size_t off, size_t mx)
    {
        try
        {
            if (auto eptr = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(eptr);
                }
                catch (const std::exception &ex)
                {
                    off = bufAppend(buf, off, mx, "Exception: %s\r\n", ex.what());
                }
                catch (...)
                {
                    off = bufAppend(buf, off, mx, "Exception: (unknown non-std::exception type)\r\n");
                }
            }
            else
            {
                off = bufAppend(buf, off, mx, "No active exception (terminate called directly or from noexcept violation).\r\n");
            }
        }
        catch (...)
        {
            off = bufAppend(buf, off, mx, "Failed to retrieve exception info (possible heap corruption)\r\n");
        }
        return off;
    }

    // Helper: symbolicated stack trace (uses __try/__except, must be separate from C++ EH)
    static size_t terminateStackTrace(char *buf, size_t off, size_t mx)
    {
        off = bufAppend(buf, off, mx, "\r\n-- Stack Trace -----------------------------------------\r\n");
        __try
        {
            HANDLE proc = GetCurrentProcess();
            SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            if (SymInitialize(proc, nullptr, TRUE))
            {
                void *stack[64];
                USHORT frames = CaptureStackBackTrace(0, 64, stack, nullptr);
                for (USHORT i = 0; i < frames; i++)
                {
                    DWORD64 pc = (DWORD64)stack[i];
                    alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 512];
                    SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
                    sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                    sym->MaxNameLen = 511;
                    DWORD64 symDisp = 0;

                    IMAGEHLP_LINE64 line{};
                    line.SizeOfStruct = sizeof(line);
                    DWORD lineDisp = 0;

                    if (SymFromAddr(proc, pc, &symDisp, sym))
                    {
                        off = bufAppend(buf, off, mx, "  #%-3d 0x%016llX  %s +0x%llX", i, pc, sym->Name, symDisp);
                        if (SymGetLineFromAddr64(proc, pc, &lineDisp, &line))
                            off = bufAppend(buf, off, mx, "  [%s:%lu]", line.FileName, line.LineNumber);
                        off = bufAppend(buf, off, mx, "\r\n");
                    }
                    else
                    {
                        off = bufAppend(buf, off, mx, "  #%-3d 0x%016llX  <unknown>\r\n", i, pc);
                    }
                }
                SymCleanup(proc);
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            off = bufAppend(buf, off, mx, "  (symbol resolution failed - heap corruption?)\r\n");
        }
        return off;
    }

    static void terminateHandler()
    {
        CreateDirectoryA(g_crashDir, nullptr);
        static char path[1024];
        snprintf(path, sizeof(path), "%s/crash_%s_terminate.txt",
                 g_crashDir, makeTimestampSafe());

        HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            _exit(3);
            return;
        }

        size_t off = 0;
        char *buf = g_crashBuf;
        const size_t mx = CRASH_BUF_SIZE;
        off = bufAppend(buf, off, mx, "StreaMonitor - std::terminate() called\r\n");
        off = bufAppend(buf, off, mx, "Time: %s\r\n", makeReadableTimeSafe());
        off = bufAppend(buf, off, mx, "This usually means an unhandled exception on a background thread.\r\n\r\n");

        // Get exception info (separate function — uses C++ try/catch)
        off = grabExceptionInfo(buf, off, mx);

        // Symbolicated stack trace (separate function — uses __try/__except)
        off = terminateStackTrace(buf, off, mx);

        off = bufAppend(buf, off, mx, "\r\n");
        writeRaw(h, buf, off);
        FlushFileBuffers(h);
        CloseHandle(h);

        // stderr
        {
            static char msg[256];
            int n = snprintf(msg, sizeof(msg),
                             "\n[FATAL] std::terminate() called - crash report: %s\n", path);
            if (n > 0)
            {
                HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
                if (hErr != INVALID_HANDLE_VALUE)
                    writeRaw(hErr, msg, (size_t)n);
            }
        }
        _exit(3);
    }

    void installCrashHandler(const std::string &crashDir)
    {
        std::call_once(g_initFlag, [&]()
                       {
            // Copy into pre-allocated static buffer (no heap dependency at crash time)
            strncpy(g_crashDir, crashDir.c_str(), sizeof(g_crashDir) - 1);
            g_crashDir[sizeof(g_crashDir) - 1] = '\0';

            // Create the crash directory up front (Win32 API — no heap)
            CreateDirectoryA(g_crashDir, nullptr);

            // SEH top-level exception filter
            SetUnhandledExceptionFilter(crashFilter);

            // C++ runtime handlers
            _set_purecall_handler(purecallHandler);
            _set_invalid_parameter_handler(invalidParameterHandler);
            std::set_terminate(terminateHandler);

            // abort() signal handler
            signal(SIGABRT, [](int) {
                CreateDirectoryA(g_crashDir, nullptr);
                static char path[1024];
                snprintf(path, sizeof(path), "%s/crash_%s_abort.txt",
                         g_crashDir, makeTimestampSafe());
                HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (h != INVALID_HANDLE_VALUE) {
                    static char msg[512];
                    int n = snprintf(msg, sizeof(msg),
                        "StreaMonitor - abort() called\r\nTime: %s\r\n",
                        makeReadableTimeSafe());
                    if (n > 0) writeRaw(h, msg, (size_t)n);
                    CloseHandle(h);
                }
                _exit(3);
            }); });
    }

    // ═══════════════════════════════════════════════════════════
    //  UNIX IMPLEMENTATION  (Linux / macOS)
    // ═══════════════════════════════════════════════════════════
#else

    static const char *signalName(int sig)
    {
        switch (sig)
        {
        case SIGSEGV:
            return "SIGSEGV (Segmentation Fault)";
        case SIGABRT:
            return "SIGABRT (Aborted)";
        case SIGFPE:
            return "SIGFPE (Floating Point Exception)";
        case SIGILL:
            return "SIGILL (Illegal Instruction)";
        case SIGBUS:
            return "SIGBUS (Bus Error)";
        case SIGTRAP:
            return "SIGTRAP (Trap)";
        default:
            return "UNKNOWN SIGNAL";
        }
    }

    // async-signal-safe write
    static void writeAllFd(int fd, const char *data, size_t len)
    {
        while (len > 0)
        {
            ssize_t n = write(fd, data, len);
            if (n <= 0)
                break;
            data += n;
            len -= (size_t)n;
        }
    }

    static void crashSignalHandler(int sig, siginfo_t *info, void * /*context*/)
    {
        (void)mkdir(g_crashDir, 0755);

        const char *crashPath = makeCrashPath();
        int fd = open(crashPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
        {
            const char *msg = "StreaMonitor CRASHED (could not open crash file)\n";
            writeAllFd(STDERR_FILENO, msg, strlen(msg));
            _exit(128 + sig);
        }

        size_t off = 0;
        char *buf = g_crashBuf;
        const size_t mx = CRASH_BUF_SIZE;

        off = bufAppend(buf, off, mx, "================================================================\n");
        off = bufAppend(buf, off, mx, "  StreaMonitor - Crash Report\n");
        off = bufAppend(buf, off, mx, "================================================================\n\n");
        off = bufAppend(buf, off, mx, "  Time   : %s\n", makeReadableTimeSafe());
        off = bufAppend(buf, off, mx, "  Signal : %d - %s\n", sig, signalName(sig));
        off = bufAppend(buf, off, mx, "  Address: %p\n", info->si_addr);
        off = bufAppend(buf, off, mx, "  PID    : %d\n", getpid());
        off = bufAppend(buf, off, mx, "  Code   : %d\n\n", info->si_code);

        // Stack trace via backtrace()
        off = bufAppend(buf, off, mx, "-- Stack Trace -----------------------------------------\n");

        constexpr int MAX_FRAMES = 128;
        void *frames[MAX_FRAMES];
        int count = backtrace(frames, MAX_FRAMES);
        char **symbols = backtrace_symbols(frames, count);

        for (int i = 0; i < count; i++)
        {
            if (symbols && symbols[i])
            {
                char *begin = nullptr;
                char *end = nullptr;
                char *plus = nullptr;

                for (char *p = symbols[i]; *p; p++)
                {
                    if (*p == '(')
                        begin = p + 1;
                    else if (*p == '+' && begin)
                        plus = p;
                    else if (*p == ')' && plus)
                    {
                        end = plus;
                        break;
                    }
                }

                if (begin && end && end > begin)
                {
                    char saved = *end;
                    *end = '\0';
                    int status = 0;
                    char *demangled = abi::__cxa_demangle(begin, nullptr, nullptr, &status);
                    *end = saved;

                    if (status == 0 && demangled)
                    {
                        *begin = '\0';
                        off = bufAppend(buf, off, mx, "  #%-3d %s%s%s\n", i, symbols[i], demangled, end);
                        *begin = saved;
                        free(demangled);
                    }
                    else
                    {
                        off = bufAppend(buf, off, mx, "  #%-3d %s\n", i, symbols[i]);
                    }
                }
                else
                {
                    off = bufAppend(buf, off, mx, "  #%-3d %s\n", i, symbols[i]);
                }
            }
            else
            {
                off = bufAppend(buf, off, mx, "  #%-3d 0x%p\n", i, frames[i]);
            }
        }

        if (symbols)
            free(symbols);

#ifdef __linux__
        off = bufAppend(buf, off, mx, "\n-- Memory Map ------------------------------------------\n");
        int mapsFd = open("/proc/self/maps", O_RDONLY);
        if (mapsFd >= 0)
        {
            while (off < mx - 1)
            {
                ssize_t n = read(mapsFd, buf + off, mx - off - 1);
                if (n <= 0)
                    break;
                off += (size_t)n;
            }
            close(mapsFd);
        }
#endif

        off = bufAppend(buf, off, mx, "\n================================================================\n");
        off = bufAppend(buf, off, mx, "End of crash report.\n");

        writeAllFd(fd, buf, off);
        close(fd);

        // stderr
        {
            static char stderrMsg[512];
            int n = snprintf(stderrMsg, sizeof(stderrMsg),
                             "\n[FATAL] StreaMonitor crashed: signal %d (%s)\n"
                             "[FATAL] Crash report: %s\n",
                             sig, signalName(sig), crashPath);
            if (n > 0)
                writeAllFd(STDERR_FILENO, stderrMsg, (size_t)n);
        }

        // Re-raise with default handler
        struct sigaction sa{};
        sa.sa_handler = SIG_DFL;
        sigemptyset(&sa.sa_mask);
        sigaction(sig, &sa, nullptr);
        raise(sig);
    }

    void installCrashHandler(const std::string &crashDir)
    {
        std::call_once(g_initFlag, [&]()
                       {
            strncpy(g_crashDir, crashDir.c_str(), sizeof(g_crashDir) - 1);
            g_crashDir[sizeof(g_crashDir) - 1] = '\0';

            try { std::filesystem::create_directories(g_crashDir); }
            catch (...) {}

            struct sigaction sa{};
            sa.sa_sigaction = crashSignalHandler;
            sa.sa_flags = SA_SIGINFO | SA_RESETHAND;
            sigemptyset(&sa.sa_mask);

            sigaction(SIGSEGV, &sa, nullptr);
            sigaction(SIGABRT, &sa, nullptr);
            sigaction(SIGFPE,  &sa, nullptr);
            sigaction(SIGILL,  &sa, nullptr);
            sigaction(SIGBUS,  &sa, nullptr);
            sigaction(SIGTRAP, &sa, nullptr);

            std::set_terminate([]() {
                (void)mkdir(g_crashDir, 0755);
                static char path[1024];
                snprintf(path, sizeof(path), "%s/crash_%s_terminate.txt",
                         g_crashDir, makeTimestampSafe());
                int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    size_t off = 0;
                    char* buf = g_crashBuf;
                    const size_t mx = CRASH_BUF_SIZE;
                    off = bufAppend(buf, off, mx, "StreaMonitor - std::terminate() called\n");
                    off = bufAppend(buf, off, mx, "Time: %s\n", makeReadableTimeSafe());
                    if (auto eptr = std::current_exception()) {
                        try { std::rethrow_exception(eptr); }
                        catch (const std::exception& ex) {
                            off = bufAppend(buf, off, mx, "Exception: %s\n", ex.what());
                        }
                        catch (...) {
                            off = bufAppend(buf, off, mx, "Exception: (unknown type)\n");
                        }
                    }
                    writeAllFd(fd, buf, off);
                    close(fd);
                }
                std::abort();
            }); });
    }

#endif // _WIN32 / else

} // namespace sm
