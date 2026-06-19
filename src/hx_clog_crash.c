/*
 * hx_clog - crash handler.
 *
 * Conservative crash capture: on a fatal signal / unhandled exception we write
 * a crash report (exception info + stacktrace + last N business logs) using
 * low-level I/O, then optionally chain to the previous handler or terminate.
 *
 * Signal-safety policy (POSIX): the handler avoids malloc, stdio streams and
 * any function that may take a lock it could deadlock on. Symbolization uses
 * dladdr() only — no popen()/addr2line, which fork and allocate and made the
 * old handler deadlock-prone exactly when the heap was corrupted. Resolve
 * addresses offline with addr2line/atos using the module+offset values in the
 * report. The handler runs on a dedicated sigaltstack so stack-overflow
 * crashes still produce a report.
 *
 * Windows: the exception filter deliberately does NOT call hx_clog_flush() —
 * the crashing thread may hold the sink lock and would self-deadlock. The
 * report's "last_logs" section (ring buffer) carries the most recent lines
 * instead.
 *
 * Built only when HX_CLOG_ENABLE_CRASH is defined. Stacktrace capture,
 * symbolization and minidumps are additionally gated by the
 * HX_CLOG_ENABLE_STACKTRACE / HX_CLOG_ENABLE_SYMBOLIZE /
 * HX_CLOG_ENABLE_MINIDUMP build options, which also set the config defaults.
 */
#include "hx_clog_internal.h"

#if defined(HX_CLOG_ENABLE_CRASH)

#include <fcntl.h>
#include <signal.h>
#if defined(HX_PLATFORM_WINDOWS)
#  include <share.h>
#  include <sys/stat.h>
#endif

static hx_clog_crash_config_t g_cc;
static int g_crash_installed = 0;
static char g_crash_dir[HX_CLOG_PATH_MAX];

void hx_clog_crash_config_default(hx_clog_crash_config_t* config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->crash_dir = "./logs";
    config->dump_fault_location = 1;
#if defined(HX_CLOG_ENABLE_STACKTRACE)
    config->dump_stacktrace = 1;
#else
    config->dump_stacktrace = 0;
#endif
    config->dump_registers = 0;
#if defined(HX_CLOG_ENABLE_SYMBOLIZE)
    config->symbolize_stacktrace = 1;
#else
    config->symbolize_stacktrace = 0;
#endif
    config->stacktrace_max_depth = 64;
    config->symbol_search_path = NULL;
#if defined(HX_CLOG_ENABLE_MINIDUMP)
    config->create_minidump = 1;
#else
    config->create_minidump = 0;
#endif
    config->chain_previous_handler = 1;
}

/* ---- tiny async-signal-safe writers ---- */
static void s_write(int fd, const char* s) {
#if defined(HX_PLATFORM_WINDOWS)
    _write(fd, s, (unsigned int)strlen(s));
#else
    ssize_t w = write(fd, s, strlen(s));
    (void)w;
#endif
}

static void s_write_n(int fd, const char* s, unsigned int n) {
#if defined(HX_PLATFORM_WINDOWS)
    _write(fd, s, n);
#else
    ssize_t w = write(fd, s, n);
    (void)w;
#endif
}

static void s_write_hex(int fd, unsigned long long v) {
    char buf[19];
    int i;
    const char* hex = "0123456789abcdef";
    buf[0] = '0'; buf[1] = 'x';
    for (i = 0; i < 16; ++i) {
        buf[2 + i] = hex[(v >> ((15 - i) * 4)) & 0xF];
    }
    s_write_n(fd, buf, 18);
}

