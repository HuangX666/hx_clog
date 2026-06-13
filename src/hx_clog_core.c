/*
 * hx_clog - core.
 *
 * Owns global logger state, level filtering, the synchronous write path,
 * sink management, statistics, the crash ring buffer, and all platform /
 * threading / allocator helpers shared across the library.
 */
#include "hx_clog_internal.h"

#include <stdarg.h>
#include <limits.h>

#if defined(HX_PLATFORM_WINDOWS)
#  include <sys/stat.h>
#endif

#if defined(__linux__)
#  include <sys/syscall.h>
#endif

#if defined(_MSC_VER)
#  define HX_THREAD_LOCAL __declspec(thread)
#elif defined(__GNUC__)
#  define HX_THREAD_LOCAL __thread
#else
#  define HX_THREAD_LOCAL
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
        /* the public allocator signature takes unsigned int; refuse requests
         * that would silently truncate (only reachable with
         * HX_CLOG_UNLIMITED_LINE on 64-bit) */
#if defined(SIZE_MAX) && (SIZE_MAX > UINT_MAX)
        if (size > (size_t)UINT_MAX) {
            return NULL;
        }
#endif
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

/* Timed waits use the monotonic clock where possible so a wall-clock jump
 * (NTP step, manual adjustment) cannot stall the async flush timer. */
void hx_cond_init(hx_cond_t* c) {
#if !defined(HX_PLATFORM_APPLE) && defined(CLOCK_MONOTONIC)
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(c, &attr);
    pthread_condattr_destroy(&attr);
#else
    pthread_cond_init(c, NULL);
#endif
}
void hx_cond_destroy(hx_cond_t* c)   { pthread_cond_destroy(c); }
void hx_cond_signal(hx_cond_t* c)    { pthread_cond_signal(c); }
void hx_cond_broadcast(hx_cond_t* c) { pthread_cond_broadcast(c); }

int hx_cond_wait_ms(hx_cond_t* c, hx_mutex_t* m, unsigned int timeout_ms) {
    struct timespec ts;
    int r;
#if defined(HX_PLATFORM_APPLE)
    /* macOS lacks pthread_condattr_setclock; the relative wait is unaffected
     * by wall-clock jumps. */
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    r = pthread_cond_timedwait_relative_np(c, m, &ts);
    return r == 0 ? 1 : 0;
#elif defined(CLOCK_MONOTONIC)
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec += 1;
        ts.tv_nsec -= 1000000000L;
    }
    r = pthread_cond_timedwait(c, m, &ts);
    return r == 0 ? 1 : 0;
#else
    {
        struct timeval tv;
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
#endif
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
    /* SYS_gettid resolves to the right number for every architecture
     * (x86-64: 186, aarch64: 178, arm: 224, ...). */
    return (unsigned long)syscall(SYS_gettid);
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

#if defined(HX_PLATFORM_WINDOWS)
/* All paths handled by the library are UTF-8 by convention. The Windows
 * narrow (A) APIs interpret narrow strings in the system ANSI codepage, so a
 * UTF-8 path with non-ASCII characters would be mangled. Convert to UTF-16
 * and use the wide APIs everywhere. */
int hx_utf8_to_wide(const char* utf8, wchar_t* out, int out_cap) {
    int n;
    if (!utf8 || !out || out_cap <= 0) {
        return -1;
    }
    n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, out_cap);
    return n > 0 ? n : -1;
}

int hx_wide_to_utf8(const wchar_t* wide, char* out, int out_cap) {
    int n;
    if (!wide || !out || out_cap <= 0) {
        return -1;
    }
    n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, out, out_cap, NULL, NULL);
    return n > 0 ? n : -1;
}

static int hx_mkdir_one(const char* path) {
    wchar_t wpath[HX_CLOG_PATH_MAX];
    if (hx_utf8_to_wide(path, wpath, HX_CLOG_PATH_MAX) < 0) {
        return -1;
    }
    return _wmkdir(wpath);
}
#else
static int hx_mkdir_one(const char* path) {
    return mkdir(path, 0755);
}
#endif

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
            hx_mkdir_one(tmp);
            tmp[i] = saved;
        }
    }
    if (hx_mkdir_one(tmp) != 0 && !hx_file_exists(tmp)) {
        return -1;
    }
    return 0;
}

int hx_file_exists(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    wchar_t wpath[HX_CLOG_PATH_MAX];
    DWORD a;
    if (hx_utf8_to_wide(path, wpath, HX_CLOG_PATH_MAX) < 0) {
        return 0;
    }
    a = GetFileAttributesW(wpath);
    return (a != INVALID_FILE_ATTRIBUTES);
#else
    struct stat st;
    return stat(path, &st) == 0;
#endif
}

