# Mixtile Blade 3 board modules that ship with openmiop:
#   blade3-leds          activity LEDs of the RTL8125 2.5GbE ports
#   blade3-nvme-recover  M.2 NVMe back after a Cluster Box PERST#
#
# Out-of-tree build against a full kernel build tree (blade3-nvme-recover
# includes the private DesignWare header drivers/pci/controller/dwc/
# pcie-designware.h, which header-only packages do not ship):
#   make -C drivers/blade3 KDIR=<kernel build tree>
obj-m += blade3-leds.o
obj-m += blade3-nvme-recover.o
CFLAGS_blade3-nvme-recover.o += -I$(srctree)/drivers/pci/controller/dwc

KDIR ?= /lib/modules/$(shell uname -r)/build

# Command-line ARCH= / CROSS_COMPILE= are forwarded. Empty values are
# left unset so a native build on the board still works.
KBUILD_FLAGS :=
ifneq ($(ARCH),)
KBUILD_FLAGS += ARCH=$(ARCH)
endif
ifneq ($(CROSS_COMPILE),)
KBUILD_FLAGS += CROSS_COMPILE=$(CROSS_COMPILE)
endif

all:
	$(MAKE) -C $(KDIR) $(KBUILD_FLAGS) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) $(KBUILD_FLAGS) M=$(CURDIR) clean
