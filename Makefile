# Convenience targets. See README.md for the full procedure.
#   make module   KDIR=... [LOCALVERSION=...]
#   make initramfs
OUT ?= out

.PHONY: all module initramfs clean
all: module initramfs

module:
	$(MAKE) -C module KDIR=$(KDIR) LOCALVERSION=$(LOCALVERSION)

$(OUT)/busybox:
	initramfs/busybox.sh $@

initramfs: $(OUT)/busybox
	python3 initramfs/build.py --busybox $(OUT)/busybox --output $(OUT)/initramfs.gz

clean:
	-$(MAKE) -C module clean KDIR=$(KDIR)
	rm -rf $(OUT)
