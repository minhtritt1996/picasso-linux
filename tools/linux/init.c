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
		dprintf(fd, "commands: reboot, help, uname, mem, cat FILE\n");
	} else if (!strcmp(line, "uname")) {
		struct utsname u;

		uname(&u);
		dprintf(fd, "%s %s %s\n", u.sysname, u.release, u.machine);
	} else if (!strcmp(line, "mem")) {
		dprintf(fd, "MemTotal %ld kB\n", memtotal_kb());
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
		fd = open("/dev/ttyGS0", O_RDWR | O_NOCTTY);
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
	printf("==========================================\n");
	dump_pstore();
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
	if (cmdline_seconds("auto_reboot=") > 0 && fork() == 0) {
		sleep((unsigned)cmdline_seconds("auto_reboot="));
		klog("picasso-init: auto_reboot\n");
		sync();
		reboot(RB_AUTOBOOT);
	}

	/* PID 1 must not exit; killing init panics the kernel. */
	for (;;)
		pause();
}
