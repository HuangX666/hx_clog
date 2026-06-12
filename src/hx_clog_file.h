/*
 * hx_clog - shared file sink state (internal).
 *
 * Defined in its own header so hx_clog_file.c and hx_clog_rotate.c can both
 * see the layout.
 */
#ifndef HX_CLOG_FILE_H
#define HX_CLOG_FILE_H

#include "hx_clog_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

struct hx_file_sink_impl {
    FILE* fp;
    char  dir[HX_CLOG_PATH_MAX];
    char  base_name[256];          /* e.g. "app.log" */
    char  active_path[HX_CLOG_PATH_MAX];

    unsigned long long current_size;

    hx_clog_rotate_policy_t policy;
    unsigned long long max_file_size;
    int max_backup_files;
    int max_backup_days;
    int rotate_daily;
    unsigned int rotate_interval_seconds;
    int rotate_align;              /* align interval rotation to wall clock */
    int rotate_on_startup;
    int max_compressed_files;      /* cap on .gz backups; 0 = max_backup_files */

    int  cur_year;                 /* date of the currently open file */
    int  cur_yday;
    time_t opened_sec;

    hx_mutex_t lock;
};

/* Open (or create) the active log file. Returns HX_CLOG_OK / error code. */
int hx_file_open_active(struct hx_file_sink_impl* fs);

#ifdef __cplusplus
}
#endif

#endif /* HX_CLOG_FILE_H */