static void s_write_uint(int fd, unsigned long v) {
    char tmp[24];
    int i = 0, j;
    if (v == 0) {
        s_write(fd, "0");
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (j = i - 1; j >= 0; --j) {
        s_write_n(fd, &tmp[j], 1);
    }
}

/* Build a crash file path with a timestamp. snprintf/localtime are not on the
 * strict async-signal-safe list but do not allocate in practice; the report
 * file name is worth the residual risk. */
static int open_crash_file(char* path_out, unsigned int cap) {
    hx_timestamp_t ts;
    struct tm tmv;
    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);
    snprintf(path_out, cap, "%s/crash_%04d%02d%02d_%02d%02d%02d.log",
             g_crash_dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
#if defined(HX_PLATFORM_WINDOWS)
    {
        wchar_t wpath[HX_CLOG_PATH_MAX];
        int fd = -1;
        if (hx_utf8_to_wide(path_out, wpath, HX_CLOG_PATH_MAX) < 0) {
            return -1;
        }
        _wsopen_s(&fd, wpath, _O_CREAT | _O_WRONLY | _O_TRUNC,
                  _SH_DENYNO, _S_IREAD | _S_IWRITE);
        return fd;
    }
#else
    return open(path_out, O_CREAT | O_WRONLY | O_TRUNC, 0644);
#endif
}

static void write_common_header(int fd, const char* exc_type) {
    hx_timestamp_t ts;
    struct tm tmv;
    char tbuf[40];
    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d.%03u",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.msec);

    s_write(fd, "========== hx_clog crash report ==========\n");
    s_write(fd, "time: ");      s_write(fd, tbuf); s_write(fd, "\n");
    s_write(fd, "pid: ");       s_write_uint(fd, hx_get_pid()); s_write(fd, "\n");
    s_write(fd, "thread: ");    s_write_uint(fd, hx_get_tid()); s_write(fd, "\n");
    s_write(fd, "\nexception:\n  type: "); s_write(fd, exc_type); s_write(fd, "\n");
}

static void write_footer_and_logs(int fd) {
    hx_clog_crash_callback_t cb;
    void* cb_ud = NULL;

    s_write(fd, "\nlast_logs:\n");
    hx_ring_dump_fd(fd);

    /* user hook: append app-specific context (must be async-signal-safe) */
    cb = hx_crash_get_callback(&cb_ud);
    if (cb) {
        s_write(fd, "\nuser_context:\n");
        cb(fd, cb_ud);
    }
    s_write(fd, "==========================================\n");
}

/* ============================ Windows ============================ */
#if defined(HX_PLATFORM_WINDOWS)

#include <dbghelp.h>

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = NULL;

static const char* seh_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        default:                              return "EXCEPTION_UNKNOWN";
    }
}

#if defined(HX_CLOG_ENABLE_MINIDUMP)
static void write_minidump(EXCEPTION_POINTERS* ep) {
    char dmp[HX_CLOG_PATH_MAX];
    wchar_t wdmp[HX_CLOG_PATH_MAX];
    hx_timestamp_t ts;
    struct tm tmv;
    HANDLE hFile;
    MINIDUMP_EXCEPTION_INFORMATION mei;

    if (!g_cc.create_minidump) {
        return;
    }
    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);
    snprintf(dmp, sizeof(dmp), "%s/crash_%04d%02d%02d_%02d%02d%02d.dmp",
             g_crash_dir, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    if (hx_utf8_to_wide(dmp, wdmp, HX_CLOG_PATH_MAX) < 0) {
        return;
    }

    hFile = CreateFileW(wdmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        return;
    }
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                      MiniDumpWithIndirectlyReferencedMemory,
                      &mei, NULL, NULL);
    CloseHandle(hFile);
}
#endif /* HX_CLOG_ENABLE_MINIDUMP */

#if defined(HX_CLOG_ENABLE_STACKTRACE)
static void write_stacktrace_win(int fd, CONTEXT* ctx) {
    HANDLE proc = GetCurrentProcess();
    HANDLE thr  = GetCurrentThread();
    STACKFRAME64 frame;
    DWORD machine;
    int depth = 0;
    int maxd = g_cc.stacktrace_max_depth > 0 ? g_cc.stacktrace_max_depth : 64;

#if defined(HX_CLOG_ENABLE_SYMBOLIZE)
    if (g_cc.symbolize_stacktrace) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, g_cc.symbol_search_path, TRUE);
    }
#endif

    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrStack.Offset = ctx->Rsp;
