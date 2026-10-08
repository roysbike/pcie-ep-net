// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Cluster Box side of openmiop, protocol version 4.
 *
 * Data between blades does not pass through this process. It
 *
 *   - finds openmiop endpoints and gives each a node index from the
 *     switch port it sits behind,
 *   - restores BAR0, the command register and a uniform Max Payload
 *     Size after an endpoint reloads (probe resets its config space),
 *   - never sends a request to an endpoint whose link is down, has
 *     not been up for LINK_STABLE_MS, or is about to drop (leave): it
 *     watches Data Link Layer Link Active on the switch port instead,
 *     which the switch answers itself,
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
#define PCI_EXP_LNKSTA		0x12
#define PCI_EXP_LNKSTA_DLLLA	0x2000
#define PCI_EXT_CAP_ID_ERR	0x0001
#define PCI_ERR_UNCOR_STATUS	4
#define PCI_ERR_COR_STATUS	16

/* A downstream port reports Data Link Layer Link Active itself, so
 * reading its Link Status never sends anything over the blade's link.
 * Every request to an endpoint (config or memory) waits until that bit
 * has been set for LINK_STABLE_MS, and stops the moment it clears: a
 * request in flight while a blade's link drops or retrains completes
 * with a timeout on the MT7620A root port, and that can take the whole
 * fabric down.
 */
#define LINK_POLL_MS	5
#define LINK_STABLE_MS	1500
/* After a leave the blade drops its link; wait for that before probing. */
#define LEAVE_DOWN_MS	30000
/* Header check of an active endpoint (magic, epoch, leave flag). */
#define HDR_CHECK_MS	50
/* Gateway poll of an endpoint without recent gateway traffic. */
#define GW_IDLE_MS	10
#define GW_BUSY_MS	100	/* stay at full rate this long after a frame */

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

