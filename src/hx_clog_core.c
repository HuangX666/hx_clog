/*
 * hx_clog - core.
 *
 * Owns global logger state, level filtering, the synchronous write path,
 * sink management, statistics, the crash ring buffer, and all platform /
 * threading / allocator helpers shared across the library.
 */
#include "hx_clog_internal.h"

#include <stdarg.h>

#if defined(HX_PLATFORM_WINDOWS)
#  include <sys/stat.h>
#endif

/* =========================================================================
 * Allocator
 * ========================================================================= */
static hx_clog_allocator_t g_alloc = {0};

void hx_clog__set_allocator(const hx_clog_allocator_t* a) {
    if (a && a->malloc_fn && a->free_fn) {
        g_alloc = *a;
    } else {
        memset(&g_alloc, 0, sizeof(g_alloc));
    }
}

void* hx_clog__malloc(size_t size) {
    if (g_alloc.malloc_fn) {
        return g_alloc.malloc_fn((unsigned int)size, g_alloc.user_data);
    }
    return malloc(size);
}

void hx_clog__free(void* ptr) {
    if (!ptr) {
        return;
    }
    if (g_alloc.free_fn) {
        g_alloc.free_fn(ptr, g_alloc.user_data);
    } else {
        free(ptr);
    }
}

void* hx_clog__realloc(void* ptr, size_t old_size, size_t new_size) {
    if (!g_alloc.malloc_fn) {
        return realloc(ptr, new_size);
    }
    {
        void* np = g_alloc.malloc_fn((unsigned int)new_size, g_alloc.user_data);
        if (np && ptr) {
            memcpy(np, ptr, old_size < new_size ? old_size : new_size);
        }
        if (ptr) {
            g_alloc.free_fn(ptr, g_alloc.user_data);
        }
        return np;
    }
}

/* =========================================================================
 * Threading primitives
 * ========================================================================= */
#if defined(HX_PLATFORM_WINDOWS)

void hx_mutex_init(hx_mutex_t* m)    { InitializeCriticalSection(m); }
void hx_mutex_destroy(hx_mutex_t* m) { DeleteCriticalSection(m); }
void hx_mutex_lock(hx_mutex_t* m)    { EnterCriticalSection(m); }
void hx_mutex_unlock(hx_mutex_t* m)  { LeaveCriticalSection(m); }

void hx_cond_init(hx_cond_t* c)      { InitializeConditionVariable(c); }
void hx_cond_destroy(hx_cond_t* c)   { (void)c; }
void hx_cond_signal(hx_cond_t* c)    { WakeConditionVariable(c); }
void hx_cond_broadcast(hx_cond_t* c) { WakeAllConditionVariable(c); }

int hx_cond_wait_ms(hx_cond_t* c, hx_mutex_t* m, unsigned int timeout_ms) {
    BOOL ok = SleepConditionVariableCS(c, m, timeout_ms);
    return ok ? 1 : 0;
}
void hx_cond_wait(hx_cond_t* c, hx_mutex_t* m) {
    SleepConditionVariableCS(c, m, INFINITE);
}

typedef struct { hx_thread_fn fn; void* arg; } win_thread_ctx;
static unsigned __stdcall win_thread_trampoline(void* p) {
    win_thread_ctx ctx = *(win_thread_ctx*)p;
    hx_clog__free(p);
    ctx.fn(ctx.arg);
    return 0;
}
int hx_thread_create(hx_thread_t* t, hx_thread_fn fn, void* arg) {
    win_thread_ctx* ctx = (win_thread_ctx*)hx_clog__malloc(sizeof(*ctx));
    if (!ctx) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    *t = (HANDLE)_beginthreadex(NULL, 0, win_thread_trampoline, ctx, 0, NULL);
    if (*t == 0) {
        hx_clog__free(ctx);
        return HX_CLOG_ERR_THREAD_FAILED;
    }
    return HX_CLOG_OK;
}
void hx_thread_join(hx_thread_t t) {
    if (t) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
    }
}