long long hx_file_size(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    wchar_t wpath[HX_CLOG_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA ad;
    if (hx_utf8_to_wide(path, wpath, HX_CLOG_PATH_MAX) < 0) {
        return -1;
    }
    if (GetFileAttributesExW(wpath, GetFileExInfoStandard, &ad)) {
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
    wchar_t wfrom[HX_CLOG_PATH_MAX];
    wchar_t wto[HX_CLOG_PATH_MAX];
    if (hx_utf8_to_wide(from, wfrom, HX_CLOG_PATH_MAX) < 0 ||
        hx_utf8_to_wide(to, wto, HX_CLOG_PATH_MAX) < 0) {
        return -1;
    }
    /* rename() fails if target exists on Windows; replace atomically. */
    if (MoveFileExW(wfrom, wto, MOVEFILE_REPLACE_EXISTING)) {
        return 0;
    }
    return -1;
#else
    return rename(from, to);
#endif
}

int hx_remove(const char* path) {
#if defined(HX_PLATFORM_WINDOWS)
    wchar_t wpath[HX_CLOG_PATH_MAX];
    if (hx_utf8_to_wide(path, wpath, HX_CLOG_PATH_MAX) < 0) {
        return -1;
    }
    return DeleteFileW(wpath) ? 0 : -1;
#else
    return remove(path);
#endif
}

FILE* hx_fopen(const char* path, const char* mode) {
#if defined(HX_PLATFORM_WINDOWS)
    wchar_t wpath[HX_CLOG_PATH_MAX];
    wchar_t wmode[16];
    if (hx_utf8_to_wide(path, wpath, HX_CLOG_PATH_MAX) < 0 ||
        hx_utf8_to_wide(mode, wmode, 16) < 0) {
        return NULL;
    }
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
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

void hx_ring_after_fork_child(void) {
    if (g_ring_inited) {
        hx_mutex_init(&g_ring_lock);
    }
}

/* =========================================================================
 * Thread-local context and logger state
 * ========================================================================= */
#define HX_CONTEXT_MAX_ITEMS 16
#define HX_CONTEXT_KEY_MAX   64
#define HX_CONTEXT_VALUE_MAX 128

typedef struct {
    int count;
    char keys[HX_CONTEXT_MAX_ITEMS][HX_CONTEXT_KEY_MAX];
    char values[HX_CONTEXT_MAX_ITEMS][HX_CONTEXT_VALUE_MAX];
} hx_context_state_t;

static HX_THREAD_LOCAL hx_context_state_t g_tls_context;

struct hx_clog_logger {
    char name[128];
    volatile int level;
    int is_default;
};

static int context_key_index(const char* key) {
    int i;
    if (!key || !key[0]) {
        return -1;
    }
    for (i = 0; i < g_tls_context.count; ++i) {
        if (strcmp(g_tls_context.keys[i], key) == 0) {
            return i;
        }
    }
    return -1;
}

void hx_context_snapshot_text(char* out, unsigned int cap) {
    int i;
    unsigned int pos = 0;
    if (!out || cap == 0) {
        return;
    }
    out[0] = '\0';
    for (i = 0; i < g_tls_context.count; ++i) {
        const char* k = g_tls_context.keys[i];
        const char* v = g_tls_context.values[i];
        unsigned int need = (unsigned int)strlen(k) + (unsigned int)strlen(v) + 2;
        if (pos > 0) {
            if (pos + 1 >= cap) break;
            out[pos++] = ' ';
        }
        if (pos + need >= cap) {
            break;
        }
        strcpy(out + pos, k);
        pos += (unsigned int)strlen(k);
        out[pos++] = '=';
        strcpy(out + pos, v);
        pos += (unsigned int)strlen(v);
    }
    out[pos < cap ? pos : cap - 1] = '\0';
}

int hx_clog_context_put(const char* key, const char* value) {
    int idx;
    if (!key || !key[0] || !value) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    idx = context_key_index(key);
    if (idx < 0) {
        if (g_tls_context.count >= HX_CONTEXT_MAX_ITEMS) {
            return HX_CLOG_ERR_INVALID_ARGUMENT;
        }
        idx = g_tls_context.count++;
        strncpy(g_tls_context.keys[idx], key, HX_CONTEXT_KEY_MAX - 1);
        g_tls_context.keys[idx][HX_CONTEXT_KEY_MAX - 1] = '\0';
    }
    strncpy(g_tls_context.values[idx], value, HX_CONTEXT_VALUE_MAX - 1);
    g_tls_context.values[idx][HX_CONTEXT_VALUE_MAX - 1] = '\0';
    return HX_CLOG_OK;
}

void hx_clog_context_remove(const char* key) {
    int idx = context_key_index(key);
    if (idx >= 0) {
        int i;
        for (i = idx; i + 1 < g_tls_context.count; ++i) {
            strcpy(g_tls_context.keys[i], g_tls_context.keys[i + 1]);
            strcpy(g_tls_context.values[i], g_tls_context.values[i + 1]);
        }
        g_tls_context.count--;
    }
}

void hx_clog_context_clear(void) {
    memset(&g_tls_context, 0, sizeof(g_tls_context));
}

/* =========================================================================
 * Global logger state
 * ========================================================================= */
typedef struct {
    volatile int initialized;    /* atomic: read lock-free on the hot path */
    volatile int level;          /* atomic */
    volatile int mode;           /* atomic: hx_clog_mode_t stored as int */
    struct hx_clog_logger default_logger;

    hx_clog_sink_t* sinks[HX_CLOG_MAX_SINKS];
    int sink_count;
    hx_clog_sink_id_t next_sink_id;

    char pattern[512];
    hx_clog_format_mode_t format_mode;
    hx_clog_formatter_t formatter;
    void* formatter_user_data;
    volatile int format_gen;     /* bumped on any format-settings change so
                                  * per-thread caches can refresh lazily */
    volatile int override_count; /* sinks with a per-sink format override */

    hx_mutex_t sink_lock;        /* guards sink writes in sync mode + sink list */
    hx_mutex_t init_lock;        /* serializes init/shutdown/reconfigure */

    /* stats */
    hx_mutex_t stats_lock;
    unsigned long long written_lines;
    unsigned long long rotated_files;
    unsigned long long dropped_lines;
} hx_core_state_t;

static hx_core_state_t g_core;

/* Atomic accessors for the two fields read lock-free on the hot path
 * (write path / flush) while being written under init_lock. Keeping them
 * atomic avoids a data race with concurrent reconfigure/shutdown. */
static int core_is_initialized(void) {
    return hx_atomic_load_level(&g_core.initialized);
}
static void core_set_initialized(int v) {
    hx_atomic_store_level(&g_core.initialized, v);
}
static hx_clog_mode_t core_get_mode(void) {
    return (hx_clog_mode_t)hx_atomic_load_level(&g_core.mode);
}
static void core_set_mode(hx_clog_mode_t m) {
    hx_atomic_store_level(&g_core.mode, (int)m);
}

/* Per-thread snapshot of the global format settings, refreshed only when
 * format_gen changes, so the hot path normally takes no lock for them. */
typedef struct {
    int gen;                     /* 0 = never filled */
    char pattern[512];
    hx_clog_format_mode_t mode;
    hx_clog_formatter_t formatter;
    void* formatter_user_data;
} tls_format_cache_t;

static HX_THREAD_LOCAL tls_format_cache_t g_tls_fmt;

/* Internal error handler (cold path). */
static hx_mutex_t g_err_lock;
static hx_clog_error_handler_t g_err_handler = NULL;
static void* g_err_user_data = NULL;

/* Crash callback storage (lives here so the setter exists even when crash
 * support is compiled out). Written rarely; read inside a crash handler. */
static hx_clog_crash_callback_t g_crash_cb = NULL;
static void* g_crash_cb_ud = NULL;

/* Duplicate suppression state. */
typedef struct {
    int enabled;
    unsigned int window_ms;
    hx_mutex_t lock;
    int have_last;
    hx_clog_level_t last_level;
    int last_line;
    char last_logger[128];
    char last_file[256];
    char last_msg[256];
    unsigned int last_msg_len;   /* full length (key uses first 255 bytes) */
    unsigned long long repeats;
    long long last_ms;           /* timestamp of last duplicate */
} dup_state_t;

static dup_state_t g_dup;

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
    config->enable_syslog = 0;
    config->enable_event_log = 0;
    config->enable_android_log = 0;
    config->enable_apple_log = 0;
    config->rotate_policy = HX_CLOG_ROTATE_BY_SIZE_AND_TIME;
    config->max_file_size = 10ULL * 1024ULL * 1024ULL;
    config->max_backup_files = -1;     /* -1 = keep all backups (no auto delete) */
    config->max_backup_days = 0;
    config->rotate_daily = 1;
    config->rotate_interval_seconds = 0;
    config->rotate_align = 0;
    config->rotate_on_startup = 0;
    config->max_compressed_files = -1; /* -1 = never delete .gz by count */
    config->date_subdir = 0;
    config->async_queue_size = 8192;
    config->async_batch_size = 64;
    config->flush_interval_ms = 1000;
    config->overflow_policy = HX_CLOG_OVERFLOW_BLOCK;
    config->pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [tid:%t] %s:%# %!() - %v%n";
    config->format_mode = HX_CLOG_FORMAT_PATTERN;
    config->formatter = NULL;
    config->formatter_user_data = NULL;
    config->system_logger_name = "hx_clog";
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
static void ensure_once(void);

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

/* Write one formatted line to the matching sinks. Used by sync path and
 * async worker. target_sink_id == 0 routes to every sink without a per-sink
 * format override; a non-zero target routes only to that sink (its line was
 * rendered with the sink's own format). */
void hx_core_emit_to_sinks(hx_clog_level_t level, const char* line,
                           unsigned int len, hx_clog_sink_id_t target_sink_id,
                           int count_stats) {
    int i;
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_clog_sink_t* s = g_core.sinks[i];
        if (!s) {
            continue;
        }
        if (target_sink_id == 0) {
            if (s->override_set) {
                continue;
            }
        } else if (s->id != target_sink_id) {
            continue;
        }
        hx_sink_write(s, level, line, len);
    }
    hx_mutex_unlock(&g_core.sink_lock);
    if (count_stats) {
        hx_core_add_written(1);
    }
}

void hx_core_report_error(int err, const char* message) {
    hx_clog_error_handler_t h;
    void* ud;
    hx_mutex_lock(&g_err_lock);
    h = g_err_handler;
    ud = g_err_user_data;
    hx_mutex_unlock(&g_err_lock);
    if (h) {
        h(err, message ? message : "", ud);
    }
}

int hx_clog_set_error_handler(hx_clog_error_handler_t handler, void* user_data) {
    ensure_once();
    hx_mutex_lock(&g_err_lock);
    g_err_handler = handler;
    g_err_user_data = user_data;
    hx_mutex_unlock(&g_err_lock);
    return HX_CLOG_OK;
}

hx_clog_crash_callback_t hx_crash_get_callback(void** user_data_out) {
    if (user_data_out) {
        *user_data_out = g_crash_cb_ud;
    }
    return g_crash_cb;
}

int hx_clog_set_crash_callback(hx_clog_crash_callback_t cb, void* user_data) {
    /* set ud first so a concurrent crash never sees the new cb with the old
     * user_data */
    g_crash_cb = NULL;
    g_crash_cb_ud = user_data;
    g_crash_cb = cb;
    return HX_CLOG_OK;
}

void hx_core_flush_sinks(void) {
    int i;
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_sink_flush(g_core.sinks[i]);
    }
    hx_mutex_unlock(&g_core.sink_lock);
}

/* =========================================================================
 * Lifecycle
 * ========================================================================= */
static void core_once_init(void) {
    memset(&g_core, 0, sizeof(g_core));
    hx_mutex_init(&g_core.sink_lock);
    hx_mutex_init(&g_core.stats_lock);
    hx_mutex_init(&g_core.init_lock);
    hx_mutex_init(&g_err_lock);
    memset(&g_dup, 0, sizeof(g_dup));
    hx_mutex_init(&g_dup.lock);
    g_core.level = HX_CLOG_LEVEL_INFO;
    g_core.default_logger.is_default = 1;
    strcpy(g_core.default_logger.name, "hx_clog");
    g_core.default_logger.level = HX_CLOG_LEVEL_INFO;
    g_core.next_sink_id = 1;
    g_core.format_mode = HX_CLOG_FORMAT_PATTERN;
    g_core.format_gen = 1; /* TLS caches start at 0 => first use refreshes */
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

static int file_name_is_plain(const char* file_name) {
    const char* p;
    if (!file_name || !file_name[0]) {
        return 0;
    }
    for (p = file_name; *p; ++p) {
        if (*p == '/' || *p == '\\' || *p == ':') {
            return 0;
        }
    }
    return 1;
}

static int add_sink_locked(hx_clog_sink_t* s, hx_clog_sink_id_t* out_id) {
    if (!s) {
        return HX_CLOG_ERR_PLATFORM;
    }
    if (g_core.sink_count >= HX_CLOG_MAX_SINKS) {
        hx_sink_close(s);
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    if (s->id == 0) {
        s->id = g_core.next_sink_id++;
        if (g_core.next_sink_id == 0) {
            g_core.next_sink_id = 1;
        }
        /* fresh sink: no per-sink format override yet */
        s->override_set = 0;
        s->override_mode = HX_CLOG_FORMAT_PATTERN;
        s->override_has_pattern = 0;
        s->override_pattern[0] = '\0';
    }
    if (s->min_level < HX_CLOG_LEVEL_TRACE || s->min_level > HX_CLOG_LEVEL_OFF) {
        s->min_level = HX_CLOG_LEVEL_TRACE;
    }
    g_core.sinks[g_core.sink_count++] = s;
    if (out_id) {
        *out_id = s->id;
    }
    return HX_CLOG_OK;
}

static int add_sink(hx_clog_sink_t* s) {
    return add_sink_locked(s, NULL);
}

static void add_system_sink_or_report(hx_clog_sink_t* s, const char* what) {
    if (!s) {
        hx_core_report_error(HX_CLOG_ERR_PLATFORM, what);
        return;
    }
    if (add_sink(s) != HX_CLOG_OK) {
        hx_core_report_error(HX_CLOG_ERR_INVALID_ARGUMENT, what);
    }
}

static void add_configured_system_sinks(const hx_clog_config_t* cfg) {
    const char* ident = cfg->system_logger_name ? cfg->system_logger_name :
                        (cfg->logger_name ? cfg->logger_name : "hx_clog");
    if (cfg->enable_syslog) {
        add_system_sink_or_report(hx_sink_syslog_create(ident),
                                  "syslog sink unavailable on this platform/build");
    }
    if (cfg->enable_event_log) {
        add_system_sink_or_report(hx_sink_event_log_create(ident),
                                  "event log sink unavailable or registration failed");
    }
    if (cfg->enable_android_log) {
        add_system_sink_or_report(hx_sink_android_log_create(ident),
                                  "android log sink unavailable on this platform");
    }
    if (cfg->enable_apple_log) {
        add_system_sink_or_report(hx_sink_apple_log_create(ident),
                                  "apple os_log sink unavailable on this platform");
    }
}

static void close_non_callback_sinks_locked(void) {
    int i = 0;
    while (i < g_core.sink_count) {
        hx_clog_sink_t* s = g_core.sinks[i];
        if (s && s->kind != HX_SINK_KIND_CALLBACK) {
            int j;
            hx_sink_flush(s);
            hx_sink_close(s);
            for (j = i; j + 1 < g_core.sink_count; ++j) {
                g_core.sinks[j] = g_core.sinks[j + 1];
            }
            g_core.sinks[g_core.sink_count - 1] = NULL;
            g_core.sink_count--;
        } else {
            ++i;
        }
    }
}

static int hx_clog_init_locked(const hx_clog_config_t* in_config) {
    hx_clog_config_t cfg;

    if (core_is_initialized()) {
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
    if (!cfg.logger_name) cfg.logger_name = "hx_clog";
    if (!cfg.system_logger_name) cfg.system_logger_name = cfg.logger_name;
    if (!cfg.pattern)   cfg.pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [tid:%t] %s:%# %!() - %v%n";

    apply_env_overrides(&cfg);

    if (!file_name_is_plain(cfg.file_name)) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }

    strncpy(g_core.pattern, cfg.pattern, sizeof(g_core.pattern) - 1);
    g_core.pattern[sizeof(g_core.pattern) - 1] = '\0';
    g_core.format_mode = cfg.format_mode;
    g_core.formatter = cfg.formatter;
    g_core.formatter_user_data = cfg.formatter_user_data;
    core_set_mode(cfg.mode);
    g_core.sink_count = 0;
    hx_atomic_store_level(&g_core.override_count, 0);
    hx_atomic_store_level(&g_core.format_gen,
                          hx_atomic_load_level(&g_core.format_gen) + 1);
    hx_atomic_store_level(&g_core.level, (int)cfg.level);
    strncpy(g_core.default_logger.name, cfg.logger_name,
            sizeof(g_core.default_logger.name) - 1);
    g_core.default_logger.name[sizeof(g_core.default_logger.name) - 1] = '\0';
    hx_atomic_store_level(&g_core.default_logger.level, (int)cfg.level);

    hx_ring_init();

    /* build sinks */
    if (cfg.enable_console) {
        hx_clog_sink_t* cs = hx_sink_console_create(1, cfg.enable_color);
        if (!cs) {
            hx_core_report_error(HX_CLOG_ERR_OUT_OF_MEMORY,
                                 "console sink creation failed");
        } else {
            add_sink(cs);
        }
    }
    if (cfg.enable_file) {
        hx_clog_sink_t* fs = hx_sink_file_create(cfg.log_dir, cfg.file_name, &cfg);
        if (!fs) {
            /* file is important; report failure but keep console working */
            hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                                 "file sink creation failed (log_dir/file_name)");
            if (g_core.sink_count == 0) {
                return HX_CLOG_ERR_OPEN_FILE_FAILED;
            }
        } else {
            add_sink(fs);
        }
    }
    add_configured_system_sinks(&cfg);

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (cfg.mode == HX_CLOG_MODE_ASYNC) {
        if (hx_async_start(&cfg) != HX_CLOG_OK) {
            core_set_mode(HX_CLOG_MODE_SYNC); /* fall back */
        }
    }
#else
    if (cfg.mode == HX_CLOG_MODE_ASYNC) {
        core_set_mode(HX_CLOG_MODE_SYNC);
    }
#endif

    core_set_initialized(1);

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

int hx_clog_init(const hx_clog_config_t* in_config) {
    int rc;
    ensure_once();
    hx_mutex_lock(&g_core.init_lock);
    rc = hx_clog_init_locked(in_config);
    hx_mutex_unlock(&g_core.init_lock);
    return rc;
}

int hx_clog_is_initialized(void) {
    return core_is_initialized();
}

static void dup_flush_pending(void); /* defined with the write path below */

void hx_clog_flush(void) {
    if (!core_is_initialized()) {
        return;
    }
    dup_flush_pending(); /* emit a pending "repeated N times" line first */
#if defined(HX_CLOG_ENABLE_ASYNC)
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
        hx_async_flush();
    }
#endif
    hx_core_flush_sinks();
}

void hx_clog_shutdown(void) {
    int i;
    ensure_once();
    hx_mutex_lock(&g_core.init_lock);
    if (!core_is_initialized()) {
        hx_mutex_unlock(&g_core.init_lock);
        return;
    }
    dup_flush_pending(); /* emit a trailing "repeated N times" if pending */
    core_set_initialized(0); /* stop accepting new logs first */

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
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
    hx_atomic_store_level(&g_core.override_count, 0);
    hx_mutex_unlock(&g_core.sink_lock);

    hx_ring_destroy();
    hx_mutex_unlock(&g_core.init_lock);
}

void hx_clog_set_level(hx_clog_level_t level) {
    hx_atomic_store_level(&g_core.level, (int)level);
    hx_atomic_store_level(&g_core.default_logger.level, (int)level);
}

hx_clog_level_t hx_clog_get_level(void) {
    return (hx_clog_level_t)hx_atomic_load_level(&g_core.level);
}

static void bump_format_gen_locked(void) {
    int g = hx_atomic_load_level(&g_core.format_gen) + 1;
    if (g == 0) {
        g = 1; /* 0 is reserved for "TLS cache never filled" */
    }
    hx_atomic_store_level(&g_core.format_gen, g);
}

int hx_clog_set_pattern(const char* pattern) {
    if (!pattern) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    strncpy(g_core.pattern, pattern, sizeof(g_core.pattern) - 1);
    g_core.pattern[sizeof(g_core.pattern) - 1] = '\0';
    g_core.format_mode = HX_CLOG_FORMAT_PATTERN;
    bump_format_gen_locked();
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_OK;
}

int hx_clog_set_format_mode(hx_clog_format_mode_t mode) {
    if (mode != HX_CLOG_FORMAT_PATTERN && mode != HX_CLOG_FORMAT_JSON) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    g_core.format_mode = mode;
    bump_format_gen_locked();
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_OK;
}

int hx_clog_set_formatter(hx_clog_formatter_t formatter, void* user_data) {
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    g_core.formatter = formatter;
    g_core.formatter_user_data = user_data;
    bump_format_gen_locked();
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_OK;
}

static void recompute_override_count_locked(void) {
    int i, n = 0;
    for (i = 0; i < g_core.sink_count; ++i) {
        if (g_core.sinks[i] && g_core.sinks[i]->override_set) {
            n++;
        }
    }
    hx_atomic_store_level(&g_core.override_count, n);
}

static int hx_clog_reconfigure_locked(const hx_clog_config_t* in_config) {
    hx_clog_config_t cfg;
    hx_clog_config_t defaults;
    hx_clog_sink_t* new_file = NULL;

    if (!core_is_initialized()) {
        return HX_CLOG_ERR_NOT_INITIALIZED;
    }
    if (!in_config) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    hx_clog_config_default(&defaults);
    cfg = *in_config;
    if (!cfg.file_name) cfg.file_name = defaults.file_name;
    if (!cfg.log_dir) cfg.log_dir = defaults.log_dir;
    if (!cfg.logger_name) cfg.logger_name = defaults.logger_name;
    if (!cfg.pattern) cfg.pattern = defaults.pattern;
    if (!cfg.system_logger_name) cfg.system_logger_name = cfg.logger_name;
    if (cfg.format_mode != HX_CLOG_FORMAT_PATTERN &&
        cfg.format_mode != HX_CLOG_FORMAT_JSON) {
        cfg.format_mode = HX_CLOG_FORMAT_PATTERN;
    }
    if (!file_name_is_plain(cfg.file_name)) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }

    /* create the new file sink up front so a bad log_dir cannot destroy the
     * existing file output: on failure nothing has been torn down yet */
    if (cfg.enable_file) {
        new_file = hx_sink_file_create(cfg.log_dir, cfg.file_name, &cfg);
        if (!new_file) {
            hx_core_report_error(HX_CLOG_ERR_OPEN_FILE_FAILED,
                                 "reconfigure: new file sink creation failed;"
                                 " keeping the previous sinks");
            return HX_CLOG_ERR_OPEN_FILE_FAILED;
        }
    }

    hx_clog_flush();

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
        hx_async_stop();
    }
#endif

    hx_mutex_lock(&g_core.sink_lock);
    close_non_callback_sinks_locked();

    strncpy(g_core.pattern, cfg.pattern, sizeof(g_core.pattern) - 1);
    g_core.pattern[sizeof(g_core.pattern) - 1] = '\0';
    g_core.format_mode = cfg.format_mode;
    g_core.formatter = cfg.formatter;
    g_core.formatter_user_data = cfg.formatter_user_data;
    bump_format_gen_locked();
    hx_atomic_store_level(&g_core.level, (int)cfg.level);
    strncpy(g_core.default_logger.name, cfg.logger_name,
            sizeof(g_core.default_logger.name) - 1);
    g_core.default_logger.name[sizeof(g_core.default_logger.name) - 1] = '\0';
    hx_atomic_store_level(&g_core.default_logger.level, (int)cfg.level);

    if (cfg.enable_console) {
        hx_clog_sink_t* cs = hx_sink_console_create(1, cfg.enable_color);
        if (!cs) {
            hx_core_report_error(HX_CLOG_ERR_OUT_OF_MEMORY,
                                 "console sink creation failed");
        } else {
            add_sink(cs);
        }
    }
    if (new_file) {
        add_sink(new_file);
        /* the old sink may have appended to the same file between the early
         * create and the swap; re-sync the size from disk */
        hx_sink_file_reopen(new_file);
    }
    add_configured_system_sinks(&cfg);
    core_set_mode(cfg.mode);
    recompute_override_count_locked(); /* surviving callback sinks may have overrides */
    hx_mutex_unlock(&g_core.sink_lock);

#if defined(HX_CLOG_ENABLE_ASYNC)
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
        if (hx_async_start(&cfg) != HX_CLOG_OK) {
            core_set_mode(HX_CLOG_MODE_SYNC);
        }
    }
