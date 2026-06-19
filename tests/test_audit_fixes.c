/* hx_clog regression tests for the 1.2.0 release-readiness audit findings.
 *
 * Each test pins a specific fixed defect so it cannot silently regress:
 *   S-03  hx_clog_get_stats() before init must not crash.
 *   M-04  putting the same over-long context key repeatedly must be idempotent
 *         and not exhaust the 16 context slots.
 *   S-01  rotation cleanup must never touch a non-rotation file that merely
 *         shares the active log's stem and extension.
 *   S-02  async + BLOCK must not lose lines across a concurrent reconfigure;
 *         every attempted write is delivered (no silent, uncounted drops).
 *
 * Driven entirely through the public API, per the project's testing rules. */
#include "hx_clog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <windows.h>
#  include <io.h>
#else
#  include <pthread.h>
#  include <unistd.h>
#endif

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", #cond, __LINE__); \
        return 1; \
    } \
} while (0)

/* ---- small cross-platform helpers ---- */

static long atomic_add(volatile long* p, long v) {
#if defined(_WIN32)
    return InterlockedExchangeAdd(p, v) + v;
#else
    return __sync_add_and_fetch(p, v);
#endif
}

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

/* ---- shared capture sink ---- */

static volatile long g_delivered;
static unsigned int g_last_size;

static int count_cb(hx_clog_level_t level, const char* data,
                    unsigned int size, void* user) {
    (void)level; (void)data; (void)size; (void)user;
    atomic_add(&g_delivered, 1);
    return 0;
}

static int size_cb(hx_clog_level_t level, const char* data,
                   unsigned int size, void* user) {
    (void)level; (void)data; (void)user;
    g_last_size = size;
    return 0;
}

/* ============================ S-03 ============================ */
/* get_stats before any init must return cleanly, not access-violate. This must
 * run before any hx_clog_init() in this process to exercise the cold path. */
static int test_get_stats_before_init(void) {
    hx_clog_stats_t st;
    memset(&st, 0xAB, sizeof(st));
    CHECK(hx_clog_get_stats(&st) == HX_CLOG_OK);
    CHECK(st.written_lines == 0);
    CHECK(st.dropped_lines == 0);
    CHECK(st.queue_high_watermark == 0);
    /* NULL argument is still rejected */
    CHECK(hx_clog_get_stats(NULL) == HX_CLOG_ERR_INVALID_ARGUMENT);
    printf("ok: get_stats before init\n");
    return 0;
}

/* ============================ M-04 ============================ */
/* The same over-long (>63 byte) context key must map to one slot no matter how
 * many times it is put, and must remain removable. */
static int test_long_context_key(void) {
    /* 95 'k' chars: longer than HX_CONTEXT_KEY_MAX-1 (63) */
    char key[96];
    int i;
    memset(key, 'k', sizeof(key) - 1);
    key[sizeof(key) - 1] = '\0';

    /* well over the 16-slot capacity: with the bug this returned an error once
     * the slots filled up; with the fix every put reuses the same slot */
    for (i = 0; i < 50; ++i) {
        CHECK(hx_clog_context_put(key, "v") == HX_CLOG_OK);
    }
    /* the key must still be findable for removal */
    hx_clog_context_remove(key);
    /* and re-addable afterwards (slots were not leaked) */
    CHECK(hx_clog_context_put(key, "v2") == HX_CLOG_OK);
    hx_clog_context_clear();
    printf("ok: long context key idempotent\n");
    return 0;
}

/* ============================ S-01 ============================ */
/* A user file "audit.notes.log" sharing the active log's stem ("audit") and
 * extension ("log") must survive size rotation + cleanup. */
