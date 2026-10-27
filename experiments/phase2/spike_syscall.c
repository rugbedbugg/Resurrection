/* Spike 2: can we drive syscalls inside a stopped target from outside,
 * without root and without writing a parasite blob?
 *
 * compel_prepare() sets up a netlink socket in the target's netns and needs
 * privileges we do not have. compel_prepare_noctx() skips that, leaving us to
 * fill struct infect_ctx ourselves. This probe finds the minimum viable set. */
#define _GNU_SOURCE
#include <compel/infect.h>
#include <compel/log.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int log_level = COMPEL_LOG_ERROR;
static void spike_log(unsigned int lvl, const char *fmt, va_list a)
{ if ((int)lvl <= log_level) vfprintf(stderr, fmt, a); }

/* compel calls this to open files under /proc/<pid>/. */
static int spike_open_proc(int pid, int mode, const char *fmt, ...)
{
    char path[256], rest[192];
    va_list a;
    va_start(a, fmt);
    vsnprintf(rest, sizeof rest, fmt, a);
    va_end(a);
    snprintf(path, sizeof path, "/proc/%d/%s", pid, rest);
    int fd = open(path, mode);
    if (fd < 0) fprintf(stderr, "open_proc(%s) failed: %s\n", path, strerror(errno));
    return fd;
}

/* compel pokes a small syscall stub into the target and restores the original
 * bytes afterwards, so syscall_ip must be a mapped executable address. With
 * compel_prepare_noctx() nothing fills this in for us. */
static unsigned long first_exec_vaddr(pid_t pid)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    unsigned long start = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long a, b;
        char perms[8];
        if (sscanf(line, "%lx-%lx %7s", &a, &b, perms) != 3) continue;
        if (perms[2] == 'x') { start = a; break; }
    }
    fclose(f);
    return start;
}

int main(void)
{
    compel_log_init(spike_log, log_level);

    int tube[2];
    if (pipe(tube) < 0) return 2;
    pid_t child = fork();
    if (child < 0) return 2;
    if (child == 0) {
        close(tube[0]);
        char r = 1; if (write(tube[1], &r, 1) != 1) _exit(3);
        close(tube[1]);
        for (;;) pause();
        _exit(0);
    }
    close(tube[1]);
    char r; if (read(tube[0], &r, 1) != 1) return 3;
    close(tube[0]);

    int st;
    if (compel_stop_task(child) < 0) { fprintf(stderr, "FAIL stop_task\n"); goto out; }

    struct parasite_ctl *ctl = compel_prepare_noctx(child, false);
    if (!ctl) { fprintf(stderr, "FAIL: compel_prepare_noctx\n"); goto out; }
    printf("compel_prepare_noctx: ok\n");

    struct infect_ctx *ictx = compel_infect_ctx(ctl);
    ictx->open_proc = spike_open_proc;
    ictx->task_size = compel_task_size();
    ictx->log_fd    = -1;
    ictx->syscall_ip = first_exec_vaddr(child);
    printf("task_size  = 0x%lx\n", ictx->task_size);
    printf("syscall_ip = 0x%lx\n", ictx->syscall_ip);
    if (!ictx->syscall_ip) { fprintf(stderr, "FAIL: no executable mapping found\n"); goto cure; }

    /* The headline question: an mmap executed inside the target, driven from
     * out here, with no parasite blob written by us. */
    long ret = 0;
    int rc = compel_syscall(ctl, __NR_mmap, &ret,
                            0, 8192, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (rc < 0) {
        fprintf(stderr, "FAIL: compel_syscall(mmap) rc=%d\n", rc);
    } else if (ret < 0 && ret > -4096) {
        fprintf(stderr, "FAIL: target mmap returned %ld (%s)\n", ret, strerror((int)-ret));
    } else {
        printf("compel_syscall(mmap): ok -> 0x%lx\n", (unsigned long)ret);
        printf("RESULT: syscalls can be driven inside the target with no parasite blob\n");

        /* Does the new mapping show up in the target's own /proc? */
        char cmd[128];
        snprintf(cmd, sizeof cmd, "grep -q '^%lx-' /proc/%d/maps", (unsigned long)ret, child);
        printf("visible in /proc/<pid>/maps: %s\n", system(cmd) == 0 ? "yes" : "NO");
    }

cure:
    if (compel_cure(ctl) < 0) fprintf(stderr, "WARN: compel_cure failed\n");
out:
    kill(child, SIGKILL);
    waitpid(child, &st, 0);
    return 0;
}
