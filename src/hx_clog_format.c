/*
 * hx_clog - log formatting.
 *
 * Implements the pattern engine described in the design doc. Supported
 * placeholders:
 *   %Y %m %d %H %M %S   date/time components
 *   %e                  milliseconds (3 digits)
 *   %l                  level (padded short name)
 *   %t                  thread id
 *   %p                  process id
 *   %s                  source file name (basename only)
 *   %F                  source file full path (as passed in __FILE__)
 *   %#                  line number
 *   %!                  function name
 *   %v                  log message body
 *   %n                  newline
 *   %%                  literal percent
 */
#include "hx_clog_internal.h"

static const char* const k_level_names_padded[] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", "OFF  "
};

const char* hx_level_short_name(hx_clog_level_t level) {
    if (level < 0 || level > HX_CLOG_LEVEL_OFF) {
        return "?????";
    }
    return k_level_names_padded[level];
}

/* Return the basename portion of a path (handles / and \). */
static const char* basename_of(const char* path) {
    const char* base = path;
    const char* p;
    if (!path) {
        return "";
    }
    for (p = path; *p; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

/* Small bounded appender. Tracks position; never overflows. */
typedef struct {
    char* buf;
    unsigned int cap;   /* total capacity including space for NUL */
    unsigned int pos;
} appender_t;

static void ap_char(appender_t* a, char c) {
    if (a->pos + 1 < a->cap) {
        a->buf[a->pos++] = c;
    }
}

static void ap_str(appender_t* a, const char* s) {
    if (!s) {
        return;
    }
    while (*s && a->pos + 1 < a->cap) {
        a->buf[a->pos++] = *s++;
    }
}

static void ap_strn(appender_t* a, const char* s, unsigned int n) {
    unsigned int i;
    if (!s) {
        return;
    }
    for (i = 0; i < n && a->pos + 1 < a->cap; ++i) {
        a->buf[a->pos++] = s[i];
    }
}

static void ap_uint(appender_t* a, unsigned long v) {
    char tmp[24];
    int i = 0;
    if (v == 0) {
        ap_char(a, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        ap_char(a, tmp[--i]);
    }
}

/* Zero-padded unsigned to a fixed width (width <= 9). */
static void ap_uint_pad(appender_t* a, unsigned long v, int width) {
    char tmp[16];
    int i = 0;
    int j;
    if (v == 0) {
        tmp[i++] = '0';
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    for (j = i; j < width; ++j) {
        ap_char(a, '0');
    }
    while (i > 0) {
        ap_char(a, tmp[--i]);
    }
}

unsigned int hx_format_record(const char* pattern,
                              const hx_clog_record_t* rec,
                              char* out, unsigned int out_size) {
    appender_t a;
    struct tm tmv;
    const char* p;

    if (!out || out_size == 0) {
        return 0;
    }
    a.buf = out;
    a.cap = out_size;
    a.pos = 0;

    if (!pattern) {
        pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [tid:%t] %s:%# %!() - %v%n";
    }

    hx_localtime(rec->ts.sec, &tmv);

    for (p = pattern; *p; ++p) {
        if (*p != '%') {
            ap_char(&a, *p);
            continue;
        }
        ++p;
        switch (*p) {
            case 'Y': ap_uint_pad(&a, (unsigned long)(tmv.tm_year + 1900), 4); break;
            case 'm': ap_uint_pad(&a, (unsigned long)(tmv.tm_mon + 1), 2); break;
            case 'd': ap_uint_pad(&a, (unsigned long)tmv.tm_mday, 2); break;
            case 'H': ap_uint_pad(&a, (unsigned long)tmv.tm_hour, 2); break;
            case 'M': ap_uint_pad(&a, (unsigned long)tmv.tm_min, 2); break;
            case 'S': ap_uint_pad(&a, (unsigned long)tmv.tm_sec, 2); break;
            case 'e': ap_uint_pad(&a, (unsigned long)rec->ts.msec, 3); break;
            case 'l': ap_str(&a, hx_level_short_name(rec->level)); break;
            case 't': ap_uint(&a, rec->tid); break;
            case 'p': ap_uint(&a, hx_get_pid()); break;
            case 's': ap_str(&a, basename_of(rec->file)); break;
            case 'F': ap_str(&a, rec->file ? rec->file : ""); break;
            case '#': ap_uint(&a, (unsigned long)rec->line); break;
            case '!': ap_str(&a, rec->func ? rec->func : ""); break;
            case 'v': ap_strn(&a, rec->msg, rec->msg_len); break;
            case 'n': ap_char(&a, '\n'); break;
            case '%': ap_char(&a, '%'); break;
            case '\0':
                /* trailing percent at end of pattern */
                ap_char(&a, '%');
                --p; /* so the loop's ++p sees the NUL and stops */
                break;
            default:
                /* unknown specifier: emit verbatim */
                ap_char(&a, '%');
                ap_char(&a, *p);
                break;
        }
        if (*p == '\0') {
            break;
        }
    }

    a.buf[a.pos] = '\0';
    return a.pos;
}
