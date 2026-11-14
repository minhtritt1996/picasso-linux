/*
 * Minimal init for the picasso mainline ramdisk.
 *
 * Prints what the kernel reached, dumps /pstore, brings up a USB CDC-ACM
 * gadget and serves a tiny console on /dev/ttyGS0 (reboot, cat, uname, mem).
 * `reboot` warm-resets the phone; with the boot image installed the loader
 * comes back up, which is the XNU-style test loop.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#include <poll.h>
#include <termios.h>
#include <stdarg.h>

static int read_file(const char *path, char *buf, size_t len)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = read(fd, buf, len - 1);
	close(fd);
	if (n < 0)
		return -1;
	buf[n] = 0;
	return (int)n;
}


/* userland messages the loader can pick up after a reset (kernel printk ring) */

/* last resort debug channel: a plain-text area at the end of the reserved
 * window, written straight through /dev/mem (the loader dumps the window) */
static int memfd = -2;
static unsigned int memoff;

static void mlog(const char *fmt, ...)
{
	char buf[200];
	va_list ap;
	int n;

	if (memfd == -2) {
		static char zero[0x1000];

		memfd = open("/dev/mem", O_RDWR | O_SYNC);
		if (memfd >= 0)
			(void)!pwrite(memfd, zero, sizeof(zero), 0xF3FF0000ULL);
	}
	if (memfd < 0)
		return;
	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n <= 0 || memoff + (unsigned)n > 0xF000)
		return;
	if (pwrite(memfd, buf, (size_t)n, 0xF3FF0000ULL + memoff) == n)
		memoff += (unsigned)n;
}

static void klog(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	int fd, n;

	va_start(ap, fmt);
	n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	fd = open("/dev/kmsg", O_WRONLY);
	if (fd < 0) {
		mknod("/dev/kmsg", S_IFCHR | 0600, makedev(1, 11));
		fd = open("/dev/kmsg", O_WRONLY);
	}
	if (fd < 0)
		return;
	(void)!write(fd, buf, (size_t)(n > 0 ? n : 0));
	close(fd);
}

static void write_file(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY);

	if (fd < 0)
		return;
	(void)!write(fd, s, strlen(s));
	close(fd);
}

static void print_prop(const char *path, const char *label)
{
	char buf[512];

	if (read_file(path, buf, sizeof(buf)) > 0) {
		buf[strcspn(buf, "\n")] = 0;
		printf("%-14s %s\n", label, buf);
	}
}

static int count_cpus(void)
{
	char buf[8192];
	char *p;
	int n = 0;

	if (read_file("/proc/cpuinfo", buf, sizeof(buf)) <= 0)
		return -1;
	for (p = buf; (p = strstr(p, "processor")); p++)
		n++;
	return n;
}

static long memtotal_kb(void)
{
	char buf[8192];
	char *p;
	long v = -1;

	if (read_file("/proc/meminfo", buf, sizeof(buf)) <= 0)
		return -1;
	p = strstr(buf, "MemTotal:");
	if (p)
		v = atol(p + 9);
	return v;
}


static int cmdline_seconds(const char *key)
{
	char buf[1024];
	char *p;

	if (read_file("/proc/cmdline", buf, sizeof(buf)) <= 0)
		return -1;
	p = strstr(buf, key);
	return p ? atoi(p + strlen(key)) : -1;
}


/* true when the kernel cmdline contains the given token */
static int cmdline_flag(const char *flag)
{
	static char buf[1024];
	size_t n = strlen(flag);

	if (read_file("/proc/cmdline", buf, sizeof(buf)) <= 0)
		return 0;
	return strstr(buf, flag) && (strstr(buf, flag)[n] == ' ' ||
				     strstr(buf, flag)[n] == '\n' ||
				     strstr(buf, flag)[n] == 0);
}