#elif defined(_M_ARM64) || defined(__aarch64__)
    machine = IMAGE_FILE_MACHINE_ARM64;
    frame.AddrPC.Offset    = ctx->Pc;
    frame.AddrFrame.Offset = ctx->Fp;
    frame.AddrStack.Offset = ctx->Sp;
#else
    machine = IMAGE_FILE_MACHINE_I386;
    frame.AddrPC.Offset    = ctx->Eip;
    frame.AddrFrame.Offset = ctx->Ebp;
    frame.AddrStack.Offset = ctx->Esp;
#endif
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Mode = AddrModeFlat;

    s_write(fd, "\nstacktrace:\n");
    while (depth < maxd &&
           StackWalk64(machine, proc, thr, &frame, ctx, NULL,
                       SymFunctionTableAccess64, SymGetModuleBase64, NULL)) {
        DWORD64 addr = frame.AddrPC.Offset;
        if (addr == 0) {
            break;
        }
        s_write(fd, "  #");
        if (depth < 10) s_write(fd, "0");
        s_write_uint(fd, (unsigned long)depth);
        s_write(fd, " ");
        s_write_hex(fd, (unsigned long long)addr);

#if defined(HX_CLOG_ENABLE_SYMBOLIZE)
        if (g_cc.symbolize_stacktrace) {
            char symbuf[sizeof(SYMBOL_INFO) + 256];
            SYMBOL_INFO* sym = (SYMBOL_INFO*)symbuf;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 lineinfo;
            DWORD ldisp = 0;
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            if (SymFromAddr(proc, addr, &disp, sym)) {
                s_write(fd, " ");
                s_write(fd, sym->Name);
            }
            memset(&lineinfo, 0, sizeof(lineinfo));
            lineinfo.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
            if (SymGetLineFromAddr64(proc, addr, &ldisp, &lineinfo)) {
                s_write(fd, "\n      ");
                s_write(fd, lineinfo.FileName);
                s_write(fd, ":");
                s_write_uint(fd, (unsigned long)lineinfo.LineNumber);
            }
        }
#endif
        s_write(fd, "\n");
        ++depth;
    }

#if defined(HX_CLOG_ENABLE_SYMBOLIZE)
    if (g_cc.symbolize_stacktrace) {
        SymCleanup(proc);
    }
#endif
}
#endif /* HX_CLOG_ENABLE_STACKTRACE */

