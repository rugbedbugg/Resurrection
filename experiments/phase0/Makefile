# Feasibility probes -- build + run with `make` then `make check`.
# These are throwaway spikes, deliberately kept out of the CMake tree.

CC      ?= cc
CFLAGS  ?= -O1 -g -Wall -Wextra -std=c11   # each probe defines _GNU_SOURCE itself
LDLIBS_userfaultfd = -lpthread

PROBES := probe_ptrace_regs probe_process_vm_readv probe_userfaultfd probe_aslr

all: $(PROBES)

probe_userfaultfd: probe_userfaultfd.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS_userfaultfd)

probe_%: probe_%.c
	$(CC) $(CFLAGS) -o $@ $<

check: all
	@rc=0; \
	for p in $(PROBES); do \
	  echo "== $$p =="; ./$$p || rc=1; echo; \
	done; \
	echo "phase0 overall: $$( [ $$rc -eq 0 ] && echo PASS || echo FAIL )"; \
	exit $$rc

clean:
	rm -f $(PROBES)

.PHONY: all check clean
