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
 * instead. The filter is additionally hardened for the "heap already
 * corrupted" case: a re-entrancy guard stops nested faults from recursing,
 * the artifact name buffers are static (so EXCEPTION_STACK_OVERFLOW can still
 * report without ~3 KB of path buffers on the exhausted stack), dbghelp is
 * warmed up at install time instead of being initialized inside the filter,
 * timestamps are computed as UTC without localtime (whose MinGW path takes a
 * lock the crashing thread may hold), and minidump failures are recorded in
 * the report instead of vanishing silently. abort(), CRT invalid parameters
 * and C++ pure virtual calls are funnelled into the filter as custom
 * exceptions (they otherwise terminate without any SEH dispatch), and after
 * writing its artifacts the filter hands the exception to Windows Error
 * Reporting so an out-of-process dump is still possible — see
 * hx_clog_wer_local_dumps_enable for the LocalDumps registration that makes
 * fail-fast terminations (heap metadata corruption, /GS stack cookie, ...)
 * dumpable at all, since no in-process handler can ever observe those.
 *
 * Built only when HX_CLOG_ENABLE_CRASH is defined. Stacktrace capture,
 * symbolization and minidumps are additionally gated by the
 * HX_CLOG_ENABLE_STACKTRACE / HX_CLOG_ENABLE_SYMBOLIZE /
 * HX_CLOG_ENABLE_MINIDUMP build options, which also set the config defaults.
 *
 * Copyright (c) 2026 HuangX
 * SPDX-License-Identifier: MIT
 */
#include "hx_clog_internal.h"

#if defined(HX_CLOG_ENABLE_CRASH)

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
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

#if defined(HX_PLATFORM_WINDOWS)
/* UTC conversion from a unix timestamp, pure arithmetic (Howard Hinnant's
 * civil-from-days). Used by the crash path instead of hx_localtime: on MinGW
 * hx_localtime serializes on a lock the crashing thread may already hold —
 * calling it from the exception filter could self-deadlock and lose the whole
 * report. Only year/month/day/hour/min/sec are filled; that is all the report
 * needs. */
static void crash_utc(time_t t, struct tm* out) {
    long long secs = (long long)t;
    long long days = secs / 86400;
    long long rem  = secs % 86400;
    long long z, era, doe, yoe, y, doy, mp, m, d;
    if (rem < 0) {
        rem += 86400;
        --days;
    }
    out->tm_hour = (int)(rem / 3600);
    out->tm_min  = (int)((rem % 3600) / 60);
    out->tm_sec  = (int)(rem % 60);
    z   = days + 719468;
    era = (z >= 0 ? z : z - 146096) / 146097;
    doe = z - era * 146097;
    yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y   = yoe + era * 400;
    doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    mp  = (5 * doy + 2) / 153;
    d   = doy - (153 * mp + 2) / 5 + 1;
    m   = (mp < 10) ? mp + 3 : mp - 9;
    out->tm_year = (int)(y + (m <= 2)) - 1900;
    out->tm_mon  = (int)(m - 1);
    out->tm_mday = (int)d;
    out->tm_wday = (int)((days + 4) % 7); /* 1970-01-01 was a Thursday */
    if (out->tm_wday < 0) {
        out->tm_wday += 7;
    }
    out->tm_yday = 0;
    out->tm_isdst = 0;
}
#else
/* Build a crash file path with a timestamp. snprintf/localtime are not on the
 * strict async-signal-safe list but do not allocate in practice; the report
 * file name is worth the residual risk. POSIX only — the Windows filter uses
 * open_crash_file_win() with static buffers and UTC timestamps. */
