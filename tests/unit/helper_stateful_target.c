/* A target holding state the image cannot represent, so the capture-side guard
 * has something real to catch. Which kind is selected by argv[1]. */
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "adaptive_restore/marker.h"

static void handler(int sig) { (void)sig; }

int main(int argc, char **argv) {
    const char *kind = argc > 1 ? argv[1] : "clean";

    if (!strcmp(kind, "blocked")) {
        sigset_t s;
        sigemptyset(&s);
        sigaddset(&s, SIGUSR1);
        sigprocmask(SIG_BLOCK, &s, NULL);
    } else if (!strcmp(kind, "handler")) {
        signal(SIGUSR1, handler);
    } else if (!strcmp(kind, "chdir")) {
        if (chdir("/") != 0) return 1;
    }

    ar_checkpoint_here();
    return 0;
}
