/* hx_clog test: fork() safety. Exercises the pthread_atfork handlers and
 * hx_clog_after_fork_child() at runtime — the child must be able to keep
 * logging/flushing without inheriting a locked mutex. POSIX only (built only
 * on non-Windows by CMake). */
#include "hx_clog.h"

#include <stdio.h>

#if defined(_WIN32)
int main(void) { printf("ALL PASS (fork: skipped on Windows)\n"); return 0; }
#else

#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

#define CHECK(cond) do { \
    if (!(cond)) { printf("FAIL: %s (line %d)\n", #cond, __LINE__); return 1; } \
} while (0)

int main(void) {
    hx_clog_config_t cfg;
    pid_t pid;
    int status = 0, i;

    hx_clog_config_default(&cfg);
    cfg.enable_console = 0;
    cfg.enable_file = 1;
    cfg.log_dir = "./test_fork_logs";
    cfg.file_name = "fork.log";
    cfg.mode = HX_CLOG_MODE_ASYNC;   /* exercise worker restart in the child */
    CHECK(hx_clog_init(&cfg) == HX_CLOG_OK);

    /* log a bit in the parent so the async worker is live at fork time */
    for (i = 0; i < 50; ++i) {
        HX_LOG_INFO("parent pre-fork %d", i);
    }

    pid = fork();
    CHECK(pid >= 0);

    if (pid == 0) {
        /* child: a deadlock here would hang forever — guard with an alarm so
         * the test fails loudly instead of timing out the whole suite */
        alarm(15);
        hx_clog_after_fork_child();
        for (i = 0; i < 200; ++i) {
            HX_LOG_INFO("child %d", i);
        }
        hx_clog_flush();
        hx_clog_shutdown();
        _exit(0);
    }

    /* parent keeps logging concurrently, then reaps the child */
    for (i = 0; i < 200; ++i) {
        HX_LOG_INFO("parent post-fork %d", i);
    }
    hx_clog_flush();

    CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));            /* not killed by SIGALRM/SIGSEGV */
    CHECK(WEXITSTATUS(status) == 0);

    hx_clog_shutdown();
    printf("ALL PASS\n");
    return 0;
}

#endif /* _WIN32 */