static int open_crash_file(char* path_out, unsigned int cap) {
    hx_timestamp_t ts;
    struct tm tmv;
    hx_now(&ts);
    hx_localtime(ts.sec, &tmv);
    snprintf(path_out, cap, "%s/crash_%04d%02d%02d_%02d%02d%02d.log",
             g_crash_dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return open(path_out, O_CREAT | O_WRONLY | O_TRUNC, 0644);
}
#endif

static void write_common_header(int fd, const char* exc_type) {
    hx_timestamp_t ts;
    struct tm tmv;
    char tbuf[48];
    hx_now(&ts);
#if defined(HX_PLATFORM_WINDOWS)
    crash_utc(ts.sec, &tmv);
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d.%03u UTC",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.msec);
#else
    hx_localtime(ts.sec, &tmv);
    snprintf(tbuf, sizeof(tbuf), "%04d-%02d-%02d %02d:%02d:%02d.%03u",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.msec);
#endif

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

#ifndef STATUS_HEAP_CORRUPTION
#define STATUS_HEAP_CORRUPTION ((DWORD)0xC0000374L)
#endif
#ifndef STATUS_STACK_BUFFER_OVERRUN
#define STATUS_STACK_BUFFER_OVERRUN ((DWORD)0xC0000409L)
#endif

/* Termination causes funnelled into the unhandled-exception filter from the
 * CRT termination paths below (application-range exception codes). */
#define HX_CLOG_EXC_ABORT       0xE0004001L /* abort() / SIGABRT */
#define HX_CLOG_EXC_INVALID_ARG 0xE0004002L /* CRT invalid parameter */
#define HX_CLOG_EXC_PURECALL    0xE0004003L /* C++ pure virtual call */

static LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = NULL;

/* Runtime options (see hx_clog_crash_set_option). Independent statics so they
 * can be set before install; read at crash time. */
static volatile long g_opt_extra_handlers = 1;
static volatile long g_opt_wer_passthrough = 1;
static volatile long g_opt_minidump_type = 1;

/* Re-entrancy guard: if anything inside the filter itself faults (dbghelp on
 * a corrupted heap, an exhausted stack), the nested unhandled exception must
 * not re-enter the filter. Set on entry and never cleared — after the first
 * invocation the process is terminating anyway. */
static volatile LONG g_in_filter = 0;

/* Artifact name/work buffers. Static, not on the stack: with the re-entrancy
 * guard at most one filter invocation is ever in flight, and keeping ~3 KB of
 * path buffers off the stack is what lets EXCEPTION_STACK_OVERFLOW still
 * produce a report (Windows has no sigaltstack equivalent for filters). */
static char    g_log_path[HX_CLOG_PATH_MAX];
static char    g_dmp_path[HX_CLOG_PATH_MAX];
static wchar_t g_wlog_path[HX_CLOG_PATH_MAX];
static wchar_t g_wdmp_path[HX_CLOG_PATH_MAX];

/* Crash sequence number: makes two crashes within the same second distinct
 * (the previous second-resolution names silently overwrote each other). */
static volatile LONG g_crash_seq = 0;

static const char* seh_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_STACK_OVERFLOW:        return "EXCEPTION_STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "EXCEPTION_PRIV_INSTRUCTION";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:    return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "EXCEPTION_DATATYPE_MISALIGNMENT";
        case STATUS_HEAP_CORRUPTION:          return "STATUS_HEAP_CORRUPTION";
        case STATUS_STACK_BUFFER_OVERRUN:     return "STATUS_STACK_BUFFER_OVERRUN";
        case HX_CLOG_EXC_ABORT:               return "hx_clog: abort()/SIGABRT";
        case HX_CLOG_EXC_INVALID_ARG:         return "hx_clog: CRT invalid parameter";
        case HX_CLOG_EXC_PURECALL:            return "hx_clog: pure virtual call";
        default:                              return "EXCEPTION_UNKNOWN";
    }
}

/* Build both artifact names from one timestamp/pid/seq triple (so the .log and
 * the .dmp of the same crash correlate) into the static buffers, and open the
 * report file. Returns the fd or -1. */
static int open_crash_file_win(void) {
    hx_timestamp_t ts;
    struct tm tmv;
    unsigned long seq;
    int fd = -1;

    hx_now(&ts);
    crash_utc(ts.sec, &tmv);
    seq = (unsigned long)InterlockedIncrement(&g_crash_seq);
    snprintf(g_log_path, sizeof(g_log_path),
             "%s/crash_%04d%02d%02d_%02d%02d%02d_pid%lu_%lu.log",
             g_crash_dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             hx_get_pid(), seq);
    snprintf(g_dmp_path, sizeof(g_dmp_path),
             "%s/crash_%04d%02d%02d_%02d%02d%02d_pid%lu_%lu.dmp",
             g_crash_dir,
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
             tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
             hx_get_pid(), seq);
    if (hx_utf8_to_wide(g_log_path, g_wlog_path, HX_CLOG_PATH_MAX) < 0) {
        return -1;
    }
    if (hx_utf8_to_wide(g_dmp_path, g_wdmp_path, HX_CLOG_PATH_MAX) < 0) {
        return -1;
    }
    _wsopen_s(&fd, g_wlog_path, _O_CREAT | _O_WRONLY | _O_TRUNC,
              _SH_DENYNO, _S_IREAD | _S_IWRITE);
    return fd;
}