#else
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
        core_set_mode(HX_CLOG_MODE_SYNC);
    }
#endif
    return HX_CLOG_OK;
}

int hx_clog_reconfigure(const hx_clog_config_t* in_config) {
    int rc;
    ensure_once();
    hx_mutex_lock(&g_core.init_lock);
    rc = hx_clog_reconfigure_locked(in_config);
    hx_mutex_unlock(&g_core.init_lock);
    return rc;
}

int hx_clog_reopen(void) {
    int i, rc = HX_CLOG_OK;
    if (!core_is_initialized()) {
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
    return hx_clog_add_callback_sink_ex(cb, user_data, NULL);
}

int hx_clog_add_callback_sink_ex(hx_clog_callback_t cb, void* user_data,
                                 hx_clog_sink_id_t* out_id) {
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
    {
        int r = add_sink_locked(s, out_id);
        hx_mutex_unlock(&g_core.sink_lock);
        return r;
    }
}

static int add_system_sink_runtime(hx_clog_sink_t* s, hx_clog_sink_id_t* out_id) {
    int r;
    if (!s) {
        return HX_CLOG_ERR_PLATFORM;
    }
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    r = add_sink_locked(s, out_id);
    hx_mutex_unlock(&g_core.sink_lock);
    return r;
}

int hx_clog_add_syslog_sink(const char* ident, hx_clog_sink_id_t* out_id) {
    return add_system_sink_runtime(hx_sink_syslog_create(ident), out_id);
}

int hx_clog_add_event_log_sink(const char* source, hx_clog_sink_id_t* out_id) {
    return add_system_sink_runtime(hx_sink_event_log_create(source), out_id);
}

int hx_clog_add_android_log_sink(const char* tag, hx_clog_sink_id_t* out_id) {
    return add_system_sink_runtime(hx_sink_android_log_create(tag), out_id);
}

int hx_clog_add_apple_log_sink(const char* subsystem, hx_clog_sink_id_t* out_id) {
    return add_system_sink_runtime(hx_sink_apple_log_create(subsystem), out_id);
}

int hx_clog_remove_sink(hx_clog_sink_id_t id) {
    int i;
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        if (g_core.sinks[i] && g_core.sinks[i]->id == id) {
            int j;
            hx_sink_flush(g_core.sinks[i]);
            hx_sink_close(g_core.sinks[i]);
            for (j = i; j + 1 < g_core.sink_count; ++j) {
                g_core.sinks[j] = g_core.sinks[j + 1];
            }
            g_core.sinks[g_core.sink_count - 1] = NULL;
            g_core.sink_count--;
            recompute_override_count_locked();
            hx_mutex_unlock(&g_core.sink_lock);
            return HX_CLOG_OK;
        }
    }
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_ERR_INVALID_ARGUMENT;
}

