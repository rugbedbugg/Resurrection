CFLAGS ?= -O2 -g -Wall -Wextra
LDLIBS ?= -lpthread
all: spike_calibrate
%: %.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
clean:
	$(RM) spike_calibrate
.PHONY: all clean
