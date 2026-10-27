/* Spike 1: does the compel toolchain work at all in this environment?
 *
 * Prepares a stopped child for infection and cures it again, without any
 * parasite blob. Establishes the baseline before anything harder is attempted.
 *
 * Build: see experiments/phase2/Makefile */
#define _GNU_SOURCE
#include <compel/infect.h>
#include <compel/log.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

static int log_level = COMPEL_LOG_DEBUG;

static void spike_log(unsigned int lvl, const char *fmt, va_list args)
{
    if ((int)lvl > log_level) return;
    vfprintf(stderr, fmt, args);
}

int main(void)
{
    compel_log_init(spike_log, log_level);

    /* FINDING (spike 1): the target must NOT PTRACE_TRACEME itself. compel
     * seizes with PTRACE_SEIZE, which returns EPERM if the task already has a
     * tracer. So the child only signals readiness and then idles; compel does
     * every bit of the ptrace attachment itself. */
    int tube[2];
    if (pipe(tube) < 0) { perror("pipe"); return 2; }

    pid_t child = fork();
    if (child < 0) { perror("fork"); return 2; }
    if (child == 0) {
        close(tube[0]);
        char ready = 1;
        if (write(tube[1], &ready, 1) != 1) _exit(3);
        close(tube[1]);
        for (;;) pause();
        _exit(0);
    }

    close(tube[1]);
    char ready = 0;
    if (read(tube[0], &ready, 1) != 1) { fprintf(stderr, "child never signalled\n"); return 3; }
    close(tube[0]);

    int st;
    if (compel_stop_task(child) < 0) {
        fprintf(stderr, "FAIL: compel_stop_task\n");
        kill(child, SIGKILL); waitpid(child, &st, 0);
        return 4;
    }
    printf("compel_stop_task: ok\n");

    struct parasite_ctl *ctl = compel_prepare(child, false);
    if (!ctl) {
        fprintf(stderr, "FAIL: compel_prepare\n");
        kill(child, SIGKILL); waitpid(child, &st, 0);
        return 5;
    }
    printf("compel_prepare: ok (ctl=%p)\n", (void *)ctl);

    if (compel_cure(ctl) < 0)
        fprintf(stderr, "WARN: compel_cure failed\n");
    else
        printf("compel_cure: ok\n");

    kill(child, SIGKILL);
    waitpid(child, &st, 0);
    printf("RESULT: compel toolchain usable\n");
    return 0;
}