#else /* POSIX */

void hx_mutex_init(hx_mutex_t* m)    { pthread_mutex_init(m, NULL); }
void hx_mutex_destroy(hx_mutex_t* m) { pthread_mutex_destroy(m); }
void hx_mutex_lock(hx_mutex_t* m)    { pthread_mutex_lock(m); }
void hx_mutex_unlock(hx_mutex_t* m)  { pthread_mutex_unlock(m); }

void hx_cond_init(hx_cond_t* c)      { pthread_cond_init(c, NULL); }
void hx_cond_destroy(hx_cond_t* c)   { pthread_cond_destroy(c); }
void hx_cond_signal(hx_cond_t* c)    { pthread_cond_signal(c); }
void hx_cond_broadcast(hx_cond_t* c) { pthread_cond_broadcast(c); }

int hx_cond_wait_ms(hx_cond_t* c, hx_mutex_t* m, unsigned int timeout_ms) {
    struct timespec ts;
    struct timeval tv;
    int r;
    gettimeofday(&tv, NULL);
    ts.tv_sec = tv.tv_sec + timeout_ms / 1000;
    ts.tv_nsec = (tv.tv_usec + (timeout_ms % 1000) * 1000) * 1000;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    r = pthread_cond_timedwait(c, m, &ts);
    return r == 0 ? 1 : 0;
}
void hx_cond_wait(hx_cond_t* c, hx_mutex_t* m) {
    pthread_cond_wait(c, m);
}

typedef struct { hx_thread_fn fn; void* arg; } pthr_ctx;
static void* pthr_trampoline(void* p) {
    pthr_ctx ctx = *(pthr_ctx*)p;
    hx_clog__free(p);
    ctx.fn(ctx.arg);
    return NULL;
}
int hx_thread_create(hx_thread_t* t, hx_thread_fn fn, void* arg) {
    pthr_ctx* ctx = (pthr_ctx*)hx_clog__malloc(sizeof(*ctx));
    if (!ctx) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    if (pthread_create(t, NULL, pthr_trampoline, ctx) != 0) {
        hx_clog__free(ctx);
        return HX_CLOG_ERR_THREAD_FAILED;
    }
    return HX_CLOG_OK;
}
void hx_thread_join(hx_thread_t t) {
    pthread_join(t, NULL);
}

#endif

/* =========================================================================
 * Atomic level
 * ========================================================================= */
void hx_atomic_store_level(volatile int* p, int v) {
#if defined(HX_PLATFORM_WINDOWS)
    InterlockedExchange((volatile LONG*)p, v);
#elif defined(__GNUC__)
    __atomic_store_n(p, v, __ATOMIC_RELAXED);
#else
    *p = v;
#endif
}
int hx_atomic_load_level(volatile int* p) {
#if defined(HX_PLATFORM_WINDOWS)
    return (int)InterlockedCompareExchange((volatile LONG*)p, 0, 0);
#elif defined(__GNUC__)
    return __atomic_load_n(p, __ATOMIC_RELAXED);
#else
    return *p;
#endif
}

/* =========================================================================
 * Platform helpers
 * ========================================================================= */
unsigned long hx_get_pid(void) {
#if defined(HX_PLATFORM_WINDOWS)
    return (unsigned long)GetCurrentProcessId();
#else
    return (unsigned long)getpid();
#endif
}

unsigned long hx_get_tid(void) {
#if defined(HX_PLATFORM_WINDOWS)
    return (unsigned long)GetCurrentThreadId();
#elif defined(HX_PLATFORM_APPLE)
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (unsigned long)tid;
#elif defined(__linux__)
    return (unsigned long)syscall(186 /* SYS_gettid on x86-64 */);
#else
    return (unsigned long)(size_t)pthread_self();
#endif
}

