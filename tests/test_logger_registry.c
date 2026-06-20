/* hx_clog test: named-logger registry (get/find/count/drop_all) and the
 * dotted-name hierarchy (prefix level set + create-time inheritance). */
#include "hx_clog.h"

#include <stdio.h>

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_logger_t *net, *tcp, *http, *db, *again;

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.level = HX_CLOG_LEVEL_WARN;   /* global default level */
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);

    /* get creates + registers; inherits the global level when no ancestor */
    net = hx_clog_logger_get("network");
    CHECK(net != NULL);
    CHECK(hx_clog_logger_get_level(net) == HX_CLOG_LEVEL_WARN);
    CHECK(hx_clog_logger_count() == 1);

    /* get with the same name returns the SAME object (not a duplicate) */
    again = hx_clog_logger_get("network");
    CHECK(again == net);
    CHECK(hx_clog_logger_count() == 1);

    /* set the subtree level: the prefix logger itself updates */
    CHECK(hx_clog_set_level_for_prefix("network", HX_CLOG_LEVEL_DEBUG) == HX_CLOG_OK);
    CHECK(hx_clog_logger_get_level(net) == HX_CLOG_LEVEL_DEBUG);

    /* a descendant created AFTER the prefix set inherits from "network" */
    tcp = hx_clog_logger_get("network.tcp");
    http = hx_clog_logger_get("network.http");
    CHECK(hx_clog_logger_get_level(tcp) == HX_CLOG_LEVEL_DEBUG);
    CHECK(hx_clog_logger_get_level(http) == HX_CLOG_LEVEL_DEBUG);

    /* an unrelated logger still inherits the global level, not the subtree's */
    db = hx_clog_logger_get("database");
    CHECK(hx_clog_logger_get_level(db) == HX_CLOG_LEVEL_WARN);

    /* find: existing vs absent */
    CHECK(hx_clog_logger_find("network.tcp") == tcp);
    CHECK(hx_clog_logger_find("does.not.exist") == NULL);
    CHECK(hx_clog_logger_count() == 4); /* network, .tcp, .http, database */

    /* re-setting the prefix updates existing descendants too, leaving the
     * unrelated logger alone */
    CHECK(hx_clog_set_level_for_prefix("network", HX_CLOG_LEVEL_TRACE) == HX_CLOG_OK);
    CHECK(hx_clog_logger_get_level(tcp) == HX_CLOG_LEVEL_TRACE);
    CHECK(hx_clog_logger_get_level(http) == HX_CLOG_LEVEL_TRACE);
    CHECK(hx_clog_logger_get_level(db) == HX_CLOG_LEVEL_WARN);

    /* "network" prefix must not match "networkx" (boundary check) */
    {
        hx_clog_logger_t* nx = hx_clog_logger_get("networkx");
        CHECK(hx_clog_logger_get_level(nx) == HX_CLOG_LEVEL_WARN);
    }

    hx_clog_logger_drop_all();
    CHECK(hx_clog_logger_count() == 0);

    hx_clog_shutdown();
    printf("ALL PASS\n");
    return 0;
}
