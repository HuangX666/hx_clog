/*
 * hx_clog - optional C++11 RAII wrapper.
 *
 * A thin convenience layer over the C API. It does not reimplement any core
 * logic and exports no C++ ABI from the library. Include it only in C++
 * translation units that want the sugar; the C macros (HX_LOG_INFO, ...) keep
 * working unchanged and remain the recommended way to log (accurate file/line).
 */
#ifndef HX_CLOG_CPP_HPP
#define HX_CLOG_CPP_HPP

#include "hx_clog.h"

#include <string>
#include <cstdarg>

namespace hx {
namespace clog {

/*
 * Config owns copies of the string fields so the pointers handed to the C API
 * remain valid for the lifetime of the object. Use the chainable setters, then
 * pass it to a Logger.
 */
class Config {
public:
    Config() {
        hx_clog_config_default(&cfg_);
        // capture default string fields into our owned storage
        log_dir_     = cfg_.log_dir     ? cfg_.log_dir     : "";
        file_name_   = cfg_.file_name   ? cfg_.file_name   : "";
        logger_name_ = cfg_.logger_name ? cfg_.logger_name : "";
        pattern_     = cfg_.pattern     ? cfg_.pattern     : "";
    }

    Config& dir(const std::string& v)         { log_dir_ = v; return *this; }
    Config& file(const std::string& v)        { file_name_ = v; return *this; }
    Config& name(const std::string& v)        { logger_name_ = v; return *this; }
    Config& pattern(const std::string& v)     { pattern_ = v; return *this; }
    Config& level(hx_clog_level_t v)          { cfg_.level = v; return *this; }
    Config& mode(hx_clog_mode_t v)            { cfg_.mode = v; return *this; }
    Config& console(bool v)                   { cfg_.enable_console = v ? 1 : 0; return *this; }
    Config& fileOutput(bool v)                { cfg_.enable_file = v ? 1 : 0; return *this; }
    Config& color(bool v)                     { cfg_.enable_color = v ? 1 : 0; return *this; }
    Config& crashHandler(bool v)              { cfg_.enable_crash_handler = v ? 1 : 0; return *this; }
    Config& rotatePolicy(hx_clog_rotate_policy_t v) { cfg_.rotate_policy = v; return *this; }
    Config& maxFileSize(unsigned long long v) { cfg_.max_file_size = v; return *this; }
    Config& maxBackupFiles(int v)             { cfg_.max_backup_files = v; return *this; }
    Config& rotateDaily(bool v)               { cfg_.rotate_daily = v ? 1 : 0; return *this; }
    Config& asyncQueueSize(unsigned int v)    { cfg_.async_queue_size = v; return *this; }
    Config& asyncBatchSize(unsigned int v)    { cfg_.async_batch_size = v; return *this; }
    Config& flushIntervalMs(unsigned int v)   { cfg_.flush_interval_ms = v; return *this; }

    /* Build the C config struct, repointing string fields at our storage.
     * The returned struct is valid only while this Config is alive. */
    const hx_clog_config_t& build() {
        cfg_.log_dir     = log_dir_.c_str();
        cfg_.file_name   = file_name_.c_str();
        cfg_.logger_name = logger_name_.c_str();
        cfg_.pattern     = pattern_.c_str();
        return cfg_;
    }

    hx_clog_config_t& raw() { return cfg_; }

private:
    hx_clog_config_t cfg_;
    std::string log_dir_;
    std::string file_name_;
    std::string logger_name_;
    std::string pattern_;
};

/*
 * RAII logger: initializes hx_clog on construction, shuts it down on
 * destruction. The C core is a process-wide singleton, so only one Logger
 * should be alive at a time.
 */
class Logger {
public:
    explicit Logger(Config& config) {
        owns_ = (hx_clog_init(&config.build()) == HX_CLOG_OK);
    }
    explicit Logger(const hx_clog_config_t& cfg) {
        owns_ = (hx_clog_init(&cfg) == HX_CLOG_OK);
    }
    Logger() {
        hx_clog_config_t cfg;
        hx_clog_config_default(&cfg);
        owns_ = (hx_clog_init(&cfg) == HX_CLOG_OK);
    }
    ~Logger() { if (owns_) hx_clog_shutdown(); }

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    bool ok() const { return owns_; }

    void flush()                     { hx_clog_flush(); }
    void setLevel(hx_clog_level_t l) { hx_clog_set_level(l); }
    hx_clog_level_t level() const    { return hx_clog_get_level(); }

    /*
     * printf-style helpers. file/line refer to this header, so for precise
     * source location prefer the HX_LOG_* macros from hx_clog.h.
     */
    void trace(const char* fmt, ...) { va_list ap; va_start(ap, fmt); hx_clog_writev(HX_CLOG_LEVEL_TRACE, "cpp", 0, "trace", fmt, ap); va_end(ap); }
    void debug(const char* fmt, ...) { va_list ap; va_start(ap, fmt); hx_clog_writev(HX_CLOG_LEVEL_DEBUG, "cpp", 0, "debug", fmt, ap); va_end(ap); }
    void info(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); hx_clog_writev(HX_CLOG_LEVEL_INFO,  "cpp", 0, "info",  fmt, ap); va_end(ap); }
    void warn(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); hx_clog_writev(HX_CLOG_LEVEL_WARN,  "cpp", 0, "warn",  fmt, ap); va_end(ap); }
    void error(const char* fmt, ...) { va_list ap; va_start(ap, fmt); hx_clog_writev(HX_CLOG_LEVEL_ERROR, "cpp", 0, "error", fmt, ap); va_end(ap); }
    void fatal(const char* fmt, ...) { va_list ap; va_start(ap, fmt); hx_clog_writev(HX_CLOG_LEVEL_FATAL, "cpp", 0, "fatal", fmt, ap); va_end(ap); }

private:
    bool owns_ = false;
};

} // namespace clog
} // namespace hx

#endif /* HX_CLOG_CPP_HPP */
