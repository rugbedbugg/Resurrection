CFLAGS ?= -O0 -g -Wall -Wextra
LDLIBS ?= -lcompel
SPIKES := spike_uffd_remote
all: $(SPIKES)
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
clean:
	$(RM) $(SPIKES)
.PHONY: all clean
