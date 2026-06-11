/*
 * hx_clog - asynchronous logging engine.
 *
 * A bounded ring queue of pre-allocated slots plus one background worker that
 * pops batches and writes them to the sinks. Overflow behaviour is selectable
 * (BLOCK / DROP_NEW / DROP_OLD).
 *
 * Built only when HX_CLOG_ENABLE_ASYNC is defined.
 */
#include "hx_clog_internal.h"

#if defined(HX_CLOG_ENABLE_ASYNC)

/*
 * A queue slot holds a heap buffer that grows on demand and is reused across
 * the slot's lifetime, so the steady state is allocation-free for typical line
 * sizes while still supporting very large lines (up to HX_CLOG_MAX_LINE, or
 * unbounded when HX_CLOG_UNLIMITED_LINE is defined).
 */
typedef struct {
    hx_clog_level_t level;
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

    hx_mutex_t lock;
    hx_cond_t  not_empty;
    hx_cond_t  not_full;
    hx_cond_t  flushed;           /* signalled when queue drained on demand */

    int        running;
    int        stop;
    int        flush_request;

    hx_thread_t worker;

    char*        scratch;      /* worker-owned, grows on demand */
    unsigned int scratch_cap;

    unsigned long long dropped;
    unsigned long long high_watermark;
} async_engine_t;

static async_engine_t g_async;

static void worker_main(void* arg);

int hx_async_start(const hx_clog_config_t* cfg) {
    unsigned int cap = cfg->async_queue_size ? cfg->async_queue_size : 8192;

    memset(&g_async, 0, sizeof(g_async));
    g_async.capacity = cap;
    g_async.batch_size = cfg->async_batch_size ? cfg->async_batch_size : 64;
    g_async.flush_interval_ms = cfg->flush_interval_ms ? cfg->flush_interval_ms : 1000;
    g_async.overflow = cfg->overflow_policy;

    g_async.slots = (async_slot_t*)hx_clog__malloc(sizeof(async_slot_t) * cap);
    if (!g_async.slots) {
        return HX_CLOG_ERR_OUT_OF_MEMORY;
    }
    memset(g_async.slots, 0, sizeof(async_slot_t) * cap); /* data=NULL, cap=0 */

    hx_mutex_init(&g_async.lock);
    hx_cond_init(&g_async.not_empty);
    hx_cond_init(&g_async.not_full);
    hx_cond_init(&g_async.flushed);

    g_async.running = 1;
    g_async.stop = 0;

    if (hx_thread_create(&g_async.worker, worker_main, NULL) != HX_CLOG_OK) {
        g_async.running = 0;
        hx_clog__free(g_async.slots);
        g_async.slots = NULL;
        hx_mutex_destroy(&g_async.lock);
        hx_cond_destroy(&g_async.not_empty);
        hx_cond_destroy(&g_async.not_full);
        hx_cond_destroy(&g_async.flushed);
        return HX_CLOG_ERR_THREAD_FAILED;
    }
    return HX_CLOG_OK;
}

int hx_async_enqueue(hx_clog_level_t level, const char* data, unsigned int size) {
    async_slot_t* slot;

    if (!g_async.running) {
        return HX_CLOG_ERR_NOT_INITIALIZED;
    }

    hx_mutex_lock(&g_async.lock);

    if (g_async.count >= g_async.capacity) {
        switch (g_async.overflow) {
            case HX_CLOG_OVERFLOW_DROP_NEW:
                g_async.dropped++;
                hx_mutex_unlock(&g_async.lock);
                return HX_CLOG_ERR_QUEUE_FULL;
            case HX_CLOG_OVERFLOW_DROP_OLD:
                /* drop oldest: advance head */
                g_async.head = (g_async.head + 1) % g_async.capacity;
                g_async.count--;
                g_async.dropped++;
                break;
            case HX_CLOG_OVERFLOW_BLOCK:
            default:
                while (g_async.count >= g_async.capacity && !g_async.stop) {
                    hx_cond_wait(&g_async.not_full, &g_async.lock);
                }
                if (g_async.stop) {
                    hx_mutex_unlock(&g_async.lock);
                    return HX_CLOG_ERR_QUEUE_FULL;
                }
                break;
        }
    }

    slot = &g_async.slots[g_async.tail];
    if (slot->cap < size + 1) {
        unsigned int newcap = slot->cap ? slot->cap : 256;
        char* nb;
        while (newcap < size + 1) {
            newcap *= 2;
        }
        nb = (char*)hx_clog__malloc(newcap);
        if (!nb) {
            /* cannot grow: drop this line rather than corrupt the queue */
            g_async.dropped++;
            hx_mutex_unlock(&g_async.lock);
            return HX_CLOG_ERR_OUT_OF_MEMORY;
        }
        if (slot->data) {
            hx_clog__free(slot->data);
        }
        slot->data = nb;
        slot->cap = newcap;
    }
    slot->level = level;
    slot->len = size;
    memcpy(slot->data, data, size);

    g_async.tail = (g_async.tail + 1) % g_async.capacity;
    g_async.count++;
    if (g_async.count > g_async.high_watermark) {
        g_async.high_watermark = g_async.count;
    }

    hx_cond_signal(&g_async.not_empty);
    hx_mutex_unlock(&g_async.lock);
    return HX_CLOG_OK;
}