#if defined(HX_CLOG_ENABLE_MINIDUMP)
/* Map the HX_CLOG_CRASH_OPT_MINIDUMP_TYPE value to dbghelp flags. The default
 * (1) matches the historical behaviour. 2 adds enough state (data segments,
 * handles, thread info) to actually debug memory corruption; 3 is a full
 * memory dump. */
static MINIDUMP_TYPE minidump_type_from_option(long t) {
    switch (t) {
        case 0:
            return MiniDumpNormal;
        case 2:
            return (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory |
                                   MiniDumpWithDataSegs |
                                   MiniDumpWithHandleData |
                                   MiniDumpWithProcessThreadData |
                                   MiniDumpWithUnloadedModules |
                                   MiniDumpWithThreadInfo);
        case 3:
            return MiniDumpWithFullMemory;
        case 1:
        default:
            return MiniDumpWithIndirectlyReferencedMemory;
    }
}

/* Write the minidump using the name built by open_crash_file_win(). Failures
 * are recorded in the open report fd instead of vanishing: under heap
 * corruption MiniDumpWriteDump can fail (or, worst case, deadlock — the
 * re-entrancy guard then limits the damage), and knowing that it failed is
 * the difference between "no dump and no idea why" and a diagnosable state. */
static void write_minidump_win(int report_fd, EXCEPTION_POINTERS* ep) {
    HANDLE hFile;
    MINIDUMP_EXCEPTION_INFORMATION mei;
    BOOL ok;

    if (!g_cc.create_minidump) {
        return;
    }
    hFile = CreateFileW(g_wdmp_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        s_write(report_fd, "\nminidump: not created (CreateFileW error ");
        s_write_hex(report_fd, (unsigned long long)GetLastError());
        s_write(report_fd, ")\n");
        return;
    }
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                           minidump_type_from_option(g_opt_minidump_type),
                           &mei, NULL, NULL);
    CloseHandle(hFile);
    if (!ok) {
        s_write(report_fd, "\nminidump: write FAILED (error ");
        s_write_hex(report_fd, (unsigned long long)GetLastError());
        s_write(report_fd, ")\n");
    } else {
        s_write(report_fd, "\nminidump: ");
        s_write(report_fd, g_dmp_path);
        s_write(report_fd, "\n");
    }
}
#endif /* HX_CLOG_ENABLE_MINIDUMP */

#if defined(HX_CLOG_ENABLE_STACKTRACE) && defined(HX_CLOG_ENABLE_SYMBOLIZE)
/* Set at install time (see hx_clog_install_crash_handler): dbghelp's symbol
 * engine was initialized once, in a normal context, so the crash path only
 * performs lookups. */
static int g_syms_ready = 0;
#endif