void hx_sleep_ms(unsigned int ms) {
#if defined(HX_PLATFORM_WINDOWS)
    Sleep(ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

void hx_now(hx_timestamp_t* ts) {
#if defined(HX_PLATFORM_WINDOWS)
    FILETIME ft;
    ULARGE_INTEGER u;
    unsigned long long t100;
    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    /* 100ns intervals since 1601-01-01; convert to unix epoch */
    t100 = u.QuadPart - 116444736000000000ULL;
    ts->sec = (time_t)(t100 / 10000000ULL);
    ts->msec = (unsigned int)((t100 / 10000ULL) % 1000ULL);
#else
    struct timeval tv;
    gettimeofday(&tv, NULL);
    ts->sec = tv.tv_sec;
    ts->msec = (unsigned int)(tv.tv_usec / 1000);
#endif
}

void hx_localtime(time_t t, struct tm* out) {
#if defined(_MSC_VER)
    localtime_s(out, &t);
#elif defined(HX_PLATFORM_WINDOWS)
    {
        struct tm* tmp = localtime(&t); /* MinGW: no localtime_r guarantee */
        if (tmp) {
            *out = *tmp;
        } else {
            memset(out, 0, sizeof(*out));
        }
    }
#else
    localtime_r(&t, out);
#endif
}

int hx_mkdir_p(const char* path) {
    char tmp[HX_CLOG_PATH_MAX];
    size_t len, i;
    if (!path || !path[0]) {
        return -1;
    }
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    /* strip trailing slash */
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\')) {
        tmp[len - 1] = '\0';
        len--;
    }
    for (i = 1; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
#if defined(HX_PLATFORM_WINDOWS)
            _mkdir(tmp);
#else
            mkdir(tmp, 0755);
#endif
            tmp[i] = saved;
        }
    }
#if defined(HX_PLATFORM_WINDOWS)
    if (_mkdir(tmp) != 0 && !hx_file_exists(tmp)) {
        return -1;
    }
#else
    if (mkdir(tmp, 0755) != 0 && !hx_file_exists(tmp)) {
        return -1;
    }
#endif
    return 0;
}

