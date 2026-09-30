# Static ARM64 helpers: qkx_fsmap and qkx_rawcp run under Android, qkx_dm runs
# in the kexec target's initramfs.
CROSS_COMPILE ?= aarch64-linux-gnu-
CC := $(CROSS_COMPILE)gcc
STRIP := $(CROSS_COMPILE)strip
CFLAGS ?= -static -O2 -Wall -Wextra -Wno-sign-compare

HELPERS := qkx_fsmap qkx_rawcp qkx_dm

all: $(HELPERS)

$(HELPERS): %: %.c
	$(CC) $(CFLAGS) $< -o $@
	$(STRIP) $@

clean:
	rm -f $(HELPERS)

.PHONY: all clean
