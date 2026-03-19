// ═══════════════════════════════════════════════════════════════════
//  StreaMonitor — Universal Crash Handler  (implementation)
//
//  Windows : SetUnhandledExceptionFilter + DbgHelp StackWalk64
//  Linux   : signal handlers + backtrace() / backtrace_symbols()
//  macOS   : signal handlers + backtrace() / backtrace_symbols()
//
//  Writes a timestamped crash report into the configured directory.
//  File-name format:  crash_YYYYMMDD_HHMMSS.txt
// ═══════════════════════════════════════════════════════════════════

#include "crash_handler.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <chrono>
#include <filesystem>
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
#include <execinfo.h> // backtrace, backtrace_symbols
#include <cxxabi.h>   // __cxa_demangle
#endif

namespace sm
{
    // ─── Shared state ───────────────────────────────────────────
    static std::string g_crashDir = "crashes";
    static std::once_flag g_initFlag;

    // ─── Helpers ────────────────────────────────────────────────

    static std::string makeTimestamp()
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tm);
        return buf;
    }

    static std::string makeReadableTime()
    {
        auto now = std::chrono::system_clock::now();
        auto tt = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &tt);
#else
        localtime_r(&tt, &tm);
#endif
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
        return buf;
    }

    static FILE *openCrashFile(const char *suffix = "")
    {
        try
        {
            std::filesystem::create_directories(g_crashDir);
        }
        catch (...)
        {
        }

        std::string ts = makeTimestamp();
        std::string path = g_crashDir + "/crash_" + ts + suffix + ".txt";

        FILE *f = nullptr;
#ifdef _WIN32
        fopen_s(&f, path.c_str(), "w");
#else
        f = std::fopen(path.c_str(), "w");
#endif
        return f;
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
        case 0xE06D7363:
            return "C++ EXCEPTION (0xE06D7363)";
        default:
            return "UNKNOWN";
        }
    }

    static LONG WINAPI crashFilter(EXCEPTION_POINTERS *ep)
    {
        FILE *f = openCrashFile();
        if (!f)
            return EXCEPTION_CONTINUE_SEARCH;

        DWORD code = ep->ExceptionRecord->ExceptionCode;
        void *addr = ep->ExceptionRecord->ExceptionAddress;

        fprintf(f, "════════════════════════════════════════════════════\n");
        fprintf(f, "  StreaMonitor — Crash Report\n");
        fprintf(f, "════════════════════════════════════════════════════\n\n");
        fprintf(f, "  Time     : %s\n", makeReadableTime().c_str());
        fprintf(f, "  Exception: 0x%08lX  (%s)\n", code, exceptionCodeToString(code));
        fprintf(f, "  Address  : 0x%p\n\n", addr);

        // Access violation details
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        {
            ULONG_PTR rw = ep->ExceptionRecord->ExceptionInformation[0];
            ULONG_PTR target = ep->ExceptionRecord->ExceptionInformation[1];
            fprintf(f, "  AV Type  : %s at 0x%p\n\n",
                    rw == 0 ? "READ" : rw == 1 ? "WRITE"
                                   : rw == 8   ? "DEP"
                                               : "UNKNOWN",
                    reinterpret_cast<void *>(target));
        }

        // ── Register dump ───────────────────────────────────────
        CONTEXT *ctx = ep->ContextRecord;
        fprintf(f, "── Registers ──────────────────────────────────────\n");
#ifdef _M_X64
        fprintf(f, "  RAX=%016llX  RBX=%016llX  RCX=%016llX\n", ctx->Rax, ctx->Rbx, ctx->Rcx);
        fprintf(f, "  RDX=%016llX  RSI=%016llX  RDI=%016llX\n", ctx->Rdx, ctx->Rsi, ctx->Rdi);
        fprintf(f, "  RBP=%016llX  RSP=%016llX  RIP=%016llX\n", ctx->Rbp, ctx->Rsp, ctx->Rip);
        fprintf(f, "  R8 =%016llX  R9 =%016llX  R10=%016llX\n", ctx->R8, ctx->R9, ctx->R10);
        fprintf(f, "  R11=%016llX  R12=%016llX  R13=%016llX\n", ctx->R11, ctx->R12, ctx->R13);
        fprintf(f, "  R14=%016llX  R15=%016llX\n", ctx->R14, ctx->R15);
#elif defined(_M_IX86)
        fprintf(f, "  EAX=%08lX  EBX=%08lX  ECX=%08lX  EDX=%08lX\n", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
        fprintf(f, "  ESI=%08lX  EDI=%08lX  EBP=%08lX  ESP=%08lX\n", ctx->Esi, ctx->Edi, ctx->Ebp, ctx->Esp);
        fprintf(f, "  EIP=%08lX\n", ctx->Eip);
#elif defined(_M_ARM64)
        for (int i = 0; i < 29; i++)
            fprintf(f, "  X%02d=%016llX%s", i, ctx->X[i], (i % 3 == 2) ? "\n" : "");
        fprintf(f, "\n  FP=%016llX  LR=%016llX  SP=%016llX  PC=%016llX\n", ctx->Fp, ctx->Lr, ctx->Sp, ctx->Pc);
#endif
        fprintf(f, "\n");

        // ── Stack trace via StackWalk64 ─────────────────────────
        HANDLE proc = GetCurrentProcess();
        HANDLE thread = GetCurrentThread();

        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, nullptr, TRUE);

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

        fprintf(f, "── Stack Trace ────────────────────────────────────\n");

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

            // Module name
            char moduleName[MAX_PATH] = "<unknown>";
            HMODULE hMod = nullptr;
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(pc), &hMod))
            {
                GetModuleFileNameA(hMod, moduleName, MAX_PATH);
                // Just the filename
                if (const char *slash = std::strrchr(moduleName, '\\'))
                    std::memmove(moduleName, slash + 1, std::strlen(slash + 1) + 1);
            }

            // Symbol name
            alignas(SYMBOL_INFO) char symBuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(symBuf);
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;

            DWORD64 symDisp = 0;
            bool hasSym = SymFromAddr(proc, pc, &symDisp, sym) == TRUE;

            // Source file + line
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            bool hasLine = SymGetLineFromAddr64(proc, pc, &lineDisp, &line) == TRUE;

            fprintf(f, "  #%-3d 0x%016llX  %s!", i, pc, moduleName);
            if (hasSym)
                fprintf(f, "%s +0x%llX", sym->Name, symDisp);
            else
                fprintf(f, "<unknown>");

            if (hasLine)
                fprintf(f, "  [%s:%lu]", line.FileName, line.LineNumber);

            fprintf(f, "\n");
        }

        SymCleanup(proc);

        // ── Loaded modules ──────────────────────────────────────
        fprintf(f, "\n── Loaded Modules ─────────────────────────────────\n");
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
                fprintf(f, "  0x%p  %7lu KB  %s\n",
                        mi.lpBaseOfDll, mi.SizeOfImage / 1024, name);
            }
        }

        fprintf(f, "\n════════════════════════════════════════════════════\n");
        fprintf(f, "End of crash report.\n");
        fclose(f);

        // Also dump a .dmp file for WinDbg analysis
        {
            std::string dmpPath = g_crashDir + "/crash_" + makeTimestamp() + ".dmp";
            HANDLE dmpFile = CreateFileA(dmpPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (dmpFile != INVALID_HANDLE_VALUE)
            {
                MINIDUMP_EXCEPTION_INFORMATION mei;
                mei.ThreadId = GetCurrentThreadId();
                mei.ExceptionPointers = ep;
                mei.ClientPointers = FALSE;

                MiniDumpWriteDump(proc, GetCurrentProcessId(), dmpFile,
                                  static_cast<MINIDUMP_TYPE>(
                                      MiniDumpWithDataSegs |
                                      MiniDumpWithHandleData |
                                      MiniDumpWithThreadInfo |
                                      MiniDumpWithUnloadedModules),
                                  &mei, nullptr, nullptr);
                CloseHandle(dmpFile);
            }
        }

        // Print to stderr as well
        fprintf(stderr, "\n[FATAL] StreaMonitor crashed: %s (0x%08lX) at %p\n",
                exceptionCodeToString(code), code, addr);
        fprintf(stderr, "[FATAL] Crash report written to: %s/\n", g_crashDir.c_str());

        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Also catch pure-call errors and invalid parameter
    static void purecallHandler()
    {
        FILE *f = openCrashFile("_purecall");
        if (f)
        {
            fprintf(f, "StreaMonitor — Pure Virtual Function Call\n");
            fprintf(f, "Time: %s\n", makeReadableTime().c_str());
            fprintf(f, "A pure virtual function was called.\n");
            fclose(f);
        }
        std::abort();
    }

    static void invalidParameterHandler(const wchar_t *expr, const wchar_t *func,
                                        const wchar_t *file, unsigned int line,
                                        uintptr_t)
    {
        FILE *f = openCrashFile("_invalidparam");
        if (f)
        {
            fprintf(f, "StreaMonitor — Invalid Parameter\n");
            fprintf(f, "Time: %s\n", makeReadableTime().c_str());
            if (expr)
                fwprintf(f, L"Expression: %s\n", expr);
            if (func)
                fwprintf(f, L"Function  : %s\n", func);
            if (file)
                fwprintf(f, L"File      : %s  Line: %u\n", file, line);
            fclose(f);
        }
        std::abort();
    }

    static void terminateHandler()
    {
        FILE *f = openCrashFile("_terminate");
        if (f)
        {
            fprintf(f, "StreaMonitor — std::terminate() called\n");
            fprintf(f, "Time: %s\n", makeReadableTime().c_str());

            // Try to get current exception info
            if (auto eptr = std::current_exception())
            {
                try
                {
                    std::rethrow_exception(eptr);
                }
                catch (const std::exception &ex)
                {
                    fprintf(f, "Exception: %s\n", ex.what());
                }
                catch (...)
                {
                    fprintf(f, "Exception: (unknown non-std::exception type)\n");
                }
            }
            fclose(f);
        }
        std::abort();
    }

    void installCrashHandler(const std::string &crashDir)
    {
        std::call_once(g_initFlag, [&]()
                       {
            g_crashDir = crashDir;

            // Create the crash directory upfront
            try { std::filesystem::create_directories(g_crashDir); }
            catch (...) {}

            // SEH top-level exception filter
            SetUnhandledExceptionFilter(crashFilter);

            // C++ runtime handlers
            _set_purecall_handler(purecallHandler);
            _set_invalid_parameter_handler(invalidParameterHandler);
            std::set_terminate(terminateHandler);

            // Also handle abort() signals
            signal(SIGABRT, [](int) {
                FILE* f = openCrashFile("_abort");
                if (f) {
                    fprintf(f, "StreaMonitor — abort() called\n");
                    fprintf(f, "Time: %s\n", makeReadableTime().c_str());
                    fclose(f);
                }
                // Don't call abort() again — just exit with error
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

    static void crashSignalHandler(int sig, siginfo_t *info, void * /*context*/)
    {
        // Use only async-signal-safe operations where possible,
        // but we need backtrace() which is widely used in signal handlers.

        FILE *f = openCrashFile();
        if (!f)
        {
            // Last resort: write to stderr
            const char *msg = "StreaMonitor CRASHED (could not open crash file)\n";
            (void)write(STDERR_FILENO, msg, strlen(msg));
            _exit(128 + sig);
        }

        fprintf(f, "════════════════════════════════════════════════════\n");
        fprintf(f, "  StreaMonitor — Crash Report\n");
        fprintf(f, "════════════════════════════════════════════════════\n\n");
        fprintf(f, "  Time   : %s\n", makeReadableTime().c_str());
        fprintf(f, "  Signal : %d — %s\n", sig, signalName(sig));
        fprintf(f, "  Address: %p\n", info->si_addr);
        fprintf(f, "  PID    : %d\n", getpid());
        fprintf(f, "  Code   : %d\n\n", info->si_code);

        // ── Stack trace via backtrace() ─────────────────────────
        fprintf(f, "── Stack Trace ────────────────────────────────────\n");

        constexpr int MAX_FRAMES = 128;
        void *frames[MAX_FRAMES];
        int count = backtrace(frames, MAX_FRAMES);
        char **symbols = backtrace_symbols(frames, count);

        for (int i = 0; i < count; i++)
        {
            // Try to demangle C++ symbols
            if (symbols && symbols[i])
            {
                // Format: "module(mangled+offset) [address]"
                // Try to extract and demangle the mangled name
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
                    // Null-terminate the mangled name temporarily
                    char saved = *end;
                    *end = '\0';

                    int status = 0;
                    char *demangled = abi::__cxa_demangle(begin, nullptr, nullptr, &status);
                    *end = saved;

                    if (status == 0 && demangled)
                    {
                        // Print with demangled name
                        *begin = '\0'; // null after '('
                        fprintf(f, "  #%-3d %s%s%s\n", i, symbols[i], demangled, end);
                        *begin = saved; // restore (not strictly needed)
                        free(demangled);
                    }
                    else
                    {
                        fprintf(f, "  #%-3d %s\n", i, symbols[i]);
                    }
                }
                else
                {
                    fprintf(f, "  #%-3d %s\n", i, symbols[i]);
                }
            }
            else
            {
                fprintf(f, "  #%-3d 0x%p\n", i, frames[i]);
            }
        }

        if (symbols)
            free(symbols);

        // ── /proc/self/maps (Linux only — shows loaded libraries) ──
#ifdef __linux__
        fprintf(f, "\n── Memory Map ─────────────────────────────────────\n");
        FILE *maps = fopen("/proc/self/maps", "r");
        if (maps)
        {
            char line[512];
            while (fgets(line, sizeof(line), maps))
                fputs(line, f);
            fclose(maps);
        }
#endif

        fprintf(f, "\n════════════════════════════════════════════════════\n");
        fprintf(f, "End of crash report.\n");
        fclose(f);

        // Print a short message to stderr
        fprintf(stderr, "\n[FATAL] StreaMonitor crashed: signal %d (%s)\n",
                sig, signalName(sig));
        fprintf(stderr, "[FATAL] Crash report written to: %s/\n", g_crashDir.c_str());

        // Re-raise with default handler to get the proper exit code
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
            g_crashDir = crashDir;

            // Create the crash directory upfront
            try { std::filesystem::create_directories(g_crashDir); }
            catch (...) {}

            // Install signal handlers with SA_SIGINFO for extra context
            struct sigaction sa{};
            sa.sa_sigaction = crashSignalHandler;
            sa.sa_flags = SA_SIGINFO | SA_RESETHAND;  // one-shot: reset after first signal
            sigemptyset(&sa.sa_mask);

            sigaction(SIGSEGV, &sa, nullptr);
            sigaction(SIGABRT, &sa, nullptr);
            sigaction(SIGFPE,  &sa, nullptr);
            sigaction(SIGILL,  &sa, nullptr);
            sigaction(SIGBUS,  &sa, nullptr);
            sigaction(SIGTRAP, &sa, nullptr);

            // Also register std::terminate handler
            std::set_terminate([]() {
                FILE* f = openCrashFile("_terminate");
                if (f) {
                    fprintf(f, "StreaMonitor — std::terminate() called\n");
                    fprintf(f, "Time: %s\n", makeReadableTime().c_str());
                    if (auto eptr = std::current_exception()) {
                        try { std::rethrow_exception(eptr); }
                        catch (const std::exception& ex) {
                            fprintf(f, "Exception: %s\n", ex.what());
                        }
                        catch (...) {
                            fprintf(f, "Exception: (unknown type)\n");
                        }
                    }
                    fclose(f);
                }
                std::abort();
            }); });
    }

#endif // _WIN32 / else

} // namespace sm