#if defined(HX_CLOG_ENABLE_STACKTRACE)
static void write_stacktrace_win(int fd, CONTEXT* ctx) {
    HANDLE proc = GetCurrentProcess();
    HANDLE thr  = GetCurrentThread();
    STACKFRAME64 frame;
    DWORD machine;
    int depth = 0;
    int maxd = g_cc.stacktrace_max_depth > 0 ? g_cc.stacktrace_max_depth : 64;

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
        if (g_cc.symbolize_stacktrace && g_syms_ready) {
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
    DWORD code;
    int fd;

    /* Re-entrancy guard (see g_in_filter): a nested fault while writing the
     * report must not re-enter this filter — bail out and let the system
     * terminate the process. */
    if (InterlockedExchange(&g_in_filter, 1) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    /* NOTE: deliberately no hx_clog_flush() here — the crashing thread may
     * hold the sink lock; the ring buffer below carries the recent lines */

    code = ep->ExceptionRecord->ExceptionCode;
    fd = open_crash_file_win();
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
#if defined(HX_CLOG_ENABLE_MINIDUMP)
        /* before the footer, so a minidump failure is recorded in the report */
        write_minidump_win(fd, ep);
#endif
        write_footer_and_logs(fd);
        _close(fd);
    }

    if (g_cc.chain_previous_handler && g_prev_filter) {
        /* the previous filter decides what happens next (including WER) */
        return g_prev_filter(ep);
    }
    if (!g_opt_wer_passthrough) {
        return EXCEPTION_EXECUTE_HANDLER; /* terminate without WER */
    }
    /* Hand the exception to Windows Error Reporting. WER runs out of process,
     * so it still works when our in-process dump could not be written (e.g.
     * the heap is too corrupted for MiniDumpWriteDump), and it is the only
     * way to obtain a dump for fail-fast terminations — provided LocalDumps
     * is registered (see hx_clog_wer_local_dumps_enable). Side effect: the
     * system "program stopped working" flow may become visible. */
    return EXCEPTION_CONTINUE_SEARCH;
}

/* ---- capture termination paths that never raise an SEH exception ----
 *
 * abort(), CRT invalid parameters and C++ pure virtual calls terminate the
 * process WITHOUT any exception reaching the unhandled-exception filter, so
 * by default they produce neither a report nor a dump. These handlers funnel
 * them into the filter as custom exceptions, turning them into ordinary
 * (reportable, dumpable) crashes. Retail CRT invalid parameters would
 * otherwise fail fast — uncatchable by ANY in-process handler. */
static void __cdecl funnel_sigabrt(int sig) {
    (void)sig;
    RaiseException(HX_CLOG_EXC_ABORT, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

#if defined(_MSC_VER)
/* _set_invalid_parameter_handler / _set_purecall_handler are MSVC CRT entry
 * points; availability on MinGW depends on its CRT flavour (msvcrt vs UCRT),
 * so there only the portable signal(SIGABRT) capture above is installed. */
static void __cdecl funnel_invalid_parameter(const wchar_t* expression,
                                             const wchar_t* function,
                                             const wchar_t* file,
                                             unsigned int line,
                                             uintptr_t reserved) {
    (void)expression; (void)function; (void)file; (void)line; (void)reserved;
    RaiseException(HX_CLOG_EXC_INVALID_ARG, EXCEPTION_NONCONTINUABLE, 0, NULL);
}

static int __cdecl funnel_purecall(void) {
    RaiseException(HX_CLOG_EXC_PURECALL, EXCEPTION_NONCONTINUABLE, 0, NULL);
    return 0; /* not reached: the exception is non-continuable */
}
#endif /* _MSC_VER */

static void (__cdecl* g_prev_sigabrt)(int) = NULL;
static int g_sigabrt_installed = 0;
#if defined(_MSC_VER)
static _invalid_parameter_handler g_prev_iph = NULL;
static int g_iph_installed = 0;
static _purecall_handler g_prev_purecall = NULL;
static int g_purecall_installed = 0;
#endif

/* Install or remove the extra termination handlers. Idempotent; remembers
 * what was actually installed so uninstall restores exactly that even when
 * the option changed in between. */
static void extra_handlers_apply(int enable) {
    if (enable && !g_sigabrt_installed) {
        void (__cdecl* prev)(int) = signal(SIGABRT, funnel_sigabrt);
        if (prev != SIG_ERR) {
            g_prev_sigabrt = prev;
            g_sigabrt_installed = 1;
        }
    } else if (!enable && g_sigabrt_installed) {
        signal(SIGABRT, g_prev_sigabrt);
        g_sigabrt_installed = 0;
    }
#if defined(_MSC_VER)
    if (enable && !g_iph_installed) {
        g_prev_iph = _set_invalid_parameter_handler(funnel_invalid_parameter);
        g_iph_installed = 1;
    } else if (!enable && g_iph_installed) {
        _set_invalid_parameter_handler(g_prev_iph);
        g_iph_installed = 0;
    }
    if (enable && !g_purecall_installed) {
        g_prev_purecall = _set_purecall_handler(funnel_purecall);
        g_purecall_installed = 1;
    } else if (!enable && g_purecall_installed) {
        _set_purecall_handler(g_prev_purecall);
        g_purecall_installed = 0;
    }
#endif
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
    g_crash_dir[sizeof(g_crash_dir) - 1] = '\0';
    hx_mkdir_p(g_crash_dir);
    hx_ring_init();

#if defined(HX_CLOG_ENABLE_STACKTRACE) && defined(HX_CLOG_ENABLE_SYMBOLIZE)
    if (g_cc.symbolize_stacktrace) {
        /* Warm dbghelp up NOW, in a normal context: SymInitialize brings up
         * the symbol engine (allocations, internal locks) and the throwaway
         * stack walk below makes it exercise StackWalk64/SymFromAddr, instead
         * of all of that happening for the first time inside the exception
         * filter where a corrupted heap could turn it into a nested fault.
         * Kept initialized until uninstall (dbghelp is not thread-safe, but
         * the re-entrancy guard means at most one filter invocation runs).
         * Writes to fd -1 fail harmlessly; the walk itself is the point. */
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
        if (SymInitialize(GetCurrentProcess(), g_cc.symbol_search_path, TRUE)) {
            g_syms_ready = 1;
            {
                /* Throwaway walk into the NUL device: exercises StackWalk64 +
                 * symbol lookup now, not for the first time inside the filter. */
                int nul_fd = -1;
                CONTEXT ctx;
                _wsopen_s(&nul_fd, L"NUL", _O_WRONLY, _SH_DENYNO,
                          _S_IREAD | _S_IWRITE);
                if (nul_fd >= 0) {
                    RtlCaptureContext(&ctx);
                    write_stacktrace_win(nul_fd, &ctx);
                    _close(nul_fd);
                }
            }
        }
    }
#endif

    extra_handlers_apply(g_opt_extra_handlers ? 1 : 0);

    g_prev_filter = SetUnhandledExceptionFilter(win_exception_filter);
    g_crash_installed = 1;
    return HX_CLOG_OK;
}

void hx_clog_uninstall_crash_handler(void) {
    if (!g_crash_installed) {
        return;
    }
    SetUnhandledExceptionFilter(g_prev_filter);
    extra_handlers_apply(0);
#if defined(HX_CLOG_ENABLE_STACKTRACE) && defined(HX_CLOG_ENABLE_SYMBOLIZE)
    if (g_syms_ready) {
        SymCleanup(GetCurrentProcess());
        g_syms_ready = 0;
    }
#endif
    g_crash_installed = 0;
}

int hx_clog_crash_set_option(int option, long value) {
    switch (option) {
        case HX_CLOG_CRASH_OPT_EXTRA_HANDLERS:
            g_opt_extra_handlers = value ? 1 : 0;
            if (g_crash_installed) {
                extra_handlers_apply((int)g_opt_extra_handlers);
            }
            return HX_CLOG_OK;
        case HX_CLOG_CRASH_OPT_WER_PASSTHROUGH:
            g_opt_wer_passthrough = value ? 1 : 0;
            return HX_CLOG_OK;
        case HX_CLOG_CRASH_OPT_MINIDUMP_TYPE:
            if (value < 0 || value > 3) {
                return HX_CLOG_ERR_INVALID_ARGUMENT;
            }
            g_opt_minidump_type = value;
            return HX_CLOG_OK;
        default:
            return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
}

/* ---- WER LocalDumps registration (opt-in) ---- */

/* Build HKCU\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\
 * <exe name> for the current executable. Returns 0 on success. */
static int wer_key_path(wchar_t* key, int cap) {
    wchar_t exe[HX_CLOG_PATH_MAX];
    const wchar_t* base;
    DWORD n = GetModuleFileNameW(NULL, exe, HX_CLOG_PATH_MAX);
    if (n == 0 || n >= HX_CLOG_PATH_MAX) {
        return -1;
    }
    base = exe + n;
    while (base > exe && base[-1] != L'\\' && base[-1] != L'/') {
        --base;
    }
    if (!*base) {
        return -1;
    }
    return swprintf(key, (size_t)cap,
                    L"Software\\Microsoft\\Windows\\Windows Error Reporting\\"
                    L"LocalDumps\\%ls", base) < 0 ? -1 : 0;
}

int hx_clog_wer_local_dumps_enable(const char* folder, int dump_type,
                                   int max_count) {
    wchar_t key[HX_CLOG_PATH_MAX];
    wchar_t wfolder[HX_CLOG_PATH_MAX];
    wchar_t abs[HX_CLOG_PATH_MAX];
    char    abs_utf8[HX_CLOG_PATH_MAX];
    DWORD dw_type, dw_count;
    HKEY hkey;

    if (wer_key_path(key, HX_CLOG_PATH_MAX) != 0) {
        return HX_CLOG_ERR_PLATFORM;
    }
    if (hx_utf8_to_wide(folder ? folder
                               : (g_crash_dir[0] ? g_crash_dir : "./logs"),
                        wfolder, HX_CLOG_PATH_MAX) < 0) {
        return HX_CLOG_ERR_PLATFORM;
    }
    if (!_wfullpath(abs, wfolder, HX_CLOG_PATH_MAX)) {
        return HX_CLOG_ERR_PLATFORM;
    }
    /* Create the target directory NOW. WER does not reliably create a
     * missing DumpFolder at crash time — a missing folder silently means
     * "no out-of-process dump", which is exactly the failure this safety
     * net exists to prevent. */
    if (hx_wide_to_utf8(abs, abs_utf8, HX_CLOG_PATH_MAX) > 0) {
        hx_mkdir_p(abs_utf8);
    }
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL,
                        REG_OPTION_NON_VOLATILE,
                        KEY_SET_VALUE | KEY_QUERY_VALUE, NULL,
                        &hkey, NULL) != ERROR_SUCCESS) {
        hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                             "crash handler: WER LocalDumps registration "
                             "failed (registry create error)");
        return HX_CLOG_ERR_PLATFORM;
    }
    dw_type = (dump_type >= 2) ? 2 : 1;
    dw_count = (max_count > 0) ? (DWORD)max_count : 10;
    /* Values are written most-important-first: if the process dies between
     * two writes the registration is only partial, and WER substitutes its
     * documented defaults for MISSING values (DumpType -> minidump,
     * DumpCount -> 10), so the degraded registration still produces dumps —
     * with DumpFolder already in place they land in the configured folder. */
    if (RegSetValueExW(hkey, L"DumpFolder", 0, REG_SZ, (const BYTE*)abs,
                       (DWORD)((wcslen(abs) + 1) * sizeof(wchar_t)))
            != ERROR_SUCCESS ||
        RegSetValueExW(hkey, L"DumpType", 0, REG_DWORD,
                       (const BYTE*)&dw_type, sizeof(dw_type))
            != ERROR_SUCCESS ||
        RegSetValueExW(hkey, L"DumpCount", 0, REG_DWORD,
                       (const BYTE*)&dw_count, sizeof(dw_count))
            != ERROR_SUCCESS) {
        RegCloseKey(hkey);
        hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                             "crash handler: WER LocalDumps registration "
                             "failed (registry write error)");
        return HX_CLOG_ERR_PLATFORM;
    }
    /* Force the configuration manager to write the dirty hive pages to disk
     * now. RegCloseKey alone does NOT guarantee persistence — hives are
     * flushed lazily — so without this a power loss shortly after enable()
     * could silently lose the registration. The write itself is
     * kernel-mediated (in-process heap corruption cannot corrupt the hive);
     * this flush closes the power-loss window. */
    if (RegFlushKey(hkey) != ERROR_SUCCESS) {
        RegCloseKey(hkey);
        hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                             "crash handler: WER LocalDumps registration "
                             "written but not flushed to disk (power-loss "
                             "durability not guaranteed)");
        return HX_CLOG_ERR_PLATFORM;
    }
    RegCloseKey(hkey);
    return HX_CLOG_OK;
}