static void write_registers_win(int fd, CONTEXT* ctx) {
    s_write(fd, "\nregisters:\n");
#if defined(_M_X64) || defined(__x86_64__)
    s_write(fd, "  RIP: "); s_write_hex(fd, (unsigned long long)ctx->Rip); s_write(fd, "\n");
    s_write(fd, "  RSP: "); s_write_hex(fd, (unsigned long long)ctx->Rsp); s_write(fd, "\n");
    s_write(fd, "  RBP: "); s_write_hex(fd, (unsigned long long)ctx->Rbp); s_write(fd, "\n");
    s_write(fd, "  RAX: "); s_write_hex(fd, (unsigned long long)ctx->Rax); s_write(fd, "\n");
    s_write(fd, "  RBX: "); s_write_hex(fd, (unsigned long long)ctx->Rbx); s_write(fd, "\n");
    s_write(fd, "  RCX: "); s_write_hex(fd, (unsigned long long)ctx->Rcx); s_write(fd, "\n");
    s_write(fd, "  RDX: "); s_write_hex(fd, (unsigned long long)ctx->Rdx); s_write(fd, "\n");
    s_write(fd, "  RSI: "); s_write_hex(fd, (unsigned long long)ctx->Rsi); s_write(fd, "\n");
    s_write(fd, "  RDI: "); s_write_hex(fd, (unsigned long long)ctx->Rdi); s_write(fd, "\n");
    s_write(fd, "  R8 : "); s_write_hex(fd, (unsigned long long)ctx->R8);  s_write(fd, "\n");
    s_write(fd, "  R9 : "); s_write_hex(fd, (unsigned long long)ctx->R9);  s_write(fd, "\n");
    s_write(fd, "  R10: "); s_write_hex(fd, (unsigned long long)ctx->R10); s_write(fd, "\n");
    s_write(fd, "  R11: "); s_write_hex(fd, (unsigned long long)ctx->R11); s_write(fd, "\n");
    s_write(fd, "  R12: "); s_write_hex(fd, (unsigned long long)ctx->R12); s_write(fd, "\n");
    s_write(fd, "  R13: "); s_write_hex(fd, (unsigned long long)ctx->R13); s_write(fd, "\n");
    s_write(fd, "  R14: "); s_write_hex(fd, (unsigned long long)ctx->R14); s_write(fd, "\n");
    s_write(fd, "  R15: "); s_write_hex(fd, (unsigned long long)ctx->R15); s_write(fd, "\n");
#elif defined(_M_ARM64) || defined(__aarch64__)
    {
        int i;
        for (i = 0; i < 31; ++i) {
            s_write(fd, "  X"); s_write_uint(fd, (unsigned long)i); s_write(fd, ": ");
            s_write_hex(fd, (unsigned long long)ctx->X[i]);
            s_write(fd, "\n");
        }
        s_write(fd, "  SP: "); s_write_hex(fd, (unsigned long long)ctx->Sp); s_write(fd, "\n");
        s_write(fd, "  PC: "); s_write_hex(fd, (unsigned long long)ctx->Pc); s_write(fd, "\n");
    }
#else
    s_write(fd, "  EIP: "); s_write_hex(fd, (unsigned long long)ctx->Eip); s_write(fd, "\n");
    s_write(fd, "  ESP: "); s_write_hex(fd, (unsigned long long)ctx->Esp); s_write(fd, "\n");
    s_write(fd, "  EBP: "); s_write_hex(fd, (unsigned long long)ctx->Ebp); s_write(fd, "\n");
    s_write(fd, "  EAX: "); s_write_hex(fd, (unsigned long long)ctx->Eax); s_write(fd, "\n");
    s_write(fd, "  EBX: "); s_write_hex(fd, (unsigned long long)ctx->Ebx); s_write(fd, "\n");
    s_write(fd, "  ECX: "); s_write_hex(fd, (unsigned long long)ctx->Ecx); s_write(fd, "\n");
    s_write(fd, "  EDX: "); s_write_hex(fd, (unsigned long long)ctx->Edx); s_write(fd, "\n");
    s_write(fd, "  ESI: "); s_write_hex(fd, (unsigned long long)ctx->Esi); s_write(fd, "\n");
    s_write(fd, "  EDI: "); s_write_hex(fd, (unsigned long long)ctx->Edi); s_write(fd, "\n");
#endif
}

