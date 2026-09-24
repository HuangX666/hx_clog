/*
 * hx_clog - asynchronous logging engine.
 *
 * Decouples producers (business threads calling HX_LOG_*) from the sinks: a
 * bounded ring queue of pre-allocated slots plus ONE background worker that
 * pops lines in batches and hands them to the synchronous emit path
 * (hx_core_emit_to_sinks, in hx_clog_core.c).
 *
 *   - producers: hx_async_enqueue (level filtering already happened
 *     upstream). Overflow behaviour is selectable per configuration: BLOCK
 *     (backpressure, lossless), DROP_NEW (cheap, keeps old lines) or
 *     DROP_OLD (keeps the freshest lines).
 *   - worker: batches up to batch_size lines into a reused scratch buffer,
 *     releases the lock, then writes; flushes all sinks every
 *     flush_interval_ms; drains the queue and exits on stop. Drop counters
 *     and a high-watermark feed the statistics API.
 *   - slot buffers are kept and reused across lines so the steady state is
 *     allocation-free; a buffer that ballooned for one huge line is shrunk
 *     back on the next reuse (HX_ASYNC_SLOT_KEEP_CAP) so a single outlier
 *     cannot pin memory forever.
 *
 * Built only when HX_CLOG_ENABLE_ASYNC is defined.
 *
 * Locking note: the queue mutex and condition variables are created once and
 * never destroyed. Destroying them on stop would race with producers that
 * passed the "running" check moments before shutdown (destroying a mutex
 * another thread still holds is undefined behaviour). All queue state,
 * including the running/stop flags, is only read or written under the lock.
 *
 * Copyright (c) 2026 HuangX
 * SPDX-License-Identifier: MIT
 */
#include "hx_clog_internal.h"

#if defined(HX_CLOG_ENABLE_ASYNC)

/* A slot's buffer is kept and reused across lines so the steady state is
 * allocation-free; buffers that ballooned for one huge line are shrunk back
 * on the next reuse so a single outlier cannot pin memory forever. */
#define HX_ASYNC_SLOT_KEEP_CAP (64u * 1024u)

/* Report queue drops to the error handler on the first drop and then once
 * every this many drops, so a drop storm cannot flood the handler. */
#define HX_ASYNC_DROP_REPORT_EVERY 10000ULL

typedef struct {
    hx_clog_level_t level;
    hx_clog_sink_id_t target;   /* 0 = all non-override sinks */
    unsigned char   count_stats;
    unsigned int    len;
    unsigned int    cap;   /* allocated bytes of data */
    char*           data;  /* heap, NULL until first use */
} async_slot_t;

typedef struct {
    async_slot_t* slots;
    unsigned int  capacity;       /* number of slots */
    unsigned int  head;           /* pop index */
    unsigned int  tail;           /* push index */
    unsigned int  count;          /* queued items */

    unsigned int  batch_size;
    unsigned int  flush_interval_ms;
    hx_clog_overflow_policy_t overflow;

    int        running;
    int        stop;
    int        flush_request;
    int        worker_exited;   /* set by the worker on its way out (POSIX
                                * liveness check; Windows also probes the
                                * thread handle) */

    hx_thread_t worker;

    char*        scratch;      /* worker-owned, grows on demand */
    unsigned int scratch_cap;

    unsigned long long dropped;
    unsigned long long high_watermark;
} async_engine_t;

static async_engine_t g_async;

/* Synchronization primitives live outside the engine struct: created once on
 * first start, re-initialized only after fork, never destroyed. */
static hx_mutex_t g_async_lock;
static hx_cond_t  g_async_not_empty;
static hx_cond_t  g_async_not_full;
static hx_cond_t  g_async_flushed;
static volatile int g_async_prims_ready = 0;

static void worker_main(void* arg);

/* Count a drop under the lock; returns 1 when the caller should notify the
 * error handler (after releasing the lock — never call out while holding it,
 * the unlock window would let other producers invalidate queue invariants
 * mid-operation). */
static int async_count_drop_locked(void) {
    g_async.dropped++;
    return (g_async.dropped == 1 ||
            (g_async.dropped % HX_ASYNC_DROP_REPORT_EVERY) == 0);
}

static void async_report_drops(void) {
    hx_core_report_error(HX_CLOG_ERR_QUEUE_FULL,
                         "async queue overflow: lines are being dropped");
}

