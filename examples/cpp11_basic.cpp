/* hx_clog example: C++11 RAII wrapper. */
#include "hx_clog_cpp.hpp"

int main() {
    hx::clog::Config config;
    config.dir("./logs")
          .file("cpp_demo.log")
          .level(HX_CLOG_LEVEL_DEBUG);

    hx::clog::Logger logger(config);
    if (!logger.ok()) {
        return 1;
    }

    // The C macros still work and capture accurate file/line.
    HX_LOG_INFO("C macro still works from C++");

    // The wrapper helpers use printf-style formatting.
    logger.info("C++ wrapper message: %s", "hello");
    logger.warn("value = %d", 42);
    logger.error("something failed: %s", "reason");

    // The C++11 convenience helpers also support lightweight {} formatting.
    logger.context("example", "cpp11");
    logger.infof("brace-style message: {} {}", "hello", 2026);

    return 0; // Logger destructor calls hx_clog_shutdown()
}