int hx_file_exists(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

long long hx_file_size(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (GetFileAttributesExA(path, GetFileExInfoStandard, &ad)) {
        ULARGE_INTEGER u;
        u.LowPart = ad.nFileSizeLow;
        u.HighPart = ad.nFileSizeHigh;
        return (long long)u.QuadPart;
    }
    return -1;
#else
    struct stat st;
    if (stat(path, &st) == 0) {
        return (long long)st.st_size;
    }
    return -1;
#endif
}

int hx_rename(const char* from, const char* to) {
#if defined(HX_PLATFORM_WINDOWS)
    /* rename() fails if target exists on Windows; remove first. */
    if (hx_file_exists(to)) {
        DeleteFileA(to);
    }
    return MoveFileA(from, to) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

int hx_remove(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    return DeleteFileA(path) ? 0 : -1;
#else
    return remove(path);
#endif
}

/* =========================================================================
 * Crash ring buffer ("last N logs"), pre-allocated.
 * ========================================================================= */
typedef struct {
    char  data[HX_CLOG_RING_ENTRY_SIZE];
    unsigned int len;
} ring_entry_t;

static ring_entry_t* g_ring = NULL;
static unsigned int  g_ring_head = 0;   /* next slot to write */
static unsigned int  g_ring_count = 0;
static hx_mutex_t    g_ring_lock;
static int           g_ring_inited = 0;

void hx_ring_init(void) {
    if (g_ring_inited) {
        return;
    }
    g_ring = (ring_entry_t*)hx_clog__malloc(sizeof(ring_entry_t) * HX_CLOG_RING_CAPACITY);
    if (!g_ring) {
        return;
    }
    memset(g_ring, 0, sizeof(ring_entry_t) * HX_CLOG_RING_CAPACITY);
    g_ring_head = 0;
    g_ring_count = 0;
    hx_mutex_init(&g_ring_lock);
    g_ring_inited = 1;
}

void hx_ring_push(const char* line, unsigned int len) {
    ring_entry_t* e;
    if (!g_ring_inited || !g_ring) {
        return;
    }
    if (len >= HX_CLOG_RING_ENTRY_SIZE) {
        len = HX_CLOG_RING_ENTRY_SIZE - 1;
    }
    hx_mutex_lock(&g_ring_lock);
    e = &g_ring[g_ring_head];
    memcpy(e->data, line, len);
    e->data[len] = '\0';
    e->len = len;
    g_ring_head = (g_ring_head + 1) % HX_CLOG_RING_CAPACITY;
    if (g_ring_count < HX_CLOG_RING_CAPACITY) {
        g_ring_count++;
    }
    hx_mutex_unlock(&g_ring_lock);
}

/* Dump in chronological order using only low-level writes (signal-safer). */
void hx_ring_dump_fd(int fd) {
    unsigned int i, idx, start;
    if (!g_ring_inited || !g_ring) {
        return;
    }
    start = (g_ring_head + HX_CLOG_RING_CAPACITY - g_ring_count) % HX_CLOG_RING_CAPACITY;
    for (i = 0; i < g_ring_count; ++i) {
        idx = (start + i) % HX_CLOG_RING_CAPACITY;
#if defined(HX_PLATFORM_WINDOWS)
        _write(fd, g_ring[idx].data, g_ring[idx].len);
#else
        {
            ssize_t w = write(fd, g_ring[idx].data, g_ring[idx].len);
            (void)w;
        }
#endif
    }
}

void hx_ring_destroy(void) {
    if (!g_ring_inited) {
        return;
    }
    hx_mutex_destroy(&g_ring_lock);
    hx_clog__free(g_ring);
    g_ring = NULL;
    g_ring_count = 0;
    g_ring_head = 0;
    g_ring_inited = 0;
}

/* =========================================================================
 * Global logger state
 * ========================================================================= */
typedef struct {
    int initialized;
    volatile int level;          /* atomic */
    hx_clog_mode_t mode;

    hx_clog_sink_t* sinks[HX_CLOG_MAX_SINKS];
    int sink_count;

    char pattern[512];

    hx_mutex_t sink_lock;        /* guards sink writes in sync mode + sink list */

    /* stats */
    hx_mutex_t stats_lock;
    unsigned long long written_lines;
    unsigned long long rotated_files;
    unsigned long long dropped_lines;
} hx_core_state_t;

static hx_core_state_t g_core;

/* Once-init guard for the whole module. */
#if defined(HX_PLATFORM_WINDOWS)
static INIT_ONCE g_init_once = INIT_ONCE_STATIC_INIT;
#else
static pthread_once_t g_init_once = PTHREAD_ONCE_INIT;
#endif

/* =========================================================================
 * Config defaults
 * ========================================================================= */
void hx_clog_config_default(hx_clog_config_t* config) {
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->logger_name = "hx_clog";
    config->log_dir = "./logs";
    config->file_name = "app.log";
    config->level = HX_CLOG_LEVEL_INFO;
    config->mode = HX_CLOG_MODE_SYNC;
    config->enable_console = 1;
    config->enable_file = 1;
    config->enable_color = 1;
    config->enable_crash_handler = 0;
    config->rotate_policy = HX_CLOG_ROTATE_BY_SIZE_AND_TIME;
    config->max_file_size = 10ULL * 1024ULL * 1024ULL;
    config->max_backup_files = 10;
    config->max_backup_days = 0;
    config->rotate_daily = 1;
    config->async_queue_size = 8192;
    config->async_batch_size = 64;
    config->flush_interval_ms = 1000;
    config->overflow_policy = HX_CLOG_OVERFLOW_BLOCK;
    config->pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [tid:%t] %s:%# %!() - %v%n";
}

/* =========================================================================
 * Level name helpers
 * ========================================================================= */
const char* hx_clog_level_name(hx_clog_level_t level) {
    switch (level) {
        case HX_CLOG_LEVEL_TRACE: return "TRACE";
        case HX_CLOG_LEVEL_DEBUG: return "DEBUG";
        case HX_CLOG_LEVEL_INFO:  return "INFO";
        case HX_CLOG_LEVEL_WARN:  return "WARN";
        case HX_CLOG_LEVEL_ERROR: return "ERROR";
        case HX_CLOG_LEVEL_FATAL: return "FATAL";
        case HX_CLOG_LEVEL_OFF:   return "OFF";
        default:                  return "UNKNOWN";
    }
}

static int ieq(const char* a, const char* b) {
    while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
        if (cb >= 'A' && cb <= 'Z') cb = (char)(cb - 'A' + 'a');
        if (ca != cb) return 0;
        ++a; ++b;
    }
    return *a == *b;
}

hx_clog_level_t hx_clog_level_from_name(const char* name) {
    if (!name) return HX_CLOG_LEVEL_INFO;
    if (ieq(name, "trace")) return HX_CLOG_LEVEL_TRACE;
    if (ieq(name, "debug")) return HX_CLOG_LEVEL_DEBUG;
    if (ieq(name, "info"))  return HX_CLOG_LEVEL_INFO;
    if (ieq(name, "warn") || ieq(name, "warning")) return HX_CLOG_LEVEL_WARN;
    if (ieq(name, "error")) return HX_CLOG_LEVEL_ERROR;
    if (ieq(name, "fatal")) return HX_CLOG_LEVEL_FATAL;
    if (ieq(name, "off"))   return HX_CLOG_LEVEL_OFF;
    return HX_CLOG_LEVEL_INFO;
}

const char* hx_clog_strerror(int err) {
    switch (err) {
        case HX_CLOG_OK:                       return "ok";
        case HX_CLOG_ERR_INVALID_ARGUMENT:     return "invalid argument";
        case HX_CLOG_ERR_NOT_INITIALIZED:      return "not initialized";
        case HX_CLOG_ERR_ALREADY_INITIALIZED:  return "already initialized";
        case HX_CLOG_ERR_OPEN_FILE_FAILED:     return "open file failed";
        case HX_CLOG_ERR_OUT_OF_MEMORY:        return "out of memory";
        case HX_CLOG_ERR_THREAD_FAILED:        return "thread failed";
        case HX_CLOG_ERR_QUEUE_FULL:           return "queue full";
        case HX_CLOG_ERR_PLATFORM:             return "platform error";
        default:                               return "unknown error";
    }
}

/* =========================================================================
 * Env var overrides
 * ========================================================================= */
static void apply_env_overrides(hx_clog_config_t* cfg) {
    const char* v;
    v = getenv("HX_CLOG_LEVEL");
    if (v && v[0]) {
        cfg->level = hx_clog_level_from_name(v);
    }
    v = getenv("HX_CLOG_DIR");
    if (v && v[0]) {
        cfg->log_dir = v;
    }
    v = getenv("HX_CLOG_MODE");
    if (v && v[0]) {
        cfg->mode = ieq(v, "async") ? HX_CLOG_MODE_ASYNC : HX_CLOG_MODE_SYNC;
    }
    v = getenv("HX_CLOG_CONSOLE");
    if (v && v[0]) {
        cfg->enable_console = (ieq(v, "1") || ieq(v, "true") || ieq(v, "yes")) ? 1 : 0;
    }
}

/* =========================================================================
 * Emit path
 * ========================================================================= */
void hx_core_add_written(unsigned long long n) {
    hx_mutex_lock(&g_core.stats_lock);
    g_core.written_lines += n;
    hx_mutex_unlock(&g_core.stats_lock);
}
void hx_core_add_rotated(unsigned long long n) {
    hx_mutex_lock(&g_core.stats_lock);
    g_core.rotated_files += n;
    hx_mutex_unlock(&g_core.stats_lock);
}

/* Write one formatted line to every sink. Used by sync path and async worker.
 * Level is recovered from the leading bytes? No — we pass it explicitly. */
void hx_core_emit_to_sinks(hx_clog_level_t level, const char* line, unsigned int len) {
    int i;
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_sink_write(g_core.sinks[i], level, line, len);
    }
    hx_mutex_unlock(&g_core.sink_lock);
    hx_core_add_written(1);
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */
static void core_once_init(void) {
    memset(&g_core, 0, sizeof(g_core));
    hx_mutex_init(&g_core.sink_lock);
    hx_mutex_init(&g_core.stats_lock);
    g_core.level = HX_CLOG_LEVEL_INFO;
}

#if defined(HX_PLATFORM_WINDOWS)
static BOOL CALLBACK once_cb(PINIT_ONCE o, PVOID p, PVOID* c) {
    (void)o; (void)p; (void)c;
    core_once_init();
    return TRUE;
}
static void ensure_once(void) {
    InitOnceExecuteOnce(&g_init_once, once_cb, NULL, NULL);
}
#else
static void ensure_once(void) {
    pthread_once(&g_init_once, core_once_init);
}
#endif

static void add_sink(hx_clog_sink_t* s) {
    if (s && g_core.sink_count < HX_CLOG_MAX_SINKS) {
        g_core.sinks[g_core.sink_count++] = s;
    } else if (s) {
        hx_sink_close(s);
    }
}

int hx_clog_init(const hx_clog_config_t* in_config) {
    hx_clog_config_t cfg;

    ensure_once();

    if (g_core.initialized) {
        return HX_CLOG_ERR_ALREADY_INITIALIZED;
    }

    if (in_config) {
        cfg = *in_config;
    } else {
        hx_clog_config_default(&cfg);
    }
    /* fill any zeroed required fields with sane defaults */
    if (!cfg.file_name) cfg.file_name = "app.log";
    if (!cfg.log_dir)   cfg.log_dir = "./logs";
    if (!cfg.pattern)   cfg.pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [tid:%t] %s:%# %!() - %v%n";

    apply_env_overrides(&cfg);

    strncpy(g_core.pattern, cfg.pattern, sizeof(g_core.pattern) - 1);
    g_core.pattern[sizeof(g_core.pattern) - 1] = '\0';
    g_core.mode = cfg.mode;
    g_core.sink_count = 0;
    hx_atomic_store_level(&g_core.level, (int)cfg.level);

    hx_ring_init();

    /* build sinks */
    if (cfg.enable_console) {
        add_sink(hx_sink_console_create(1, cfg.enable_color));
    }
    if (cfg.enable_file) {
        hx_clog_sink_t* fs = hx_sink_file_create(cfg.log_dir, cfg.file_name, &cfg);
        if (!fs) {
            /* file is important; report failure but keep console working */
            if (g_core.sink_count == 0) {
                return HX_CLOG_ERR_OPEN_FILE_FAILED;
            }
        } else {
            add_sink(fs);
        }
    }

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (cfg.mode == HX_CLOG_MODE_ASYNC) {
        if (hx_async_start(&cfg) != HX_CLOG_OK) {
            g_core.mode = HX_CLOG_MODE_SYNC; /* fall back */
        }
    }
#else
    if (cfg.mode == HX_CLOG_MODE_ASYNC) {
        g_core.mode = HX_CLOG_MODE_SYNC;
    }
#endif

    g_core.initialized = 1;

#if defined(HX_CLOG_ENABLE_CRASH)
    if (cfg.enable_crash_handler) {
        hx_clog_crash_config_t cc;
        hx_clog_crash_config_default(&cc);
        cc.crash_dir = cfg.log_dir;
        hx_clog_install_crash_handler(&cc);
    }
#endif

    return HX_CLOG_OK;
}

int hx_clog_is_initialized(void) {
    return g_core.initialized;
}

void hx_clog_flush(void) {
    int i;
    if (!g_core.initialized) {
        return;
    }
#if defined(HX_CLOG_ENABLE_ASYNC)
    if (g_core.mode == HX_CLOG_MODE_ASYNC) {
        hx_async_flush();
    }
#endif
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_sink_flush(g_core.sinks[i]);
    }
    hx_mutex_unlock(&g_core.sink_lock);
}

