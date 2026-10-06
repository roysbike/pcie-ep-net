// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * omi-peek: dump the openmiop header of an endpoint BAR from the
 * Cluster Box. Diagnostic only; reads 2 KiB with CPU MMIO.
 *
 *   omi-peek 0000:04:00.0
 *
 * Built from the same freestanding prelude as openmiop-rc.
 *
 * (prelude follows)
 *
 * Data between blades does not pass through this process. It
 *
 *   - finds openmiop endpoints and gives each a node index from the
 *     switch port it sits behind,
 *   - restores BAR0, the command register and a uniform Max Payload
 *     Size after an endpoint reloads (probe resets its config space),
 *   - publishes a peer table into every endpoint so they can reach
 *     each other directly,
 *   - detaches an endpoint that announces it is leaving, and tells it
 *     when every peer has stopped using it,
 *   - moves the slow gateway rings between the endpoints and one TAP
 *     (omi0), and relays frames between endpoints that have no direct
 *     path yet.
 *
 * Freestanding on purpose. The MT7620A has no FPU, and the Debian
 * mipsel glibc we can cross-compile against is hard-float, so a normal
 * static binary dies with SIGILL before main. This file uses Linux
 * syscalls only and is built -msoft-float -nostdlib.
 */
#include <stddef.h>

#include "openmiop.h"

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;
typedef int s32;

#define SYS_exit_group	4246
#define SYS_read	4003
#define SYS_write	4004
#define SYS_open	4005
#define SYS_close	4006
#define SYS_lseek	4019
#define SYS_ioctl	4054
#define SYS_readlink	4085
#define SYS_munmap	4091
#define SYS_nanosleep	4166
#define SYS_socket	4183
#define SYS_mmap2	4210
#define SYS_clock_gettime 4263
#define CLOCK_MONOTONIC	1

#define O_RDONLY	0
#define O_WRONLY	1
#define O_RDWR		2
#define O_NONBLOCK	0x0080	/* MIPS */

#define PROT_READ	1
#define PROT_WRITE	2
#define MAP_SHARED	1

#define TUNSETIFF	0x800454ca	/* MIPS _IOW('T', 202, int) */
#define SIOCSIFHWADDR	0x8924
#define SIOCGIFFLAGS	0x8913
#define SIOCSIFFLAGS	0x8914
#define IFF_TAP		0x0002
#define IFF_NO_PI	0x1000
#define IFF_UP		0x1
#define ARPHRD_ETHER	1
#define IFNAMSIZ	16

#define PCI_COMMAND		0x04
#define PCI_COMMAND_MEMORY	0x2
#define PCI_COMMAND_MASTER	0x4
#define PCI_BAR0		0x10
#define PCI_CAP_PTR		0x34
#define PCI_CAP_ID_EXP		0x10
#define PCI_EXP_DEVCAP		4
#define PCI_EXP_DEVCTL		8
#define PCI_EXP_DEVCTL_MPS	0x00e0

struct timespec32 {
	s32 tv_sec;
	s32 tv_nsec;
};

struct ifreq_flags {
	char name[IFNAMSIZ];
	u16 flags;
	u8 pad[14];
};

struct ifreq_hw {
	char name[IFNAMSIZ];
	u16 family;
	u8 mac[6];
	u8 pad[8];
};

static long sc(long n, long a, long b, long c, long d, long e, long f)
{
	register long r2 __asm__("$2") = n;
	register long r4 __asm__("$4") = a;
	register long r5 __asm__("$5") = b;
	register long r6 __asm__("$6") = c;
	register long r7 __asm__("$7") = d;

	__asm__ volatile(
		"subu $29, $29, 32\n\t"
		"sw %5, 16($29)\n\t"
		"sw %6, 20($29)\n\t"
		"syscall\n\t"
		"addu $29, $29, 32\n\t"
		: "+r"(r2), "+r"(r7)
		: "r"(r4), "r"(r5), "r"(r6), "r"(e), "r"(f)
		: "memory", "$8", "$9", "$10", "$11", "$12", "$13",
		  "$14", "$15", "$24", "$25");
	if (r7)
		return -r2;
	return r2;
}

/* ---- tiny libc ---- */

static unsigned long slen(const char *s)
{
	unsigned long n = 0;

	while (s[n])
		n++;
	return n;
}

static void wr(const char *s)
{
	sc(SYS_write, 2, (long)s, slen(s), 0, 0, 0);
}

static void hexout(u32 v)
{
	char b[11];
	int i;

	b[0] = '0';
	b[1] = 'x';
	for (i = 0; i < 8; i++) {
		u32 nib = (v >> ((7 - i) * 4)) & 0xf;

		b[2 + i] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
	}
	b[10] = 0;
	wr(b);
}

static void decout(u32 v)
{
	char b[12];
	int i = 11;

	b[i] = 0;
	do {
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v && i);
	wr(b + i);
}

static void die(const char *s, long err)
{
	wr(s);
	wr(" err ");
	hexout((u32)(-err));
	wr("\n");
	sc(SYS_exit_group, 1, 0, 0, 0, 0, 0);
}

