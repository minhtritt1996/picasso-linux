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
#include <sys/ioctl.h>
#include <linux/input.h>
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
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/utsname.h>
#include <sys/sysmacros.h>
#include <poll.h>
#include <termios.h>
#include <stdarg.h>
#include <time.h>
#include <linux/fb.h>
#include "boot_logo.h"

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


/* path of the state file of the remoteproc whose name matches (adsp/modem/...) */
static int rproc_state_path(const char *name, char *out, size_t outlen)
{
	DIR *d = opendir("/sys/class/remoteproc");
	struct dirent *e;

	if (!d)
		return -1;
	while ((e = readdir(d)) != NULL) {
		char nm[128], st[128];

		if (strncmp(e->d_name, "remoteproc", 10))
			continue;
		snprintf(nm, sizeof(nm), "/sys/class/remoteproc/%s/name", e->d_name);
		if (read_file(nm, st, sizeof(st)) <= 0)
			continue;
		st[strcspn(st, "\n")] = 0;
		if (strcmp(st, name))
			continue;
		snprintf(out, outlen, "/sys/class/remoteproc/%s/state", e->d_name);
		closedir(d);
		return 0;
	}
	closedir(d);
	return -1;
}

#include <sys/syscall.h>

/* load one .ko through finit_module */
static int insmod(const char *path)
{
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	int ret;

	if (fd < 0)
		return -errno;
	ret = (int)syscall(__NR_finit_module, fd, "", 0);
	close(fd);
	return ret < 0 ? -errno : 0;
}

