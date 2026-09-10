# Phase 2 spikes. Not part of the CMake build: these are throwaway probes whose
# output is a written finding, per the Phase 2 plan Task 2.
CFLAGS  ?= -O0 -g -Wall -Wextra
LDLIBS  ?= -lcompel
SPIKES  := spike_infect spike_syscall spike_evict spike_regs

all: $(SPIKES)
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
check: all
	@for s in $(SPIKES); do echo "== $$s =="; ./$$s || echo "(exit $$?)"; echo; done
clean:
	$(RM) $(SPIKES)
.PHONY: all check clean
