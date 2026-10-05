# Out-of-tree build against the Blade 3 vendor headers:
#   make -C /usr/src/linux-headers-6.1-rockchip M=$PWD modules
obj-m += openmiop-ep.o
ccflags-y += -I$(src)

KDIR ?= /usr/src/linux-headers-6.1-rockchip

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