void hx_clog_shutdown(void) {
    int i;
    if (!g_core.initialized) {
        return;
    }
    g_core.initialized = 0; /* stop accepting new logs first */

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (g_core.mode == HX_CLOG_MODE_ASYNC) {
        hx_async_stop(); /* drains queue into sinks */
    }
#endif

#if defined(HX_CLOG_ENABLE_CRASH)
    hx_clog_uninstall_crash_handler();
#endif

    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_sink_flush(g_core.sinks[i]);
        hx_sink_close(g_core.sinks[i]);
        g_core.sinks[i] = NULL;
    }
    g_core.sink_count = 0;
    hx_mutex_unlock(&g_core.sink_lock);

    hx_ring_destroy();
}

void hx_clog_set_level(hx_clog_level_t level) {
    hx_atomic_store_level(&g_core.level, (int)level);
}

hx_clog_level_t hx_clog_get_level(void) {
    return (hx_clog_level_t)hx_atomic_load_level(&g_core.level);
}

int hx_clog_reopen(void) {
    int i, rc = HX_CLOG_OK;
    if (!g_core.initialized) {
        return HX_CLOG_ERR_NOT_INITIALIZED;
    }
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        if (g_core.sinks[i] && g_core.sinks[i]->is_file) {
            int r = hx_sink_file_reopen(g_core.sinks[i]);
            if (r != HX_CLOG_OK) {
                rc = r;
            }
        }
    }
    hx_mutex_unlock(&g_core.sink_lock);
    return rc;
}

