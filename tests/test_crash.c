/*
 * hx_clog test: crash handler install / uninstall + runtime options.
 *
 * By default this only verifies the install/uninstall API path (so CTest stays
 * green). Define HX_CLOG_TEST_TRIGGER_CRASH to actually trigger a fault and
 * inspect the generated crash report manually; with that build the first
 * argv selects the mode:
 *
 *   null     null-pointer dereference        (SEH: EXCEPTION_ACCESS_VIOLATION)
 *   abort    abort()                         (funnelled via SIGABRT hook)
 *   invalid  CRT invalid parameter, fclose(NULL)
 *            (retail: fail-fast, only dumpable via the funnel/LocalDumps)
 *
 * The trigger modes are NOT part of the default CTest set: they terminate the
 * process by design. Build e.g.
 *   cmake -B build-crash -DHX_CLOG_BUILD_TESTS=ON \
 *         -DHX_CLOG_TEST_TRIGGER_CRASH=ON -DCMAKE_BUILD_TYPE=Release
 * and run the binary manually, then check ./test_crash_logs for
 * crash_*_pid<N>_<seq>.log / .dmp.
 */
/* Copyright (c) 2026 HuangX - MIT license (see LICENSE). Test/example code, not part of the shipped library. */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#  include <windows.h>
#endif

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(int argc, char** argv) {
    hx_clog_config_t cfg;
    hx_clog_crash_config_t cc;
    int rc;

    (void)argc;
    (void)argv;

    hx_clog_config_default(&cfg);
    cfg.log_dir = "./test_crash_logs";
    cfg.file_name = "crash_test.log";
    cfg.enable_console = 0;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);

    HX_LOG_INFO("about to install crash handler");
    HX_LOG_INFO("recent log line 1");
    HX_LOG_INFO("recent log line 2");

    hx_clog_crash_config_default(&cc);
    cc.crash_dir = "./test_crash_logs";
    rc = hx_clog_install_crash_handler(&cc);

#if defined(HX_CLOG_ENABLE_CRASH)
    CHECK(rc == HX_CLOG_OK);

    /* runtime options: valid ids accepted, unknown ids rejected, the
     * minidump type is range-checked */
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_EXTRA_HANDLERS, 1)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_EXTRA_HANDLERS, 0)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_EXTRA_HANDLERS, 1)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_WER_PASSTHROUGH, 0)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_WER_PASSTHROUGH, 1)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE, 2)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE, 0)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE, 3)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE, 1)
          == HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE, -1)
          == HX_CLOG_ERR_INVALID_ARGUMENT);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_MINIDUMP_TYPE, 4)
          == HX_CLOG_ERR_INVALID_ARGUMENT);
    CHECK(hx_clog_crash_set_option(0, 1) == HX_CLOG_ERR_INVALID_ARGUMENT);
    CHECK(hx_clog_crash_set_option(999, 1) == HX_CLOG_ERR_INVALID_ARGUMENT);

    /* recheck: handler is ours and was just installed → 0 (no re-install
     * needed); before any install it is an error (checked after uninstall
     * below) */
    CHECK(hx_clog_crash_handler_recheck() == 0);

#if defined(_WIN32)
    /* WER LocalDumps round trip against HKCU for THIS test executable;
     * disable() removes the key again so nothing is left behind. enable()
     * must also CREATE the dump folder (WER may silently skip a missing
     * folder at crash time) and force the hive flush before returning. */
    CHECK(hx_clog_wer_local_dumps_enable("./test_crash_wer_logs", 1, 3)
          == HX_CLOG_OK);
    {
        DWORD attr = GetFileAttributesA("./test_crash_wer_logs");
        CHECK(attr != INVALID_FILE_ATTRIBUTES);
        CHECK((attr & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    CHECK(hx_clog_wer_local_dumps_disable() == HX_CLOG_OK);
    /* idempotent: removing a key that is not there is fine */
    CHECK(hx_clog_wer_local_dumps_disable() == HX_CLOG_OK);
#else
    CHECK(hx_clog_wer_local_dumps_enable(NULL, 1, 3)
          == HX_CLOG_ERR_PLATFORM);
    CHECK(hx_clog_wer_local_dumps_disable() == HX_CLOG_ERR_PLATFORM);
#endif
#else
    /* crash support compiled out: stubs return a platform error */
    CHECK(rc != HX_CLOG_OK);
    CHECK(hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_WER_PASSTHROUGH, 1)
          == HX_CLOG_ERR_PLATFORM);
    CHECK(hx_clog_wer_local_dumps_enable(NULL, 1, 3) == HX_CLOG_ERR_PLATFORM);
    CHECK(hx_clog_wer_local_dumps_disable() == HX_CLOG_ERR_PLATFORM);
#endif

#if defined(HX_CLOG_TEST_TRIGGER_CRASH)
    {
        const char* mode = (argc > 1) ? argv[1] : "null";
        printf("triggering crash mode: %s\n", mode);
        fflush(stdout);
#if defined(HX_CLOG_ENABLE_CRASH)
        /* terminate directly after writing the artifacts so the manual run is
         * deterministic (no interactive WER dialog); pass-through itself is
         * the default behaviour of normal installs */
        hx_clog_crash_set_option(HX_CLOG_CRASH_OPT_WER_PASSTHROUGH, 0);
#endif
        if (strcmp(mode, "abort") == 0) {
            abort();
        } else if (strcmp(mode, "invalid") == 0) {
            /* retail UCRT: invalid parameter -> fail-fast (only the funnel
             * hook makes this reportable); debug CRT: assertion — run this
             * mode against a Release build */
            fclose((FILE*)0);
        } else {
            volatile int* p = (volatile int*)0;
            *p = 42; /* boom: should produce a crash report in ./test_crash_logs */
        }
    }
#endif

    hx_clog_uninstall_crash_handler();
#if defined(HX_CLOG_ENABLE_CRASH)
    CHECK(hx_clog_crash_handler_recheck() == HX_CLOG_ERR_NOT_INITIALIZED);
#endif
    hx_clog_shutdown();

    printf("test_crash: OK\n");
    return 0;
}