int hx_async_start(const hx_clog_config_t* cfg) {
    unsigned int cap = cfg->async_queue_size ? cfg->async_queue_size : 8192;
    async_slot_t* slots;

    if (!g_async_prims_ready) {
        hx_mutex_init(&g_async_lock);
        hx_cond_init(&g_async_not_empty);
        hx_cond_init(&g_async_not_full);
        hx_cond_init(&g_async_flushed);
        g_async_prims_ready = 1;
    }

    slots = (async_slot_t*)hx_clog__malloc(sizeof(async_slot_t) * cap);
    if (!slots) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    memset(slots, 0, sizeof(async_slot_t) * cap); /* data=NULL, cap=0 */

    hx_mutex_lock(&g_async_lock);
    memset(&g_async, 0, sizeof(g_async));
    g_async.slots = slots;
    g_async.capacity = cap;
    g_async.batch_size = cfg->async_batch_size ? cfg->async_batch_size : 64;
    g_async.flush_interval_ms = cfg->flush_interval_ms ? cfg->flush_interval_ms : 1000;
    g_async.overflow = cfg->overflow_policy;
    g_async.running = 1;
    g_async.stop = 0;
    hx_mutex_unlock(&g_async_lock);

    if (hx_thread_create(&g_async.worker, worker_main, NULL) != HX_CLOG_OK) {
        hx_mutex_lock(&g_async_lock);
        g_async.running = 0;
        g_async.slots = NULL;
        hx_mutex_unlock(&g_async_lock);
        hx_clog__free(slots);
        return HX_CLOG_ERR_THREAD_FAILED;
    }
    return HX_CLOG_OK;
}

int hx_async_enqueue(hx_clog_level_t level, const char* data, unsigned int size,
                     hx_clog_sink_id_t target_sink_id, int count_stats) {
    async_slot_t* slot;
    int report = 0;

    if (!g_async_prims_ready) {
        return HX_CLOG_ERR_NOT_INITIALIZED;
    }

    hx_mutex_lock(&g_async_lock);

    if (!g_async.running || g_async.stop) {
        hx_mutex_unlock(&g_async_lock);
        return HX_CLOG_ERR_NOT_INITIALIZED;
    }

    if (g_async.count >= g_async.capacity) {
        switch (g_async.overflow) {
            case HX_CLOG_OVERFLOW_DROP_NEW:
                report = async_count_drop_locked();
                hx_mutex_unlock(&g_async_lock);
                if (report) {
                    async_report_drops();
                }
                return HX_CLOG_ERR_QUEUE_FULL;
            case HX_CLOG_OVERFLOW_DROP_OLD:
                /* drop oldest: advance head */
                g_async.head = (g_async.head + 1) % g_async.capacity;
                g_async.count--;
                report = async_count_drop_locked();
                break;
            case HX_CLOG_OVERFLOW_BLOCK:
            default:
                while (g_async.count >= g_async.capacity && !g_async.stop) {
                    hx_cond_wait(&g_async_not_full, &g_async_lock);
                }
                if (g_async.stop || !g_async.running) {
                    /* the engine is being torn down (e.g. mid-reconfigure)
                     * while we were blocked. Report "engine unavailable" so the
                     * caller falls back to a synchronous write instead of
                     * silently losing the line — BLOCK must never drop. */
                    hx_mutex_unlock(&g_async_lock);
                    return HX_CLOG_ERR_NOT_INITIALIZED;
                }
                break;
        }
    }

    slot = &g_async.slots[g_async.tail];
    /* shrink a buffer that ballooned for one oversized line */
    if (slot->cap > HX_ASYNC_SLOT_KEEP_CAP && size + 1 <= HX_ASYNC_SLOT_KEEP_CAP) {
        hx_clog__free(slot->data);
        slot->data = NULL;
        slot->cap = 0;
    }
    if (slot->cap < size + 1) {
        unsigned int newcap = slot->cap ? slot->cap : 256;
        char* nb;
        while (newcap < size + 1) {
            newcap *= 2;
        }
        nb = (char*)hx_clog__malloc(newcap);
        if (!nb) {
            /* cannot grow: drop this line rather than corrupt the queue */
            report = async_count_drop_locked();
            hx_mutex_unlock(&g_async_lock);
            if (report) {
                async_report_drops();
            }
            return HX_CLOG_ERR_OUT_OF_MEMORY;
        }
        if (slot->data) {
            hx_clog__free(slot->data);
        }
        slot->data = nb;
        slot->cap = newcap;
    }
    slot->level = level;
    slot->target = target_sink_id;
    slot->count_stats = (unsigned char)(count_stats ? 1 : 0);
    slot->len = size;
    memcpy(slot->data, data, size);

    g_async.tail = (g_async.tail + 1) % g_async.capacity;
    g_async.count++;
    if (g_async.count > g_async.high_watermark) {
        g_async.high_watermark = g_async.count;
    }

    hx_cond_signal(&g_async_not_empty);
    hx_mutex_unlock(&g_async_lock);
    if (report) {
        async_report_drops();
    }
    return HX_CLOG_OK;
}

