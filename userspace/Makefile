# Freestanding MIPS32 soft-float binary for the MT7620A.
# A glibc build is hard-float and dies with SIGILL on this CPU.
# make predefines CC, so ?= would never apply.
ifeq ($(origin CC),default)
CC := mipsel-linux-gnu-gcc
endif
CFLAGS := -msoft-float -nostdlib -fno-builtin -fno-pic -mno-abicalls \
	-march=mips32r2 -mabi=32 -Os -Wall -I../drivers/openmiop
LDFLAGS := -nostdlib -static -Wl,-e,_start

.PHONY: all clean
all: openmiop-rc omi-peek

openmiop-rc: openmiop-rc.c ../drivers/openmiop/openmiop.h
	$(CC) $(CFLAGS) -o $@ openmiop-rc.c $(LDFLAGS)

omi-peek: omi-peek.c ../drivers/openmiop/openmiop.h
	$(CC) $(CFLAGS) -Wno-unused-function -o $@ omi-peek.c $(LDFLAGS)

clean:
	rm -f openmiop-rc omi-peek