int hx_clog_set_sink_level(hx_clog_sink_id_t id, hx_clog_level_t min_level) {
    int i;
    if (min_level < HX_CLOG_LEVEL_TRACE || min_level > HX_CLOG_LEVEL_OFF) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        if (g_core.sinks[i] && g_core.sinks[i]->id == id) {
            g_core.sinks[i]->min_level = min_level;
            hx_mutex_unlock(&g_core.sink_lock);
            return HX_CLOG_OK;
        }
    }
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_ERR_INVALID_ARGUMENT;
}

int hx_clog_set_sink_pattern(hx_clog_sink_id_t id, const char* pattern) {
    int i;
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_clog_sink_t* s = g_core.sinks[i];
        if (s && s->id == id) {
            if (pattern) {
                strncpy(s->override_pattern, pattern,
                        sizeof(s->override_pattern) - 1);
                s->override_pattern[sizeof(s->override_pattern) - 1] = '\0';
                s->override_has_pattern = 1;
                s->override_mode = HX_CLOG_FORMAT_PATTERN;
                s->override_set = 1;
            } else {
                /* clear the whole override */
                s->override_set = 0;
                s->override_has_pattern = 0;
                s->override_pattern[0] = '\0';
            }
            recompute_override_count_locked();
            hx_mutex_unlock(&g_core.sink_lock);
            return HX_CLOG_OK;
        }
    }
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_ERR_INVALID_ARGUMENT;
}