static void worker_main(void* arg) {
    (void)arg;
    for (;;) {
        hx_clog_level_t emit_level;
        hx_clog_sink_id_t emit_target;
        int emit_count;
        unsigned int emit_len;

        hx_mutex_lock(&g_async_lock);
        while (g_async.count == 0 && !g_async.stop && !g_async.flush_request) {
            if (!hx_cond_wait_ms(&g_async_not_empty, &g_async_lock,
                                 g_async.flush_interval_ms)) {
                /* periodic flush tick */
                break;
            }
        }

        if (g_async.count == 0) {
            int should_exit = g_async.stop;
            int do_flush = g_async.flush_request;
            g_async.flush_request = 0;
            hx_mutex_unlock(&g_async_lock);

            /* periodic / requested flush of sinks */
            hx_core_flush_sinks();
            if (do_flush) {
                hx_mutex_lock(&g_async_lock);
                hx_cond_broadcast(&g_async_flushed);
                hx_mutex_unlock(&g_async_lock);
            }
            if (should_exit) {
                break;
            }
            continue;
        }

        {
            unsigned int written = 0;
            while (g_async.count > 0 && written < g_async.batch_size) {
                async_slot_t* s = &g_async.slots[g_async.head];

                /* copy the line into the worker scratch buffer so the queue
                 * lock can be released during the (potentially slow) sink IO */
                if (g_async.scratch_cap < s->len + 1) {
                    unsigned int nc = g_async.scratch_cap ? g_async.scratch_cap : 256;
                    char* nb;
                    while (nc < s->len + 1) {
                        nc *= 2;
                    }
                    nb = (char*)hx_clog__malloc(nc);
                    if (!nb) {
                        /* cannot grow scratch: skip this line to stay alive
                         * (drop reporting is skipped here; the producers
                         * already report drops, and this path only occurs
                         * under severe memory pressure) */
                        g_async.head = (g_async.head + 1) % g_async.capacity;
                        g_async.count--;
                        g_async.dropped++;
                        hx_cond_signal(&g_async_not_full);
                        written++;
                        continue;
                    }
                    if (g_async.scratch) {
                        hx_clog__free(g_async.scratch);
                    }
                    g_async.scratch = nb;
                    g_async.scratch_cap = nc;
                }
                emit_level = s->level;
                emit_target = s->target;
                emit_count = s->count_stats;
                emit_len = s->len;
                memcpy(g_async.scratch, s->data, s->len);

                g_async.head = (g_async.head + 1) % g_async.capacity;
                g_async.count--;
                hx_cond_signal(&g_async_not_full);

                hx_mutex_unlock(&g_async_lock);
                hx_core_emit_to_sinks(emit_level, g_async.scratch, emit_len,
                                      emit_target, emit_count);
                hx_mutex_lock(&g_async_lock);
                written++;
            }

            if (g_async.flush_request && g_async.count == 0) {
                g_async.flush_request = 0;
                hx_cond_broadcast(&g_async_flushed);
            }
        }
        hx_mutex_unlock(&g_async_lock);
    }

    /* mark the exit so flush waiters stop waiting for a drained queue that
     * will never drain (POSIX liveness check) */
    hx_mutex_lock(&g_async_lock);
    g_async.worker_exited = 1;
    hx_mutex_unlock(&g_async_lock);
}

/* Caller holds g_async_lock. A drained-by-worker outcome requires a live
 * worker. On Windows a terminated thread's handle is signaled, so this also
 * detects a worker killed by ExitProcess before DLL_PROCESS_DETACH — exactly
 * the case where waiting forever would hang the process under the loader
 * lock. */
static int worker_alive_locked(void) {
    if (!g_async.running || g_async.worker_exited) {
        return 0;
    }
#if defined(HX_PLATFORM_WINDOWS)
    if (g_async.worker != NULL &&
        WaitForSingleObject((HANDLE)g_async.worker, 0) == WAIT_OBJECT_0) {
        return 0;
    }
#endif
    return 1;
}

