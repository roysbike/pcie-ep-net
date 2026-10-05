// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cluster Box side of openmiop.
 *
 * Data between blades does not pass through this process. It only
 * publishes each endpoint's PCI BAR address to the other endpoint
 * and bridges the slow gateway rings onto one TAP.
 *
 * Freestanding on purpose. The MT7620A has no FPU, and the Debian
 * mipsel glibc we can cross-compile against is hard-float, so a normal
 * static binary dies with SIGILL before main. This file uses Linux
 * syscalls only and is built -msoft-float -nostdlib.
 */
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
#define SYS_ioctl	4054
#define SYS_nanosleep	4166
#define SYS_mmap2	4210
#define SYS_munmap	4091

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

static void wr(const char *s)
{
	const char *p = s;
	u32 n = 0;

	while (p[n])
		n++;
	sc(SYS_write, 2, (long)s, n, 0, 0, 0);
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

static void die(const char *s, long err)
{
	wr(s);
	wr(" err ");
	hexout((u32)(-err));
	wr("\n");
	sc(SYS_exit_group, 1, 0, 0, 0, 0, 0);
}

static void *memcpy(void *dst, const void *src, unsigned long n)
{
	u8 *d = dst;
	const u8 *s = src;

	while (n--)
		*d++ = *s++;
	return dst;
}

static void memset0(void *dst, unsigned long n)
{
	u8 *d = dst;

	while (n--)
		*d++ = 0;
}

static void io_mb(void)
{
	__asm__ volatile("sync" ::: "memory");
}

static u32 load32(volatile u32 *p)
{
	u32 v = *p;

	io_mb();
	return v;
}

static void store32(volatile u32 *p, u32 v)
{
	io_mb();
	*p = v;
	io_mb();
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

static int parse_hex(const char *s, u32 *out)
{
	u32 v = 0;
	int any = 0;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (*s) {
		char c = *s++;
		u32 d;

		if (c == '\n' || c == '\r' || c == ' ')
			break;
		if (c >= '0' && c <= '9')
			d = (u32)(c - '0');
		else if (c >= 'a' && c <= 'f')
			d = (u32)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			d = (u32)(c - 'A' + 10);
		else
			return -1;
		v = (v << 4) | d;
		any = 1;
	}
	if (!any)
		return -1;
	*out = v;
	return 0;
}

static void put_hex2(char *p, unsigned v)
{
	static const char h[] = "0123456789abcdef";

	p[0] = h[(v >> 4) & 0xf];
	p[1] = h[v & 0xf];
}

#define MAX_EP 4

struct ep_slot {
	char sysfs[40];
	struct openmiop_bar *bar;
	u32 pci_lo;
	u32 pci_hi;
};

static struct ep_slot eps[MAX_EP];
static int nep;

static int parse_bar_addr(const char *s, u32 *lo, u32 *hi)
{
	u32 h = 0;
	u32 l = 0;
	int any = 0;

	while (*s == ' ' || *s == '\t')
		s++;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	while (*s) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			break;
		h = (h << 4) | (l >> 28);
		l = (l << 4) | (u32)d;
		any = 1;
		s++;
	}
	if (!any)
		return -1;
	*lo = l;
	*hi = h;
	return 0;
}

static int same_path(const char *a, const char *b)
{
	int i;

	for (i = 0; i < 40; i++) {
		if (a[i] != b[i])
			return 0;
		if (!a[i])
			return 1;
	}
	return 1;
}

static int mac_same(const u8 *a, const u8 *b)
{
	int i;

	for (i = 0; i < 6; i++) {
		if (a[i] != b[i])
			return 0;
	}
	return 1;
}

static int find_ep(char *sysfs)
{
	unsigned bus, dev;

	for (bus = 0; bus < 16; bus++) {
		for (dev = 0; dev < 32; dev++) {
			char path[80];
			char buf[32];
			u32 vendor, device;
			int i;

			for (i = 0; i < 80; i++)
				path[i] = 0;
			memcpy(path, "/sys/bus/pci/devices/0000:", 26);
			put_hex2(path + 26, bus);
			path[28] = ':';
			put_hex2(path + 29, dev);
			path[31] = '.';
			path[32] = '0';
			memcpy(sysfs, path, 33);
			sysfs[33] = 0;
			memcpy(path + 33, "/vendor", 8);
			if (read_file(path, buf, sizeof(buf)) < 0)
				continue;
			if (parse_hex(buf, &vendor) || vendor != OPENMIOP_PCI_VENDOR)
				continue;
			memcpy(path + 33, "/device", 8);
			if (read_file(path, buf, sizeof(buf)) < 0)
				continue;
			if (parse_hex(buf, &device) || device != OPENMIOP_PCI_DEVICE)
				continue;
			return 0;
		}
	}
	return -1;
}

static void join(char *dst, const char *a, const char *b)
{
	int i = 0;
	int j = 0;

	while (a[i]) {
		dst[i] = a[i];
		i++;
	}
	while (b[j])
		dst[i++] = b[j++];
	dst[i] = 0;
}

static void *map_bar(const char *sysfs)
{
	char path[96];
	long fd, w;
	void *map;

	join(path, sysfs, "/enable");
	fd = sc(SYS_open, (long)path, O_WRONLY, 0, 0, 0, 0);
	if (fd >= 0) {
		w = sc(SYS_write, fd, (long)"1\n", 2, 0, 0, 0);
		sc(SYS_close, fd, 0, 0, 0, 0, 0);
		if (w < 0)
			wr("enable write failed\n");
	}
	join(path, sysfs, "/resource0");
	fd = sc(SYS_open, (long)path, O_RDWR, 0, 0, 0, 0);
	if (fd < 0)
		return 0;
	map = (void *)sc(SYS_mmap2, 0, OPENMIOP_BAR_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
	sc(SYS_close, fd, 0, 0, 0, 0, 0);
	if ((unsigned long)map >= (unsigned long)-4095)
		return 0;
	return map;
}

static int open_tap(char *name, const u8 mac[6])
{
	struct ifreq_flags ifr;
	struct ifreq_hw hw;
	long fd;

	fd = sc(SYS_open, (long)"/dev/net/tun", O_RDWR | O_NONBLOCK, 0, 0, 0, 0);
	if (fd < 0)
		die("open tun", fd);
	memset0(&ifr, sizeof(ifr));
	memcpy(ifr.name, name, IFNAMSIZ);
	ifr.flags = IFF_TAP | IFF_NO_PI;
	{
		long rc = sc(SYS_ioctl, fd, TUNSETIFF, (long)&ifr, 0, 0, 0);

		if (rc < 0)
			die("TUNSETIFF", rc);
	}
	memcpy(name, ifr.name, IFNAMSIZ);

	{
		long s = sc(4183 /* socket */, 2 /* AF_INET */, 2 /* SOCK_DGRAM */, 0, 0, 0, 0);

		if (s < 0)
			die("socket", s);
		memset0(&hw, sizeof(hw));
		memcpy(hw.name, name, IFNAMSIZ);
		hw.family = ARPHRD_ETHER;
		memcpy(hw.mac, mac, 6);
		if (sc(SYS_ioctl, s, SIOCSIFHWADDR, (long)&hw, 0, 0, 0) < 0)
			wr("set mac failed\n");
		memset0(&ifr, sizeof(ifr));
		memcpy(ifr.name, name, IFNAMSIZ);
		if (sc(SYS_ioctl, s, SIOCGIFFLAGS, (long)&ifr, 0, 0, 0) == 0) {
			ifr.flags |= IFF_UP;
			sc(SYS_ioctl, s, SIOCSIFFLAGS, (long)&ifr, 0, 0, 0);
		}
		sc(SYS_close, s, 0, 0, 0, 0, 0);
	}
	return (int)fd;
}

static void fanout(int tap)
{
	u8 buf[OPENMIOP_SLOT_DATA];
	int budget = 32;

	while (budget--) {
		long n = sc(SYS_read, tap, (long)buf, sizeof(buf), 0, 0, 0);
		int e;

		if (n < 0)
			break;
		if (n < 14 || n > OPENMIOP_MAX_FRAME)
			continue;
		for (e = 0; e < nep; e++) {
			struct openmiop_bar *bar = eps[e].bar;
			u32 head, tail;
			struct openmiop_slot *slot;

			if (!bar)
				continue;
			head = load32(&bar->rc_tx_head);
			tail = load32(&bar->rc_tx_tail);
			if (head - tail >= OPENMIOP_SLOTS)
				continue;
			slot = &bar->rc_tx[head % OPENMIOP_SLOTS];
			memcpy(slot->data, buf, (unsigned long)n);
			store32(&slot->len, (u32)n);
			store32(&bar->rc_tx_head, head + 1);
			store32(&bar->rc_kick, head + 1);
		}
	}
}

static void drain_ep(int tap, struct openmiop_bar *bar)
{
	u32 head = load32(&bar->ep_tx_head);
	u32 tail = load32(&bar->ep_tx_tail);
	int budget = 32;

	while (tail != head && budget--) {
		struct openmiop_slot *slot = &bar->ep_tx[tail % OPENMIOP_SLOTS];
		u32 len = load32(&slot->len);

		if (len >= 14 && len <= OPENMIOP_MAX_FRAME)
			sc(SYS_write, tap, (long)slot->data, len, 0, 0, 0);
		tail++;
		head = load32(&bar->ep_tx_head);
	}
	if (tail != load32(&bar->ep_tx_tail))
		store32(&bar->ep_tx_tail, tail);
}

static void publish_peers(void)
{
	int i;

	if (nep < 2)
		return;
	for (i = 0; i < nep; i++) {
		struct ep_slot *me = &eps[i];
		struct ep_slot *peer = &eps[i ^ 1];
		struct openmiop_bar *bar;
		u32 gen;

		if (nep > 2)
			peer = &eps[(i + 1) % nep];
		if (!me->bar || !peer->bar)
			continue;
		bar = me->bar;
		if (load32(&bar->peer_gen) &&
		    load32(&bar->peer_bar_lo) == peer->pci_lo &&
		    load32(&bar->peer_bar_hi) == peer->pci_hi &&
		    mac_same(bar->peer_mac, peer->bar->ep_mac))
			continue;
		memcpy(bar->peer_mac, peer->bar->ep_mac, 6);
		store32(&bar->peer_bar_lo, peer->pci_lo);
		store32(&bar->peer_bar_hi, peer->pci_hi);
		gen = load32(&bar->peer_gen) + 1;
		if (!gen)
			gen = 1;
		store32(&bar->peer_gen, gen);
		wr("peer for ");
		wr(me->sysfs);
		wr(" is ");
		hexout(peer->pci_lo);
		wr("\n");
	}
}

static void scan_eps(void)
{
	unsigned bus, dev;

	for (bus = 0; bus < 16 && nep < MAX_EP; bus++) {
		for (dev = 0; dev < 32 && nep < MAX_EP; dev++) {
			char path[80];
			char sysfs[40];
			char buf[80];
			u32 vendor, device, lo, hi;
			int i, known;
			struct openmiop_bar *bar;

			for (i = 0; i < 80; i++)
				path[i] = 0;
			memcpy(path, "/sys/bus/pci/devices/0000:", 26);
			put_hex2(path + 26, bus);
			path[28] = ':';
			put_hex2(path + 29, dev);
			path[31] = '.';
			path[32] = '0';
			memcpy(sysfs, path, 33);
			sysfs[33] = 0;
			known = 0;
			for (i = 0; i < nep; i++) {
				if (same_path(eps[i].sysfs, sysfs))
					known = 1;
			}
			if (known)
				continue;
			memcpy(path + 33, "/vendor", 8);
			if (read_file(path, buf, sizeof(buf)) < 0)
				continue;
			if (parse_hex(buf, &vendor) || vendor != OPENMIOP_PCI_VENDOR)
				continue;
			memcpy(path + 33, "/device", 8);
			if (read_file(path, buf, sizeof(buf)) < 0)
				continue;
			if (parse_hex(buf, &device) || device != OPENMIOP_PCI_DEVICE)
				continue;
			memcpy(path + 33, "/resource", 9);
			path[42] = 0;
			if (read_file(path, buf, sizeof(buf)) < 0)
				continue;
			if (parse_bar_addr(buf, &lo, &hi) || (lo == 0 && hi == 0))
				continue;
			bar = map_bar(sysfs);
			if (!bar)
				continue;
			if (load32(&bar->magic) != OPENMIOP_MAGIC ||
			    load32(&bar->version) != OPENMIOP_VERSION) {
				wr("skip wrong version ");
				wr(sysfs);
				wr(" magic ");
				hexout(load32(&bar->magic));
				wr("\n");
				sc(SYS_munmap, (long)bar, OPENMIOP_BAR_SIZE, 0, 0, 0, 0);
				continue;
			}
			memcpy(eps[nep].sysfs, sysfs, 40);
			eps[nep].bar = bar;
			eps[nep].pci_lo = lo;
			eps[nep].pci_hi = hi;
			wr("endpoint ");
			wr(sysfs);
			wr(" BAR ");
			hexout(lo);
			wr("\n");
			nep++;
		}
	}
}

static int main_loop(void)
{
	char ifname[IFNAMSIZ];
	u8 mac[6];
	int tap = -1;
	int waits = 0;
	int e;
	long ur;

	memset0(ifname, sizeof(ifname));
	memcpy(ifname, "omi0", 5);
	memset0(eps, sizeof(eps));

	for (;;) {
		if ((waits++ % 200) == 0)
			scan_eps();
		if (!nep) {
			if (waits % 1000 == 1)
				wr("waiting for endpoint\n");
			msleep(1);
			continue;
		}
		if (tap < 0) {
			mac[0] = 0x02;
			ur = sc(SYS_open, (long)"/dev/urandom", O_RDONLY, 0, 0, 0, 0);
			if (ur < 0 || sc(SYS_read, ur, (long)(mac + 1), 5, 0, 0, 0) != 5) {
				mac[1] = 0x4f;
				mac[2] = 0x4d;
				mac[3] = 0x49;
				mac[4] = 0x00;
				mac[5] = 0x01;
			}
			if (ur >= 0)
				sc(SYS_close, ur, 0, 0, 0, 0, 0);
			tap = open_tap(ifname, mac);
			wr("omi0 up\n");
		}
		for (e = 0; e < nep; e++) {
			if (!eps[e].bar)
				continue;
			memcpy(eps[e].bar->rc_mac, mac, 6);
			store32(&eps[e].bar->rc_flags, OPENMIOP_FLAG_UP);
			drain_ep(tap, eps[e].bar);
		}
		publish_peers();
		fanout(tap);
		msleep(1);
	}
}

void _start(void)
{
	main_loop();
	sc(SYS_exit_group, 0, 0, 0, 0, 0, 0);
}