int hx_clog_set_sink_format_mode(hx_clog_sink_id_t id,
                                 hx_clog_format_mode_t mode) {
    int i;
    if (mode != HX_CLOG_FORMAT_PATTERN && mode != HX_CLOG_FORMAT_JSON) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        hx_clog_sink_t* s = g_core.sinks[i];
        if (s && s->id == id) {
            s->override_mode = mode;
            s->override_set = 1;
            recompute_override_count_locked();
            hx_mutex_unlock(&g_core.sink_lock);
            return HX_CLOG_OK;
        }
    }
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_ERR_INVALID_ARGUMENT;
}

int hx_clog_set_duplicate_suppression(int enable, unsigned int window_ms) {
    ensure_once();
    if (!enable) {
        dup_flush_pending();
    }
    hx_mutex_lock(&g_dup.lock);
    g_dup.enabled = enable ? 1 : 0;
    g_dup.window_ms = window_ms ? window_ms : 1000;
    if (!enable) {
        g_dup.have_last = 0;
        g_dup.repeats = 0;
    }
    hx_mutex_unlock(&g_dup.lock);
    return HX_CLOG_OK;
}

int hx_clog_flush_sink(hx_clog_sink_id_t id) {
    int i;
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    for (i = 0; i < g_core.sink_count; ++i) {
        if (g_core.sinks[i] && g_core.sinks[i]->id == id) {
            hx_sink_flush(g_core.sinks[i]);
            hx_mutex_unlock(&g_core.sink_lock);
            return HX_CLOG_OK;
        }
    }
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_ERR_INVALID_ARGUMENT;
}

