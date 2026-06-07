/* hx_clog test: formatting and level filtering, via a capture callback sink. */
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>

static char  g_last[2048];
static int   g_count;

static int capture_cb(hx_clog_level_t level, const char* data,
                      unsigned int size, void* user) {
    (void)level; (void)user;
    if (size >= sizeof(g_last)) {
        size = sizeof(g_last) - 1;
    }
    memcpy(g_last, data, size);
    g_last[size] = '\0';
    g_count++;
    return 0;
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.level = HX_CLOG_LEVEL_INFO;
    cfg.pattern = "[%l] %s:%# %!() %v";

    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    /* register capture sink after init (init builds the sink list) */
    CHECK(hx_clog_add_callback_sink(capture_cb, NULL) == HX_CLOG_OK);

    /* below threshold: must be filtered out entirely */
    g_count = 0;
    HX_LOG_DEBUG("should be filtered");
    CHECK(g_count == 0);

    /* at threshold: formatted and delivered */
    g_count = 0;
    HX_LOG_INFO("hello %d", 7);
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "[INFO ]") != NULL);
    CHECK(strstr(g_last, "hello 7") != NULL);
    CHECK(strstr(g_last, "test_format.c") != NULL);
    CHECK(strstr(g_last, "main") != NULL);

    /* warn routes through too */
    g_count = 0;
    HX_LOG_WARN("warn body");
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "[WARN ]") != NULL);

    /* dynamic level change */
    hx_clog_set_level(HX_CLOG_LEVEL_ERROR);
    g_count = 0;
    HX_LOG_WARN("now filtered");
    CHECK(g_count == 0);
    HX_LOG_ERROR("error passes");
    CHECK(g_count == 1);

    hx_clog_shutdown();
    printf("test_format: OK\n");
    return 0;
}
