/*
 * hx_clog - log formatting.
 *
 * Implements the pattern engine described in the design doc. Supported
 * placeholders:
 *   %Y %m %d %H %M %S   date/time components
 *   %e                  milliseconds (3 digits)
 *   %l                  level (padded short name)
 *   %c                  logger/category name
 *   %t                  thread id
 *   %p                  process id
 *   %s                  source file name (basename only)
 *   %F                  source file full path (as passed in __FILE__)
 *   %#                  line number
 *   %!                  function name
 *   %v                  log message body
 *   %x                  thread-local context (key=value list)
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

/* Small bounded appender. Writes never overflow the buffer, but `needed`
 * keeps counting past the capacity so the caller learns the full length the
 * line requires (snprintf semantics) and can grow + reformat exactly once.
 * `needed` saturates at UINT_MAX so a pathological line cannot wrap it. */
typedef struct {
    char* buf;
    unsigned int cap;    /* total capacity including space for NUL */
    unsigned int pos;    /* bytes actually written (always < cap) */
    unsigned int needed; /* bytes that would be written if cap were unbounded */
} appender_t;

static void ap_bump(appender_t* a) {
    if (a->needed != 0xFFFFFFFFu) {
        a->needed++;
    }
}

static void ap_char(appender_t* a, char c) {
    ap_bump(a);
    if (a->pos + 1 < a->cap) {
        a->buf[a->pos++] = c;
    }
}

static void ap_str(appender_t* a, const char* s) {
    if (!s) {
        return;
    }
    while (*s) {
        ap_bump(a);
        if (a->pos + 1 < a->cap) {
            a->buf[a->pos++] = *s;
        }
        ++s;
    }
}

static void ap_strn(appender_t* a, const char* s, unsigned int n) {
    unsigned int i;
    if (!s) {
        return;
    }
    for (i = 0; i < n; ++i) {
        ap_bump(a);
        if (a->pos + 1 < a->cap) {
            a->buf[a->pos++] = s[i];
        }
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

static void ap_ull(appender_t* a, unsigned long long v) {
    char tmp[32];
    int i = 0;
    if (v == 0) {
        ap_char(a, '0');
        return;
    }
    while (v > 0 && i < (int)sizeof(tmp)) {
        tmp[i++] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
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
    a.needed = 0;

    if (!pattern) {
        pattern = "%Y-%m-%d %H:%M:%S.%e [%l] [tid:%t] %s:%# %!() - %v%n";
    }

    hx_localtime((time_t)rec->timestamp_sec, &tmv);

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
            case 'e': ap_uint_pad(&a, (unsigned long)rec->timestamp_msec, 3); break;
            case 'l': ap_str(&a, hx_level_short_name(rec->level)); break;
            case 'c': ap_str(&a, rec->logger_name ? rec->logger_name : ""); break;
            case 't': ap_uint(&a, rec->tid); break;
            case 'p': ap_uint(&a, rec->pid); break;
            case 's': ap_str(&a, basename_of(rec->file)); break;
            case 'F': ap_str(&a, rec->file ? rec->file : ""); break;
            case '#': ap_uint(&a, (unsigned long)rec->line); break;
            case '!': ap_str(&a, rec->func ? rec->func : ""); break;
            case 'v': ap_strn(&a, rec->message, rec->message_len); break;
            case 'x': ap_str(&a, rec->context ? rec->context : ""); break;
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
    /* return the full required length (may exceed pos) so the caller can grow
     * the buffer to fit patterns that repeat %v/%x/%F many times */
    return a.needed;
}

static void ap_json_escaped(appender_t* a, const char* s, unsigned int n) {
    unsigned int i;
    for (i = 0; s && i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"':  ap_str(a, "\\\""); break;
            case '\\': ap_str(a, "\\\\"); break;
            case '\b': ap_str(a, "\\b"); break;
            case '\f': ap_str(a, "\\f"); break;
            case '\n': ap_str(a, "\\n"); break;
            case '\r': ap_str(a, "\\r"); break;
            case '\t': ap_str(a, "\\t"); break;
            default:
                if (c < 0x20) {
                    const char* hex = "0123456789abcdef";
                    ap_str(a, "\\u00");
                    ap_char(a, hex[(c >> 4) & 0xF]);
                    ap_char(a, hex[c & 0xF]);
                } else {
                    ap_char(a, (char)c);
                }
                break;
        }
    }
}

static void ap_json_cstr(appender_t* a, const char* s) {
    ap_json_escaped(a, s ? s : "", s ? (unsigned int)strlen(s) : 0);
}

unsigned int hx_format_json_record(const hx_clog_record_t* rec,
                                   char* out, unsigned int out_size) {
    appender_t a;
    struct tm tmv;

    if (!out || out_size == 0) {
        return 0;
    }
    a.buf = out;
    a.cap = out_size;
    a.pos = 0;
    a.needed = 0;

    hx_localtime((time_t)rec->timestamp_sec, &tmv);

    ap_char(&a, '{');
    ap_str(&a, "\"ts\":\"");
    ap_uint_pad(&a, (unsigned long)(tmv.tm_year + 1900), 4);
    ap_char(&a, '-');
    ap_uint_pad(&a, (unsigned long)(tmv.tm_mon + 1), 2);
    ap_char(&a, '-');
    ap_uint_pad(&a, (unsigned long)tmv.tm_mday, 2);
    ap_char(&a, 'T');
    ap_uint_pad(&a, (unsigned long)tmv.tm_hour, 2);
    ap_char(&a, ':');
    ap_uint_pad(&a, (unsigned long)tmv.tm_min, 2);
    ap_char(&a, ':');
    ap_uint_pad(&a, (unsigned long)tmv.tm_sec, 2);
    ap_char(&a, '.');
    ap_uint_pad(&a, (unsigned long)rec->timestamp_msec, 3);
    ap_str(&a, "\",\"epoch_ms\":");
    ap_ull(&a, (unsigned long long)rec->timestamp_sec * 1000ULL +
               (unsigned long long)rec->timestamp_msec);
    ap_str(&a, ",\"level\":\"");
    ap_json_cstr(&a, hx_clog_level_name(rec->level));
    ap_str(&a, "\",\"logger\":\"");
    ap_json_cstr(&a, rec->logger_name ? rec->logger_name : "");
    ap_str(&a, "\",\"pid\":");
    ap_uint(&a, rec->pid);
    ap_str(&a, ",\"tid\":");
    ap_uint(&a, rec->tid);
    ap_str(&a, ",\"file\":\"");
    ap_json_cstr(&a, rec->file ? rec->file : "");
    ap_str(&a, "\",\"line\":");
    ap_uint(&a, (unsigned long)rec->line);
    ap_str(&a, ",\"func\":\"");
    ap_json_cstr(&a, rec->func ? rec->func : "");
    ap_str(&a, "\",\"message\":\"");
    ap_json_escaped(&a, rec->message, rec->message_len);
    ap_str(&a, "\",\"context\":\"");
    ap_json_cstr(&a, rec->context ? rec->context : "");
    ap_str(&a, "\"}\n");

    a.buf[a.pos] = '\0';
    return a.needed;
}
