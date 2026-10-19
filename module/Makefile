# Out-of-tree build of the loader and the retained-log reader.
#
#   make -C module KDIR=<android kernel build dir> KSRC=<android kernel source>
#
# KDIR must be a fully built (Image + modules) output directory of the kernel
# the headset is running, so Module.symvers carries the right CRCs.
ifneq ($(KERNELRELEASE),)
obj-m += quest_kexec.o marker_read.o
obj-m += usb_log.o
obj-m += ion_secmap.o
quest_kexec-y := loader.o transition.o
# Without this define the module can only stage (execute=1 is refused).
ccflags-y += -DQKX_ENABLE_EXPERIMENTAL_JUMP
ccflags-y += -I$(srctree)/drivers/usb/dwc3
# ion_secmap reads stock ION and kgsl internals, so it needs the kernel source
# tree (KSRC) on its include path in addition to the build dir (KDIR).
CFLAGS_ion_secmap.o += -I$(KSRC)/drivers/gpu/msm \
                       -I$(KSRC)/drivers/staging/android/ion
else
KDIR ?= $(error set KDIR to the built Android kernel output directory)
# ion_secmap.o pulls stock ION/kgsl headers from the kernel source tree;
# default to the sibling checkout, override for a different layout.
KSRC ?= $(abspath $(CURDIR)/../../oculus-linux-kernel)
LOCALVERSION ?=
KMAKE = $(MAKE) -C $(abspath $(KDIR)) M=$(CURDIR) ARCH=arm64 CC=clang LD=ld.lld \
	HOSTCC=clang CROSS_COMPILE=aarch64-linux-gnu- CLANG_TRIPLE=aarch64-linux-gnu- \
	KSRC=$(KSRC) LOCALVERSION=$(LOCALVERSION)
.PHONY: all clean
all:
	$(KMAKE) modules
clean:
	$(KMAKE) clean
endif
