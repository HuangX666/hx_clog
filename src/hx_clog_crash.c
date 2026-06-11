/*
 * hx_clog - crash handler.
 *
 * Conservative crash capture: on a fatal signal / unhandled exception we write
 * a crash report (exception info + stacktrace + last N business logs) using
 * low-level I/O, then optionally chain to the previous handler or terminate.
 *
 * Built only when HX_CLOG_ENABLE_CRASH is defined.
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
    config->dump_stacktrace = 1;
    config->dump_registers = 0;
    config->symbolize_stacktrace = 1;
    config->stacktrace_max_depth = 64;
    config->symbol_search_path = NULL;
    config->create_minidump = 1;
    config->chain_previous_handler = 1;
}

/* ---- tiny async-signal-safe-ish writers ---- */
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

/* Build a crash file path with a timestamp. Uses normal calls (pre-crash is
 * fine; this runs inside the handler but path building is read-only stack work).*/
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
        int fd = -1;
        _sopen_s(&fd, path_out, _O_CREAT | _O_WRONLY | _O_TRUNC,
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
    s_write(fd, "\nlast_logs:\n");
    hx_ring_dump_fd(fd);
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

static void write_minidump(EXCEPTION_POINTERS* ep) {
    char dmp[HX_CLOG_PATH_MAX];
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

    hFile = CreateFileA(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
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

static void write_stacktrace_win(int fd, CONTEXT* ctx) {
    HANDLE proc = GetCurrentProcess();
    HANDLE thr  = GetCurrentThread();
    STACKFRAME64 frame;
    DWORD machine;
    int depth = 0;
    int maxd = g_cc.stacktrace_max_depth > 0 ? g_cc.stacktrace_max_depth : 64;

    if (g_cc.symbolize_stacktrace) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        SymInitialize(proc, g_cc.symbol_search_path, TRUE);
    }

    memset(&frame, 0, sizeof(frame));
#if defined(_M_X64) || defined(__x86_64__)
    machine = IMAGE_FILE_MACHINE_AMD64;
    frame.AddrPC.Offset    = ctx->Rip;
    frame.AddrFrame.Offset = ctx->Rbp;
    frame.AddrStack.Offset = ctx->Rsp;
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
        s_write(fd, "\n");
        ++depth;
    }

    if (g_cc.symbolize_stacktrace) {
        SymCleanup(proc);
    }
}

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

    hx_clog_flush();

    fd = open_crash_file(path, sizeof(path));
    if (fd >= 0) {
        write_common_header(fd, seh_name(code));
        if (g_cc.dump_fault_location) {
            s_write(fd, "  code: ");
            s_write_hex(fd, (unsigned long long)code);
            s_write(fd, "\n  instruction_pointer: ");
#if defined(_M_X64) || defined(__x86_64__)
            s_write_hex(fd, (unsigned long long)ep->ContextRecord->Rip);
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
        if (g_cc.dump_stacktrace) {
            CONTEXT ctx = *ep->ContextRecord;
            write_stacktrace_win(fd, &ctx);
        }
        if (g_cc.dump_registers) {
            write_registers_win(fd, ep->ContextRecord);
        }
        write_footer_and_logs(fd);
        _close(fd);
    }

    write_minidump(ep);

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

#include <execinfo.h>
#include <dlfcn.h>
#include <ucontext.h>

static const int k_signals[] = { SIGSEGV, SIGABRT, SIGFPE, SIGILL, SIGBUS };
#define K_NSIGNALS ((int)(sizeof(k_signals) / sizeof(k_signals[0])))
static struct sigaction g_prev_actions[K_NSIGNALS];

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

static void write_symbolized_frame_posix(int fd, void* frame, int depth) {
    Dl_info info;
    s_write(fd, "  #");
    if (depth < 10) s_write(fd, "0");
    s_write_uint(fd, (unsigned long)depth);
    s_write(fd, " ");
    s_write_hex(fd, (unsigned long long)(size_t)frame);

    if (dladdr(frame, &info) && info.dli_fname) {
        unsigned long long offset = 0;
        s_write(fd, " ");
        s_write(fd, info.dli_fname);
        if (info.dli_sname) {
            s_write(fd, " ");
            s_write(fd, info.dli_sname);
        }
        if (info.dli_fbase) {
            offset = (unsigned long long)((char*)frame - (char*)info.dli_fbase);
        }
        if (g_cc.symbolize_stacktrace) {
            char cmd[HX_CLOG_PATH_MAX + 96];
            char line[512];
            FILE* fp;
            snprintf(cmd, sizeof(cmd), "addr2line -f -p -e \"%s\" 0x%llx",
                     info.dli_fname, offset);
            fp = popen(cmd, "r");
            if (fp) {
                if (fgets(line, sizeof(line), fp)) {
                    s_write(fd, "\n      ");
                    s_write(fd, line);
                    if (line[0] && line[strlen(line) - 1] != '\n') {
                        s_write(fd, "\n");
                    }
                }
                pclose(fp);
            }
        }
    }
    s_write(fd, "\n");
}

static void posix_handler(int sig, siginfo_t* info, void* ucontext) {
    char path[HX_CLOG_PATH_MAX];
    int fd;
    int i;
    void* frames[256];
    int nframes;
    int maxd = g_cc.stacktrace_max_depth > 0 ? g_cc.stacktrace_max_depth : 64;

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
        if (g_cc.dump_stacktrace) {
            s_write(fd, "\nstacktrace:\n");
            nframes = backtrace(frames, maxd < 256 ? maxd : 256);
            if (g_cc.symbolize_stacktrace) {
                for (i = 0; i < nframes; ++i) {
                    write_symbolized_frame_posix(fd, frames[i], i);
                }
            } else {
                /* backtrace_symbols_fd is async-signal-safe enough for our use */
                backtrace_symbols_fd(frames, nframes, fd);
            }
        }
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
    hx_mkdir_p(g_crash_dir);
    hx_ring_init();

    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sa.sa_sigaction = posix_handler;

    for (i = 0; i < K_NSIGNALS; ++i) {
        sigaction(k_signals[i], &sa, &g_prev_actions[i]);
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
    g_crash_installed = 0;
}

#endif /* platform */

#endif /* HX_CLOG_ENABLE_CRASH */