int hx_clog_get_stats(hx_clog_stats_t* stats) {
    if (!stats) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    hx_mutex_lock(&g_core.stats_lock);
    stats->written_lines = g_core.written_lines;
    stats->rotated_files = g_core.rotated_files;
#if defined(HX_CLOG_ENABLE_ASYNC)
    stats->dropped_lines = hx_async_dropped();
    stats->queue_high_watermark = hx_async_high_watermark();
#else
    stats->dropped_lines = g_core.dropped_lines;
    stats->queue_high_watermark = 0;
#endif
    hx_mutex_unlock(&g_core.stats_lock);
    return HX_CLOG_OK;
}

int hx_clog_add_callback_sink(hx_clog_callback_t cb, void* user_data) {
    hx_clog_sink_t* s;
    if (!cb) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    ensure_once();
    s = hx_sink_callback_create(cb, user_data);
    if (!s) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    hx_mutex_lock(&g_core.sink_lock);
    if (g_core.sink_count >= HX_CLOG_MAX_SINKS) {
        hx_mutex_unlock(&g_core.sink_lock);
        hx_sink_close(s);
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    g_core.sinks[g_core.sink_count++] = s;
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_OK;
}

int hx_clog_set_allocator(const hx_clog_allocator_t* allocator) {
    if (g_core.initialized) {
        return HX_CLOG_ERR_ALREADY_INITIALIZED;
    }
    hx_clog__set_allocator(allocator);
    return HX_CLOG_OK;
}

void hx_clog_after_fork_child(void) {
    /* Re-init locks that may have been held by other threads at fork time. */
    hx_mutex_init(&g_core.sink_lock);
    hx_mutex_init(&g_core.stats_lock);
}

/* =========================================================================
 * Crash API stubs (when crash support is compiled out, keep the ABI stable)
 * ========================================================================= */
#if !defined(HX_CLOG_ENABLE_CRASH)
void hx_clog_crash_config_default(hx_clog_crash_config_t* config) {
    if (config) {
        memset(config, 0, sizeof(*config));
    }
}
int hx_clog_install_crash_handler(const hx_clog_crash_config_t* config) {
    (void)config;
    return HX_CLOG_ERR_PLATFORM;
}
void hx_clog_uninstall_crash_handler(void) {
}
#endif

/* =========================================================================
 * Write path
 * ========================================================================= */
void hx_clog_writev(hx_clog_level_t level,
                    const char* file, int line, const char* func,
                    const char* fmt, va_list args) {
    char msg_stack[HX_CLOG_STACK_BUF_SIZE];
    char* msg = msg_stack;
    char* msg_heap = NULL;
    char line_stack[HX_CLOG_STACK_BUF_SIZE + 256];
    char* outline = line_stack;
    char* outline_heap = NULL;
    int n;
    unsigned int msg_len;
    unsigned int line_len;
    hx_clog_record_t rec;
    va_list args_copy;

    /* fast level filter before any work */
    if (!g_core.initialized) {
        return;
    }
    if ((int)level < hx_atomic_load_level(&g_core.level) ||
        level == HX_CLOG_LEVEL_OFF) {
        return;
    }

    /* format the user message */
    va_copy(args_copy, args);
    n = vsnprintf(msg_stack, sizeof(msg_stack), fmt ? fmt : "", args);
    if (n < 0) {
        va_end(args_copy);
        return;
    }
    if ((unsigned int)n >= sizeof(msg_stack)) {
        unsigned int need = (unsigned int)n + 1;
        if (need > HX_CLOG_MAX_LINE) {
            need = HX_CLOG_MAX_LINE;
        }
        msg_heap = (char*)hx_clog__malloc(need);
        if (msg_heap) {
            vsnprintf(msg_heap, need, fmt ? fmt : "", args_copy);
            msg = msg_heap;
            msg_len = (unsigned int)strlen(msg_heap);
        } else {
            msg = msg_stack;
            msg_len = sizeof(msg_stack) - 1;
        }
    } else {
        msg_len = (unsigned int)n;
    }
    va_end(args_copy);

    /* assemble the full line via the pattern */
    rec.level = level;
    rec.file = file;
    rec.line = line;
    rec.func = func;
    rec.tid = hx_get_tid();
    hx_now(&rec.ts);
    rec.msg = msg;
    rec.msg_len = msg_len;

    line_len = hx_format_record(g_core.pattern, &rec, line_stack, sizeof(line_stack));
    if (line_len + 1 >= sizeof(line_stack)) {
        /* message likely truncated; retry on heap */
        unsigned int cap = msg_len + 1024;
        if (cap > HX_CLOG_MAX_LINE) cap = HX_CLOG_MAX_LINE;
        outline_heap = (char*)hx_clog__malloc(cap);
        if (outline_heap) {
            line_len = hx_format_record(g_core.pattern, &rec, outline_heap, cap);
            outline = outline_heap;
        }
    }

    /* feed the crash ring buffer (always, cheap) */
    hx_ring_push(outline, line_len);

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (g_core.mode == HX_CLOG_MODE_ASYNC) {
        if (hx_async_enqueue(level, outline, line_len) != HX_CLOG_OK) {
            /* enqueue failed (drop policy); counted inside async */
        }
    } else {
        hx_core_emit_to_sinks(level, outline, line_len);
    }
#else
    hx_core_emit_to_sinks(level, outline, line_len);
#endif

    /* FATAL is flushed immediately to maximize crash survivability */
    if (level >= HX_CLOG_LEVEL_FATAL) {
        hx_clog_flush();
    }

    if (msg_heap)     hx_clog__free(msg_heap);
    if (outline_heap) hx_clog__free(outline_heap);
}

void hx_clog_write(hx_clog_level_t level,
                   const char* file, int line, const char* func,
                   const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    hx_clog_writev(level, file, line, func, fmt, args);
    va_end(args);
}