static LONG WINAPI win_exception_filter(EXCEPTION_POINTERS* ep) {
    char path[HX_CLOG_PATH_MAX];
    int fd;
    DWORD code = ep->ExceptionRecord->ExceptionCode;

    /* NOTE: deliberately no hx_clog_flush() here — the crashing thread may
     * hold the sink lock; the ring buffer below carries the recent lines */

    fd = open_crash_file(path, sizeof(path));
    if (fd >= 0) {
        write_common_header(fd, seh_name(code));
        if (g_cc.dump_fault_location) {
            s_write(fd, "  code: ");
            s_write_hex(fd, (unsigned long long)code);
            s_write(fd, "\n  instruction_pointer: ");
#if defined(_M_X64) || defined(__x86_64__)
            s_write_hex(fd, (unsigned long long)ep->ContextRecord->Rip);
#elif defined(_M_ARM64) || defined(__aarch64__)
            s_write_hex(fd, (unsigned long long)ep->ContextRecord->Pc);
#else
            s_write_hex(fd, (unsigned long long)ep->ContextRecord->Eip);
#endif
            s_write(fd, "\n");
            if (code == EXCEPTION_ACCESS_VIOLATION &&
                ep->ExceptionRecord->NumberParameters >= 2) {
                s_write(fd, "  fault_address: ");
                s_write_hex(fd, (unsigned long long)
                            ep->ExceptionRecord->ExceptionInformation[1]);
                s_write(fd, "\n");
            }
        }
#if defined(HX_CLOG_ENABLE_STACKTRACE)
        if (g_cc.dump_stacktrace) {
            CONTEXT ctx = *ep->ContextRecord;
            write_stacktrace_win(fd, &ctx);
        }
#endif
        if (g_cc.dump_registers) {
            write_registers_win(fd, ep->ContextRecord);
        }
        write_footer_and_logs(fd);
        _close(fd);
    }

#if defined(HX_CLOG_ENABLE_MINIDUMP)
    write_minidump(ep);
#endif

    if (g_cc.chain_previous_handler && g_prev_filter) {
        return g_prev_filter(ep);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int hx_clog_install_crash_handler(const hx_clog_crash_config_t* config) {
    if (g_crash_installed) {
        return HX_CLOG_OK;
    }
    if (config) {
        g_cc = *config;
    } else {
        hx_clog_crash_config_default(&g_cc);
    }
    strncpy(g_crash_dir, g_cc.crash_dir ? g_cc.crash_dir : "./logs",
            sizeof(g_crash_dir) - 1);
    hx_mkdir_p(g_crash_dir);
    hx_ring_init();

    g_prev_filter = SetUnhandledExceptionFilter(win_exception_filter);
    g_crash_installed = 1;
    return HX_CLOG_OK;
}

void hx_clog_uninstall_crash_handler(void) {
    if (!g_crash_installed) {
        return;
    }
    SetUnhandledExceptionFilter(g_prev_filter);
    g_crash_installed = 0;
}

/* ============================ POSIX ============================ */
#else

#include <dlfcn.h>

/* Backtrace capture: glibc and Apple platforms provide <execinfo.h>;
 * Android (bionic) and musl do not, so fall back to the C++ ABI unwinder,
 * which is available everywhere as part of the compiler runtime. */
#if defined(__GLIBC__) || defined(HX_PLATFORM_APPLE)
#  define HX_CRASH_HAVE_EXECINFO 1
#  include <execinfo.h>
#else
#  include <unwind.h>
typedef struct {
    void** frames;
    int    max;
    int    count;
} hx_unwind_state_t;

static _Unwind_Reason_Code hx_unwind_cb(struct _Unwind_Context* ctx, void* arg) {
    hx_unwind_state_t* st = (hx_unwind_state_t*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc) {
        if (st->count >= st->max) {
            return _URC_END_OF_STACK;
        }
        st->frames[st->count++] = (void*)pc;
    }
    return _URC_NO_REASON;
}
#endif

static int hx_capture_backtrace(void** frames, int max) {
#if defined(HX_CRASH_HAVE_EXECINFO)
    return backtrace(frames, max);
#else
    hx_unwind_state_t st;
    st.frames = frames;
    st.max = max;
    st.count = 0;
    _Unwind_Backtrace(hx_unwind_cb, &st);
    return st.count;
#endif
}

static const int k_signals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
#define K_NSIGNALS ((int)(sizeof(k_signals) / sizeof(k_signals[0])))
static struct sigaction g_prev_actions[K_NSIGNALS];

/* Dedicated signal stack so stack-overflow SIGSEGV still gets a report. */
static char g_altstack[64 * 1024];
static stack_t g_prev_altstack;
static int g_altstack_installed = 0;

static const char* signal_name(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV";
        case SIGABRT: return "SIGABRT";
        case SIGFPE:  return "SIGFPE";
        case SIGILL:  return "SIGILL";
        case SIGBUS:  return "SIGBUS";
        default:      return "SIGNAL";
    }
}

#if defined(__linux__)
#  include <ucontext.h>
#endif

static void write_registers_posix(int fd, void* ucontext) {
    s_write(fd, "\nregisters:\n");
    if (!ucontext) {
        s_write(fd, "  unavailable\n");
        return;
    }
#if defined(__linux__) && defined(__x86_64__)
    {
        ucontext_t* uc = (ucontext_t*)ucontext;
        greg_t* g = uc->uc_mcontext.gregs;
        s_write(fd, "  RIP: "); s_write_hex(fd, (unsigned long long)g[REG_RIP]); s_write(fd, "\n");
        s_write(fd, "  RSP: "); s_write_hex(fd, (unsigned long long)g[REG_RSP]); s_write(fd, "\n");
        s_write(fd, "  RBP: "); s_write_hex(fd, (unsigned long long)g[REG_RBP]); s_write(fd, "\n");
        s_write(fd, "  RAX: "); s_write_hex(fd, (unsigned long long)g[REG_RAX]); s_write(fd, "\n");
        s_write(fd, "  RBX: "); s_write_hex(fd, (unsigned long long)g[REG_RBX]); s_write(fd, "\n");
        s_write(fd, "  RCX: "); s_write_hex(fd, (unsigned long long)g[REG_RCX]); s_write(fd, "\n");
        s_write(fd, "  RDX: "); s_write_hex(fd, (unsigned long long)g[REG_RDX]); s_write(fd, "\n");
        s_write(fd, "  RSI: "); s_write_hex(fd, (unsigned long long)g[REG_RSI]); s_write(fd, "\n");
        s_write(fd, "  RDI: "); s_write_hex(fd, (unsigned long long)g[REG_RDI]); s_write(fd, "\n");
    }
#elif defined(__linux__) && defined(__aarch64__)
    {
        ucontext_t* uc = (ucontext_t*)ucontext;
        int i;
        for (i = 0; i < 31; ++i) {
            s_write(fd, "  X"); s_write_uint(fd, (unsigned long)i); s_write(fd, ": ");
            s_write_hex(fd, (unsigned long long)uc->uc_mcontext.regs[i]);
            s_write(fd, "\n");
        }
        s_write(fd, "  SP: "); s_write_hex(fd, (unsigned long long)uc->uc_mcontext.sp); s_write(fd, "\n");
        s_write(fd, "  PC: "); s_write_hex(fd, (unsigned long long)uc->uc_mcontext.pc); s_write(fd, "\n");
    }
#else
    s_write(fd, "  unavailable on this POSIX architecture\n");
#endif
}

/* One stack frame: address, then module + symbol + offset via dladdr (no
 * fork, no malloc). Offline: addr2line -e <module> <module_offset>. */
static void write_frame_posix(int fd, void* frame, int depth) {
    s_write(fd, "  #");
    if (depth < 10) s_write(fd, "0");
    s_write_uint(fd, (unsigned long)depth);
    s_write(fd, " ");
    s_write_hex(fd, (unsigned long long)(size_t)frame);

#if defined(HX_CLOG_ENABLE_SYMBOLIZE)
    if (g_cc.symbolize_stacktrace) {
        Dl_info info;
        if (dladdr(frame, &info) && info.dli_fname) {
            s_write(fd, " ");
            s_write(fd, info.dli_fname);
            if (info.dli_fbase) {
                s_write(fd, "+");
                s_write_hex(fd, (unsigned long long)
                            ((char*)frame - (char*)info.dli_fbase));
            }
            if (info.dli_sname) {
                s_write(fd, " ");
                s_write(fd, info.dli_sname);
                if (info.dli_saddr) {
                    s_write(fd, "+");
                    s_write_hex(fd, (unsigned long long)
                                ((char*)frame - (char*)info.dli_saddr));
                }
            }
        }
    }
#endif
    s_write(fd, "\n");
}

static void posix_handler(int sig, siginfo_t* info, void* ucontext) {
    char path[HX_CLOG_PATH_MAX];
    int fd;
    int i;

    fd = open_crash_file(path, sizeof(path));
    if (fd >= 0) {
        write_common_header(fd, signal_name(sig));
        s_write(fd, "  signal: ");
        s_write_uint(fd, (unsigned long)sig);
        s_write(fd, "\n");
        if (g_cc.dump_fault_location && info) {
            s_write(fd, "  fault_address: ");
            s_write_hex(fd, (unsigned long long)(size_t)info->si_addr);
            s_write(fd, "\n");
        }
#if defined(HX_CLOG_ENABLE_STACKTRACE)
        if (g_cc.dump_stacktrace) {
            void* frames[256];
            int maxd = g_cc.stacktrace_max_depth > 0
                           ? g_cc.stacktrace_max_depth : 64;
            int nframes;
            s_write(fd, "\nstacktrace:\n");
            nframes = hx_capture_backtrace(frames, maxd < 256 ? maxd : 256);
            for (i = 0; i < nframes; ++i) {
                write_frame_posix(fd, frames[i], i);
            }
        }
#endif
        if (g_cc.dump_registers) {
            write_registers_posix(fd, ucontext);
        }
        write_footer_and_logs(fd);
        close(fd);
    }

    /* restore and re-raise to get default behaviour / chain */
    for (i = 0; i < K_NSIGNALS; ++i) {
        if (k_signals[i] == sig) {
            if (g_cc.chain_previous_handler) {
                sigaction(sig, &g_prev_actions[i], NULL);
            } else {
                signal(sig, SIG_DFL);
            }
            break;
        }
    }
    raise(sig);
}

int hx_clog_install_crash_handler(const hx_clog_crash_config_t* config) {
    struct sigaction sa;
    stack_t ss;
    int i;

    if (g_crash_installed) {
        return HX_CLOG_OK;
    }
    if (config) {
        g_cc = *config;
    } else {
        hx_clog_crash_config_default(&g_cc);
    }
    strncpy(g_crash_dir, g_cc.crash_dir ? g_cc.crash_dir : "./logs",
            sizeof(g_crash_dir) - 1);
    if (hx_mkdir_p(g_crash_dir) != 0) {
        hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                             "crash handler: could not create the crash directory;"
                             " reports may fail to write");
    }
    hx_ring_init();

#if defined(HX_CLOG_ENABLE_STACKTRACE) && defined(HX_CRASH_HAVE_EXECINFO)
    {
        /* warm up: glibc's first backtrace() call may dlopen/allocate, which
         * would not be safe inside the handler */
        void* warm[4];
        (void)backtrace(warm, 4);
    }
#endif

    /* run the handler on its own stack so stack-overflow faults still produce a
     * report. If sigaltstack is unavailable (some embedded/POSIX-lite targets)
     * do NOT pass SA_ONSTACK below: the kernel could otherwise be told to use a
     * stack that was never installed. */
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_altstack;
    ss.ss_size = sizeof(g_altstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, &g_prev_altstack) == 0) {
        g_altstack_installed = 1;
    } else {
        hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                             "crash handler: sigaltstack failed; installing "
                             "without an alternate signal stack");
    }

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    if (g_altstack_installed) {
        sa.sa_flags |= SA_ONSTACK;
    }
    sa.sa_sigaction = posix_handler;

    {
        int installed = 0;
        for (i = 0; i < K_NSIGNALS; ++i) {
            if (sigaction(k_signals[i], &sa, &g_prev_actions[i]) == 0) {
                installed++;
            } else {
                /* leave a known-good "previous action" so uninstall is a no-op
                 * for this slot rather than restoring garbage */
                memset(&g_prev_actions[i], 0, sizeof(g_prev_actions[i]));
                g_prev_actions[i].sa_handler = SIG_DFL;
            }
        }
        if (installed == 0) {
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "crash handler: no signal handlers could be "
                                 "installed");
            if (g_altstack_installed) {
                sigaltstack(&g_prev_altstack, NULL);
                g_altstack_installed = 0;
            }
            return HX_CLOG_ERR_PLATFORM;
        }
        if (installed < K_NSIGNALS) {
            hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                                 "crash handler: some signal handlers could not "
                                 "be installed");
        }
    }
    g_crash_installed = 1;
    return HX_CLOG_OK;
}

void hx_clog_uninstall_crash_handler(void) {
    int i;
    if (!g_crash_installed) {
        return;
    }
    for (i = 0; i < K_NSIGNALS; ++i) {
        sigaction(k_signals[i], &g_prev_actions[i], NULL);
    }
    if (g_altstack_installed) {
        sigaltstack(&g_prev_altstack, NULL);
        g_altstack_installed = 0;
    }
    g_crash_installed = 0;
}

#endif /* platform */

#endif /* HX_CLOG_ENABLE_CRASH */