/* find the block device of a GPT partition by its PARTNAME */
static int find_part(const char *name, char *dev, size_t devlen)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char path[256], buf[1024];
		char *p;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
		if (read_file(path, buf, sizeof(buf)) <= 0)
			continue;
		p = strstr(buf, "PARTNAME=");
		if (!p || strncmp(p + 9, name, strlen(name)) != 0 ||
		    p[9 + strlen(name)] != '\n')
			continue;
		p = strstr(buf, "DEVNAME=");
		if (!p)
			break;
		snprintf(dev, devlen, "/dev/%.*s", (int)strcspn(p + 8, "\n"), p + 8);
		closedir(d);
		return 0;
	}
	closedir(d);
	return -1;
}

/* Paint /dev/fb0 with noise: the panel must then show snow. If it keeps the
 * bootloader splash instead, the DRM framebuffer is not being scanned out. */
static void paint_fb(void)
{
	char buf[65536];
	int fd, ur, i;

	fd = open("/dev/fb0", O_WRONLY);
	if (fd < 0) {
		klog("fb0: open failed: %s\n", strerror(errno));
		return;
	}
	ur = open("/dev/urandom", O_RDONLY);
	for (i = 0; i < 240 && ur >= 0; i++) {
		ssize_t n = read(ur, buf, sizeof(buf));

		if (n <= 0 || write(fd, buf, n) != n)
			break;
	}
	if (ur >= 0)
		close(ur);
	close(fd);
	klog("picasso-init: fb0 painted %d x 64K\n", i);
}

/* create every parent directory of path (absolute) */
static void mkdir_parents(const char *path)
{
	char tmp[256];
	char *p;

	snprintf(tmp, sizeof(tmp), "%s", path);
	for (p = tmp + 1; *p; p++) {
		if (*p != '/')
			continue;
		*p = 0;
		mkdir(tmp, 0755);
		*p = '/';
	}
	mkdir(tmp, 0755);
}

static int try_mount(const char *dev, const char *dir, const char *type)
{
	return mount(dev, dir, type, MS_RDONLY, NULL);
}

/* unbind+bind a platform device so probe runs again with firmware present */
static void rebind(const char *drv, const char *dev)
{
	char path[256];
	int fd;

	snprintf(path, sizeof(path), "/sys/bus/platform/drivers/%s/unbind", drv);
	fd = open(path, O_WRONLY);
	if (fd >= 0) {
		write(fd, dev, strlen(dev));
		close(fd);
	}
	snprintf(path, sizeof(path), "/sys/bus/platform/drivers/%s/bind", drv);
	fd = open(path, O_WRONLY);
	if (fd < 0) {
		klog("firmware: no bind for %s\n", dev);
		return;
	}
	if (write(fd, dev, strlen(dev)) < 0)
		klog("firmware: bind %s failed: %s\n", dev, strerror(errno));
	else
		klog("firmware: bound %s\n", dev);
	close(fd);
}

/* scan the loader log partition for an exception banner (previous boot) */
static void scan_logdump(void)
{
	static char buf[1 << 20];
	char dev[64];
	ssize_t n;
	int fd;

	if (find_part("logdump", dev, sizeof(dev)) < 0) {
		klog("logdump: no partition\n");
		return;
	}
	fd = open(dev, O_RDONLY);
	if (fd < 0)
		return;
	n = read(fd, buf, sizeof(buf));
	close(fd);
	if (n <= 0)
		return;
	klog("logdump: %zd bytes\n", n);
	{
		char *p = buf, *end = buf + n;

		while (p < end) {
			char *nl = memchr(p, '\n', end - p);
			size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

			if (len < 200 &&
			    (memmem(p, len, "Exception", 9) ||
			     memmem(p, len, "Synchronous", 11) ||
			     memmem(p, len, "PANIC", 5) ||
			     memmem(p, len, "panic", 5)))
				klog("logdump| %.*s\n", (int)len, p);
			if (!nl)
				break;
			p = nl + 1;
		}
	}
}