static int async_flush_internal(unsigned int timeout_ms) {
    long long deadline = 0;
    int rc = 0;

    if (!g_async_prims_ready) {
        return 0;
    }
    if (timeout_ms != 0) {
        deadline = hx_monotonic_ms() + (long long)timeout_ms;
    }
    hx_mutex_lock(&g_async_lock);
    if (!g_async.running || g_async.count == 0) {
        hx_mutex_unlock(&g_async_lock);
        return 0;
    }
    if (!worker_alive_locked()) {
        /* nobody will ever drain the queue; do not wait at all */
        hx_mutex_unlock(&g_async_lock);
        return -1;
    }
    g_async.flush_request = 1;
    hx_cond_signal(&g_async_not_empty);
    while (g_async.flush_request && g_async.running && !g_async.stop) {
        if (!worker_alive_locked()) {
            rc = -1;
            break;
        }
        hx_cond_wait_ms(&g_async_flushed, &g_async_lock, 50);
        if (g_async.count == 0) {
            break;
        }
        if (timeout_ms != 0 && hx_monotonic_ms() >= deadline) {
            rc = -1;
            break;
        }
    }
    g_async.flush_request = 0;
    hx_mutex_unlock(&g_async_lock);
    return rc;
}

void hx_async_flush(void) {
    (void)async_flush_internal(0);
}

int hx_async_flush_bounded(unsigned int timeout_ms) {
    return async_flush_internal(timeout_ms);
}

void hx_async_stop(void) {
    async_slot_t* slots;
    unsigned int capacity;
    char* scratch;
    hx_thread_t worker;

    if (!g_async_prims_ready) {
        return;
    }
    hx_mutex_lock(&g_async_lock);
    if (!g_async.running) {
        hx_mutex_unlock(&g_async_lock);
        return;
    }
    g_async.stop = 1;
    worker = g_async.worker;
    hx_cond_broadcast(&g_async_not_empty);
    hx_cond_broadcast(&g_async_not_full);
    hx_mutex_unlock(&g_async_lock);

    hx_thread_join(worker); /* worker drains the queue before exiting */

    /* detach the buffers under the lock, free them outside; producers that
     * raced past the running check see stop/running under the lock and back
     * off before ever touching the slots */
    hx_mutex_lock(&g_async_lock);
    slots = g_async.slots;
    capacity = g_async.capacity;
    scratch = g_async.scratch;
    g_async.slots = NULL;
    g_async.scratch = NULL;
    g_async.scratch_cap = 0;
    g_async.count = 0;
    g_async.head = 0;
    g_async.tail = 0;
    g_async.running = 0;
    hx_mutex_unlock(&g_async_lock);

    if (slots) {
        unsigned int i;
        for (i = 0; i < capacity; ++i) {
            if (slots[i].data) {
                hx_clog__free(slots[i].data);
            }
        }
        hx_clog__free(slots);
    }
    if (scratch) {
        hx_clog__free(scratch);
    }
}

void hx_async_after_fork_child(void) {
    if (!g_async_prims_ready) {
        return;
    }
    /* the parent's worker thread does not exist in the child; locks may have
     * been held at fork time. Re-create the primitives, drop whatever the
     * parent had queued (it still owns those lines), and restart a worker. */
    hx_mutex_init(&g_async_lock);
    hx_cond_init(&g_async_not_empty);
    hx_cond_init(&g_async_not_full);
    hx_cond_init(&g_async_flushed);
    g_async.head = 0;
    g_async.tail = 0;
    g_async.count = 0;
    g_async.flush_request = 0;
    g_async.stop = 0;
    g_async.worker_exited = 0; /* a fresh worker is started below */
    if (g_async.running) {
        if (hx_thread_create(&g_async.worker, worker_main, NULL) != HX_CLOG_OK) {
            g_async.running = 0;
            hx_core_report_error(HX_CLOG_ERR_THREAD_FAILED,
                                 "async worker restart after fork failed");
        }
    }
}

void hx_async_atfork_lock(void) {
    if (g_async_prims_ready) {
        hx_mutex_lock(&g_async_lock);
    }
}
void hx_async_atfork_unlock(void) {
    if (g_async_prims_ready) {
        hx_mutex_unlock(&g_async_lock);
    }
}

unsigned long long hx_async_dropped(void) {
    unsigned long long v;
    if (!g_async_prims_ready) {
        return 0;
    }
    /* dropped/high_watermark are written under g_async_lock by the producers
     * and the worker; read them under the same lock so a concurrent update
     * cannot tear the value (and to stay free of C11 data-race UB). */
    hx_mutex_lock(&g_async_lock);
    v = g_async.dropped;
    hx_mutex_unlock(&g_async_lock);
    return v;
}
unsigned long long hx_async_high_watermark(void) {
    unsigned long long v;
    if (!g_async_prims_ready) {
        return 0;
    }
    hx_mutex_lock(&g_async_lock);
    v = g_async.high_watermark;
    hx_mutex_unlock(&g_async_lock);
    return v;
}

#endif /* HX_CLOG_ENABLE_ASYNC */