/* Load the modules listed in /lib/modules/load.order (one file name per line). */
static void load_modules(void)
{
	char list[4096], path[160], name[128];
	char *line, *save = NULL;
	int n_ok = 0, n_err = 0;

	if (read_file("/lib/modules/load.order", list, sizeof(list)) <= 0) {
		klog("modules: no load.order\n");
		return;
	}
	for (line = strtok_r(list, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		int r;

		if (!line[0] || line[0] == '#')
			continue;
		snprintf(name, sizeof(name), "%s", line);
		snprintf(path, sizeof(path), "/lib/modules/%s", name);
		r = insmod(path);
		if (r == 0)
			n_ok++;
		else if (r != -EEXIST) {
			n_err++;
			if (n_err <= 12)
				klog("modules: %s failed: %d\n", name, -r);
		}
	}
	klog("modules: %d loaded, %d failed\n", n_ok, n_err);
}

/* devtmpfs does not always populate class subdirectories without udev: create the
 * ALSA nodes from /sys/class/sound, then run the tone test once. */
static void audio_selftest(void)
{
	static const char *const names[] = { "controlC0", "pcmC0D0p", "timer" };
	char path[128], buf[64];
	int i;

	mkdir("/dev/snd", 0755);
	for (i = 0; i < (int)ARRAY_SIZE(names); i++) {
		struct stat st;
		char major[16], minor[16], *colon;

		snprintf(path, sizeof(path), "/dev/snd/%s", names[i]);
		if (stat(path, &st) == 0)
			continue;
		snprintf(path, sizeof(path), "/sys/class/sound/%s/dev", names[i]);
		if (read_file(path, buf, sizeof(buf)) <= 0)
			continue;
		colon = strchr(buf, ':');
		if (!colon)
			continue;
		snprintf(major, sizeof(major), "%.*s", (int)(colon - buf), buf);
		snprintf(minor, sizeof(minor), "%s", colon + 1);
		minor[strcspn(minor, "\n")] = 0;
		snprintf(path, sizeof(path), "/dev/snd/%s", names[i]);
		if (mknod(path, S_IFCHR | 0666, makedev(atoi(major), atoi(minor))) < 0)
			klog("audio: mknod %s failed: %s\n", names[i], strerror(errno));
	}
	if (access("/dev/snd/pcmC0D0p", F_OK) != 0) {
		klog("audio: no playback PCM\n");
		return;
	}
	if (fork() == 0) {
		int fd = open("/dev/kmsg", O_WRONLY);

		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
			close(fd);
		}
		execl("/bin/tone", "tone", "/dev/snd/pcmC0D0p", NULL);
		_exit(127);
	}
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

/* Paint /dev/fb0 with random noise. Noise is the only trustworthy signal: the
 * bootloader keeps its splash until the first kernel frame, so a logo means the
 * data path is dead, snow means real scanout. */
static void paint_logo(void)
{
	struct fb_var_screeninfo vinfo;
	struct fb_fix_screeninfo finfo;
	unsigned int fb_w = 1080, fb_h = 2400, stride_px = 1088;
	size_t total;
	unsigned int *fb;
	int fd;
	size_t i, x0, y0, idx = 0;

	fd = open("/dev/fb0", O_RDWR);
	if (fd < 0) {
		klog("fb0: open failed: %s\n", strerror(errno));
		return;
	}
	if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
	    ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 &&
	    vinfo.xres > 0 && vinfo.yres > 0 && finfo.line_length >= vinfo.xres * 4) {
		fb_w = vinfo.xres;
		fb_h = vinfo.yres;
		stride_px = finfo.line_length / 4;
	}
	total = (size_t)stride_px * fb_h * 4;
	fb = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (fb == MAP_FAILED) {
		klog("fb0: mmap failed: %s\n", strerror(errno));
		close(fd);
		return;
	}
	for (i = 0; i < (size_t)stride_px * fb_h; i++)
		fb[i] = BOOT_LOGO_BG;

	x0 = fb_w > BOOT_LOGO_W ? (fb_w - BOOT_LOGO_W) / 2 : 0;
	y0 = fb_h > BOOT_LOGO_H ? (fb_h - BOOT_LOGO_H) / 2 : 0;
	for (i = 0; i < ARRAY_SIZE(boot_logo_rle); i++) {
		unsigned short cnt = boot_logo_rle[i].count;
		unsigned int rgb = boot_logo_rle[i].rgb;
		while (cnt-- && idx < (size_t)BOOT_LOGO_W * BOOT_LOGO_H) {
			size_t x = idx % BOOT_LOGO_W;
			size_t y = idx / BOOT_LOGO_W;
			if (x0 + x < fb_w && y0 + y < fb_h)
				fb[(y0 + y) * stride_px + (x0 + x)] = rgb;
			idx++;
		}
	}
	msync(fb, total, MS_SYNC);
	munmap(fb, total);
	close(fd);
	klog("picasso-init: fb0 denial logo (%ux%u, stride=%u)\n",
	     BOOT_LOGO_W, BOOT_LOGO_H, stride_px);
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
		/* the loader writes its log at the start of the partition: show the head */
		char head[768];
		size_t i;

		for (i = 0; i < sizeof(head) - 1 && i < (size_t)n; i++)
			head[i] = (buf[i] >= 32 && buf[i] < 127) || buf[i] == '\n' ? buf[i] : '.';
		head[i] = 0;
		klog("logdump head: %s\n", head);
	}
	{
		char *p = buf, *end = buf + n;

		while (p < end) {
			char *nl = memchr(p, '\n', end - p);
			size_t len = nl ? (size_t)(nl - p) : (size_t)(end - p);

			if (len >= 4 && len < 220 &&
			    (memmem(p, len, "Exception", 9) ||
			     memmem(p, len, "Synchronous", 11) ||
			     memmem(p, len, "PANIC", 5) ||
			     memmem(p, len, "panic", 5) ||
			     memmem(p, len, "watchdog", 8) ||
			     memmem(p, len, "pet=", 4) ||
			     memmem(p, len, "set_time", 8) ||
			     memmem(p, len, "budget", 6) ||
			     memmem(p, len, "jumping to", 10) ||
			     memmem(p, len, "payload:", 8)))
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

static int write_text(const char *path, const char *text);

/* The firmware lives on the phone's own partitions; the remoteproc/wifi probes
 * run before init mounts them, so bind the devices again afterwards. */
static void mount_firmware(void)
{
	static const char pil[] = "/lib/firmware/qcom/sm7250/xiaomi/picasso/image";
	char dev[64];

	mkdir_parents("/mnt/modem_fw");
	mkdir_parents("/mnt/dsp_fw");
	mkdir_parents(pil);
	if (mount("tmpfs", pil, "tmpfs", 0, "mode=0755"))
		klog("firmware: pil tmpfs failed: %s\n", strerror(errno));

	if (find_part("modem", dev, sizeof(dev)) == 0) {
		if (try_mount(dev, "/mnt/modem_fw", "ext4") == 0)
			klog("firmware: modem %s ext4\n", dev);
		else if (try_mount(dev, "/mnt/modem_fw", "vfat") == 0)
			klog("firmware: modem %s vfat\n", dev);
		else
			klog("firmware: modem %s mount failed: %s\n", dev, strerror(errno));
	} else {
		klog("firmware: no modem partition\n");
	}
	if (find_part("dsp", dev, sizeof(dev)) == 0) {
		if (try_mount(dev, "/mnt/dsp_fw", "ext4") == 0 ||
		    try_mount(dev, "/mnt/dsp_fw", "vfat") == 0)
			klog("firmware: dsp %s mounted\n", dev);
	}
	{
		static const char *const src_dirs[] = { "/mnt/modem_fw/image", "/mnt/dsp_fw/image" };
		int k;

		for (k = 0; k < (int)ARRAY_SIZE(src_dirs); k++) {
			DIR *d = opendir(src_dirs[k]);
			struct dirent *e;

			if (!d)
				continue;
			while ((e = readdir(d)) != NULL) {
				char src[320], dst[320];

				if (e->d_name[0] == '.')
					continue;
				snprintf(src, sizeof(src), "%s/%s", src_dirs[k], e->d_name);
				snprintf(dst, sizeof(dst), "%s/%s", pil, e->d_name);
				(void)!symlink(src, dst);
			}
			closedir(d);
		}
		/* Xiaomi PD maps omit tms/pddump_disabled; declare it for modem/adsp/cdsp
		 * so pd-mapper answers and modem's PD watchdog (dog_hal_common.c:180) does not fire. */
		(void)!write_text("/lib/firmware/qcom/sm7250/xiaomi/picasso/image/tms-pddump-modem-root_pd.jsn",
				  "{\n\t\"sr_domain\": {\n\t\t\"soc\": \"msm\",\n\t\t\"domain\": \"modem\",\n\t\t\"subdomain\": \"root_pd\",\n\t\t\"qmi_instance_id\": 180\n\t},\n\t\"sr_service\": [\n\t\t{ \"provider\": \"tms\", \"service\": \"pddump_disabled\" }\n\t]\n}\n");
		(void)!write_text("/lib/firmware/qcom/sm7250/xiaomi/picasso/image/tms-pddump-adsp-audio_pd.jsn",
				  "{\n\t\"sr_domain\": {\n\t\t\"soc\": \"msm\",\n\t\t\"domain\": \"adsp\",\n\t\t\"subdomain\": \"audio_pd\",\n\t\t\"qmi_instance_id\": 74\n\t},\n\t\"sr_service\": [\n\t\t{ \"provider\": \"tms\", \"service\": \"pddump_disabled\" }\n\t]\n}\n");
		(void)!write_text("/lib/firmware/qcom/sm7250/xiaomi/picasso/image/tms-pddump-cdsp-root_pd.jsn",
				  "{\n\t\"sr_domain\": {\n\t\t\"soc\": \"msm\",\n\t\t\"domain\": \"cdsp\",\n\t\t\"subdomain\": \"root_pd\",\n\t\t\"qmi_instance_id\": 76\n\t},\n\t\"sr_service\": [\n\t\t{ \"provider\": \"tms\", \"service\": \"pddump_disabled\" }\n\t]\n}\n");
	}

	mkdir_parents("/mnt/bt_fw");
	mkdir_parents("/lib/firmware/qca");
	if (find_part("bluetooth", dev, sizeof(dev)) == 0) {
		if (try_mount(dev, "/mnt/bt_fw", "vfat") == 0 ||
		    try_mount(dev, "/mnt/bt_fw", "ext4") == 0) {
			struct stat st;
			const char *src = (stat("/mnt/bt_fw/image", &st) == 0 && S_ISDIR(st.st_mode))
					  ? "/mnt/bt_fw/image" : "/mnt/bt_fw";

			if (mount(src, "/lib/firmware/qca", NULL, MS_BIND, NULL) == 0) {
				klog("firmware: bluetooth %s bound from %s\n", dev, src);
				list_dir("/lib/firmware/qca");
			} else {
				klog("firmware: bluetooth bind %s failed: %s\n", src, strerror(errno));
			}
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

	/* Wire /readonly/... and /lib/firmware/<entry> + ath10k board.bin before
	 * starting tqftpserv/pd-mapper/ath10k_snoc. */
	{
		DIR *d;
		struct dirent *e;

		mkdir_parents("/readonly/firmware");
		mkdir_parents("/readonly/vendor/firmware_mnt");
		mkdir_parents("/var/lib/tqftpserv/readonly/firmware");
		(void)!symlink("/lib/firmware/qcom/sm7250/xiaomi/picasso/image",
			       "/readonly/firmware/image");
		(void)!symlink("/lib/firmware/qcom/sm7250/xiaomi/picasso/image",
			       "/readonly/vendor/firmware_mnt/image");
		(void)!symlink("/lib/firmware/qcom/sm7250/xiaomi/picasso/image",
			       "/var/lib/tqftpserv/readonly/firmware/image");

		d = opendir("/lib/firmware/qcom/sm7250/xiaomi/picasso/image");
		if (d) {
			while ((e = readdir(d)) != NULL) {
				char src[320], dst[320];

				if (e->d_name[0] == '.')
					continue;
				snprintf(src, sizeof(src),
					 "/lib/firmware/qcom/sm7250/xiaomi/picasso/image/%s",
					 e->d_name);
				snprintf(dst, sizeof(dst), "/lib/firmware/%s", e->d_name);
				(void)!symlink(src, dst);
			}
			closedir(d);
		}
		mkdir_parents("/lib/firmware/ath10k/WCN3990/hw1.0");
		if (access("/lib/firmware/qcom/sm7250/xiaomi/picasso/image/bdf_g7a.bin", R_OK) == 0)
			(void)!symlink("/lib/firmware/qcom/sm7250/xiaomi/picasso/image/bdf_g7a.bin",
				       "/lib/firmware/ath10k/WCN3990/hw1.0/board.bin");
		else
			(void)!symlink("/lib/firmware/qcom/sm7250/xiaomi/picasso/image/bdwlan.bin",
				       "/lib/firmware/ath10k/WCN3990/hw1.0/board.bin");
	}

	scan_logdump();
	list_parts();
	list_dir("/lib/firmware/qcom/sm7250/xiaomi/picasso");
	list_dir("/lib/firmware/qcom/sm7250/xiaomi/picasso/image");
	list_devices("remoteproc");
	list_devices("wifi");
	/* rmtfs (EFS), pd-mapper (PD map), and tqftpserv must run BEFORE the
	 * modem/wifi rebind so the modem sees tms/pddump_disabled and wlanmdsp.mbn
	 * on its very first boot. */
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
	/* Now rebind BT, modem remoteproc, and wifi after firmware & QRTR services are up */
	rebind("qcom_geni_serial", "894000.serial");
	rebind("qcom_q6v5_pas", "4080000.remoteproc");
	rebind("qcom_q6v5_pas", "8300000.remoteproc");
	rebind("ath10k_snoc", "18800000.wifi");
	rebind("qcom-venus", "aa00000.video-codec");

	/* start the ADSP (audio) if the board enabled it */
	{
		DIR *d = opendir("/sys/class/remoteproc");
		struct dirent *e;

		if (d) {
			while ((e = readdir(d)) != NULL) {
				char nm[128], st[160];

				if (strncmp(e->d_name, "remoteproc", 10))
					continue;
				snprintf(nm, sizeof(nm), "/sys/class/remoteproc/%s/name", e->d_name);
				if (read_file(nm, st, sizeof(st)) <= 0)
					continue;
				if (!strstr(st, "adsp") && !strstr(st, "cdsp"))
					continue;
				snprintf(nm, sizeof(nm), "/sys/class/remoteproc/%s/state", e->d_name);
				{
					int fd = open(nm, O_WRONLY);

					if (fd >= 0) {
						if (write(fd, "start", 5) < 0)
							klog("%s: start failed: %s\n",
							     strstr(st, "cdsp") ? "cdsp" : "adsp",
							     strerror(errno));
						else
							klog("%s: start written\n",
							     strstr(st, "cdsp") ? "cdsp" : "adsp");
						close(fd);
					}
				}
			}
			closedir(d);
		}
	}

	/* start the modem so PAS authenticates the firmware */
	{
		char mp[128];
		int fd = rproc_state_path("modem", mp, sizeof(mp)) == 0 ? open(mp, O_WRONLY) : -1;
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
		char buf[256], pth[160];
		int i;

		(void)pth;

		for (i = 0; i < 120; i++) {
			sleep(1);
			if (i == 3 || i == 10 || i % 30 == 0) {
				if (rproc_state_path("adsp", pth, sizeof(pth)) == 0 &&
				    read_file(pth, buf, sizeof(buf)) > 0)
					klog("state: adsp = %s", buf);
				if (rproc_state_path("modem", pth, sizeof(pth)) == 0 &&
				    read_file(pth, buf, sizeof(buf)) > 0)
					klog("state: modem = %s", buf);
				if (rproc_state_path("cdsp", pth, sizeof(pth)) == 0 &&
				    read_file(pth, buf, sizeof(buf)) > 0)
					klog("state: cdsp = %s", buf);
				list_dir("/sys/class/net");
				list_dir("/dev/dri");
				{
					char buf2[512];
					static const char *const drm[] = {
						"/sys/class/drm/card0/device/uevent",
						"/sys/kernel/debug/dri/0/name",
					};
					int k;

					for (k = 0; k < (int)ARRAY_SIZE(drm); k++)
						if (read_file(drm[k], buf2, sizeof(buf2)) > 0)
							klog("drm: %s = %.200s\n", drm[k], buf2);
				}
				{
					char cards[512];

					if (read_file("/proc/asound/cards", cards, sizeof(cards)) > 0)
						klog("asound: %s\n", cards);
				}
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

/* ---------------------------------------------------------------- UFS root */

/*
 * The console child keeps its own root, so after the hand-over its absolute
 * paths would still be the ramdisk's. It therefore gets a handle to the new
 * tree (root_dfd) and a byte on switch_pipe once PID 1 has moved it into place:
 * from then on it re-chroots and is a shell *inside* the system on UFS, which is
 * the only way to look at a boot that has no network yet.
 */
/*
 * Boot log that survives a failed hand-over: the log lives on the system's own
 * partition (picasso-init.log at its root), the descriptor is opened before
 * switch_root and stays valid afterwards, and every boot first prints the tail
 * of the previous one, so a boot that loses its console still explains itself.
 */
#define BOOTLOG "picasso-init.log"
static int bootlog_fd = -1;

static void blog(const char *fmt, ...)
{
	char buf[512];
	struct timespec ts;
	va_list ap;
	int n;

	if (bootlog_fd < 0)
		return;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	n = snprintf(buf, sizeof(buf), "[%ld.%03ld] ", (long)ts.tv_sec,
		     ts.tv_nsec / 1000000);
	va_start(ap, fmt);
	n += vsnprintf(buf + n, sizeof(buf) - (size_t)n, fmt, ap);
	va_end(ap);
	if (n > 0)
		(void)!write(bootlog_fd, buf, (size_t)n);
}

static void bootlog_open(const char *path)
{
	bootlog_fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
	if (bootlog_fd < 0)
		klog("bootlog: %s: %s\n", path, strerror(errno));
}

/* print the tail of the previous boot's log to the kernel log (the panel) */
static void bootlog_dump(const char *path)
{
	char buf[4096];
	int fd = open(path, O_RDONLY), n;

	if (fd < 0) {
		klog("bootlog: no %s from the previous boot\n", path);
		return;
	}
	n = (int)lseek(fd, 0, SEEK_END);
	if (n > (int)sizeof(buf) - 1)
		lseek(fd, -(off_t)(sizeof(buf) - 1), SEEK_END);
	else
		lseek(fd, 0, SEEK_SET);
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return;
	buf[n] = 0;
	klog("bootlog: previous boot, tail of %s:\n%s\n", path, buf);
}

static int root_dfd = -1;
static int switch_pipe[2] = { -1, -1 };
static int console_switched;
static pid_t auto_reboot_pid = -1;

static void console_enter_new_root(void)
{
	if (console_switched)
		return;
	/* log through /denial first: the child was forked before switchroot, so
	 * root_dfd is only valid in PID 1. /denial is the bind mount of the new root. */
	if (bootlog_fd < 0) {
		if (root_dfd >= 0)
			bootlog_fd = openat(root_dfd, BOOTLOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
		else
			bootlog_fd = open("/denial/" BOOTLOG, O_WRONLY | O_CREAT | O_APPEND, 0644);
	}
	blog("console: entering the new root (dfd %d)\n", root_dfd);
	klog("console: entering the new root (dfd %d)\n", root_dfd);
	if (chdir("/denial")) {
		if (root_dfd >= 0 && fchdir(root_dfd) == 0) {
			/* ok via root_dfd */
		} else {
			blog("console: chdir /denial failed: %s\n", strerror(errno));
			klog("console: chdir /denial failed: %s\n", strerror(errno));
			return;
		}
	}
	blog("console: cwd on the new root, chroot\n");
	if (chroot(".")) {
		blog("console: chroot failed: %s\n", strerror(errno));
		klog("console: chroot failed: %s\n", strerror(errno));
		return;
	}
	blog("console: chroot ok\n");
	(void)!chdir("/");
	console_switched = 1;
	bootlog_open("/" BOOTLOG);
	blog("console: inside the system on UFS, commands follow\n");
	klog("console: now inside the system on UFS\n");
}

/*
 * Booting an installed system instead of this ramdisk. The image on the
 * partition is self-describing: /usr/lib/phonewave-system/root marks a system
 * that wants to be booted this way (docs/denial-mobile-architecture.md). Which
 * partition is asked in this order: cmdline picasso.root=<PARTNAME>, then
 * "denial", then "linux". With picasso.noswitch the ramdisk stays root and the
 * console shell below never goes away - that is the rescue path.
 */
static void cmdline_value(const char *key, char *out, size_t outlen)
{
	char buf[1024];
	const char *p, *e;

	out[0] = 0;
	if (read_file("/proc/cmdline", buf, sizeof(buf)) <= 0)
		return;
	p = strstr(buf, key);
	if (!p)
		return;
	p += strlen(key);
	e = p;
	while (*e && *e != ' ' && *e != '\n')
		e++;
	if ((size_t)(e - p) >= outlen)
		return;
	memcpy(out, p, (size_t)(e - p));
	out[e - p] = 0;
}

static int write_text(const char *path, const char *text)
{
	size_t len = strlen(text);
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, text, len);
	close(fd);
	return n == (ssize_t)len ? 0 : -1;
}

static int copy_file_mode(const char *src, const char *dst, mode_t mode)
{
	char buf[8192];
	int in, out;
	ssize_t n;

	in = open(src, O_RDONLY);
	if (in < 0)
		return -1;
	out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
	if (out < 0) {
		close(in);
		return -1;
	}
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w < 0) {
				close(in);
				close(out);
				return -1;
			}
			off += w;
		}
	}
	close(in);
	close(out);
	return n < 0 ? -1 : 0;
}

static int copy_dir_files(const char *src_dir, const char *dst_dir, mode_t mode)
{
	DIR *d;
	struct dirent *de;
	char src[512], dst[512];
	struct stat st;

	d = opendir(src_dir);
	if (!d)
		return -1;
	mkdir_parents(dst_dir);
	chmod(dst_dir, 0755);
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name) >= (int)sizeof(src))
			continue;
		if (snprintf(dst, sizeof(dst), "%s/%s", dst_dir, de->d_name) >= (int)sizeof(dst))
			continue;
		if (stat(src, &st) != 0 || !S_ISREG(st.st_mode))
			continue;
		if (copy_file_mode(src, dst, mode) == 0)
			chmod(dst, mode);
	}
	closedir(d);
	return 0;
}

/* Copy a config directory and its immediate subdirectories into dst_dir cleanly. */
static int copy_dir_tree1(const char *src_dir, const char *dst_dir, mode_t mode)
{
	DIR *d;
	struct dirent *de;
	char src[512], dst[512];
	struct stat st;

	d = opendir(src_dir);
	if (!d)
		return -1;
	mkdir_parents(dst_dir);
	chmod(dst_dir, 0755);
	while ((de = readdir(d)) != NULL) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		if (snprintf(src, sizeof(src), "%s/%s", src_dir, de->d_name) >= (int)sizeof(src))
			continue;
		if (snprintf(dst, sizeof(dst), "%s/%s", dst_dir, de->d_name) >= (int)sizeof(dst))
			continue;
		if (lstat(src, &st) != 0)
			continue;
		if (S_ISDIR(st.st_mode)) {
			copy_dir_files(src, dst, mode);
		} else if (S_ISREG(st.st_mode)) {
			if (copy_file_mode(src, dst, mode) == 0)
				chmod(dst, mode);
		}
	}
	closedir(d);
	return 0;
}

/*
 * Device policy that must not live in the image: the system is upgraded as
 * shipped, so per-phone decisions go into /run drop-ins, which systemd reads and
 * which disappear with the next boot.
 *
 *  - hexagonrpcd: Denial's unit starts it on /dev/fastrpc-adsp-secure, but this
 *    port's fastrpc node is the non-secure one (qcom,non-secure-domain), so the
 *    device is /dev/fastrpc-adsp. Serving the sensor PD is what keeps the ADSP
 *    from tripping its user-PD watchdog every ~40 s.
 *  - logind: the PMIC pwrkey already wakes the system (pm8941-pwrkey enables the
 *    wake IRQ itself). With logind's default HandlePowerKey=poweroff the queued
 *    KEY_POWER then powers the phone off right after the resume. A short press
 *    should sleep, a long press still powers off.
 */
/*
 * ECM, created from inside the system on UFS. The unit is pulled in by
 * systemd-networkd (which needs the interface anyway) and lives in /run, so the
 * image stays untouched and upgrade-clean.
 */
static void write_ecm_unit(const char *newroot)
{
	static const char script[] =
		"#!/bin/sh\n"
		"exec >>/picasso-init.log 2>&1\n"
		"echo \"[usb-ecm] starting at $(date -Iseconds 2>/dev/null || date)...\"\n"
		"G=/sys/kernel/config/usb_gadget/g1\n"
		"if [ ! -d \"$G\" ]; then\n"
		"  echo \"[usb-ecm] no gadget dir $G\"\n"
		"  exit 0\n"
		"fi\n"
		"if [ -e \"$G/configs/c.1/ecm.usb0\" ]; then\n"
		"  echo \"[usb-ecm] ecm already linked\"\n"
		"  exit 0\n"
		"fi\n"
		"UDC=$(cat \"$G/UDC\" 2>/dev/null || true)\n"
		"echo \"[usb-ecm] current UDC: '$UDC'\"\n"
		"if [ -n \"$UDC\" ]; then\n"
		"  echo \"[usb-ecm] clearing UDC before configfs link\"\n"
		"  echo \"\" > \"$G/UDC\"\n"
		"  sleep 0.2\n"
		"fi\n"
		"mkdir -p \"$G/functions/ecm.usb0\"\n"
		"echo 02:00:00:00:00:02 > \"$G/functions/ecm.usb0/dev_addr\"\n"
		"echo 02:00:00:00:00:01 > \"$G/functions/ecm.usb0/host_addr\"\n"
		"if ! ln -sf \"$G/functions/ecm.usb0\" \"$G/configs/c.1/ecm.usb0\"; then\n"
		"  echo \"[usb-ecm] ln -sf failed\"\n"
		"  exit 1\n"
		"fi\n"
		"echo \"[usb-ecm] ecm linked into c.1\"\n"
		"if [ -n \"$UDC\" ]; then\n"
		"  echo \"$UDC\" > \"$G/UDC\"\n"
		"  echo \"[usb-ecm] rebound UDC $UDC\"\n"
		"fi\n"
		"for i in 1 2 3 4 5 6 7 8 9 10; do\n"
		"  if [ -e /sys/class/net/usb0 ]; then break; fi\n"
		"  sleep 0.2\n"
		"done\n"
		"if ip link set usb0 up 2>&1; then\n"
		"  echo \"[usb-ecm] usb0 up\"\n"
		"else\n"
		"  echo \"[usb-ecm] ip link set usb0 up failed\"\n"
		"fi\n"
		"echo \"[usb-ecm] done, usb0: $(ip addr show dev usb0 2>&1 || true)\"\n"
		"exit 0\n";
	static const char unit[] =
		"[Unit]\n"
		"Description=USB ECM link for the host (added by the picasso ramdisk)\n"
		"DefaultDependencies=no\n"
		"ConditionPathExists=/sys/kernel/config/usb_gadget/g1\n"
		"Before=systemd-networkd.service\n"
		"[Service]\n"
		"Type=oneshot\n"
		"RemainAfterExit=yes\n"
		"ExecStart=/bin/sh /run/usb-ecm.sh\n";
	char p[192], link[192];

	snprintf(p, sizeof(p), "%s/run/usb-ecm.sh", newroot);
	if (write_text(p, script))
		klog("root: usb-ecm.sh failed: %s\n", strerror(errno));
	chmod(p, 0755);

	snprintf(p, sizeof(p), "%s/run/systemd/system", newroot);
	mkdir_parents(p);
	snprintf(p, sizeof(p), "%s/run/systemd/system/usb-ecm.service", newroot);
	if (write_text(p, unit))
		klog("root: usb-ecm.service failed: %s\n", strerror(errno));

	/* enabled by hand: the image's networkd pulls it in */
	snprintf(p, sizeof(p), "%s/run/systemd/system/systemd-networkd.service.wants",
		 newroot);
	mkdir_parents(p);
	snprintf(link, sizeof(link), "%s/run/systemd/system/systemd-networkd.service.wants/usb-ecm.service",
		 newroot);
	if (symlink("/run/systemd/system/usb-ecm.service", link) && errno != EEXIST)
		klog("root: usb-ecm wants link failed: %s\n", strerror(errno));
	else
		klog("root: usb-ecm unit written\n");
}

static void write_device_dropins(const char *newroot)
{
	static const char hexagon[] =
		"[Service]\n"
		"ExecStart=\n"
		"ExecStart=/usr/bin/hexagonrpcd -f /dev/fastrpc-adsp -d adsp -s -R /run/hexagonrpcd\n";
	static const char logind[] =
		"[Login]\n"
		"HandlePowerKey=ignore\n"
		"HandlePowerKeyLongPress=poweroff\n";
	static const char picasso_ssh_key[] =
		"ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIC0dn7SkqrF1jht4xYhXYtTyG3o6cTCFwKh0Y2kh1Kr/ picasso\n";
	char p[192], link[192];

	snprintf(p, sizeof(p), "%s/run/systemd/system/hexagonrpcd.service.d", newroot);
	mkdir_parents(p);
	snprintf(p, sizeof(p), "%s/run/systemd/system/hexagonrpcd.service.d/10-nonsecure.conf",
		 newroot);
	if (write_text(p, hexagon))
		klog("root: hexagonrpcd drop-in failed: %s\n", strerror(errno));
	else
		klog("root: hexagonrpcd drop-in written\n");

	/* Enable hexagonrpcd in multi-user.target.wants */
	snprintf(p, sizeof(p), "%s/run/systemd/system/multi-user.target.wants", newroot);
	mkdir_parents(p);
	snprintf(link, sizeof(link), "%s/run/systemd/system/multi-user.target.wants/hexagonrpcd.service", newroot);
	if (symlink("/usr/lib/systemd/system/hexagonrpcd.service", link) && errno != EEXIST)
		klog("root: hexagonrpcd wants link failed: %s\n", strerror(errno));
	else
		klog("root: hexagonrpcd enabled in multi-user.target.wants\n");

	snprintf(p, sizeof(p), "%s/run/systemd/logind.conf.d", newroot);
	mkdir_parents(p);
	snprintf(p, sizeof(p), "%s/run/systemd/logind.conf.d/10-picasso-powerkey.conf",
		 newroot);
	if (write_text(p, logind))
		klog("root: logind drop-in failed: %s\n", strerror(errno));
	else
		klog("root: logind power-key policy written\n");

	/* SSH authorized_keys for root */
	snprintf(p, sizeof(p), "%s/root/.ssh", newroot);
	mkdir_parents(p);
	chmod(p, 0700);
	snprintf(p, sizeof(p), "%s/root/.ssh/authorized_keys", newroot);
	if (write_text(p, picasso_ssh_key))
		klog("root: root authorized_keys failed: %s\n", strerror(errno));
	else {
		chmod(p, 0600);
		klog("root: root authorized_keys written\n");
	}

	/* Flashed authorized_keys for first-user */
	snprintf(p, sizeof(p), "%s/var/lib/phonewave-system/flashed", newroot);
	mkdir_parents(p);
	chmod(p, 0755);
	snprintf(p, sizeof(p), "%s/var/lib/phonewave-system/flashed/authorized_keys", newroot);
	if (write_text(p, picasso_ssh_key))
		klog("root: flashed authorized_keys failed: %s\n", strerror(errno));
	else {
		chmod(p, 0644);
		klog("root: flashed authorized_keys written\n");
	}

	/* Audio UCM2 override in /run/alsa-ucm2 for PipeWire/WirePlumber */
	{
		static const char wp_env[] =
			"[Service]\n"
			"Environment=ALSA_CONFIG_UCM2=/run/alsa-ucm2\n"
			"Environment=XDG_CONFIG_DIRS=/run:/etc/xdg\n";
		static const char wp_alsa[] =
			"monitor.alsa.rules = [\n"
			"  {\n"
			"    matches = [\n"
			"      { node.name = \"~alsa_output.*\" }\n"
			"    ]\n"
			"    actions = {\n"
			"      update-props = {\n"
			"        audio.format = \"S16LE\"\n"
			"        audio.rate = 48000\n"
			"        audio.channels = 2\n"
			"      }\n"
			"    }\n"
			"  },\n"
			"  {\n"
			"    matches = [\n"
			"      { node.name = \"~alsa_input.*\" }\n"
			"    ]\n"
			"    actions = {\n"
			"      update-props = {\n"
			"        audio.format = \"S16LE\"\n"
			"        audio.rate = 48000\n"
			"        audio.channels = 1\n"
			"      }\n"
			"    }\n"
			"  },\n"
			"  {\n"
			"    matches = [\n"
			"      { node.name = \"alsa_output.platform-sound.HiFi__Speaker__sink\" }\n"
			"    ]\n"
			"    actions = {\n"
			"      update-props = {\n"
			"        priority.session = 900\n"
			"      }\n"
			"    }\n"
			"  }\n"
			"]\n";
		static const char ucm_hifi[] =
			"SectionVerb {\n"
			"\tEnableSequence [\n"
			"\t\tcset \"name='SEC_MI2S_RX Audio Mixer MultiMedia1' 1\"\n"
			"\t\tcset \"name='Speaker Playback Volume' 7\"\n"
			"\t\tcset \"name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 1\"\n"
			"\t\tcset \"name='TX_AIF1_CAP Mixer DEC0' 1\"\n"
			"\t\tcset \"name='TX DEC0 MUX' 'SWR_MIC'\"\n"
			"\t\tcset \"name='TX SMIC MUX0' 'ADC0'\"\n"
			"\t\tcset \"name='ADC1_MIXER Switch' 1\"\n"
			"\t\tcset \"name='ADC1 Switch' 1\"\n"
			"\t\tcset \"name='ADC1 Volume' 18\"\n"
			"\t]\n"
			"\tDisableSequence [\n"
			"\t\tcset \"name='SEC_MI2S_RX Audio Mixer MultiMedia1' 0\"\n"
			"\t\tcset \"name='ADC1_MIXER Switch' 0\"\n"
			"\t\tcset \"name='TX_AIF1_CAP Mixer DEC0' 0\"\n"
			"\t\tcset \"name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 0\"\n"
			"\t]\n"
			"\tValue {\n"
			"\t\tTQ \"HiFi\"\n"
			"\t}\n"
			"}\n"
			"SectionDevice.\"Speaker\" {\n"
			"\tComment \"Speaker\"\n"
			"\tValue {\n"
			"\t\tPlaybackPriority 100\n"
			"\t\tPlaybackPCM \"hw:${CardId},0\"\n"
			"\t\tPlaybackMixerElem \"Speaker\"\n"
			"\t}\n"
			"}\n"
			"SectionDevice.\"Mic\" {\n"
			"\tComment \"Internal Microphone\"\n"
			"\tEnableSequence [\n"
			"\t\tcset \"name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 1\"\n"
			"\t\tcset \"name='TX_AIF1_CAP Mixer DEC0' 1\"\n"
			"\t\tcset \"name='TX DEC0 MUX' 'SWR_MIC'\"\n"
			"\t\tcset \"name='TX SMIC MUX0' 'ADC0'\"\n"
			"\t\tcset \"name='ADC1_MIXER Switch' 1\"\n"
			"\t\tcset \"name='ADC1 Switch' 1\"\n"
			"\t\tcset \"name='ADC1 Volume' 18\"\n"
			"\t]\n"
			"\tDisableSequence [\n"
			"\t\tcset \"name='ADC1_MIXER Switch' 0\"\n"
			"\t\tcset \"name='TX_AIF1_CAP Mixer DEC0' 0\"\n"
			"\t\tcset \"name='MultiMedia2 Mixer TX_CODEC_DMA_TX_3' 0\"\n"
			"\t]\n"
			"\tValue {\n"
			"\t\tCapturePriority 100\n"
			"\t\tCapturePCM \"hw:${CardId},1\"\n"
			"\t\tCaptureChannels 1\n"
			"\t}\n"
			"}\n";
		static const char ucm_top[] =
			"Syntax 3\n"
			"SectionUseCase.\"HiFi\" {\n"
			"\tFile \"/Qualcomm/sm8250/Picasso-HiFi.conf\"\n"
			"\tComment \"HiFi quality Music.\"\n"
			"}\n";
		static const char ucm_svc[] =
			"[Unit]\n"
			"Description=Overlay ALSA UCM2 profile for picasso in /run\n"
			"DefaultDependencies=no\n"
			"Before=sound.target\n"
			"[Service]\n"
			"Type=oneshot\n"
			"RemainAfterExit=yes\n"
			"ExecStart=/bin/sh -c 'cp -rn /usr/share/alsa/ucm2/. /run/alsa-ucm2/'\n";

		snprintf(p, sizeof(p), "%s/run/systemd/user/wireplumber.service.d", newroot);
		mkdir_parents(p);
		snprintf(p, sizeof(p), "%s/run/systemd/user/wireplumber.service.d/ucm2.conf", newroot);
		(void)!write_text(p, wp_env);

		snprintf(p, sizeof(p), "%s/run/wireplumber/wireplumber.conf.d", newroot);
		mkdir_parents(p);
		snprintf(p, sizeof(p), "%s/run/wireplumber/wireplumber.conf.d/50-picasso-alsa.conf", newroot);
		(void)!write_text(p, wp_alsa);

		snprintf(p, sizeof(p), "%s/home/picasso/.config", newroot);
		if (access(p, F_OK) == 0) {
			snprintf(p, sizeof(p), "%s/home/picasso/.config/chromium-flags.conf", newroot);
			if (access(p, F_OK) != 0) {
				(void)!write_text(p,
					"--ozone-platform=wayland\n"
					"--enable-features=UseOzonePlatform,AcceleratedVideoDecodeLinuxGL,AcceleratedVideoDecodeLinuxZeroCopyGL\n"
					"--ignore-gpu-blocklist\n"
					"--enable-gpu-rasterization\n"
					"--enable-zero-copy\n");
				chown(p, 1000, 1000);
			}
		}

		snprintf(p, sizeof(p), "%s/run/alsa-ucm2/Qualcomm/sm8250", newroot);
		mkdir_parents(p);
		snprintf(p, sizeof(p), "%s/run/alsa-ucm2/Qualcomm/sm8250/Picasso-HiFi.conf", newroot);
		(void)!write_text(p, ucm_hifi);

		snprintf(p, sizeof(p), "%s/run/alsa-ucm2/conf.d/sm8250", newroot);
		mkdir_parents(p);
		snprintf(p, sizeof(p), "%s/run/alsa-ucm2/conf.d/sm8250/Xiaomi-RedmiK305G-picasso-G7AE.conf", newroot);
		(void)!write_text(p, ucm_top);
		snprintf(link, sizeof(link), "%s/run/alsa-ucm2/conf.d/sm8250/Xiaomi-Redmi-K30-5G.conf", newroot);
		(void)!symlink("Xiaomi-RedmiK305G-picasso-G7AE.conf", link);

		snprintf(p, sizeof(p), "%s/run/systemd/system/picasso-ucm2.service", newroot);
		(void)!write_text(p, ucm_svc);
		snprintf(link, sizeof(link), "%s/run/systemd/system/multi-user.target.wants/picasso-ucm2.service", newroot);
		(void)!symlink("/run/systemd/system/picasso-ucm2.service", link);
		klog("root: alsa-ucm2 + wireplumber drop-in written\n");
	}

	/* 45-minute safety watchdog timer inside Denial (skip when auto_reboot=0) */
	if (cmdline_seconds("auto_reboot=") != 0) {
		char p[256], link[256];
		static const char wd_svc[] =
			"[Unit]\n"
			"Description=Picasso 45-minute safety watchdog reset\n"
			"[Service]\n"
			"Type=oneshot\n"
			"ExecStart=/bin/sh -c 'sync; echo b > /proc/sysrq-trigger; reboot -f'\n";
		static const char wd_tmr[] =
			"[Unit]\n"
			"Description=Picasso 45-minute safety watchdog timer\n"
			"[Timer]\n"
			"OnBootSec=45min\n"
			"Unit=picasso-watchdog.service\n"
			"[Install]\n"
			"WantedBy=timers.target\n";

		snprintf(p, sizeof(p), "%s/run/systemd/system/picasso-watchdog.service", newroot);
		(void)!write_text(p, wd_svc);
		snprintf(p, sizeof(p), "%s/run/systemd/system/picasso-watchdog.timer", newroot);
		(void)!write_text(p, wd_tmr);
		snprintf(p, sizeof(p), "%s/run/systemd/system/timers.target.wants", newroot);
		mkdir_parents(p);
		snprintf(link, sizeof(link), "%s/run/systemd/system/timers.target.wants/picasso-watchdog.timer", newroot);
		(void)!symlink("/run/systemd/system/picasso-watchdog.timer", link);
		klog("root: picasso-watchdog.timer (45min) enabled\n");
	}

	/* Allow sshd on both USB (10.77.71.2) and Wi-Fi (wlan0) */
	{
		char src[256], dst[256];

		snprintf(src, sizeof(src), "%s/run/11-phone-usb.conf", newroot);
		snprintf(dst, sizeof(dst), "%s/etc/ssh/sshd_config.d/11-phone-usb.conf", newroot);
		if (access(dst, F_OK) == 0 && write_text(src, "ListenAddress 0.0.0.0\n") == 0) {
			if (mount(src, dst, NULL, MS_BIND, NULL) == 0)
				klog("root: sshd ListenAddress 0.0.0.0 bound\n");
			else
				klog("root: sshd bind mount failed: %s\n", strerror(errno));
		}
	}

	/* Persistent QMI LOC GNSS daemon in /run so the QRTR client stays alive */
	if (access("/bin/picasso-gnss", X_OK) == 0) {
		char p[256], link[256];
		static const char gnss_svc[] =
			"[Unit]\n"
			"Description=Picasso persistent QMI LOC GNSS daemon\n"
			"After=systemd-journald.service\n"
			"[Service]\n"
			"Type=simple\n"
			"ExecStart=/run/picasso-gnss\n"
			"Restart=on-failure\n"
			"RestartSec=3\n"
			"[Install]\n"
			"WantedBy=multi-user.target\n";

		snprintf(p, sizeof(p), "%s/run/picasso-gnss", newroot);
		if (copy_file_mode("/bin/picasso-gnss", p, 0755) == 0) {
			snprintf(p, sizeof(p), "%s/run/systemd/system/picasso-gnss.service", newroot);
			(void)!write_text(p, gnss_svc);
			snprintf(p, sizeof(p), "%s/run/systemd/system/multi-user.target.wants", newroot);
			mkdir_parents(p);
			snprintf(link, sizeof(link), "%s/run/systemd/system/multi-user.target.wants/picasso-gnss.service", newroot);
			if (symlink("/run/systemd/system/picasso-gnss.service", link) && errno != EEXIST)
				klog("root: picasso-gnss wants link failed: %s\n", strerror(errno));
			else
				klog("root: picasso-gnss.service enabled\n");
		} else {
			klog("root: copy picasso-gnss failed: %s\n", strerror(errno));
		}
	}

	/* Camera sensor profiles (braun): /usr/share/braun/sensors */
	{
		char dst[128], run_dir[128];

		snprintf(dst, sizeof(dst), "%.96s/usr/share/braun/sensors", newroot);
		if (access(dst, F_OK) == 0 && access("/share/braun/sensors", F_OK) == 0) {
			snprintf(run_dir, sizeof(run_dir), "%.96s/run/braun/sensors", newroot);
			(void)!copy_dir_files(dst, run_dir, 0644);
			if (copy_dir_files("/share/braun/sensors", run_dir, 0644) == 0 &&
			    mount(run_dir, dst, NULL, MS_BIND, NULL) == 0)
				klog("root: braun sensor profiles bound\n");
			else
				klog("root: braun sensor profiles failed: %s\n", strerror(errno));
		}
	}

	/* systemd-sleep hook: stop/restart hexagonrpcd across s2idle */
	{
		char dst[128], run_dir[128], p[256];

		snprintf(dst, sizeof(dst), "%.96s/usr/lib/systemd/system-sleep", newroot);
		if (access(dst, F_OK) == 0 && access("/share/systemd", F_OK) == 0) {
			snprintf(run_dir, sizeof(run_dir), "%.96s/run/picasso-sleep", newroot);
			(void)!copy_dir_files(dst, run_dir, 0755);
			if (copy_dir_files("/share/systemd", run_dir, 0755) == 0 &&
			    mount(run_dir, dst, NULL, MS_BIND, NULL) == 0)
				klog("root: systemd-sleep hook bound\n");
			else
				klog("root: systemd-sleep hook failed: %s\n", strerror(errno));
		}

		/* Override 70-phone-no-sleep.conf in /run on this experimental branch */
		snprintf(p, sizeof(p), "%.96s/run/systemd/sleep.conf.d", newroot);
		mkdir_parents(p);
		snprintf(p, sizeof(p), "%.96s/run/systemd/sleep.conf.d/99-picasso-sleep.conf", newroot);
		(void)!write_text(p, "[Sleep]\nAllowSuspend=yes\nMemorySleepMode=s2idle\n");
	}

	/* Camera calibrations (braun): /usr/lib/modules/<uname -r>/braun */
	{
		struct utsname uts;
		char dst[128], run_root[128], rel_dir[200], cal_dir[256];

		snprintf(dst, sizeof(dst), "%.96s/usr/lib/modules", newroot);
		if (access(dst, F_OK) == 0 && access("/share/braun/calibrations", F_OK) == 0 &&
		    uname(&uts) == 0) {
			snprintf(run_root, sizeof(run_root), "%.96s/run/braun/modules", newroot);
			snprintf(rel_dir, sizeof(rel_dir), "%s/%.64s", run_root, uts.release);
			snprintf(cal_dir, sizeof(cal_dir), "%s/braun", rel_dir);
			mkdir_parents(cal_dir);
			chmod(run_root, 0755);
			chmod(rel_dir, 0755);
			chmod(cal_dir, 0755);
			if (copy_dir_files("/share/braun/calibrations", cal_dir, 0644) == 0 &&
			    mount(run_root, dst, NULL, MS_BIND, NULL) == 0)
				klog("root: braun calibrations bound (%s)\n", uts.release);
			else
				klog("root: braun calibrations failed: %s\n", strerror(errno));
		}
	}

	/* PipeWire speaker DSP drop-in: /etc/pipewire/pipewire.conf.d (or /etc/pipewire if fresh userdata) */
	{
		char dst_d[160], dst_etc[128], run_d[192], run_etc[128];

		snprintf(dst_d, sizeof(dst_d), "%.96s/etc/pipewire/pipewire.conf.d", newroot);
		snprintf(dst_etc, sizeof(dst_etc), "%.96s/etc/pipewire", newroot);
		if (access("/share/pipewire", F_OK) == 0 && access(dst_d, F_OK) == 0) {
			snprintf(run_d, sizeof(run_d), "%.96s/run/pipewire/pipewire.conf.d", newroot);
			(void)!copy_dir_files(dst_d, run_d, 0644);
			if (copy_dir_files("/share/pipewire", run_d, 0644) == 0 &&
			    mount(run_d, dst_d, NULL, MS_BIND, NULL) == 0)
				klog("root: pipewire speaker DSP bound\n");
			else
				klog("root: pipewire speaker DSP failed: %s\n", strerror(errno));
		} else if (access("/share/pipewire", F_OK) == 0 && access(dst_etc, F_OK) == 0) {
			snprintf(run_etc, sizeof(run_etc), "%.96s/run/pipewire/etc", newroot);
			snprintf(run_d, sizeof(run_d), "%s/pipewire.conf.d", run_etc);
			(void)!copy_dir_tree1(dst_etc, run_etc, 0644);
			if (copy_dir_files("/share/pipewire", run_d, 0644) == 0 &&
			    mount(run_etc, dst_etc, NULL, MS_BIND, NULL) == 0)
				klog("root: pipewire speaker DSP bound (/etc/pipewire)\n");
			else
				klog("root: pipewire speaker DSP (/etc/pipewire) failed: %s\n", strerror(errno));
		}
	}
}

/* The stock firmware partitions are mounted in this ramdisk; the new root must
 * see them at the same paths, as the same read-only mounts (never copied). */
static void bind_firmware(const char *newroot)
{
	static const char *const dirs[] = {
		"/mnt/modem_fw",
		"/mnt/dsp_fw",
		"/lib/firmware/qcom/sm7250/xiaomi/picasso/image",
		"/lib/firmware/qca",
		"/lib/firmware/ath10k",
		"/lib/firmware/novatek",
		"/vendor",
	};
	char dst[192];
	int i;

	for (i = 0; i < (int)ARRAY_SIZE(dirs); i++) {
		snprintf(dst, sizeof(dst), "%s%s", newroot, dirs[i]);
		mkdir_parents(dst);
		if (mount(dirs[i], dst, NULL, MS_BIND | MS_REC, NULL))
			klog("root: bind %s: %s\n", dirs[i], strerror(errno));
		else
			klog("root: bound %s\n", dirs[i]);
	}
}

static void create_partlabel_symlinks(void)
{
	const char *const parts[] = { "super", "persist" };
	char dev[64], link[128];
	int i;

	mkdir_parents("/dev/disk/by-partlabel");
	for (i = 0; i < (int)ARRAY_SIZE(parts); i++) {
		if (find_part(parts[i], dev, sizeof(dev)) == 0) {
			snprintf(link, sizeof(link), "/dev/disk/by-partlabel/%s", parts[i]);
			unlink(link);
			if (symlink(dev, link) == 0)
				klog("partlabel: %s -> %s\n", parts[i], dev);
			else
				klog("partlabel: %s -> %s failed: %s\n", parts[i], dev, strerror(errno));
		} else {
			klog("partlabel: part %s not found\n", parts[i]);
		}
	}
}

/* Returns only on failure: on success systemd in the new root owns the box. */
static int switch_to_partition(const char *part)
{
	char dev[64], marker[192];
	static char *const argv[] = { "systemd", NULL };
	int i;

	if (find_part(part, dev, sizeof(dev)) < 0) {
		klog("root: no partition named %s\n", part);
		return -1;
	}
	if (strcmp(part, "userdata") == 0)
		umount("/userdata");
	mkdir("/newroot", 0755);
	if (mount(dev, "/newroot", "ext4", 0, NULL)) {
		klog("root: %s (%s) mount failed: %s\n", part, dev, strerror(errno));
		return -1;
	}
	snprintf(marker, sizeof(marker), "/newroot/usr/lib/phonewave-system/root");
	if (access(marker, R_OK)) {
		klog("root: %s has no phonewave marker, skipped\n", part);
		umount("/newroot");
		return -1;
	}
	klog("root: %s (%s) is the system, switching to it\n", part, dev);
	bootlog_dump("/newroot/" BOOTLOG);
	bootlog_open("/newroot/" BOOTLOG);
	blog("boot: switching to %s (%s)\n", part, dev);
	/* O_RDONLY, not O_PATH: the console child fchdir()s through it */
	root_dfd = open("/newroot", O_RDONLY | O_DIRECTORY);

	/* systemd wants /run to be a tmpfs already; ours carries the drop-ins */
	mkdir("/newroot/run", 0755);
	if (mount("tmpfs", "/newroot/run", "tmpfs", 0, "mode=0755"))
		klog("root: /run tmpfs failed: %s\n", strerror(errno));
	write_device_dropins("/newroot");
	write_ecm_unit("/newroot");
	bind_firmware("/newroot");

	create_partlabel_symlinks();

	{
		static const char *const moves[] = { "dev", "proc", "sys" };

		for (i = 0; i < (int)ARRAY_SIZE(moves); i++) {
			char src[32], dst[64];

			snprintf(src, sizeof(src), "/%s", moves[i]);
			snprintf(dst, sizeof(dst), "/newroot/%s", moves[i]);
			mkdir(dst, 0755);
			if (mount(src, dst, NULL, MS_BIND | MS_REC, NULL))
				klog("root: bind /%s: %s\n", moves[i], strerror(errno));
		}
	}

	/*
	 * The console child was forked long before this mount existed.
	 * Bind /newroot recursively to /denial AFTER moving /dev, /proc, /sys
	 * so /denial carries /dev (with devtmpfs/ttyGS0), /proc, /sys, /run.
	 */
	mkdir("/denial", 0755);
	if (mount("/newroot", "/denial", NULL, MS_BIND | MS_REC, NULL))
		klog("root: bind /denial: %s\n", strerror(errno));

	if (chdir("/newroot"))
		return -1;
	if (mount(".", "/", NULL, MS_MOVE, NULL)) {
		klog("root: move /: %s\n", strerror(errno));
		return -1;
	}
	if (chroot(".")) {
		klog("root: chroot: %s\n", strerror(errno));
		return -1;
	}
	(void)!chdir("/");
	if (switch_pipe[1] >= 0) {
		ssize_t w = write(switch_pipe[1], "x", 1);

		blog("boot: pipe signalled (%zd, %s)\n", w, w < 0 ? strerror(errno) : "ok");
		close(switch_pipe[1]);
	} else {
		blog("boot: no pipe to signal\n");
	}
	if (auto_reboot_pid > 0) {
		kill(auto_reboot_pid, SIGKILL);
		auto_reboot_pid = -1;
	}
	klog("root: exec /usr/lib/systemd/systemd\n");
	execv("/usr/lib/systemd/systemd", argv);
	klog("root: exec failed: %s\n", strerror(errno));
	return -1;
}

static void try_udev_root(void)
{
	char part[64];
	int tried = 0;

	/* the system on UFS is the default; picasso.noswitch keeps the ramdisk,
	 * but still shows what the last hand-over wrote to its boot log */
	if (cmdline_flag("picasso.noswitch")) {
		const char *parts[] = { "denial", "linux", "userdata" };
		char dev[64];
		int i;

		for (i = 0; i < (int)ARRAY_SIZE(parts); i++) {
			if (find_part(parts[i], dev, sizeof(dev)) < 0)
				continue;
			mkdir("/oldroot", 0755);
			if (mount(dev, "/oldroot", "ext4", MS_RDONLY, NULL))
				continue;
			bootlog_dump("/oldroot/" BOOTLOG);
			umount("/oldroot");
			break;
		}
		return;
	}
	cmdline_value("picasso.root=", part, sizeof(part));
	if (part[0] && !switch_to_partition(part)) {
		tried = 1;
		return;
	}
	(void)tried;
	switch_to_partition("denial");
	switch_to_partition("linux");
	switch_to_partition("userdata");
	klog("root: no bootable system on UFS, staying in the ramdisk\n");
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

/* --- USB CDC-ECM: the link the host reaches this phone on ---------------- */

/* Only bring the interface up: the address is the system's business (the image
 * on UFS runs systemd-networkd, usr/lib/systemd/network/10-phone-usb.network
 * puts 10.77.71.2/30 on it). A raw ramdisk boot just exposes a bare link. */
static void link_up(const char *ifname)
{
	struct ifreq ifr;
	int fd = socket(AF_INET, SOCK_DGRAM, 0);

	if (fd < 0)
		return;
	memset(&ifr, 0, sizeof(ifr));
	strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
	if (!ioctl(fd, SIOCGIFFLAGS, &ifr)) {
		ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
		if (ioctl(fd, SIOCSIFFLAGS, &ifr))
			klog("net: SIOCSIFFLAGS %s: %s\n", ifname, strerror(errno));
		else
			klog("net: %s up\n", ifname);
	}
	close(fd);
}

/*
 * The ramdisk used to leave networking to the userspace it ran. The image on
 * UFS expects the link to exist already (its own initramfs creates it before
 * switch_root), so create both functions here: ACM for the console, ECM for
 * ssh. The address follows the phone image's convention, 10.77.71.2/30 with
 * the host on .1 - a link-local pair that cannot clash with a LAN.
 */
static void ecm_setup(void)
{
	const char *g = "/sys/kernel/config/usb_gadget/g1";
	char p[192];
	int i;

	snprintf(p, sizeof(p), "%s/functions/ecm.usb0", g);
	klog("net: mkdir %s\n", p);
	if (mkdir(p, 0755) && errno != EEXIST)
		klog("net: ecm function failed: %s\n", strerror(errno));
	klog("net: ecm function created\n");
	snprintf(p, sizeof(p), "%s/functions/ecm.usb0/dev_addr", g);
	write_file(p, "02:00:00:00:00:02");
	snprintf(p, sizeof(p), "%s/functions/ecm.usb0/host_addr", g);
	write_file(p, "02:00:00:00:00:01");
	klog("net: ecm addresses written, linking into c.1\n");
	if (symlink("/sys/kernel/config/usb_gadget/g1/functions/ecm.usb0",
		    "/sys/kernel/config/usb_gadget/g1/configs/c.1/ecm.usb0") &&
	    errno != EEXIST)
		klog("net: ecm link failed: %s\n", strerror(errno));
	klog("net: ecm linked\n");

	/* the interface exists as soon as the function does */
	for (i = 0; i < 50; i++) {
		if (access("/sys/class/net/usb0", F_OK) == 0)
			break;
		usleep(100000);
	}
	if (i == 50)
		klog("net: usb0 never appeared\n");
	else
		klog("net: usb0 present\n");
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
	/*
	 * ACM is the console and always stays. ECM is added by the system on
	 * UFS itself (see write_ecm_unit) when we are about to hand over: with
	 * ECM already present, the hand-over left the controller dead on this
	 * port, and a dead gadget means no way back in.
	 */
	if (cmdline_flag("picasso.noswitch"))
		ecm_setup();

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
				/* f_ecm reports carrier once the host configures
				 * the data interface; the netdev itself has to be
				 * administratively up first, and that only sticks
				 * after the controller is bound */
				for (int k = 0; k < 20; k++) {
					if (access("/sys/class/net/usb0", F_OK) == 0)
						link_up("usb0");
					usleep(200000);
					if (access("/sys/class/net/usb0/flags", F_OK) == 0)
						break;
				}
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
	blog("console cmd: %s\n", line);
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
		int f;

		dprintf(fd, "rebooting to the loader\n");
		sync();
		f = open("/proc/sysrq-trigger", O_WRONLY);
		if (f >= 0) {
			(void)!write(f, "b", 1);
			close(f);
		}
		reboot(RB_AUTOBOOT);
	} else if (!strcmp(line, "help")) {
		dprintf(fd, "commands: reboot, help, uname, mem, cat FILE, ls DIR\n");
	} else if (!strncmp(line, "dmesg", 5)) {
		const char *pat = line[5] ? line + 5 : "";

		while (*pat == ' ')
			pat++;
		char *buf = malloc(1 << 20);
		int kfd, total = 0, shown = 0;

		if (!buf) {
			dprintf(fd, "dmesg: no memory\n");
		} else if ((kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK)) < 0) {
			dprintf(fd, "dmesg: %m\n");
			free(buf);
		} else {
			while (total < (1 << 20) - 4096) {
				int n = read(kfd, buf + total, (1 << 20) - 4096 - total);

				if (n <= 0)
					break;
				total += n;
			}
			close(kfd);
			{
				char *p2 = buf, *end = buf + total;

				while (p2 < end) {
					char *nl = memchr(p2, '\n', end - p2);
					char *semi = memchr(p2, ';', (nl ? nl : end) - p2);

					if (!nl)
						break;
					*nl = 0;
					if (semi && strstr(semi + 1, pat) && shown < 40) {
						char keep = semi[1 + 380];
						const char *m = semi + 1;

						semi[1 + 380] = 0;
						dprintf(fd, "K %s\n", m);
						semi[1 + 380] = keep;
						shown++;
					}
					p2 = nl + 1;
				}
			}
			dprintf(fd, "dmesg: %d/%d bytes, %d match\n", total, 1 << 20, shown);
			free(buf);
		}
	} else if (!strncmp(line, "tailk", 5)) {
		int want = line[5] ? atoi(line + 6) : 40;
		char *buf = malloc(1 << 21);
		int kfd, total = 0, nrec = 0, printed = 0;
		char **recs;

		if (want <= 0 || want > 400)
			want = 40;
		recs = malloc(sizeof(char *) * 4096);
		if (!buf || !recs) {
			dprintf(fd, "tailk: no memory\n");
		} else if ((kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK)) < 0) {
			dprintf(fd, "tailk: %m\n");
		} else {
			while (total < (1 << 21) - 8192) {
				int n = read(kfd, buf + total, (1 << 21) - 8192 - total);

				if (n <= 0)
					break;
				total += n;
			}
			close(kfd);
			buf[total] = 0;
			{
				char *p2 = buf, *end = buf + total;

				while (p2 < end && nrec < 4096) {
					char *nl = memchr(p2, '\n', end - p2);
					char *semi;

					if (!nl)
						break;
					*nl = 0;
					semi = memchr(p2, ';', nl - p2);
					if (semi && nrec < 4096)
						recs[nrec++] = semi + 1;
					p2 = nl + 1;
				}
			}
			for (int r = nrec > want ? nrec - want : 0; r < nrec; r++) {
				dprintf(fd, "K %s\n", recs[r]);
				printed++;
			}
			dprintf(fd, "tailk: %d bytes, %d records, showed %d\n", total, nrec, printed);
		}
		free(recs);
		free(buf);
	} else if (!strncmp(line, "mmio", 4)) {
		char *arg = line + 4;
		unsigned long base = strtoul(arg, &arg, 0);
		int n = 1, k, mfd;
		volatile unsigned int *p;
		long pg = sysconf(_SC_PAGESIZE);

		if (*arg)
			n = atoi(arg);
		if (n < 1 || n > 32)
			n = 1;
		mfd = open("/dev/mem", O_RDONLY);
		if (mfd < 0) {
			dprintf(fd, "mmio: %m\n");
		} else {
			unsigned long page = base & ~(unsigned long)(pg - 1);

			p = (volatile unsigned int *)mmap(NULL, pg * 2, PROT_READ, MAP_SHARED,
							  mfd, (off_t)page);
			if (p == MAP_FAILED) {
				dprintf(fd, "mmio: mmap %m\n");
			} else {
				unsigned long off = (base - page) / 4;

				for (k = 0; k < n; k++)
					dprintf(fd, "%08lx = %08x\n",
						base + 4UL * k, p[off + k]);
				munmap((void *)p, pg * 2);
			}
			close(mfd);
		}
	} else if (!strncmp(line, "fbsum", 5)) {
		int ffd = open("/dev/fb0", O_RDONLY);
		unsigned int buf[4096], nz = 0, i2, k2;

		if (ffd < 0) {
			dprintf(fd, "fbsum: %m\n");
		} else {
			for (k2 = 0; k2 < 16; k2++) {
				int n = read(ffd, buf, sizeof(buf));

				if (n <= 0)
					break;
				for (i2 = 0; i2 < (unsigned int)(n / 4); i2++)
					if (buf[i2])
						nz++;
			}
			close(ffd);
			dprintf(fd, "fbsum: nonzero words in first 256K = %u\n", nz);
		}
	} else if (!strncmp(line, "mmiow", 5)) {
		char *arg = line + 5;
		unsigned long addr = strtoul(arg, &arg, 0);
		unsigned long val = strtoul(arg, &arg, 0);
		int mfd;
		long pg = sysconf(_SC_PAGESIZE);
		volatile unsigned int *p;

		mfd = open("/dev/mem", O_RDWR);
		if (mfd < 0) {
			dprintf(fd, "mmiow: %m\n");
		} else {
			unsigned long page = addr & ~(unsigned long)(pg - 1);

			p = (volatile unsigned int *)mmap(NULL, pg, PROT_READ | PROT_WRITE,
							  MAP_SHARED, mfd, (off_t)page);
			if (p == MAP_FAILED) {
				dprintf(fd, "mmiow: mmap %m\n");
			} else {
				unsigned long off = (addr - page) / 4;
				int cpid = fork();

				/* the write runs in a child: a bus stall then costs
				 * one process, not the console */
				if (cpid == 0) {
					p[off] = (unsigned int)val;
					_exit(0);
				}
				dprintf(fd, "mmiow %08lx := %08x in pid %d\n",
					addr, (unsigned int)val, cpid);
				munmap((void *)p, pg);
			}
			close(mfd);
		}
	} else if (!strncmp(line, "tinfo", 5)) {
		char path[64];
		int pid = atoi(line + 5);
		int tfd;

		snprintf(path, sizeof(path), "/proc/%d/stat", pid);
		tfd = open(path, O_RDONLY);
		if (tfd >= 0) {
			char buf[512];
			ssize_t n = read(tfd, buf, sizeof(buf) - 1);

			if (n > 0) {
				buf[n] = 0;
				dprintf(fd, "stat: %s\n", buf);
			}
			close(tfd);
		}
		snprintf(path, sizeof(path), "/proc/%d/stack", pid);
		tfd = open(path, O_RDONLY);
		if (tfd < 0) {
			dprintf(fd, "no stack: %m\n");
		} else {
			char buf[1024];
			ssize_t n = read(tfd, buf, sizeof(buf) - 1);

			if (n > 0) {
				buf[n] = 0;
				dprintf(fd, "stack:\n%s\n", buf);
			}
			close(tfd);
		}
	} else if (!strncmp(line, "pinmux", 6)) {
		/* pinmux 72 sec_mi2s : via debugfs, i.e. the kernel's own
		 * ioremap/MT_DEVICE path, unlike /dev/mem */
		char arg[128];
		char path[256];
		char sel[160];
		DIR *d;
		int done = 0;

		snprintf(arg, sizeof(arg), "%s", line + 6);
		if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) &&
		    errno != EBUSY)
			dprintf(fd, "pinmux: debugfs mount: %m\n");
		d = opendir("/sys/kernel/debug/pinctrl");
		if (!d) {
			dprintf(fd, "pinmux: %m\n");
		} else {
			struct dirent *e;

			while ((e = readdir(d))) {
				int wfd;

				if (e->d_name[0] == '.')
					continue;
				snprintf(path, sizeof(path),
					 "/sys/kernel/debug/pinctrl/%s/pinmux-select",
					 e->d_name);
				wfd = open(path, O_WRONLY);
				if (wfd < 0)
					continue;
				snprintf(sel, sizeof(sel), "%s\n", arg);
				if (write(wfd, sel, strlen(sel)) < 0)
					dprintf(fd, "pinmux %s: %m\n", arg);
				else {
					dprintf(fd, "pinmux %s ok via %s\n", arg,
						e->d_name);
					done = 1;
				}
				close(wfd);
				if (done)
					break;
			}
			closedir(d);
		}
	} else if (!strncmp(line, "keys", 4)) {
		/* keys N: print key events from every input device for N s */
		int secs = atoi(line + 4);
		struct pollfd pfd[8];
		int nfd = 0, i, end;

		if (secs <= 0)
			secs = 10;
		for (i = 0; i < 8; i++) {
			char path[48], name[80] = "?";
			int efd;

			snprintf(path, sizeof(path), "/dev/input/event%d", i);
			efd = open(path, O_RDONLY | O_NONBLOCK);
			if (efd < 0)
				continue;
			if (ioctl(efd, EVIOCGNAME(sizeof(name) - 1), name) > 0)
				name[sizeof(name) - 1] = 0;
			dprintf(fd, "keys: event%d = %s\n", i, name);
			pfd[nfd].fd = efd;
			pfd[nfd].events = POLLIN;
			nfd++;
		}
		end = (int)time(NULL) + secs;
		while ((int)time(NULL) < end) {
			if (poll(pfd, nfd, 20) <= 0)
				continue;
			for (i = 0; i < nfd; i++) {
				struct input_event ev;

				if (!(pfd[i].revents & POLLIN))
					continue;
				while (read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
					if (ev.type == EV_KEY)
						dprintf(fd, "  event%d: key %u %s\n", i,
							ev.code,
							ev.value == 0 ? "up" :
							ev.value == 1 ? "DOWN" : "repeat");
					else if (ev.type == EV_ABS)
						dprintf(fd, "  event%d: abs %u = %d\n",
							i, ev.code, ev.value);
				}
			}
		}
		for (i = 0; i < nfd; i++)
			close(pfd[i].fd);
		dprintf(fd, "keys: done\n");
	} else if (!strcmp(line, "uname")) {
		struct utsname u;

		uname(&u);
		dprintf(fd, "%s %s %s\n", u.sysname, u.release, u.machine);
	} else if (!strncmp(line, "tone", 4)) {
		pid_t pid = fork();

		if (pid == 0) {
			static char *const av[] = { "tone", "/dev/snd/pcmC0D0p", NULL };

			dup2(fd, 1);
			dup2(fd, 2);
			execv("/bin/tone", av);
			_exit(127);
		}
		if (pid > 0) {
			int st = 0;

			waitpid(pid, &st, 0);
			dprintf(fd, "tone: status %d\n", st);
		} else {
			dprintf(fd, "tone: fork failed\n");
		}
	} else if (!strncmp(line, "run ", 4)) {
		/*
		 * Run a program of the system we may be inside of, on this tty and
		 * in the foreground: `run ip addr`, `run journalctl -u sshd`.
		 * Forked on purpose - exec'ing a shell here would take the console
		 * away for good once that shell exits.
		 */
		char *argv[9];
		int argc = 0, st = 0;
		char *tok = strtok(line + 4, " ");
		pid_t pid;

		while (tok && argc < 8) {
			argv[argc++] = tok;
			tok = strtok(NULL, " ");
		}
		argv[argc] = NULL;
		if (!argc) {
			dprintf(fd, "run: what?\n");
			return;
		}
		pid = fork();
		if (pid == 0) {
			dup2(fd, 0);
			dup2(fd, 1);
			dup2(fd, 2);
			execvp(argv[0], argv);
			dprintf(fd, "run: %s: %s\n", argv[0], strerror(errno));
			_exit(127);
		}
		if (pid > 0) {
			waitpid(pid, &st, 0);
			dprintf(fd, "run: %s exited %d\n", argv[0], WEXITSTATUS(st));
		}
	} else if (!strcmp(line, "ecm")) {
		/*
		 * Bring the network function up on a bound gadget: unbind the
		 * controller, add ecm next to acm, bind again. Logged step by
		 * step (and on the panel) because the bind is the step that
		 * killed USB when this ran at boot.
		 */
		const char *u = "/sys/kernel/config/usb_gadget/g1/UDC";
		char udc[64];
		int f, n;
		pid_t pid = fork();

		if (pid != 0) {
			dprintf(fd, "ecm: started pid %d\n", (int)pid);
			return;
		}
		/* remember which controller we are on before unbinding */
		f = open(u, O_RDONLY);
		n = f >= 0 ? read(f, udc, sizeof(udc) - 1) : -1;
		if (f >= 0)
			close(f);
		if (n <= 0)
			n = 0;
		udc[n] = 0;
		while (n > 0 && (udc[n - 1] == '\n' || udc[n - 1] == ' '))
			udc[--n] = 0;
		dprintf(fd, "ecm: 1 unbinding UDC %s\n", udc);
		klog("net: step 1 unbind UDC %s\n", udc);
		f = open(u, O_WRONLY);
		if (f >= 0) {
			(void)!write(f, "", 0);
			close(f);
		}
		sleep(1);
		dprintf(fd, "ecm: 2 creating function\n");
		klog("net: step 2 ecm function\n");
		ecm_setup();
		dprintf(fd, "ecm: 3 rebinding UDC\n");
		klog("net: step 3 rebind UDC\n");
		f = open(u, O_WRONLY);
		if (f >= 0) {
			dprintf(fd, "ecm: binding to %s\n", udc);
			klog("net: binding to %s\n", udc);
			(void)!write(f, udc, strlen(udc));
			close(f);
		}
		sleep(2);
		dprintf(fd, "ecm: 4 done (usb0 %s)\n",
			access("/sys/class/net/usb0", F_OK) == 0 ? "present" : "missing");
		klog("net: step 4 done, usb0 %s\n",
		     access("/sys/class/net/usb0", F_OK) == 0 ? "present" : "missing");
		_exit(0);
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
		/* did PID 1 move the system on UFS into place? */
		if (switch_pipe[0] >= 0) {
			struct pollfd q = { .fd = switch_pipe[0], .events = POLLIN };

			if (poll(&q, 1, 0) > 0) {
				char b;

				(void)!read(switch_pipe[0], &b, 1);
				close(switch_pipe[0]);
				switch_pipe[0] = -1;
				console_enter_new_root();
			}
		}
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
		dprintf(fd, "\nmainline picasso console (help for commands)%s\npicasso> ",
			console_switched ? " [on UFS]" : "");
		len = 0;
		for (;;) {
			struct pollfd p[2];
			int np = 1;
			char c;
			ssize_t n;

			p[0].fd = fd;
			p[0].events = POLLIN;
			p[0].revents = 0;
			if (switch_pipe[0] >= 0) {
				p[1].fd = switch_pipe[0];
				p[1].events = POLLIN;
				p[1].revents = 0;
				np = 2;
			}
			n = poll(p, np, 5000);

			if (n == 0) {
				dprintf(fd, "\npicasso> ");
				continue;
			}
			if (n < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			if (np > 1 && (p[1].revents & (POLLIN | POLLHUP | POLLERR))) {
				char b;

				(void)!read(switch_pipe[0], &b, sizeof(b));
				close(switch_pipe[0]);
				switch_pipe[0] = -1;
				console_enter_new_root();
				dprintf(fd, "\nconsole: inside the system on UFS\npicasso> ");
			}
			if (p[0].revents & (POLLERR | POLLHUP))
				break;
			if (!(p[0].revents & POLLIN))
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
	load_modules();
	if (cmdline_flag("nomodem"))
		klog("safe mode: modem/BT firmware services skipped\n");
	else
		mount_firmware();
	printf("==========================================\n");
	dump_pstore();
	paint_logo();
	fflush(NULL);
	klog("picasso-init: after pstore\n");

	/* the console child follows PID 1 into the system on UFS (see
	 * console_enter_new_root); the pipe is how it learns that it happened */
	if (pipe(switch_pipe)) {
		switch_pipe[0] = -1;
		switch_pipe[1] = -1;
	}

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
		/* safety: come back to the loader after 45 minutes, so a lost USB
		 * link cannot leave the phone running Linux with no way in */
		int rs = cmdline_seconds("auto_reboot=");

		if (rs < 0)
			rs = 2700;
		if (rs > 0) {
			auto_reboot_pid = fork();
			if (auto_reboot_pid == 0) {
				int elapsed = 0;
				struct stat pet_st;
				time_t last_pet = 0;

				if (stat("/run/picasso-watchdog-pet", &pet_st) == 0)
					last_pet = pet_st.st_mtime;
				while (elapsed < rs) {
					sleep(15);
					elapsed += 15;
					if (access("/run/picasso-reboot-now", F_OK) == 0 ||
					    access("/proc/1/stat", F_OK) != 0)
						break;
					if (stat("/run/picasso-watchdog-pet", &pet_st) == 0 &&
					    pet_st.st_mtime != last_pet) {
						last_pet = pet_st.st_mtime;
						elapsed = 0;
					}
				}
				klog("picasso-watchdog: %d s timer expired, resetting to loader\n", rs);
				sync();
				{
					int f = open("/proc/sysrq-trigger", O_WRONLY);

					if (f >= 0) {
						(void)!write(f, "b", 1);
						close(f);
					}
				}
				reboot(RB_AUTOBOOT);
			}
		}
	}

	/* An installed system on UFS (Denial) takes over from here; this returns
	 * only when no candidate partition carries the marker. */
	try_udev_root();

	/* PID 1 must not exit; killing init panics the kernel. */
	for (;;)
		pause();
}