/* log the state of the rails the WCN3990 needs */
static void scan_regulators(void)
{
	DIR *d = opendir("/sys/class/regulator");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		char base[256], path[300], name[64] = "", state[32] = "";

		if (e->d_name[0] == '.')
			continue;
		snprintf(base, sizeof(base), "/sys/class/regulator/%s", e->d_name);
		snprintf(path, sizeof(path), "%s/name", base);
		if (read_file(path, name, sizeof(name)) <= 0)
			continue;
		name[strcspn(name, "\n")] = 0;
		if (!strstr(name, "vreg_l2c") && !strstr(name, "vreg_l10c") &&
		    !strstr(name, "vreg_s4a") && !strstr(name, "vreg_l14a"))
			continue;
		snprintf(path, sizeof(path), "%s/state", base);
		if (read_file(path, state, sizeof(state)) <= 0)
			state[0] = 0;
		state[strcspn(state, "\n")] = 0;
		klog("reg: %s %s\n", name, state);
	}
	closedir(d);
}

/* log every GPT partition name (find firmware partitions) */
static void list_parts(void)
{
	DIR *d = opendir("/sys/class/block");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) != NULL) {
		char path[256], buf[512];
		char *p;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
		if (read_file(path, buf, sizeof(buf)) <= 0)
			continue;
		p = strstr(buf, "PARTNAME=");
		if (p)
			klog("part: %s %.*s\n", e->d_name,
			     (int)strcspn(p + 9, "\n"), p + 9);
	}
	closedir(d);
}

/* log the first entries of a directory (bring-up aid) */
static void list_dir(const char *dir)
{
	DIR *d = opendir(dir);
	struct dirent *e;
	int n = 0;

	if (!d) {
		klog("list %s: %s\n", dir, strerror(errno));
		return;
	}
	while ((e = readdir(d)) != NULL && n < 12) {
		if (e->d_name[0] == '.')
			continue;
		klog("list %s: %s\n", dir, e->d_name);
		n++;
	}
	closedir(d);
}

/* list platform devices matching a substring */
static void list_devices(const char *match)
{
	DIR *d = opendir("/sys/bus/platform/devices");
	struct dirent *e;

	if (!d)
		return;
	while ((e = readdir(d)) != NULL)
		if (strstr(e->d_name, match))
			klog("dev: %s\n", e->d_name);
	closedir(d);
}

/* Copy a raw partition into a tmpfs file so rmtfs can serve it read-write
 * without ever writing to the phone's flash. */
static void seed_efs(const char *part, const char *dest, size_t max)
{
	char dev[64], path[128], buf[65536];
	size_t total = 0;
	int in, out;

	if (find_part(part, dev, sizeof(dev)) < 0) {
		klog("efs: %s not found\n", part);
		return;
	}
	/* never read past the real partition: the EFS image must be exact */
	snprintf(path, sizeof(path), "/sys/class/block/%s/size", dev + 5);
	if (read_file(path, buf, sizeof(buf)) > 0) {
		unsigned long long sectors = strtoull(buf, NULL, 10);

		if (sectors * 512 < max)
			max = sectors * 512;
	}
	in = open(dev, O_RDONLY);
	if (in < 0) {
		klog("efs: open %s: %s\n", dev, strerror(errno));
		return;
	}
	out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (out < 0) {
		klog("efs: create %s: %s\n", dest, strerror(errno));
		close(in);
		return;
	}
	while (total < max) {
		ssize_t n = read(in, buf, sizeof(buf));

		if (n <= 0)
			break;
		if (write(out, buf, n) != n)
			break;
		total += n;
	}
	close(in);
	close(out);
	klog("efs: %s -> %s (%zu bytes)\n", part, dest, total);
}

/* The firmware lives on the phone's own partitions; the remoteproc/wifi probes
 * run before init mounts them, so bind the devices again afterwards. */