int hx_clog_get_sink_count(unsigned int* count) {
    if (!count) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    ensure_once();
    hx_mutex_lock(&g_core.sink_lock);
    *count = (unsigned int)g_core.sink_count;
    hx_mutex_unlock(&g_core.sink_lock);
    return HX_CLOG_OK;
}

int hx_clog_set_allocator(const hx_clog_allocator_t* allocator) {
    if (core_is_initialized()) {
        return HX_CLOG_ERR_ALREADY_INITIALIZED;
    }
    hx_clog__set_allocator(allocator);
    return HX_CLOG_OK;
}

void hx_ring_after_fork_child(void); /* defined with the ring buffer above */

void hx_clog_after_fork_child(void) {
    int i;
    /* Re-init every lock that may have been held by a parent thread at fork
     * time. Threads do not survive fork, so this is safe in the child. */
    hx_mutex_init(&g_core.sink_lock);
    hx_mutex_init(&g_core.stats_lock);
    hx_mutex_init(&g_core.init_lock);
    hx_mutex_init(&g_err_lock);
    hx_mutex_init(&g_dup.lock);
    hx_ring_after_fork_child();
    for (i = 0; i < g_core.sink_count; ++i) {
        if (g_core.sinks[i] && g_core.sinks[i]->is_file) {
            hx_sink_file_after_fork(g_core.sinks[i]);
        }
    }
#if defined(HX_CLOG_ENABLE_ASYNC)
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
        hx_async_after_fork_child(); /* re-init locks + restart the worker */
    }
#endif
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
static unsigned int format_line(const hx_clog_record_t* rec,
                                char* out,
                                unsigned int out_size,
                                const char* pattern,
                                hx_clog_format_mode_t mode,
                                hx_clog_formatter_t formatter,
                                void* formatter_user_data) {
    unsigned int n;
    if (formatter) {
        n = formatter(rec, out, out_size, formatter_user_data);
        if (out && out_size > 0) {
            if (n >= out_size) {
                n = out_size - 1;
            }
            out[n] = '\0';
        }
        return n;
    }
    if (mode == HX_CLOG_FORMAT_JSON) {
        return hx_format_json_record(rec, out, out_size);
    }
    return hx_format_record(pattern, rec, out, out_size);
}

/* Format a record with stack-first / heap-retry semantics. On return
 * *line_out/*len_out describe the formatted line; *heap_out (if non-NULL)
 * must be freed by the caller. */
static void format_with_retry(const hx_clog_record_t* rec,
                              const char* pattern,
                              hx_clog_format_mode_t mode,
                              hx_clog_formatter_t formatter,
                              void* formatter_user_data,
                              char* stackbuf, unsigned int stack_size,
                              char** heap_out,
                              const char** line_out,
                              unsigned int* len_out) {
    unsigned int len = format_line(rec, stackbuf, stack_size,
                                   pattern, mode, formatter, formatter_user_data);
    *heap_out = NULL;
    *line_out = stackbuf;
    *len_out = len;
    if (len + 1 >= stack_size) {
        /* likely truncated; retry on heap. JSON escaping can expand the
         * payload up to 6x (\u00xx), so size accordingly. */
        unsigned int mul = (mode == HX_CLOG_FORMAT_JSON && !formatter) ? 6u : 1u;
        unsigned long long ctx_len =
            rec->context ? (unsigned long long)strlen(rec->context) : 0ULL;
        unsigned long long need = (unsigned long long)rec->message_len * mul +
                                  ctx_len * mul + 2048ULL;
        unsigned int cap;
        char* hp;
        if (need > 0xFFFFFFFFULL) {
            need = 0xFFFFFFFFULL;
        }
        cap = hx_clamp_line((unsigned int)need);
        hp = (char*)hx_clog__malloc(cap);
        if (hp) {
            len = format_line(rec, hp, cap,
                              pattern, mode, formatter, formatter_user_data);
            *heap_out = hp;
            *line_out = hp;
            *len_out = len;
        }
    }
}

/* Route one formatted line to its destination (queue in async mode,
 * direct sink write otherwise). */
static void core_dispatch_line(hx_clog_level_t level, const char* line,
                               unsigned int len, hx_clog_sink_id_t target,
                               int count_stats) {
#if defined(HX_CLOG_ENABLE_ASYNC)
    if (core_get_mode() == HX_CLOG_MODE_ASYNC) {
        hx_async_enqueue(level, line, len, target, count_stats);
        return;
    }
#endif
    hx_core_emit_to_sinks(level, line, len, target, count_stats);
}

/* Snapshot of one sink's format override, taken under the sink lock so the
 * actual formatting can run without it. */
typedef struct {
    hx_clog_sink_id_t id;
    hx_clog_format_mode_t mode;
    int has_pattern;
    char pattern[HX_SINK_PATTERN_MAX];
} override_snap_t;

/* Format a record (default format + per-sink overrides), feed the crash
 * ring, and dispatch every variant. */
