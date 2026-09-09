#ifndef ADAPTIVE_RESTORE_MARKER_H
#define ADAPTIVE_RESTORE_MARKER_H

#include <signal.h>

/* Mark the point at which a workload is ready to be checkpointed.
 *
 * Under arctl the tracer catches this stop, snapshots the process, and lets it
 * continue. Run bare, the process simply stops and can be resumed with SIGCONT,
 * so a workload built against this header is still runnable on its own. */
static inline void ar_checkpoint_here(void) { raise(SIGSTOP); }

#endif /* ADAPTIVE_RESTORE_MARKER_H */