static void mount_firmware(void)
{
	static const char pil[] = "/lib/firmware/qcom/sm7250/xiaomi/picasso";
	char dev[64];

	mkdir_parents(pil);
	if (find_part("modem", dev, sizeof(dev)) == 0) {
		if (try_mount(dev, pil, "ext4") == 0)
			klog("firmware: modem %s ext4\n", dev);
		else if (try_mount(dev, pil, "vfat") == 0)
			klog("firmware: modem %s vfat\n", dev);
		else
			klog("firmware: modem %s mount failed: %s\n", dev, strerror(errno));
	} else {
		klog("firmware: no modem partition\n");
	}

	mkdir_parents("/lib/firmware/bt");
	if (find_part("bluetooth", dev, sizeof(dev)) == 0) {
		if (try_mount(dev, "/lib/firmware/bt", "vfat") == 0 ||
		    try_mount(dev, "/lib/firmware/bt", "ext4") == 0) {
			klog("firmware: bluetooth %s mounted\n", dev);
			list_dir("/lib/firmware/bt");
			list_dir("/lib/firmware/bt/image");
		} else {
			klog("firmware: bluetooth mount failed: %s\n", strerror(errno));
		}
	} else {
		klog("firmware: no bluetooth partition\n");
	}

	mkdir_parents("/vendor");
	if (find_part("vendor", dev, sizeof(dev)) == 0) {
		if (try_mount(dev, "/vendor", "erofs") == 0)
			klog("firmware: vendor %s erofs\n", dev);
		else if (try_mount(dev, "/vendor", "ext4") == 0)
			klog("firmware: vendor %s ext4\n", dev);
		else
			klog("firmware: vendor %s mount failed: %s\n", dev, strerror(errno));
		mkdir_parents("/lib/firmware/vendor");
		if (mount("/vendor/firmware", "/lib/firmware/vendor", NULL, MS_BIND, NULL) == 0)
			klog("firmware: vendor bound\n");
		list_dir("/vendor");
	} else {
		klog("firmware: no vendor partition\n");
	}

	scan_logdump();
	list_parts();
	list_dir("/lib/firmware/qcom/sm7250/xiaomi/picasso");
	list_dir("/lib/firmware/qcom/sm7250/xiaomi/picasso/image");
	list_devices("remoteproc");
	list_devices("wifi");
	rebind("qcom_q6v5_pas", "4080000.remoteproc");
	rebind("ath10k_snoc", "18800000.wifi");
	/* rmtfs (EFS) and pd-mapper (PD map) must run before the modem boots;
	 * their stderr goes to /dev/kmsg so the RAM log carries it. */
	{
		seed_efs("modemst1", "/run/rmtfs/modem_fs1", 8 << 20);
		seed_efs("modemst2", "/run/rmtfs/modem_fs2", 8 << 20);
		seed_efs("fsc", "/run/rmtfs/modem_fsc", 64 << 10);
		seed_efs("fsg", "/run/rmtfs/modem_fsg", 8 << 20);

		static const char *const progs[][6] = {
			{ "/bin/rmtfs", "rmtfs", "-v", "-o", "/run/rmtfs", NULL },
			{ "/bin/pd-mapper", "pd-mapper", NULL, NULL, NULL, NULL },
			{ "/bin/tqftpserv", "tqftpserv", NULL, NULL, NULL, NULL },
		};
		int j;

		for (j = 0; j < (int)ARRAY_SIZE(progs); j++) {
			if (access(progs[j][0], X_OK) != 0) {
				klog("service: %s missing\n", progs[j][0]);
				continue;
			}
			if (fork() == 0) {
				char *const *argv = (char *const *)&progs[j][1];
				int fd = open("/dev/kmsg", O_WRONLY);

				if (fd >= 0) {
					dup2(fd, 1);
					dup2(fd, 2);
				}
				execv(progs[j][0], argv);
				_exit(127);
			}
			klog("service: started %s\n", progs[j][1]);
		}
		sleep(2);
		list_dir("/sys/class/rmtfs");
		list_dir("/sys/bus/rpmsg/devices");
		klog("service: rmtfs_mem1 %s\n",
		     access("/dev/qcom_rmtfs_mem1", R_OK | W_OK) == 0 ? "present" : "missing");
	}

	/* start the modem so PAS authenticates the firmware */
	{
		int fd = open("/sys/class/remoteproc/remoteproc0/state", O_WRONLY);
		static const char *const fw[] = {
			"/lib/firmware/qcom/sm7250/xiaomi/picasso/image/modem.mdt",
			"/lib/firmware/qcom/sm7250/xiaomi/picasso/image/bdwlan.bin",
			"/lib/firmware/qcom/sm7250/xiaomi/picasso/image/bdwlan.b19",
			"/lib/firmware/qcom/sm7250/xiaomi/picasso/image/wlanmdsp.mbn",
			"/lib/firmware/qcom/sm7250/xiaomi/picasso/image/crbtfw32.tlv",
		};
		int j;

		for (j = 0; j < (int)ARRAY_SIZE(fw); j++)
			klog("firmware: %s %s\n", fw[j], access(fw[j], R_OK) == 0 ? "present" : "missing");
		if (fd >= 0) {
			if (write(fd, "start", 5) < 0)
				klog("remoteproc0 start: %s\n", strerror(errno));
			else
				klog("remoteproc0 start written\n");
			close(fd);
		}
	}

	if (fork() == 0) {
		static const char *const f[] = {
			"/sys/class/remoteproc/remoteproc0/state",
			"/sys/class/remoteproc/remoteproc0/firmware",
			"/sys/class/remoteproc/remoteproc0/name",
		};
		char buf[256];
		int i, j;

		for (i = 0; i < 120; i++) {
			sleep(1);
			if (i == 3 || i == 10 || i % 30 == 0) {
				for (j = 0; j < (int)ARRAY_SIZE(f); j++)
					if (read_file(f[j], buf, sizeof(buf)) > 0)
						klog("state: %s = %s", f[j], buf);
				list_dir("/sys/class/net");
				scan_regulators();
				{
					char buf[4096];

					if (read_file("/sys/kernel/debug/gpio", buf, sizeof(buf)) > 0)
						klog("gpio: %s\n", buf);
				}
			}
		}
		_exit(0);
	}
}