static int test_rotation_keeps_unrelated_file(void) {
    hx_clog_config_t cfg;
    const char* dir = "test_audit_logs";
    const char* sentinel = "test_audit_logs/audit.notes.log";
    FILE* f;
    int i;

    /* create the directory by initializing a file sink there first, then drop
     * the sentinel beside the active log */
    hx_clog_config_default(&cfg);
    cfg.logger_name = "audit";
    cfg.log_dir = dir;
    cfg.file_name = "audit.log";
    cfg.enable_console = 0;
    cfg.enable_file = 1;
    cfg.mode = HX_CLOG_MODE_SYNC;
    cfg.rotate_policy = HX_CLOG_ROTATE_BY_SIZE;
    cfg.max_file_size = 1024;       /* tiny: rotate often */
    cfg.max_backup_files = 2;       /* force cleanup of older backups */
    cfg.max_compressed_files = 1;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);

    f = fopen(sentinel, "wb");
    CHECK(f != NULL);
    fputs("this is a hand-written note, not a rotation backup\n", f);
    fclose(f);
    CHECK(file_exists(sentinel));

    /* generate enough volume to rotate many times and trigger cleanup */
    for (i = 0; i < 2000; ++i) {
        HX_LOG_INFO("rotation filler line number %d padding padding padding", i);
    }
    hx_clog_flush();
    hx_clog_shutdown();

    CHECK(file_exists(sentinel));   /* the non-rotation file must remain */
    printf("ok: rotation keeps unrelated file\n");
    return 0;
}

/* ============================ S-02 ============================ */
typedef struct {
    int lines;
} producer_arg_t;

static void producer_body(producer_arg_t* a) {
    int i;
    for (i = 0; i < a->lines; ++i) {
        HX_LOG_INFO("concurrent line %d", i);
    }
}

#if defined(_WIN32)
static DWORD WINAPI producer_thread(LPVOID p) {
    producer_body((producer_arg_t*)p);
    return 0;
}
#else
static void* producer_thread(void* p) {
    producer_body((producer_arg_t*)p);
    return NULL;
}
#endif

static int test_async_reconfigure_no_loss(void) {
    hx_clog_config_t cfg;
    const int producers = 4;
    const int per_producer = 8000;
    const long attempted = (long)producers * per_producer;
    producer_arg_t arg;
    int i;
#if defined(_WIN32)
    HANDLE th[8];
#else
    pthread_t th[8];
#endif

    g_delivered = 0;
    arg.lines = per_producer;

    hx_clog_config_default(&cfg);
    cfg.logger_name = "async";
    cfg.enable_console = 0;
    cfg.enable_file = 0;            /* only the callback sink, one delivery/line */
    cfg.mode = HX_CLOG_MODE_ASYNC;
    cfg.overflow_policy = HX_CLOG_OVERFLOW_BLOCK; /* must never drop */
    cfg.async_queue_size = 1024;   /* small queue -> lots of backpressure */
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    /* callback sinks must be added after init; they survive reconfigure */
    CHECK(hx_clog_add_callback_sink(count_cb, NULL) == HX_CLOG_OK);

    for (i = 0; i < producers; ++i) {
#if defined(_WIN32)
        th[i] = CreateThread(NULL, 0, producer_thread, &arg, 0, NULL);
        CHECK(th[i] != NULL);
#else
        CHECK(pthread_create(&th[i], NULL, producer_thread, &arg) == 0);
#endif
    }

    /* hammer reconfigure while producers write; the callback sink is preserved
     * across each call, so every delivered line still reaches count_cb */
    for (i = 0; i < 40; ++i) {
        hx_clog_config_t rc;
        hx_clog_config_default(&rc);
        rc.logger_name = "async";
        rc.enable_console = 0;
        rc.enable_file = 0;
        rc.mode = HX_CLOG_MODE_ASYNC;
        rc.overflow_policy = HX_CLOG_OVERFLOW_BLOCK;
        rc.async_queue_size = 1024;
        CHECK(hx_clog_reconfigure(&rc) == HX_CLOG_OK);
    }

    for (i = 0; i < producers; ++i) {
#if defined(_WIN32)
        WaitForSingleObject(th[i], INFINITE);
        CloseHandle(th[i]);
#else
        pthread_join(th[i], NULL);
#endif
    }

    hx_clog_flush();   /* drain the async queue */
    hx_clog_shutdown();

    printf("info: attempted=%ld delivered=%ld\n", attempted, (long)g_delivered);
    CHECK(g_delivered == attempted);   /* BLOCK => no loss across reconfigure */
    printf("ok: async reconfigure no loss\n");
    return 0;
}

