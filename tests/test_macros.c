/* hx_clog test: compile-time level cutting (HX_CLOG_ACTIVE_LEVEL) and the
 * conditional macros HX_LOG_*_IF / HX_LOG_*_EVERY_N. */

/* Cut everything below INFO at compile time *before* including the header. */
#define HX_CLOG_ACTIVE_LEVEL HX_CLOG_LEVEL_NUM_INFO
#include "hx_clog.h"

#include <stdio.h>
#include <string.h>

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while (0)

static int g_side_effects;   /* counts argument evaluations */
static int g_delivered;      /* counts lines reaching the sink */

static int side(void) { g_side_effects++; return 42; }

static int count_cb(hx_clog_level_t level, const char* data,
                    unsigned int size, void* user) {
    (void)level; (void)data; (void)size; (void)user;
    g_delivered++;
    return 0;
}

int main(void) {
    hx_clog_config_t cfg;
    int i;

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.level = HX_CLOG_LEVEL_TRACE; /* runtime filter wide open */
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_callback_sink(count_cb, NULL) == HX_CLOG_OK);

    /* B1: DEBUG is below the compile-time floor → fully elided. Its argument
     * must NOT be evaluated, and nothing is delivered. */
    g_side_effects = 0;
    g_delivered = 0;
    HX_LOG_DEBUG("debug %d", side());
    HX_LOG_TRACE("trace %d", side());
    hx_clog_flush();
    CHECK(g_side_effects == 0);   /* args never evaluated */
    CHECK(g_delivered == 0);      /* nothing emitted */

    /* INFO is at the floor → compiled in; its argument IS evaluated. */
    g_side_effects = 0;
    g_delivered = 0;
    HX_LOG_INFO("info %d", side());
    hx_clog_flush();
    CHECK(g_side_effects == 1);
    CHECK(g_delivered == 1);

    /* B5: _IF logs only when the condition holds. When the condition is false
     * the message args are not evaluated. */
    g_side_effects = 0;
    g_delivered = 0;
    HX_LOG_INFO_IF(1, "yes %d", side());
    HX_LOG_INFO_IF(0, "no %d", side());
    hx_clog_flush();
    CHECK(g_delivered == 1);
    CHECK(g_side_effects == 1);   /* only the true branch evaluated its arg */

    /* _IF on a compiled-out level evaluates neither condition nor args. */
    g_side_effects = 0;
    HX_LOG_DEBUG_IF(side() == 42, "x %d", side());
    CHECK(g_side_effects == 0);

    /* B5: _EVERY_N logs on the 1st call and every Nth after. 10 calls, N=3 →
     * sites 0,3,6,9 → 4 deliveries. */
    g_delivered = 0;
    for (i = 0; i < 10; ++i) {
        HX_LOG_INFO_EVERY_N(3, "tick %d", i);
    }
    hx_clog_flush();
    CHECK(g_delivered == 4);

    /* M6: config readback reflects the applied config */
    {
        hx_clog_config_t got;
        memset(&got, 0, sizeof(got));
        CHECK(hx_clog_get_config(&got) == HX_CLOG_OK);
        CHECK(got.level == HX_CLOG_LEVEL_TRACE);
        CHECK(got.enable_console == 0);
        CHECK(got.enable_file == 0);
        CHECK(got.pattern != NULL && got.pattern[0] != '\0');
    }

    hx_clog_shutdown();
    printf("ALL PASS\n");
    return 0;
}