static void worker_main(void* arg) {
    (void)arg;
    for (;;) {
        hx_clog_level_t emit_level;
        unsigned int    emit_len;
        int got;

        hx_mutex_lock(&g_async.lock);
        while (g_async.count == 0 && !g_async.stop && !g_async.flush_request) {
            if (!hx_cond_wait_ms(&g_async.not_empty, &g_async.lock,
                                 g_async.flush_interval_ms)) {
                /* periodic flush tick */
                break;
            }
        }

        if (g_async.count == 0) {
            int should_exit = g_async.stop;
            int do_flush = g_async.flush_request;
            g_async.flush_request = 0;
            hx_mutex_unlock(&g_async.lock);

            /* periodic / requested flush of sinks */
            hx_core_flush_sinks();
            if (do_flush) {
                hx_mutex_lock(&g_async.lock);
                hx_cond_broadcast(&g_async.flushed);
                hx_mutex_unlock(&g_async.lock);
            }
            if (should_exit) {
                break;
            }
            continue;
        }

        /* pop one item under lock, write outside batching loop */
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
                        /* cannot grow scratch: skip this line to stay alive */
                        g_async.head = (g_async.head + 1) % g_async.capacity;
                        g_async.count--;
                        g_async.dropped++;
                        hx_cond_signal(&g_async.not_full);
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
                emit_len = s->len;
                memcpy(g_async.scratch, s->data, s->len);

                g_async.head = (g_async.head + 1) % g_async.capacity;
                g_async.count--;
                hx_cond_signal(&g_async.not_full);

                hx_mutex_unlock(&g_async.lock);
                hx_core_emit_to_sinks(emit_level, g_async.scratch, emit_len);
                hx_mutex_lock(&g_async.lock);
                written++;
            }
            got = (written > 0);
            (void)got;

            if (g_async.flush_request && g_async.count == 0) {
                g_async.flush_request = 0;
                hx_cond_broadcast(&g_async.flushed);
            }
        }
        hx_mutex_unlock(&g_async.lock);
    }
}

void hx_async_flush(void) {
    if (!g_async.running) {
        return;
    }
    hx_mutex_lock(&g_async.lock);
    if (g_async.count == 0) {
        hx_mutex_unlock(&g_async.lock);
        return;
    }
    g_async.flush_request = 1;
    hx_cond_signal(&g_async.not_empty);
    while (g_async.flush_request && g_async.running) {
        hx_cond_wait_ms(&g_async.flushed, &g_async.lock, 100);
        if (g_async.count == 0) {
            break;
        }
    }
    hx_mutex_unlock(&g_async.lock);
}

void hx_async_stop(void) {
    if (!g_async.running) {
        return;
    }
    hx_mutex_lock(&g_async.lock);
    g_async.stop = 1;
    hx_cond_broadcast(&g_async.not_empty);
    hx_cond_broadcast(&g_async.not_full);
    hx_mutex_unlock(&g_async.lock);

    hx_thread_join(g_async.worker);

    g_async.running = 0;

    {
        unsigned int i;
        for (i = 0; i < g_async.capacity; ++i) {
            if (g_async.slots[i].data) {
                hx_clog__free(g_async.slots[i].data);
            }
        }
    }
    hx_clog__free(g_async.slots);
    g_async.slots = NULL;
    if (g_async.scratch) {
        hx_clog__free(g_async.scratch);
        g_async.scratch = NULL;
        g_async.scratch_cap = 0;
    }
    hx_mutex_destroy(&g_async.lock);
    hx_cond_destroy(&g_async.not_empty);
    hx_cond_destroy(&g_async.not_full);
    hx_cond_destroy(&g_async.flushed);
}

unsigned long long hx_async_dropped(void) {
    return g_async.dropped;
}
unsigned long long hx_async_high_watermark(void) {
    return g_async.high_watermark;
}

#endif /* HX_CLOG_ENABLE_ASYNC */