static void core_dispatch_record(hx_clog_record_t* rec) {
    char line_stack[HX_CLOG_STACK_BUF_SIZE + 256];
    char* line_heap = NULL;
    const char* outline;
    unsigned int line_len;
    override_snap_t ovs[HX_CLOG_MAX_SINKS];
    int ov_n = 0;
    int have_default_targets = 1;
    int counted = 0;
    int i;
    int gen;

    /* refresh the per-thread snapshot of the global format settings only
     * when they changed; the steady-state hot path takes no lock here */
    gen = hx_atomic_load_level(&g_core.format_gen);
    if (gen != g_tls_fmt.gen) {
        hx_mutex_lock(&g_core.sink_lock);
        strncpy(g_tls_fmt.pattern, g_core.pattern, sizeof(g_tls_fmt.pattern) - 1);
        g_tls_fmt.pattern[sizeof(g_tls_fmt.pattern) - 1] = '\0';
        g_tls_fmt.mode = g_core.format_mode;
        g_tls_fmt.formatter = g_core.formatter;
        g_tls_fmt.formatter_user_data = g_core.formatter_user_data;
        g_tls_fmt.gen = hx_atomic_load_level(&g_core.format_gen);
        hx_mutex_unlock(&g_core.sink_lock);
    }

    format_with_retry(rec, g_tls_fmt.pattern, g_tls_fmt.mode,
                      g_tls_fmt.formatter, g_tls_fmt.formatter_user_data,
                      line_stack, (unsigned int)sizeof(line_stack),
                      &line_heap, &outline, &line_len);

    /* feed the crash ring buffer (always, cheap) */
    hx_ring_push(outline, line_len);

    /* collect per-sink format overrides, if any */
    if (hx_atomic_load_level(&g_core.override_count) > 0) {
        int defaults = 0;
        hx_mutex_lock(&g_core.sink_lock);
        for (i = 0; i < g_core.sink_count; ++i) {
            hx_clog_sink_t* s = g_core.sinks[i];
            if (!s) {
                continue;
            }
            if (s->override_set && ov_n < HX_CLOG_MAX_SINKS) {
                ovs[ov_n].id = s->id;
                ovs[ov_n].mode = s->override_mode;
                ovs[ov_n].has_pattern = s->override_has_pattern;
                if (s->override_has_pattern) {
                    memcpy(ovs[ov_n].pattern, s->override_pattern,
                           HX_SINK_PATTERN_MAX);
                }
                ov_n++;
            } else if (!s->override_set) {
                defaults++;
            }
        }
        hx_mutex_unlock(&g_core.sink_lock);
        have_default_targets = defaults > 0;
    }

    if (have_default_targets || ov_n == 0) {
        core_dispatch_line(rec->level, outline, line_len, 0, 1);
        counted = 1;
    }
    if (line_heap) {
        hx_clog__free(line_heap);
        line_heap = NULL;
    }

    /* render and dispatch each override variant (line_stack is reusable now:
     * both the sync write and the async enqueue copy synchronously) */
    for (i = 0; i < ov_n; ++i) {
        char* oheap = NULL;
        const char* oline;
        unsigned int olen;
        format_with_retry(rec,
                          ovs[i].has_pattern ? ovs[i].pattern : g_tls_fmt.pattern,
                          ovs[i].mode, NULL, NULL,
                          line_stack, (unsigned int)sizeof(line_stack),
                          &oheap, &oline, &olen);
        core_dispatch_line(rec->level, oline, olen, ovs[i].id, counted ? 0 : 1);
        counted = 1;
        if (oheap) {
            hx_clog__free(oheap);
        }
    }

    /* FATAL is flushed immediately to maximize crash survivability */
    if (rec->level >= HX_CLOG_LEVEL_FATAL) {
        hx_clog_flush();
    }
}

/* Emit the "last message repeated N times" summary for the duplicate
 * suppressor. Runs outside g_dup.lock. */
static void dup_emit_summary(hx_clog_level_t level, const char* logger_name,
                             unsigned long long repeats) {
    char msg[64];
    char context[8];
    hx_clog_record_t rec;
    hx_timestamp_t ts;
    int n;

    if (!core_is_initialized() || repeats == 0) {
        return;
    }
    n = snprintf(msg, sizeof(msg), "last message repeated %llu times",
                 (unsigned long long)repeats);
    if (n < 0) {
        return;
    }
    memset(&rec, 0, sizeof(rec));
    rec.level = level;
    rec.logger_name = logger_name && logger_name[0]
                          ? logger_name : g_core.default_logger.name;
    rec.file = "hx_clog";
    rec.line = 0;
    rec.func = "dup";
    rec.pid = hx_get_pid();
    rec.tid = hx_get_tid();
    hx_now(&ts);
    rec.timestamp_sec = (long long)ts.sec;
    rec.timestamp_msec = ts.msec;
    rec.message = msg;
    rec.message_len = (unsigned int)n;
    context[0] = '\0';
    rec.context = context;
    core_dispatch_record(&rec);
}

static void dup_flush_pending(void) {
    unsigned long long repeats = 0;
    hx_clog_level_t level = HX_CLOG_LEVEL_INFO;
    char logger[128];
    logger[0] = '\0';
    hx_mutex_lock(&g_dup.lock);
    if (g_dup.repeats > 0) {
        repeats = g_dup.repeats;
        level = g_dup.last_level;
        strncpy(logger, g_dup.last_logger, sizeof(logger) - 1);
        logger[sizeof(logger) - 1] = '\0';
        g_dup.repeats = 0;
        g_dup.have_last = 0;
    }
    hx_mutex_unlock(&g_dup.lock);
    if (repeats > 0) {
        dup_emit_summary(level, logger, repeats);
    }
}

/* Returns 1 when the current message is a suppressed duplicate (caller must
 * drop it). May emit a pending summary for the previous run of duplicates. */