void *memcpy(void *dst, const void *src, unsigned long n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
	u8 *d = dst;

	while (n--)
		*d++ = (u8)c;
	return dst;
}

static int mac_eq(const u8 *a, const u8 *b)
{
	int i;

	for (i = 0; i < 6; i++)
		if (a[i] != b[i])
			return 0;
	return 1;
}

static void scpy(char *d, const char *s)
{
	while ((*d++ = *s++))
		;
}

static void scat(char *d, const char *s)
{
	scpy(d + slen(d), s);
}

static void io_mb(void)
{
	__asm__ volatile("sync" ::: "memory");
}

static u32 rd32(const volatile void *p)
{
	u32 v = *(const volatile u32 *)p;

	io_mb();
	return v;
}

static void wr32(volatile void *p, u32 v)
{
	io_mb();
	*(volatile u32 *)p = v;
}

/* Milliseconds since boot. A 1 ms nanosleep takes a whole tick here. */
static u32 now_ms(void)
{
	struct timespec32 ts;

	sc(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts, 0, 0, 0, 0);
	return (u32)ts.tv_sec * 1000u + (u32)ts.tv_nsec / 1000000u;
}

static void msleep(s32 ms)
{
	struct timespec32 ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000;
	sc(SYS_nanosleep, (long)&ts, 0, 0, 0, 0, 0);
}

static int read_file(const char *path, char *buf, int cap)
{
	long fd = sc(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
	long n;

	if (fd < 0)
		return -1;
	n = sc(SYS_read, fd, (long)buf, cap - 1, 0, 0, 0);
	sc(SYS_close, fd, 0, 0, 0, 0, 0);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}

static int write_file(const char *path, const char *s)
{
	long fd = sc(SYS_open, (long)path, O_WRONLY, 0, 0, 0, 0);
	long n;

	if (fd < 0)
		return -1;
	n = sc(SYS_write, fd, (long)s, slen(s), 0, 0, 0);
	sc(SYS_close, fd, 0, 0, 0, 0, 0);
	return n < 0 ? -1 : 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* Parses "0x..." up to 64 bits into hi:lo. */
static int parse_hex64(const char *s, u32 *lo, u32 *hi)
{
	u32 h = 0, l = 0;
	int any = 0, d;

	while (*s == ' ' || *s == '\t')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while ((d = hexval(*s)) >= 0) {
		h = (h << 4) | (l >> 28);
		l = (l << 4) | (u32)d;
		any = 1;
		s++;
	}
	if (!any)
		return -1;
	*lo = l;
	if (hi)
		*hi = h;
	return 0;
}

static void put_hex2(char *p, unsigned v)
{
	static const char h[] = "0123456789abcdef";

	p[0] = h[(v >> 4) & 0xf];
	p[1] = h[v & 0xf];
}


static void line(const char *name, u32 v)
{
	wr(name);
	wr(" ");
	hexout(v);
	wr("\n");
}

static void peek(const char *bdf)
{
	char path[80];
	long fd;
	u8 *bar;
	struct omi_bar_head *h;
	int i;

	scpy(path, "/sys/bus/pci/devices/");
	scat(path, bdf);
	scat(path, "/resource0");
	fd = sc(SYS_open, (long)path, O_RDWR, 0, 0, 0, 0);
	if (fd < 0)
		die("open resource0", fd);
	bar = (u8 *)sc(SYS_mmap2, 0, 4096, PROT_READ, MAP_SHARED, fd, 0);
	if ((unsigned long)bar >= (unsigned long)-4095)
		die("mmap", (long)bar);
	h = (struct omi_bar_head *)bar;
	line("magic     ", rd32(&h->hdr.magic));
	line("version   ", rd32(&h->hdr.version));
	line("epoch     ", rd32(&h->hdr.epoch));
	line("flags     ", rd32(&h->hdr.flags));
	line("table_seen", rd32(&h->hdr.table_seen));
	line("rc.flags  ", rd32(&h->ctl.flags));
	line("rc.ep_epoch", rd32(&h->ctl.ep_epoch));
	line("rc.gen    ", rd32(&h->ctl.table_gen));
	line("rc.down_ack", rd32(&h->ctl.down_ack));
	for (i = 0; i < (int)OMI_N_RINGS; i++) {
		wr("node ");
		decout((u32)i);
		wr(": table epoch ");
		hexout(rd32(&h->peers[i].epoch));
		wr(" bar ");
		hexout(rd32(&h->peers[i].bar_lo));
		wr(" rx head ");
		hexout(rd32(&h->rx_prod[i].head));
		wr(" token ");
		hexout(rd32(&h->rx_prod[i].token));
		wr(" credit ");
		hexout(rd32(&h->tx_cons[i].tail));
		wr(" ack ");
		hexout(rd32(&h->tx_cons[i].ack));
		wr("\n");
	}
}

void __start(long argc, char **argv);

/* The kernel puts argc at sp; _start passes sp on to __start. */
__asm__(".text\n.globl _start\n_start:\n\tmove $4, $29\n\tj __start_sp\n\tnop\n");

void __start_sp(long *sp)
{
	if (sp[0] < 2)
		die("usage: omi-peek BDF", -22);
	peek((char *)sp[2]);
	sc(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}