static void mount_userdata(void)
{
	char dev[64];

	mkdir("/userdata", 0755);
	if (find_part("userdata", dev, sizeof(dev)) < 0) {
		printf("userdata: no partition named userdata\n");
		return;
	}
	if (!mount(dev, "/userdata", "f2fs", MS_RDONLY, NULL) ||
	    !mount(dev, "/userdata", "ext4", MS_RDONLY, NULL)) {
		printf("userdata: %s mounted read-only\n", dev);
		klog("picasso-init: userdata %s mounted read-only\n", dev);
	} else {
		printf("userdata: %s mount failed: %s\n", dev, strerror(errno));
	}
}

static void dump_pstore(void)
{
	DIR *d;
	struct dirent *e;
	char path[256], buf[4096];
	int shown = 0;

	mkdir("/pstore", 0755);
	if (mount("pstore", "/pstore", "pstore", 0, NULL) && errno != EBUSY)
		return;
	d = opendir("/pstore");
	if (!d)
		return;
	while ((e = readdir(d)) != NULL && shown < 8) {
		int fd, n;

		if (e->d_name[0] == '.')
			continue;
		snprintf(path, sizeof(path), "/pstore/%s", e->d_name);
		fd = open(path, O_RDONLY);
		if (fd < 0)
			continue;
		printf("\n--- pstore: %s ---\n", e->d_name);
		while ((n = read(fd, buf, sizeof(buf))) > 0)
			(void)!write(1, buf, (size_t)n);
		close(fd);
		shown++;
	}
	closedir(d);
}

/* --- USB CDC-ACM gadget: the only console on this board ------------------ */

