/*
 * hx_clog test: crash handler install / uninstall.
 *
 * By default this only verifies the install/uninstall API path (so CTest stays
 * green). Define HX_CLOG_TEST_TRIGGER_CRASH to actually trigger a fault and
 * inspect the generated crash report manually.
 */
#include "hx_clog.h"

#include <stdio.h>

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_crash_config_t cc;
    int rc;

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
#else
    /* crash support compiled out: stub returns a platform error */
    CHECK(rc != HX_CLOG_OK);
#endif

#if defined(HX_CLOG_TEST_TRIGGER_CRASH)
    {
        volatile int* p = (volatile int*)0;
        *p = 42; /* boom: should produce a crash report in ./test_crash_logs */
    }
#endif

    hx_clog_uninstall_crash_handler();
    hx_clog_shutdown();

    printf("test_crash: OK\n");
    return 0;
}