/* ============================ M-05 ============================ */
/* A pattern that repeats %v many times must expand fully (up to the line cap),
 * not truncate at the old message_len-based heuristic estimate. */
static int test_repeated_placeholder_not_truncated(void) {
    hx_clog_config_t cfg;
    char pattern[256];
    char msg[1001];
    int i;
    const int repeats = 50;
    const int msg_len = 1000;

    for (i = 0; i < repeats; ++i) {
        memcpy(pattern + i * 2, "%v", 2);
    }
    pattern[repeats * 2] = '\0';
    memset(msg, 'A', msg_len);
    msg[msg_len] = '\0';

    g_last_size = 0;
    hx_clog_config_default(&cfg);
    cfg.logger_name = "fmt";
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.mode = HX_CLOG_MODE_SYNC;
    cfg.pattern = pattern;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_callback_sink(size_cb, NULL) == HX_CLOG_OK);

    HX_LOG_INFO("%s", msg);
    hx_clog_flush();
    hx_clog_shutdown();

    /* fully expanded ~= repeats*msg_len = 50000 bytes. The old heuristic
     * (message_len + 2048) truncated this to ~3 KB. Anything well above that
     * proves the line was not prematurely cut. */
    printf("info: expanded line size=%u (expected ~%d)\n",
           g_last_size, repeats * msg_len);
    CHECK(g_last_size > (unsigned int)(repeats * msg_len - 1000));
    printf("ok: repeated placeholder not truncated\n");
    return 0;
}

/* ============================ M-02 ============================ */
/* A user callback runs under the sink lock; re-entering the public API from it
 * must not self-deadlock. On POSIX this required making sink_lock recursive
 * (Windows CRITICAL_SECTION already is). If this regresses the test hangs. */
static volatile int g_reentry_ok;

static int reentrant_cb(hx_clog_level_t level, const char* data,
                        unsigned int size, void* user) {
    unsigned int n = 0;
    (void)level; (void)data; (void)size; (void)user;
    /* re-enter an API that also takes the sink lock */
    if (hx_clog_get_sink_count(&n) == HX_CLOG_OK) {
        g_reentry_ok = 1;
    }
    return 0;
}

static int test_callback_reentry(void) {
    hx_clog_config_t cfg;
    g_reentry_ok = 0;
    hx_clog_config_default(&cfg);
    cfg.logger_name = "reentry";
    cfg.enable_console = 0;
    cfg.enable_file = 0;
    cfg.mode = HX_CLOG_MODE_SYNC;
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);
    CHECK(hx_clog_add_callback_sink(reentrant_cb, NULL) == HX_CLOG_OK);

    HX_LOG_INFO("trigger reentry");   /* would deadlock on POSIX before the fix */
    hx_clog_flush();
    hx_clog_shutdown();

    CHECK(g_reentry_ok == 1);
    printf("ok: callback reentry no deadlock\n");
    return 0;
}

int main(void) {
    /* S-03 first: must precede any init */
    if (test_get_stats_before_init()) return 1;
    if (test_long_context_key()) return 1;
    if (test_rotation_keeps_unrelated_file()) return 1;
    if (test_repeated_placeholder_not_truncated()) return 1;
    if (test_callback_reentry()) return 1;
    if (test_async_reconfigure_no_loss()) return 1;
    printf("ALL PASS\n");
    return 0;
}