static void gadget_setup(void)
{
	char udc[128];
	DIR *d;

	mkdir("/sys/kernel/config", 0755);
	mount("configfs", "/sys/kernel/config", "configfs", 0, NULL);
	mkdir("/sys/kernel/config/usb_gadget", 0755);
	mkdir("/sys/kernel/config/usb_gadget/g1", 0755);

	write_file("/sys/kernel/config/usb_gadget/g1/idVendor", "0x1d6b");
	write_file("/sys/kernel/config/usb_gadget/g1/idProduct", "0x0104");
	write_file("/sys/kernel/config/usb_gadget/g1/bcdUSB", "0x0200");
	write_file("/sys/kernel/config/usb_gadget/g1/bcdDevice", "0x0100");

	mkdir("/sys/kernel/config/usb_gadget/g1/strings/0x409", 0755);
	write_file("/sys/kernel/config/usb_gadget/g1/strings/0x409/serialnumber", "picasso-mainline");
	write_file("/sys/kernel/config/usb_gadget/g1/strings/0x409/manufacturer", "mainline");
	write_file("/sys/kernel/config/usb_gadget/g1/strings/0x409/product", "picasso linux");

	mkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1", 0755);
	mkdir("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409", 0755);
	write_file("/sys/kernel/config/usb_gadget/g1/configs/c.1/strings/0x409/configuration", "acm");
	mkdir("/sys/kernel/config/usb_gadget/g1/functions/acm.GS0", 0755);
	symlink("/sys/kernel/config/usb_gadget/g1/functions/acm.GS0",
		"/sys/kernel/config/usb_gadget/g1/configs/c.1/acm.GS0");

	/* the controller (and its UDC) can appear late */
	for (int i = 0; i < 60; i++) {
		d = opendir("/sys/class/udc");
		if (d) {
			struct dirent *e = readdir(d);

			while (e && (e->d_name[0] == '.'))
				e = readdir(d);
			if (e) {
				snprintf(udc, sizeof(udc), "%s", e->d_name);
				closedir(d);
				write_file("/sys/kernel/config/usb_gadget/g1/UDC", udc);
				printf("usb: gadget bound to %s\n", udc);
				klog("picasso-init: gadget bound to %s\n", udc);
				mlog("gadget %s\n", udc);
				return;
			}
			closedir(d);
		}
		sleep(1);
	}
	printf("usb: no UDC after 60 s\n");
	klog("picasso-init: no UDC after 60 s\n");
	mlog("no UDC\n");
}

static void cmd(int fd, char *line)
{
	char buf[4096];
	int n;

	line[strcspn(line, "\r\n")] = 0;
	if (strstr(line, "picasso>")) {
		char *q = strstr(line, "picasso>");

		memmove(line, q + 8, strlen(q + 8) + 1);
	}
	if (!*line)
		return;
	if (!strcmp(line, "reboot")) {
		dprintf(fd, "rebooting to the loader\n");
		sync();
		reboot(RB_AUTOBOOT);
	} else if (!strcmp(line, "help")) {
		dprintf(fd, "commands: reboot, help, uname, mem, cat FILE, ls DIR\n");
	} else if (!strcmp(line, "uname")) {
		struct utsname u;

		uname(&u);
		dprintf(fd, "%s %s %s\n", u.sysname, u.release, u.machine);
	} else if (!strcmp(line, "mem")) {
		dprintf(fd, "MemTotal %ld kB\n", memtotal_kb());
	} else if (!strncmp(line, "ls ", 3)) {
		DIR *dd = opendir(line + 3);
		struct dirent *de;

		if (!dd) {
			dprintf(fd, "ls: %s\n", strerror(errno));
			return;
		}
		while ((de = readdir(dd)) != NULL)
			dprintf(fd, "%s\n", de->d_name);
		closedir(dd);
	} else if (!strncmp(line, "cat ", 4)) {
		int f = open(line + 4, O_RDONLY);

		if (f < 0) {
			dprintf(fd, "cat: %s\n", strerror(errno));
			return;
		}
		while ((n = read(f, buf, sizeof(buf))) > 0)
			(void)!write(fd, buf, (size_t)n);
		close(f);
	} else if (*line) {
		dprintf(fd, "unknown: %s (try help)\n", line);
	}
}

