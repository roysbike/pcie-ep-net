# Freestanding MIPS32 soft-float binary for the MT7620A.
# A glibc build is hard-float and dies with SIGILL on this CPU.
CC ?= mipsel-linux-gnu-gcc
CFLAGS := -msoft-float -nostdlib -fno-builtin -fno-pic -mno-abicalls \
	-march=mips32r2 -mabi=32 -Os -Wall -I../drivers/openmiop
LDFLAGS := -nostdlib -static -Wl,-e,_start

.PHONY: all clean
all: openmiop-rc

openmiop-rc: openmiop-rc.c ../drivers/openmiop/openmiop.h
	$(CC) $(CFLAGS) -o $@ openmiop-rc.c $(LDFLAGS)

clean:
	rm -f openmiop-rc
