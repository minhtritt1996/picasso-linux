/*
 * Minimal init for the picasso mainline ramdisk.
 *
 * Proves that the kernel reached userspace on the RAM disk: prints the model,
 * command line, CPU count and memory, then stays alive as PID 1. Optional
 * `auto_reboot=<seconds>` on the kernel command line warm-resets back to the
 * Mu-Silicium loader (the xnuloader-style test loop).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/utsname.h>

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

int main(void)
{
	struct utsname u;
	char model[256] = "";
	long mem;
	int cpus, reboot_s;

	/* pid 1: the ramdisk has dev/proc/sys as empty directories */
	mount("proc", "/proc", "proc", 0, NULL);
	mount("sysfs", "/sys", "sysfs", 0, NULL);

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
	fflush(NULL);

	reboot_s = cmdline_seconds("auto_reboot=");
	if (reboot_s > 0) {
		printf("init: auto_reboot in %d s -> back to the loader\n", reboot_s);
		fflush(NULL);
		sleep((unsigned)reboot_s);
		sync();
		reboot(RB_AUTOBOOT);
	}

	/* PID 1 must not exit; killing init panics the kernel. */
	for (;;)
		pause();
}