static void console_loop(void)
{
	char line[256];
	size_t len = 0;
	int fd;

	for (;;) {
		fd = open("/dev/ttyGS0", O_RDWR | O_NOCTTY | O_NONBLOCK);
		if (fd < 0) {
			sleep(1);
			continue;
		}
		klog("picasso-init: console on /dev/ttyGS0\n");
		{
			struct termios tio;

			if (tcgetattr(fd, &tio) == 0) {
				cfmakeraw(&tio);
				tcsetattr(fd, TCSANOW, &tio);
			}
			tcflush(fd, TCIFLUSH);
		}
		dprintf(fd, "\nmainline picasso console (help for commands)\npicasso> ");
		len = 0;
		for (;;) {
			struct pollfd p = { .fd = fd, .events = POLLIN };
			char c;
			ssize_t n = poll(&p, 1, 10000);

			if (n == 0) {
				dprintf(fd, "\npicasso> ");
				continue;
			}
			if (n < 0 || (p.revents & (POLLERR | POLLHUP)))
				break;
			if (!(p.revents & POLLIN))
				continue;
			n = read(fd, &c, 1);
			if (n < 0) {
				if (errno == EAGAIN || errno == EINTR)
					continue;
				break;
			}
			if (n == 0)
				break;
			if (c == '\r')
				continue;
			if (c != '\n') {
				if (len < sizeof(line) - 1)
					line[len++] = c;
				continue;
			}
			line[len] = 0;
			len = 0;
			klog("picasso-init: cmd '%s'\n", line);
			cmd(fd, line);
			dprintf(fd, "picasso> ");
		}
		close(fd);
		sleep(1);
	}
}

int main(void)
{
	struct utsname u;
	char model[256] = "";
	long mem;
	int cpus;

	/* pid 1: the ramdisk has dev/proc/sys as empty directories */
	mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);
	mkdir("/sys/kernel/debug", 0755);
	mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL);
	klog("picasso-init: main start\n");
	mlog("main start\n");

	uname(&u);
	read_file("/proc/device-tree/model", model, sizeof(model));
	printf("\n");
	printf("=== mainline Linux on picasso (ramdisk) ===\n");
	printf("%-14s %s %s\n", "kernel", u.release, u.version);
	printf("%-14s %s\n", "machine", u.machine);
	if (model[0])
		printf("%-14s %s\n", "model", model);
	cpus = count_cpus();
	mem = memtotal_kb();
	printf("%-14s %d\n", "cpus", cpus);
	printf("%-14s %ld MiB\n", "memory", mem > 0 ? mem / 1024 : -1);
	print_prop("/proc/device-tree/chosen/bootargs", "bootargs");
	mount_userdata();
	if (cmdline_flag("nomodem"))
		klog("safe mode: modem/BT firmware services skipped\n");
	else
		mount_firmware();
	printf("==========================================\n");
	dump_pstore();
	paint_fb();
	fflush(NULL);
	klog("picasso-init: after pstore\n");

	if (fork() == 0) {
		printf("init: console child started\n");
		fflush(NULL);
		klog("picasso-init: console child\n");
		mlog("console child\n");
		gadget_setup();
		console_loop();
	}
	if (fork() == 0) {
		for (;;) {
			sleep(5);
			klog("picasso-init: alive\n");
		}
	}

	/* auto_reboot=<seconds>: a software reset keeps DDR, so the loader can
	 * hand the console log to the host on the next start */
	{
		/* safety: come back to the loader after five minutes, so a lost USB
		 * link cannot leave the phone running Linux with no way in */
		int rs = cmdline_seconds("auto_reboot=");

		if (rs < 0)
			rs = 300;
		if (rs > 0 && fork() == 0) {
			sleep((unsigned)rs);
			klog("picasso-init: auto_reboot after %d s\n", rs);
			sync();
			reboot(RB_AUTOBOOT);
		}
	}

	/* PID 1 must not exit; killing init panics the kernel. */
	for (;;)
		pause();
}