static int dup_check_and_update(hx_clog_level_t level, const char* file,
                                int line, const char* logger_name,
                                const char* msg, unsigned int msg_len) {
    int suppressed = 0;
    unsigned long long summary_repeats = 0;
    hx_clog_level_t summary_level = HX_CLOG_LEVEL_INFO;
    char summary_logger[128];
    unsigned int klen = msg_len < sizeof(g_dup.last_msg) - 1
                            ? msg_len : (unsigned int)sizeof(g_dup.last_msg) - 1;
    const char* logger = logger_name ? logger_name : "";
    hx_timestamp_t ts;
    long long now_ms;

    summary_logger[0] = '\0';

    hx_now(&ts);
    now_ms = (long long)ts.sec * 1000LL + (long long)ts.msec;

    hx_mutex_lock(&g_dup.lock);
    if (!g_dup.enabled) {
        hx_mutex_unlock(&g_dup.lock);
        return 0;
    }
    if (g_dup.have_last &&
        g_dup.last_level == level &&
        g_dup.last_line == line &&
        g_dup.last_msg_len == msg_len &&
        strncmp(g_dup.last_logger, logger, sizeof(g_dup.last_logger) - 1) == 0 &&
        strncmp(g_dup.last_file, file ? file : "",
                sizeof(g_dup.last_file) - 1) == 0 &&
        strncmp(g_dup.last_msg, msg, klen) == 0 &&
        (now_ms - g_dup.last_ms) <= (long long)g_dup.window_ms) {
        g_dup.repeats++;
        g_dup.last_ms = now_ms;
        suppressed = 1;
    } else {
        if (g_dup.repeats > 0) {
            summary_repeats = g_dup.repeats;
            summary_level = g_dup.last_level;
            /* the summary describes the previous run, so it must carry that
             * run's logger, not the new message's */
            strncpy(summary_logger, g_dup.last_logger, sizeof(summary_logger) - 1);
            summary_logger[sizeof(summary_logger) - 1] = '\0';
        }
        g_dup.repeats = 0;
        g_dup.have_last = 1;
        g_dup.last_level = level;
        g_dup.last_line = line;
        strncpy(g_dup.last_logger, logger, sizeof(g_dup.last_logger) - 1);
        g_dup.last_logger[sizeof(g_dup.last_logger) - 1] = '\0';
        strncpy(g_dup.last_file, file ? file : "", sizeof(g_dup.last_file) - 1);
        g_dup.last_file[sizeof(g_dup.last_file) - 1] = '\0';
        memcpy(g_dup.last_msg, msg, klen);
        g_dup.last_msg[klen] = '\0';
        g_dup.last_msg_len = msg_len;
        g_dup.last_ms = now_ms;
    }
    hx_mutex_unlock(&g_dup.lock);

    if (summary_repeats > 0) {
        dup_emit_summary(summary_level, summary_logger, summary_repeats);
    }
    return suppressed;
}

static void core_writev(const char* logger_name,
                        volatile int* logger_level,
                        hx_clog_level_t level,
                        const char* file, int line, const char* func,
                        const char* fmt, va_list args) {
    char msg_stack[HX_CLOG_STACK_BUF_SIZE];
    char* msg = msg_stack;
    char* msg_heap = NULL;
    char context[1024];
    int n;
    unsigned int msg_len;
    hx_clog_record_t rec;
    hx_timestamp_t ts;
    va_list args_copy;

    /* fast level filter before any work */
    if (!core_is_initialized()) {
        return;
    }
    if ((int)level < hx_atomic_load_level(&g_core.level) ||
        (logger_level && (int)level < hx_atomic_load_level(logger_level)) ||
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
        unsigned int need = hx_clamp_line((unsigned int)n + 1);
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

    /* fold consecutive duplicates when suppression is enabled */
    if (g_dup.enabled &&
        dup_check_and_update(level, file, line, logger_name, msg, msg_len)) {
        if (msg_heap) {
            hx_clog__free(msg_heap);
        }
        return;
    }

    memset(&rec, 0, sizeof(rec));
    rec.level = level;
    rec.logger_name = logger_name && logger_name[0] ? logger_name : g_core.default_logger.name;
    rec.file = file;
    rec.line = line;
    rec.func = func;
    rec.pid = hx_get_pid();
    rec.tid = hx_get_tid();
    hx_now(&ts);
    rec.timestamp_sec = (long long)ts.sec;
    rec.timestamp_msec = ts.msec;
    rec.message = msg;
    rec.message_len = msg_len;
    hx_context_snapshot_text(context, sizeof(context));
    rec.context = context;

    core_dispatch_record(&rec);

    if (msg_heap) {
        hx_clog__free(msg_heap);
    }
}

void hx_clog_writev(hx_clog_level_t level,
                    const char* file, int line, const char* func,
                    const char* fmt, va_list args) {
    core_writev(g_core.default_logger.name, &g_core.default_logger.level,
                level, file, line, func, fmt, args);
}

void hx_clog_write(hx_clog_level_t level,
                   const char* file, int line, const char* func,
                   const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    hx_clog_writev(level, file, line, func, fmt, args);
    va_end(args);
}

void hx_clog_writev_named(hx_clog_level_t level,
                          const char* logger_name,
                          const char* file,
                          int line,
                          const char* func,
                          const char* fmt,
                          va_list args) {
    core_writev(logger_name, NULL, level, file, line, func, fmt, args);
}

void hx_clog_write_named(hx_clog_level_t level,
                         const char* logger_name,
                         const char* file,
                         int line,
                         const char* func,
                         const char* fmt,
                         ...) {
    va_list args;
    va_start(args, fmt);
    hx_clog_writev_named(level, logger_name, file, line, func, fmt, args);
    va_end(args);
}

hx_clog_logger_t* hx_clog_default_logger(void) {
    ensure_once();
    return &g_core.default_logger;
}

int hx_clog_logger_create(const char* name,
                          hx_clog_level_t level,
                          hx_clog_logger_t** out_logger) {
    hx_clog_logger_t* logger;
    if (!out_logger) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    if (level < HX_CLOG_LEVEL_TRACE || level > HX_CLOG_LEVEL_OFF) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    logger = (hx_clog_logger_t*)hx_clog__malloc(sizeof(*logger));
    if (!logger) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    memset(logger, 0, sizeof(*logger));
    strncpy(logger->name, name && name[0] ? name : "hx_clog",
            sizeof(logger->name) - 1);
    logger->level = level;
    logger->is_default = 0;
    *out_logger = logger;
    return HX_CLOG_OK;
}

void hx_clog_logger_destroy(hx_clog_logger_t* logger) {
    if (logger && !logger->is_default) {
        hx_clog__free(logger);
    }
}

int hx_clog_logger_set_level(hx_clog_logger_t* logger, hx_clog_level_t level) {
    if (!logger || level < HX_CLOG_LEVEL_TRACE || level > HX_CLOG_LEVEL_OFF) {
        return HX_CLOG_ERR_INVALID_ARGUMENT;
    }
    hx_atomic_store_level(&logger->level, (int)level);
    if (logger->is_default) {
        hx_atomic_store_level(&g_core.level, (int)level);
    }
    return HX_CLOG_OK;
}

hx_clog_level_t hx_clog_logger_get_level(const hx_clog_logger_t* logger) {
    if (!logger) {
        return HX_CLOG_LEVEL_OFF;
    }
    return (hx_clog_level_t)hx_atomic_load_level((volatile int*)&logger->level);
}

const char* hx_clog_logger_name(const hx_clog_logger_t* logger) {
    return logger ? logger->name : "";
}

void hx_clog_logger_writev(hx_clog_logger_t* logger,
                           hx_clog_level_t level,
                           const char* file,
                           int line,
                           const char* func,
                           const char* fmt,
                           va_list args) {
    hx_clog_logger_t* actual = logger ? logger : &g_core.default_logger;
    core_writev(actual->name, &actual->level, level, file, line, func, fmt, args);
}

void hx_clog_logger_write(hx_clog_logger_t* logger,
                          hx_clog_level_t level,
                          const char* file,
                          int line,
                          const char* func,
                          const char* fmt,
                          ...) {
    va_list args;
    va_start(args, fmt);
    hx_clog_logger_writev(logger, level, file, line, func, fmt, args);
    va_end(args);
}