int hx_clog_wer_local_dumps_disable(void) {
    wchar_t key[HX_CLOG_PATH_MAX];
    LONG rc;
    if (wer_key_path(key, HX_CLOG_PATH_MAX) != 0) {
        return HX_CLOG_ERR_PLATFORM;
    }
    /* The registered key holds only values (no subkeys), so RegDeleteKeyW is
     * enough and avoids the Vista-only RegDeleteTreeW. */
    rc = RegDeleteKeyW(HKEY_CURRENT_USER, key);
    if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
        hx_core_report_error(HX_CLOG_ERR_PLATFORM,
                             "crash handler: WER LocalDumps removal failed "
                             "(registry delete error)");
        return HX_CLOG_ERR_PLATFORM;
    }
    return HX_CLOG_OK;
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

int hx_clog_crash_set_option(int option, long value) {
    /* The options currently describe Windows-only behaviour: POSIX already
     * catches SIGABRT, restores the previous handlers and re-raises (the
     * equivalent of WER passthrough). Accepted and ignored so cross-platform
     * code can set them unconditionally. */
    if (option != HX_CLOG_CRASH_OPT_EXTRA_HANDLERS &&
        option != HX_CLOG_CRASH_OPT_WER_PASSTHROUGH &&
        option != HX_CLOG_CRASH_OPT_MINIDUMP_TYPE) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    (void)value;
    return HX_CLOG_OK;
}

int hx_clog_wer_local_dumps_enable(const char* folder, int dump_type,
                                   int max_count) {
    (void)folder; (void)dump_type; (void)max_count;
    return HX_CLOG_ERR_PLATFORM; /* Windows-only */
}

int hx_clog_wer_local_dumps_disable(void) {
    return HX_CLOG_ERR_PLATFORM; /* Windows-only */
}

#endif /* platform */

#endif /* HX_CLOG_ENABLE_CRASH */