static int s_eq(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
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

/* ---- PCI config space through sysfs ---- */

static void dev_path(char *out, const char *bdf, const char *file)
{
	scpy(out, "/sys/bus/pci/devices/");
	scat(out, bdf);
	scat(out, "/");
	scat(out, file);
}

static int cfg_rw(const char *bdf, u32 off, void *buf, u32 len, int write)
{
	char path[80];
	long fd, n;

	dev_path(path, bdf, "config");
	fd = sc(SYS_open, (long)path, write ? O_RDWR : O_RDONLY, 0, 0, 0, 0);
	if (fd < 0)
		return -1;
	n = sc(SYS_lseek, fd, (long)off, 0, 0, 0, 0);
	if (n == (long)off)
		n = sc(write ? SYS_write : SYS_read, fd, (long)buf, len, 0, 0, 0);
	else
		n = -1;
	sc(SYS_close, fd, 0, 0, 0, 0, 0);
	return n == (long)len ? 0 : -1;
}

static u32 cfg_rd32(const char *bdf, u32 off)
{
	u32 v = 0xffffffff;

	if (cfg_rw(bdf, off, &v, 4, 0))
		return 0xffffffff;
	return v;
}

static u16 cfg_rd16(const char *bdf, u32 off)
{
	u16 v = 0xffff;

	if (cfg_rw(bdf, off, &v, 2, 0))
		return 0xffff;
	return v;
}

static void cfg_wr32(const char *bdf, u32 off, u32 v)
{
	cfg_rw(bdf, off, &v, 4, 1);
}

static void cfg_wr16(const char *bdf, u32 off, u16 v)
{
	cfg_rw(bdf, off, &v, 2, 1);
}

static u32 find_exp_cap(const char *bdf)
{
	u8 pos = 0;
	int ttl = 48;

	if (cfg_rw(bdf, PCI_CAP_PTR, &pos, 1, 0))
		return 0;
	while (pos >= 0x40 && ttl--) {
		u8 id = 0, next = 0;

		if (cfg_rw(bdf, pos, &id, 1, 0) || cfg_rw(bdf, pos + 1, &next, 1, 0))
			return 0;
		if (id == PCI_CAP_ID_EXP)
			return pos;
		pos = next & 0xfc;
	}
	return 0;
}

/* "0000:03:00.0" from the bus/dev numbers. */
static void make_bdf(char *bdf, unsigned bus, unsigned dev)
{
	scpy(bdf, "0000:00:00.0");
	put_hex2(bdf + 5, bus);
	put_hex2(bdf + 8, dev);
}

static int dev_exists(const char *bdf)
{
	char path[80], buf[16];

	dev_path(path, bdf, "vendor");
	return read_file(path, buf, sizeof(buf)) > 0;
}

/* ---- endpoints ---- */

enum ep_state {
	EP_NONE,	/* nothing known */
	EP_QUIET,	/* hands off until quiet_until: its link may drop */
	EP_PROBE,	/* device in sysfs; config/BAR checked every 200 ms */
	EP_WAIT,	/* BAR mapped; waiting for a v4 header */
	EP_ACTIVE,	/* in the peer tables */
	EP_LEAVING,	/* removed from tables; waiting for peers */
};

struct ep {
	enum ep_state state;
	char bdf[16];
	char parent[16];
	u32 bar_lo, bar_hi;
	u8 *bar;
	u32 epoch;
	u8 mac[6];
	u32 gen;			/* table_gen last written */
	u32 pub_epoch[OMI_MAX_NODES];	/* entry epochs written to it */
	u32 ep_tail;			/* gateway EP->RC consumer */
	u32 rc_head;			/* gateway RC->EP producer */
	u32 leave_start;
	u32 quiet_until;
	u32 next;			/* next config/poll time, ms */
	int warned;
	/* Link of the switch port in front of the endpoint. */
	long pfd;			/* parent config fd, kept open */
	u32 pcap;			/* parent PCIe capability */
	int link;			/* DLLLA at the last sample */
	u32 link_since;			/* when it was last set */
	u32 link_next;			/* next sample */
	u32 link_downs;			/* down events seen */
	u32 need_down;			/* left: probe only after a link drop... */
	u32 down_seen;			/* ...counted from this link_downs value */
	u32 hdr_next;			/* next header check (active) */
	u32 gw_next;			/* next gateway poll */
	u32 gw_last;			/* last frame from the endpoint */
	u32 mps_cap;			/* DEVCAP MPS + 1, read while linked */
};

static struct ep eps[OMI_MAX_NODES];
static u32 now;			/* now_ms() for this loop iteration */
static int tables_dirty;
static int reenum_wanted;
static u32 reenum_after;		/* earliest next re-enumeration */
static int tap = -1;
static u8 tap_mac[6];
static u32 frame[(OMI_GW_SLOT + 3) / 4];

#define HDR(e)		((struct omi_bar_head *)(e)->bar)
#define GW(e)		((struct omi_gw *)((e)->bar + OMI_GW_OFF))

static u8 *gw_slot(struct ep *e, int rc_dir, u32 i)
{
	return e->bar + OMI_GW_OFF + sizeof(struct omi_gw) +
	       ((rc_dir ? OMI_GW_SLOTS : 0) + (i & (OMI_GW_SLOTS - 1))) * OMI_GW_SLOT;
}

static void ts(void)
{
	u32 t = now_ms();

	wr("[");
	decout(t / 1000);
	wr(".");
	wr(t % 1000 < 100 ? (t % 1000 < 10 ? "00" : "0") : "");
	decout(t % 1000);
	wr("] ");
}

static void log_ep(struct ep *e, const char *msg)
{
	ts();
	wr("node ");
	decout((u32)(e - eps));
	wr(" ");
	wr(e->bdf);
	wr(": ");
	wr(msg);
	wr("\n");
}

/*
 * Node index = rank of the EP's parent bridge among the downstream
 * ports of the same switch. It depends only on the slot, so it is the
 * same after reloads and reboots, and empty slots keep their number.
 */
/* Root port above the endpoints. Re-enumeration starts there: its
 * bridge window is sized like the ones below it, and a window sized
 * for two endpoints cannot hold a third.
 */
static char root_port[16];

static int node_index(const char *bdf, char *parent)
{
	char path[80], link[160];
	long n;
	int i, slash = -1, prev = -1, idx = 0;
	unsigned pbus, pdev, d;

	dev_path(path, bdf, "");
	path[slen(path) - 1] = 0;
	n = sc(SYS_readlink, (long)path, (long)link, sizeof(link) - 1, 0, 0, 0);
	if (n <= 0)
		return -1;
	link[n] = 0;
	for (i = 0; i < n; i++) {
		if (link[i] == '/') {
			prev = slash;
			slash = i;
		}
	}
	if (prev < 0 || slash - prev - 1 != 12)
		return -1;
	memcpy(parent, link + prev + 1, 12);
	parent[12] = 0;
	/* ".../devices/pci0000:00/0000:00:00.0/...": the first BDF after
	 * the host bridge component is the root port.
	 */
	{
		const char *h = link;
		int k;

		for (k = 0; k + 7 < n; k++)
			if (link[k] == 'p' && link[k + 1] == 'c' && link[k + 2] == 'i' &&
			    link[k + 7] == ':') {
				h = link + k;
				break;
			}
		while (*h && *h != '/')
			h++;
		if (*h == '/' && h[13] == '/') {
			memcpy(root_port, h + 1, 12);
			root_port[12] = 0;
		}
	}
	pbus = (unsigned)(hexval(parent[5]) * 16 + hexval(parent[6]));
	pdev = (unsigned)(hexval(parent[8]) * 16 + hexval(parent[9]));
	for (d = 0; d < pdev; d++) {
		char sib[16];

		make_bdf(sib, pbus, d);
		if (dev_exists(sib))
			idx++;
	}
	return idx;
}

/* Endpoints sit alone on the secondary bus of a downstream port, so
 * only device 0 of each bus is checked. Returns 1 if a downstream
 * port of the switch has no openmiop EP yet.
 */
static int scan(void)
{
	char ports_bus[16];
	unsigned bus, dev = 0;
	int found = 0, ports = 0, want = 0;

	ports_bus[0] = 0;
	for (bus = 1; bus < 32; bus++) {
		{
			char bdf[16], path[80], buf[16], parent[16];
			u32 v;
			int idx;

			make_bdf(bdf, bus, dev);
			dev_path(path, bdf, "vendor");
			if (read_file(path, buf, sizeof(buf)) < 0)
				continue;
			if (parse_hex64(buf, &v, 0) || v != OPENMIOP_PCI_VENDOR)
				continue;
			dev_path(path, bdf, "device");
			if (read_file(path, buf, sizeof(buf)) < 0 ||
			    parse_hex64(buf, &v, 0) || v != OPENMIOP_PCI_DEVICE)
				continue;
			idx = node_index(bdf, parent);
			if (idx < 0 || idx >= (int)OMI_MAX_NODES)
				continue;
			found++;
			if (!ports_bus[0])
				scpy(ports_bus, parent);
			if (eps[idx].state != EP_NONE)
				continue;
			memset(&eps[idx], 0, sizeof(eps[idx]));
			scpy(eps[idx].bdf, bdf);
			scpy(eps[idx].parent, parent);
			eps[idx].state = EP_PROBE;
			log_ep(&eps[idx], "found");
		}
	}
	if (!found) {
		/* No EP yet, so no parent known: the Cluster Box switch has
		 * its downstream ports on bus 2. Elsewhere, rescan blindly.
		 */
		if (!dev_exists("0000:02:00.0"))
			return 1;
		scpy(ports_bus, "0000:02:00.0");
	}
	/* A rescan probes the bus behind every downstream port. Only ask
	 * for one when a port without an EP has had its link up since the
	 * previous scan: a blade trained after we enumerated. Probing a
	 * port whose link is down or still training is what we avoid.
	 */
	bus = (unsigned)(hexval(ports_bus[5]) * 16 + hexval(ports_bus[6]));
	for (dev = 0; dev < 32; dev++) {
		static u32 up_before;
		char sib[16];
		u32 cap;
		int i, has = 0;

		make_bdf(sib, bus, dev);
		if (!dev_exists(sib))
			continue;
		ports++;
		for (i = 0; i < (int)OMI_MAX_NODES; i++)
			if (eps[i].state != EP_NONE && s_eq(eps[i].parent, sib))
				has = 1;
		cap = has ? 0 : find_exp_cap(sib);
		if (cap && (cfg_rd16(sib, cap + PCI_EXP_LNKSTA) & PCI_EXP_LNKSTA_DLLLA)) {
			if (up_before & (1u << dev))
				want = 1;
			up_before |= 1u << dev;
		} else {
			up_before &= ~(1u << dev);
		}
	}
	(void)ports;
	return want;
}

/* MPS that every EP and every port in front of an EP supports. */
static u32 common_mps(void)
{
	u32 mps = 5;	/* 4096 */
	int i;

	for (i = 0; i < (int)OMI_MAX_NODES; i++) {
		struct ep *e = &eps[i];
		u32 cap, v;

		if (e->state == EP_NONE)
			continue;
		/* Only the cached value: this EP's link may be down now. */
		if (e->mps_cap && e->mps_cap - 1 < mps)
			mps = e->mps_cap - 1;
		cap = find_exp_cap(e->parent);
		if (cap) {
			v = (cfg_rd16(e->parent, cap + PCI_EXP_DEVCTL) >> 5) & 7;
			if (v < mps)
				mps = v;
		}
	}
	return mps == 5 ? 0 : mps;
}

/*
 * The EP resets its config space when its driver loads. Put back the
 * BAR the kernel assigned, Memory Space + Bus Master, and the MPS all
 * EPs share: an EP left at a larger MPS sends TLPs that a peer with a
 * smaller one drops as malformed.
 */
static int ep_restore(struct ep *e)
{
	char path[80], buf[128];
	u32 lo, hi, cmd, cap, mps;
	u16 ctl;

	if ((cfg_rd32(e->bdf, 0) & 0xffff) != OPENMIOP_PCI_VENDOR)
		return -1;	/* link down or not ours */
	dev_path(path, e->bdf, "resource");
	if (read_file(path, buf, sizeof(buf)) < 0 || parse_hex64(buf, &lo, &hi))
		return -1;
	if (!lo && !hi) {
		if (!e->warned++)
			log_ep(e, "BAR0 has no address; the bridge windows need re-sizing");
		reenum_wanted = 1;
		return -1;
	}
	e->bar_lo = lo;
	e->bar_hi = hi;
	if ((cfg_rd32(e->bdf, PCI_BAR0) & ~0xfu) != lo ||
	    cfg_rd32(e->bdf, PCI_BAR0 + 4) != hi) {
		cfg_wr32(e->bdf, PCI_BAR0, lo);
		cfg_wr32(e->bdf, PCI_BAR0 + 4, hi);
		log_ep(e, "BAR0 restored");
	}
	cap = find_exp_cap(e->bdf);
	if (cap)
		e->mps_cap = (cfg_rd32(e->bdf, cap + PCI_EXP_DEVCAP) & 7) + 1;
	mps = common_mps();
	if (cap) {
		ctl = cfg_rd16(e->bdf, cap + PCI_EXP_DEVCTL);
		if (((ctl >> 5) & 7) != mps) {
			ctl = (u16)((ctl & ~PCI_EXP_DEVCTL_MPS) | (mps << 5));
			cfg_wr16(e->bdf, cap + PCI_EXP_DEVCTL, ctl);
			log_ep(e, "MPS set");
		}
	}
	cmd = cfg_rd32(e->bdf, PCI_COMMAND) & 0xffff;
	if ((cmd & (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER)) !=
	    (PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER))
		cfg_wr16(e->bdf, PCI_COMMAND,
			 (u16)(cmd | PCI_COMMAND_MEMORY | PCI_COMMAND_MASTER));
	return 0;
}

static int ep_map(struct ep *e)
{
	char path[80];
	long fd;
	void *map;

	dev_path(path, e->bdf, "resource0");
	fd = sc(SYS_open, (long)path, O_RDWR, 0, 0, 0, 0);
	if (fd < 0)
		return -1;
	map = (void *)sc(SYS_mmap2, 0, OPENMIOP_BAR_SIZE, PROT_READ | PROT_WRITE,
			 MAP_SHARED, fd, 0);
	sc(SYS_close, fd, 0, 0, 0, 0, 0);
	if ((unsigned long)map >= (unsigned long)-4095)
		return -1;
	e->bar = map;
	return 0;
}

static void ep_unmap(struct ep *e)
{
	if (e->bar)
		sc(SYS_munmap, (long)e->bar, OPENMIOP_BAR_SIZE, 0, 0, 0, 0);
	e->bar = 0;
}

/* ---- link state of the switch port in front of an endpoint ---- */

static u32 find_ext_cap(const char *bdf, u32 id)
{
	u32 pos = 0x100, h;
	int ttl = 64;

	while (pos >= 0x100 && ttl--) {
		h = cfg_rd32(bdf, pos);
		if (!h || h == 0xffffffff)
			return 0;
		if ((h & 0xffff) == id)
			return pos;
		pos = (h >> 20) & 0xffc;
	}
	return 0;
}

static u16 parent_lnksta(struct ep *e)
{
	char path[80];
	u16 v = 0xffff;

	if (!e->pcap) {
		e->pcap = find_exp_cap(e->parent);
		if (!e->pcap)
			return 0xffff;
	}
	if (e->pfd <= 0) {
		dev_path(path, e->parent, "config");
		e->pfd = sc(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
		if (e->pfd < 0) {
			e->pfd = 0;
			return 0xffff;
		}
	}
	if (sc(SYS_lseek, e->pfd, (long)(e->pcap + PCI_EXP_LNKSTA), 0, 0, 0, 0) !=
	    (long)(e->pcap + PCI_EXP_LNKSTA) ||
	    sc(SYS_read, e->pfd, (long)&v, 2, 0, 0, 0) != 2)
		return 0xffff;
	return v;
}

static void port_dump(const char *bdf)
{
	u32 cap = find_exp_cap(bdf), aer = find_ext_cap(bdf, PCI_EXT_CAP_ID_ERR);

	wr("  ");
	wr(bdf);
	if (cap) {
		wr(" lnksta ");
		hexout(cfg_rd16(bdf, cap + PCI_EXP_LNKSTA));
	}
	if (aer) {
		wr(" uesta ");
		hexout(cfg_rd32(bdf, aer + PCI_ERR_UNCOR_STATUS));
		wr(" cesta ");
		hexout(cfg_rd32(bdf, aer + PCI_ERR_COR_STATUS));
	}
	wr(" bus ");
	hexout(cfg_rd32(bdf, 0x18));
	wr("\n");
}

/* Root port, switch upstream port and the downstream ports: all answer
 * config reads themselves, none of this goes over a blade link.
 */
static void fabric_dump(const char *why)
{
	char up[16];
	int i;

	ts();
	wr("fabric: ");
	wr(why);
	wr("\n");
	if (root_port[0])
		port_dump(root_port);
	up[0] = 0;
	for (i = 0; i < (int)OMI_MAX_NODES; i++) {
		if (eps[i].state == EP_NONE)
			continue;
		if (!up[0]) {
			char path[80], link[160];
			long n;
			int k, slash = -1, prev = -1, pp = -1;

			/* .../<root>/<upstream>/<downstream>/<ep> */
			dev_path(path, eps[i].bdf, "");
			path[slen(path) - 1] = 0;
			n = sc(SYS_readlink, (long)path, (long)link, sizeof(link) - 1, 0, 0, 0);
			for (k = 0; k < n; k++)
				if (link[k] == '/') {
					pp = prev;
					prev = slash;
					slash = k;
				}
			if (n > 0 && pp >= 0 && prev - pp - 1 == 12) {
				memcpy(up, link + pp + 1, 12);
				up[12] = 0;
				port_dump(up);
			}
		}
		port_dump(eps[i].parent);
	}
}

/*
 * Sample the link of e's switch port (at most every LINK_POLL_MS unless
 * force). Returns 1 when it is up and has been for LINK_STABLE_MS.
 */
static int link_ok(struct ep *e, int force)
{
	u16 v;
	int up;

	if (force || (s32)(now - e->link_next) >= 0) {
		e->link_next = now + LINK_POLL_MS;
		v = parent_lnksta(e);
		up = v != 0xffff && (v & PCI_EXP_LNKSTA_DLLLA);
		if (up && !e->link)
			e->link_since = now;
		if (!up && e->link) {
			e->link_downs++;
			log_ep(e, "link down at the switch port");
		}
		e->link = up;
	}
	return e->link && now - e->link_since >= LINK_STABLE_MS;
}

/*
 * Forget the mapping and leave the EP alone for quiet_ms. Its link is
 * about to drop or just dropped: a read (config or memory) that is in
 * flight when a link goes down can be discarded by the switch, and a
 * completion timeout on the MT7620A root port takes the whole fabric
 * down. Nothing goes to the EP again until its switch port has shown
 * the link up for LINK_STABLE_MS (link_ok()).
 */
static void ep_drop(struct ep *e, const char *why, u32 quiet_ms)
{
	log_ep(e, why);
	ep_unmap(e);
	e->state = EP_QUIET;
	e->quiet_until = now + quiet_ms;
	e->warned = 0;
	tables_dirty = 1;
}

static void ep_activate(struct ep *e, int idx)
{
	struct omi_bar_head *h = HDR(e);
	int i;

	e->epoch = rd32(&h->hdr.epoch);
	for (i = 0; i < 6; i++)
		e->mac[i] = h->hdr.mac[i];
	for (i = 0; i < (int)OMI_MAX_NODES; i++)
		e->pub_epoch[i] = 0xffffffff;	/* force a full write */
	e->gen = rd32(&h->ctl.table_gen);
	e->ep_tail = rd32(&GW(e)->ep_tail);
	e->rc_head = rd32(&GW(e)->rc_head);
	memcpy(h->ctl.mac, tap_mac, 6);
	h->ctl.self_idx = (u8)idx;
	wr32(&h->ctl.down_ack, 0);
	wr32(&h->ctl.ep_epoch, e->epoch);
	wr32(&h->ctl.flags, OMI_RC_UP);
	e->state = EP_ACTIVE;
	tables_dirty = 1;
	ts();
	wr("node ");
	decout((u32)idx);
	wr(" active, BAR ");
	hexout(e->bar_lo);
	wr(" epoch ");
	hexout(e->epoch);
	wr("\n");
}

/* Write node k's entry into e's table if it changed. */
static int table_put(struct ep *e, int k, struct ep *src)
{
	struct omi_peer_entry *pe = &HDR(e)->peers[k];
	u32 want = src ? src->epoch : 0;
	int i;

	if (e->pub_epoch[k] == want)
		return 0;
	wr32(&pe->epoch, 0);
	if (src) {
		wr32(&pe->bar_lo, src->bar_lo);
		wr32(&pe->bar_hi, src->bar_hi);
		for (i = 0; i < 6; i++)
			pe->mac[i] = src->mac[i];
		io_mb();
		wr32(&pe->epoch, want);
	}
	e->pub_epoch[k] = want;
	return 1;
}

static void tables_publish(void)
{
	int i, k;

	tables_dirty = 0;
	for (i = 0; i < (int)OMI_MAX_NODES; i++) {
		struct ep *e = &eps[i];
		int changed = 0;

		if (e->state != EP_ACTIVE)
			continue;
		for (k = 0; k < (int)OMI_MAX_NODES; k++) {
			struct ep *src = &eps[k];

			if (k == i)
				continue;
			changed |= table_put(e, k, src->state == EP_ACTIVE ? src : 0);
		}
		if (changed) {
			e->gen++;
			wr32(&HDR(e)->ctl.table_gen, e->gen);
		}
	}
}

/* Every active EP applied the table that no longer lists the leaver? */
static int peers_detached(void)
{
	int i;

	for (i = 0; i < (int)OMI_MAX_NODES; i++) {
		struct ep *e = &eps[i];

		if (e->state != EP_ACTIVE)
			continue;
		if ((s32)(rd32(&HDR(e)->hdr.table_seen) - e->gen) < 0)
			return 0;
	}
	return 1;
}

/* ---- gateway ---- */

static void tap_put(const u32 *buf, u32 len)
{
	if (tap >= 0)
		sc(SYS_write, tap, (long)buf, len, 0, 0, 0);
}

static void gw_put(struct ep *e, const u32 *buf, u32 len, u32 flags)
{
	struct omi_gw *gw = GW(e);
	u8 *slot;
	u32 tail, i;

	tail = rd32(&gw->rc_tail);
	if (e->rc_head - tail >= OMI_GW_SLOTS)
		return;	/* full: drop, the gateway is best effort */
	slot = gw_slot(e, 1, e->rc_head);
	for (i = 0; i < (len + 3) / 4; i++)
		((volatile u32 *)(slot + sizeof(struct omi_slot_hdr)))[i] = buf[i];
	wr32(slot + offsetof(struct omi_slot_hdr, flags), flags);
	wr32(slot + offsetof(struct omi_slot_hdr, mask), 0);
	wr32(slot + offsetof(struct omi_slot_hdr, len), len);
	e->rc_head++;
	wr32(&gw->rc_head, e->rc_head);
}

static int ep_by_mac(const u8 *mac)
{
	int i;

	for (i = 0; i < (int)OMI_MAX_NODES; i++)
		if (eps[i].state == EP_ACTIVE && mac_eq(eps[i].mac, mac))
			return i;
	return -1;
}

/* A frame from node src (or from the TAP, src < 0). mask: nodes that
 * already have it.
 */
static void dispatch(int src, const u32 *buf, u32 len, u32 mask)
{
	const u8 *dest = (const u8 *)buf;
	int k;

	if (src >= 0)
		mask |= 1u << src;
	if (!(dest[0] & 1)) {
		if (mac_eq(dest, tap_mac)) {
			tap_put(buf, len);
			return;
		}
		k = ep_by_mac(dest);
		if (k >= 0) {
			if (!(mask & (1u << k)))
				gw_put(&eps[k], buf, len, src >= 0 ? OMI_GW_RELAYED : 0);
			return;
		}
	}
	/* Broadcast, multicast, unknown unicast: everyone else. */
	if (src >= 0)
		tap_put(buf, len);
	for (k = 0; k < (int)OMI_MAX_NODES; k++)
		if (eps[k].state == EP_ACTIVE && !(mask & (1u << k)))
			gw_put(&eps[k], buf, len, src >= 0 ? OMI_GW_RELAYED : 0);
}

static int gw_drain(int idx)
{
	struct ep *e = &eps[idx];
	struct omi_gw *gw = GW(e);
	u32 head = rd32(&gw->ep_head);
	int budget = 32, got = 0;

	if (head - e->ep_tail > OMI_GW_SLOTS)
		e->ep_tail = head;	/* garbage; resync */
	while (e->ep_tail != head && budget--) {
		u8 *slot = gw_slot(e, 0, e->ep_tail);
		u32 len = rd32(slot + offsetof(struct omi_slot_hdr, len));
		u32 mask = rd32(slot + offsetof(struct omi_slot_hdr, mask));
		u32 i;

		if (len >= 14 && len <= OMI_GW_DATA) {
			for (i = 0; i < (len + 3) / 4; i++)
				frame[i] = ((volatile u32 *)(slot + sizeof(struct omi_slot_hdr)))[i];
			dispatch(idx, frame, len, mask);
		}
		e->ep_tail++;
		got++;
	}
	if (got)
		wr32(&gw->ep_tail, e->ep_tail);
	return got;
}

static void tap_drain(void)
{
	int budget = 32;

	while (budget--) {
		long n = sc(SYS_read, tap, (long)frame, sizeof(frame), 0, 0, 0);

		if (n < 14)
			break;
		if (n <= (long)OMI_GW_DATA)
			dispatch(-1, frame, (u32)n, 0);
	}
}

static int open_tap(const u8 mac[6])
{
	struct ifreq_flags ifr;
	struct ifreq_hw hw;
	long fd, s;

	fd = sc(SYS_open, (long)"/dev/net/tun", O_RDWR | O_NONBLOCK, 0, 0, 0, 0);
	if (fd < 0)
		die("open tun", fd);
	memset(&ifr, 0, sizeof(ifr));
	scpy(ifr.name, "omi0");
	ifr.flags = IFF_TAP | IFF_NO_PI;
	s = sc(SYS_ioctl, fd, TUNSETIFF, (long)&ifr, 0, 0, 0);
	if (s < 0)
		die("TUNSETIFF", s);

	s = sc(SYS_socket, 2 /* AF_INET */, 1 /* MIPS SOCK_DGRAM */, 0, 0, 0, 0);
	if (s < 0)
		die("socket", s);
	memset(&hw, 0, sizeof(hw));
	memcpy(hw.name, ifr.name, IFNAMSIZ);
	hw.family = ARPHRD_ETHER;
	memcpy(hw.mac, mac, 6);
	if (sc(SYS_ioctl, s, SIOCSIFHWADDR, (long)&hw, 0, 0, 0) < 0)
		wr("set mac failed\n");
	memset(&ifr.flags, 0, sizeof(ifr.flags));
	if (sc(SYS_ioctl, s, SIOCGIFFLAGS, (long)&ifr, 0, 0, 0) == 0) {
		ifr.flags |= IFF_UP;
		sc(SYS_ioctl, s, SIOCSIFFLAGS, (long)&ifr, 0, 0, 0);
	}
	sc(SYS_close, s, 0, 0, 0, 0, 0);
	return (int)fd;
}

/* Stable TAP MAC: eth0's address, locally administered, last byte
 * changed so it never equals eth0.
 */
static void make_tap_mac(u8 *mac)
{
	char buf[32];
	int i;

	mac[0] = 0x02;
	mac[1] = 0x4f;
	mac[2] = 0x4d;
	mac[3] = 0x49;
	mac[4] = 0x00;
	mac[5] = 0x01;
	if (read_file("/sys/class/net/eth0/address", buf, sizeof(buf)) < 17)
		return;
	for (i = 0; i < 6; i++) {
		int a = hexval(buf[i * 3]), b = hexval(buf[i * 3 + 1]);

		if (a < 0 || b < 0)
			return;
		mac[i] = (u8)(a * 16 + b);
	}
	mac[0] = (mac[0] & 0xfc) | 0x02;
	mac[5] ^= 0x4d;
}

/* ---- main loop ---- */

/*
 * Bridge windows are sized when they are first assigned: a blade that
 * trains its link after that gets no BAR address. Re-enumerate the
 * switch, but only after every endpoint stopped peer-to-peer traffic:
 * while Linux rewrites the bridge windows, a write in flight could be
 * routed to the wrong blade.
 *
 * 1. clear every peer table and wait until each endpoint applied it;
 * 2. forget all endpoints (no MMIO from here on);
 * 3. remove the switch upstream port and rescan.
 *
 * The endpoints keep their epochs; scan() finds them again, their BARs
 * may move, and the new tables make the peers reconnect.
 */
static void reenumerate(void)
{
	char path[80];
	u32 start;
	int i, k;

	reenum_wanted = 0;
	reenum_after = now + 60000;
	if (!root_port[0])
		return;
	wr("re-enumerating from the root port ");
	wr(root_port);
	wr("\n");

	for (i = 0; i < (int)OMI_MAX_NODES; i++) {
		struct ep *e = &eps[i];
		int changed = 0;

		if (e->state != EP_ACTIVE)
			continue;
		for (k = 0; k < (int)OMI_MAX_NODES; k++)
			if (k != i)
				changed |= table_put(e, k, 0);
		if (changed) {
			e->gen++;
			wr32(&HDR(e)->ctl.table_gen, e->gen);
		}
	}
	start = now_ms();
	while (!peers_detached() && now_ms() - start < 2000)
		msleep(10);

	for (i = 0; i < (int)OMI_MAX_NODES; i++) {
		ep_unmap(&eps[i]);
		if (eps[i].pfd > 0)
			sc(SYS_close, eps[i].pfd, 0, 0, 0, 0, 0);
		memset(&eps[i], 0, sizeof(eps[i]));
	}

	dev_path(path, root_port, "remove");
	write_file(path, "1");
	msleep(500);
	write_file("/sys/bus/pci/rescan", "1");
	msleep(500);
}

static int due(struct ep *e, u32 period)
{
	if ((s32)(now - e->next) < 0)
		return 0;
	e->next = now + period;
	return 1;
}

static void ep_step(int idx)
{
	struct ep *e = &eps[idx];
	struct omi_bar_head *h = HDR(e);
	u32 magic, flags, poll;
	int ok;

	if (e->state == EP_NONE)
		return;
	ok = link_ok(e, 0);
	if (e->bar && !e->link && e->state != EP_LEAVING) {
		/* Not a single request more to this endpoint. */
		ep_drop(e, "link lost", 0);
		fabric_dump("link lost");
		return;
	}

	switch (e->state) {
	case EP_NONE:
		return;
	case EP_QUIET:
		if ((s32)(now - e->quiet_until) < 0 || !ok)
			return;
		/* A leaver drops its link right after the ack: until that
		 * happened, its link being up means nothing.
		 */
		if (e->need_down && e->link_downs == e->down_seen &&
		    (s32)(now - e->need_down) < 0)
			return;
		e->need_down = 0;
		e->state = EP_PROBE;
		return;
	case EP_PROBE:
		if (!due(e, 200) || !link_ok(e, 1))
			return;
		if (ep_restore(e) || ep_map(e))
			return;
		e->state = EP_WAIT;
		return;
	case EP_WAIT:
		if (!due(e, 100) || !link_ok(e, 1))
			return;
		magic = rd32(&h->hdr.magic);
		flags = rd32(&h->hdr.flags);
		if (magic == 0xffffffff) {
			ep_drop(e, "BAR not decoding", 500);
			return;
		}
		if (magic != OPENMIOP_MAGIC) {
			if (magic && !e->warned++) {
				log_ep(e, "BAR decodes but holds no openmiop header, magic");
				hexout(magic);
				wr("\n");
			}
			return;
		}
		if (!(flags & OMI_F_UP) || (flags & OMI_F_DOWN))
			return;
		if (rd32(&h->hdr.version) != OPENMIOP_VERSION) {
			if (!e->warned++)
				log_ep(e, "wrong protocol version");
			return;
		}
		ep_activate(e, idx);
		e->hdr_next = now + HDR_CHECK_MS;
		e->gw_next = now;
		return;
	case EP_LEAVING:
		/* No reads of the leaver: it drops its link right after
		 * the ack. Only the other EPs are read here.
		 */
		if (!e->link || peers_detached() || now - e->leave_start > 1000) {
			if (e->link)
				wr32(&h->ctl.down_ack, e->epoch);
			ep_drop(e, "detached", 3000);
			e->need_down = now + LEAVE_DOWN_MS;
			e->down_seen = e->link_downs - (e->link ? 0 : 1);
		}
		return;
	case EP_ACTIVE:
		break;
	}

	if ((s32)(now - e->hdr_next) >= 0) {
		e->hdr_next = now + HDR_CHECK_MS;
		if (!link_ok(e, 1)) {
			ep_drop(e, "link lost", 0);
			fabric_dump("link lost");
			return;
		}
		magic = rd32(&h->hdr.magic);
		if (magic != OPENMIOP_MAGIC || rd32(&h->hdr.epoch) != e->epoch) {
			ep_drop(e, "reset without leaving", 500);
			fabric_dump("reset without leaving");
			return;
		}
		flags = rd32(&h->hdr.flags);
		if (flags & OMI_F_DOWN) {
			log_ep(e, "leaving");
			e->state = EP_LEAVING;
			e->leave_start = now;
			tables_dirty = 1;
			return;
		}
	}
	/* Gateway: every loop while frames flow, slower when idle, so an
	 * unannounced reset of an idle blade rarely meets a read in flight.
	 */
	poll = now - e->gw_last < GW_BUSY_MS ? 0 : GW_IDLE_MS;
	if ((s32)(now - e->gw_next) < 0)
		return;
	e->gw_next = now + poll;
	if (poll && !link_ok(e, 1))
		return;
	if (gw_drain(idx))
		e->gw_last = now;
}

void _start(void)
{
	u32 next_scan, next_rescan;
	char buf[8];
	int i;

	memset(eps, 0, sizeof(eps));
	make_tap_mac(tap_mac);
	tap = open_tap(tap_mac);
	wr("openmiop-rc protocol ");
	decout(OPENMIOP_VERSION);
	wr(", omi0 up\n");

	next_scan = now_ms();
	next_rescan = next_scan + 10000;
	for (;;) {
		now = now_ms();
		/* A rescan finds blades that trained their link after the
		 * Cluster Box booted. Only while a port is still empty.
		 */
		if ((s32)(now - next_scan) >= 0) {
			next_scan = now + 2000;
			if (scan() && (s32)(now - next_rescan) >= 0) {
				next_rescan = now + 10000;
				write_file("/sys/bus/pci/rescan", "1");
			}
		}
		/* Operators can ask for it: touch /var/run/openmiop-reenumerate */
		if (read_file("/var/run/openmiop-reenumerate", buf, sizeof(buf)) >= 0) {
			sc(4010 /* unlink */, (long)"/var/run/openmiop-reenumerate", 0, 0, 0, 0, 0);
			reenum_wanted = 1;
			reenum_after = now;
		}
		if (reenum_wanted && (s32)(now - reenum_after) >= 0) {
			reenumerate();
			continue;
		}
		for (i = 0; i < (int)OMI_MAX_NODES; i++)
			ep_step(i);
		if (tables_dirty)
			tables_publish();
		tap_drain();
		msleep(1);
	}
}
