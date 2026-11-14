// SPDX-License-Identifier: GPL-2.0-only
/*
 * Bring up the phone's USB CDC-ACM gadget from userspace, so a full distro
 * (systemd) on picasso can be reached over the same cable the loader uses:
 * the tty appears as /dev/ttyGS0 and serial-getty@ttyGS0 serves a login on it.
 *
 * The recipe is the one this port has always used (tools/linux/init.c):
 * configfs -> usb_gadget/g1 -> acm.GS0 -> bind the UDC. Idempotent: if the
 * gadget is already bound nothing happens, and if the controller appears late
 * it waits for it.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define GADGET "/sys/kernel/config/usb_gadget/g1"

static int write_file(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;
	int rc;

	if (fd < 0) {
		fprintf(stderr, "usbacm: open %s: %m\n", path);
		return -1;
	}
	n = write(fd, value, strlen(value));
	rc = (n == (ssize_t)strlen(value)) ? 0 : -1;
	if (rc)
		fprintf(stderr, "usbacm: write %s: %m\n", path);
	close(fd);
	return rc;
}

static void mkdir_if_missing(const char *path)
{
	if (mkdir(path, 0755) && errno != EEXIST)
		fprintf(stderr, "usbacm: mkdir %s: %m\n", path);
}

static int bind_udc(void)
{
	char udc[128];

	for (int i = 0; i < 120; i++) {
		DIR *d = opendir("/sys/class/udc");

		if (d) {
			struct dirent *e = readdir(d);

			while (e && e->d_name[0] == '.')
				e = readdir(d);
			if (e) {
				snprintf(udc, sizeof(udc), "%s", e->d_name);
				closedir(d);
				if (write_file(GADGET "/UDC", udc))
					return -1;
				printf("usbacm: bound to %s\n", udc);
				return 0;
			}
			closedir(d);
		}
		sleep(1);
	}
	fprintf(stderr, "usbacm: no UDC appeared\n");
	return -1;
}

/*
 * The binding lives in configfs and survives this process: the unit is
 * Type=oneshot + RemainAfterExit so systemd waits for the UDC to be bound (and
 * for /dev/ttyGS0 to exist) before starting serial-getty@ttyGS0, and never
 * rebinds it behind the getty's back.
 */
int main(void)
{
	mkdir_if_missing("/sys/kernel/config");
	if (mount("configfs", "/sys/kernel/config", "configfs", 0, NULL) && errno != EBUSY)
		fprintf(stderr, "usbacm: mount configfs: %m\n");

	if (access(GADGET, F_OK) == 0) {
		/* already configured: only make sure it is bound */
		if (access(GADGET "/UDC", F_OK) == 0 && bind_udc() == 0)
			return 0;
	}

	mkdir_if_missing("/sys/kernel/config/usb_gadget");
	mkdir_if_missing(GADGET);
	write_file(GADGET "/idVendor", "0x1d6b");
	write_file(GADGET "/idProduct", "0x0104");
	write_file(GADGET "/bcdUSB", "0x0200");
	write_file(GADGET "/bcdDevice", "0x0100");
	mkdir_if_missing(GADGET "/strings/0x409");
	write_file(GADGET "/strings/0x409/serialnumber", "picasso-fedora");
	write_file(GADGET "/strings/0x409/manufacturer", "mainline");
	write_file(GADGET "/strings/0x409/product", "picasso linux");
	mkdir_if_missing(GADGET "/configs/c.1");
	mkdir_if_missing(GADGET "/configs/c.1/strings/0x409");
	write_file(GADGET "/configs/c.1/strings/0x409/configuration", "acm");
	mkdir_if_missing(GADGET "/functions/acm.GS0");
	if (symlink(GADGET "/functions/acm.GS0", GADGET "/configs/c.1/acm.GS0") &&
	    errno != EEXIST)
		fprintf(stderr, "usbacm: symlink: %m\n");

	return bind_udc() == 0 ? 0 : 1;
}
