/* hx_clog test: named loggers, context, JSON, formatter, sink ids, reconfigure. */
#include "hx_clog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_last[4096];
static int g_count;
static unsigned int g_allocs;
static unsigned int g_frees;

static void* test_malloc(unsigned int size, void* user) {
    (void)user;
    g_allocs++;
    return malloc(size);
}

static void test_free(void* ptr, void* user) {
    (void)user;
    g_frees++;
    free(ptr);
}

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

static unsigned int custom_formatter(const hx_clog_record_t* rec,
                                     char* out,
                                     unsigned int out_size,
                                     void* user) {
    (void)user;
    return (unsigned int)snprintf(out, out_size, "CUSTOM:%s:%.*s\n",
                                  rec->logger_name,
                                  (int)rec->message_len,
                                  rec->message);
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    hx_clog_config_t cfg2;
    hx_clog_allocator_t alloc;
    hx_clog_sink_id_t sink_id = 0;
    unsigned int sink_count = 0;
    hx_clog_logger_t* logger = NULL;

    alloc.malloc_fn = test_malloc;
    alloc.free_fn = test_free;
    alloc.user_data = NULL;
    CHECK(hx_clog_set_allocator(&alloc) == HX_CLOG_OK);

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.logger_name = "root";
    cfg.level = HX_CLOG_LEVEL_TRACE;
    cfg.pattern = "[%c] %x %v";
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_callback_sink_ex(capture_cb, NULL, &sink_id) == HX_CLOG_OK);
    CHECK(sink_id != 0);
    CHECK(hx_clog_get_sink_count(&sink_count) == HX_CLOG_OK);
    CHECK(sink_count == 1);

    CHECK(hx_clog_context_put("request", "42") == HX_CLOG_OK);
    g_count = 0;
    HX_LOG_NAMED_INFO("net", "connected");
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "[net]") != NULL);
    CHECK(strstr(g_last, "request=42") != NULL);

    CHECK(hx_clog_set_sink_level(sink_id, HX_CLOG_LEVEL_WARN) == HX_CLOG_OK);
    g_count = 0;
    HX_LOG_INFO("filtered by sink");
    CHECK(g_count == 0);
    HX_LOG_WARN("sink passes");
    CHECK(g_count == 1);

    CHECK(hx_clog_remove_sink(sink_id) == HX_CLOG_OK);
    g_count = 0;
    HX_LOG_WARN("removed");
    CHECK(g_count == 0);
    CHECK(hx_clog_add_callback_sink_ex(capture_cb, NULL, &sink_id) == HX_CLOG_OK);

    hx_clog_context_clear();
    CHECK(hx_clog_context_put("user", "alice") == HX_CLOG_OK);
    CHECK(hx_clog_set_format_mode(HX_CLOG_FORMAT_JSON) == HX_CLOG_OK);
    g_count = 0;
    HX_LOG_NAMED_INFO("jsoncat", "hello \"json\"");
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "\"logger\":\"jsoncat\"") != NULL);
    CHECK(strstr(g_last, "\"message\":\"hello \\\"json\\\"\"") != NULL);
    CHECK(strstr(g_last, "\"context\":\"user=alice\"") != NULL);

    CHECK(hx_clog_set_formatter(custom_formatter, NULL) == HX_CLOG_OK);
    g_count = 0;
    HX_LOG_NAMED_ERROR("fmtcat", "body");
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "CUSTOM:fmtcat:body") != NULL);
    CHECK(hx_clog_set_formatter(NULL, NULL) == HX_CLOG_OK);

    CHECK(hx_clog_logger_create("worker", HX_CLOG_LEVEL_ERROR, &logger) == HX_CLOG_OK);
    g_count = 0;
    HX_LOGGER_WARN(logger, "hidden");
    CHECK(g_count == 0);
    HX_LOGGER_ERROR(logger, "visible");
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "worker") != NULL || strstr(g_last, "visible") != NULL);
    hx_clog_logger_destroy(logger);

    hx_clog_config_default(&cfg2);
    cfg2.enable_console = 0;
    cfg2.enable_file = 0;
    cfg2.level = HX_CLOG_LEVEL_TRACE;
    cfg2.logger_name = "after";
    cfg2.pattern = "%c:%v";
    CHECK(hx_clog_reconfigure(&cfg2) == HX_CLOG_OK);
    g_count = 0;
    HX_LOG_INFO("reconfigured");
    CHECK(g_count == 1);
    CHECK(strstr(g_last, "after:reconfigured") != NULL);

    hx_clog_shutdown();
    CHECK(g_allocs > 0);
    CHECK(g_frees > 0);
    CHECK(hx_clog_set_allocator(NULL) == HX_CLOG_OK);

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 1;
    cfg.file_name = "bad/name.log";
    CHECK(hx_clog_init(&cfg) == HX_CLOG_ERR_INVALID_ARGUMENT);

    printf("test_features: OK\n");
    return 0;
}
