/* hx_clog test: C++11 wrapper brace formatting. */
#include "hx_clog_cpp.hpp"

#include <cstring>
#include <cstdio>

static char g_last[1024];
static int g_count = 0;

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
        std::printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

int main() {
    hx::clog::Config config;
    config.console(false)
          .fileOutput(false)
          .level(HX_CLOG_LEVEL_TRACE)
          .pattern("%v");

    hx::clog::Logger logger(config);
    CHECK(logger.ok());
    CHECK(hx_clog_add_callback_sink(capture_cb, NULL) == HX_CLOG_OK);

    logger.context("cpp", "yes");
    logger.infof("brace {} {}", "hello", 7);
    CHECK(g_count == 1);
    CHECK(std::strstr(g_last, "brace hello 7") != NULL);

    logger.setFormatMode(HX_CLOG_FORMAT_JSON);
    g_count = 0;
    logger.warnf("json {}", "mode");
    CHECK(g_count == 1);
    CHECK(std::strstr(g_last, "\"message\":\"json mode\"") != NULL);
    CHECK(std::strstr(g_last, "\"context\":\"cpp=yes\"") != NULL);

    std::printf("test_cpp11: OK\n");
    return 0;
}
