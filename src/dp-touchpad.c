/* dp-touchpad — grab the panel touchscreen and re-emit it as a uinput
 * touchscreen so the container compositor (kwin) gets the panel as an
 * input device while Android-side consumers see nothing (EVIOCGRAB).
 *
 * The uinput clone copies source ABS ranges 1:1 (single contact,
 * ABS_MT_*) and sets INPUT_PROP_DIRECT, so kwin/libinput treats it as a
 * direct touchscreen: touch position == click position.
 *
 * usage: dp-touchpad on [--device /dev/input/eventN]   (foreground)
 *        dp-touchpad off                              (signal pidfile)
 * env:   TP_NAME   substring of the source device name (default "touch")
 * pid:   /run/dp-touchpad.pid
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#define PIDF "/run/dp-touchpad.pid"
#define UINPUT_NAME "dp-touchpad"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define tbit(bit, array) \
	(((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

static int src_fd = -1, uin_fd = -1;
static volatile sig_atomic_t stop_flag;

static void on_term(int sig)
{
	(void)sig;
	stop_flag = 1;
}

static int strcasestr_local(const char *h, const char *n)
{
	size_t nl = strlen(n), i;

	for (i = 0; h[i]; i++) {
		size_t j;

		for (j = 0; j < nl; j++) {
			if (tolower((unsigned char)h[i + j]) !=
			    tolower((unsigned char)n[j]))
				break;
		}
		if (j == nl)
			return 1;
		if (!h[i])
			break;
	}
	return 0;
}

static int read_sysfs_str(const char *path, char *buf, size_t n)
{
	int fd = open(path, O_RDONLY);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, buf, n - 1);
	close(fd);
	if (r <= 0)
		 return -1;
	buf[r] = 0;
	while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == '\r'))
		buf[--r] = 0;
	return 0;
}

static int dev_has_mt(int fd)
{
	unsigned long evbits[(EV_MAX + 64) / 64] = { 0 };
	unsigned long absbits[(ABS_MAX + 64) / 64] = { 0 };

	if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0)
		return 0;
	if (!tbit(EV_ABS, evbits))
		return 0;
	if (ioctl(fd, EVIOCGBIT(EV_ABS, sizeof absbits), absbits) < 0)
		return 0;
	return tbit(ABS_MT_POSITION_X, absbits) || tbit(ABS_X, absbits);
}

static int find_source(const char *want, char *path_out, size_t n)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *de;
	const char *substr = want && *want ? want : "touch";

	if (!d)
		return -1;
	while ((de = readdir(d))) {
		char sysp[300], name[256];
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(sysp, sizeof sysp, "/sys/class/input/%s/device/name",
			 de->d_name);
		if (read_sysfs_str(sysp, name, sizeof name) < 0)
			continue;
		if (strstr(name, "dp-touchpad"))
			continue;
		if (!strcasestr_local(name, substr))
			continue;
		snprintf(path_out, n, "/dev/input/%s", de->d_name);
		fd = open(path_out, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		if (!dev_has_mt(fd)) {
			close(fd);
			continue;
		}
		close(fd);
		closedir(d);
		fprintf(stderr, "dp-touchpad: source %s (%s)\n", path_out, name);
		return 0;
	}
	closedir(d);
	return -1;
}

static int mknod_uinput(void)
{
	char dev[32];
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

	if (fd >= 0)
		return fd;
	if (errno != ENOENT)
		return -1;
	if (read_sysfs_str("/sys/class/misc/uinput/dev", dev, sizeof dev) == 0) {
		unsigned maj = 0, min = 0;

		if (sscanf(dev, "%u:%u", &maj, &min) == 2) {
			unlink("/dev/uinput");
			if (mknod("/dev/uinput", S_IFCHR | 0666,
				  makedev(maj, min)) == 0)
				fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
		}
	}
	return fd;
}

static void settle_uinput(void)
{
	DIR *d;
	struct dirent *de;
	int i;

	/* ueventd/mdev may need a beat; we mknod ourselves anyway */
	for (i = 0; i < 10; i++) {
		usleep(50000);
		d = opendir("/sys/class/input");
		if (!d)
			continue;
		while ((de = readdir(d))) {
			char sysp[300], name[256], dev[32];
			unsigned maj, min;

			if (strncmp(de->d_name, "event", 5))
				continue;
			snprintf(sysp, sizeof sysp,
				 "/sys/class/input/%s/device/name", de->d_name);
			if (read_sysfs_str(sysp, name, sizeof name) < 0)
				continue;
			if (strcmp(name, UINPUT_NAME))
				continue;
			snprintf(sysp, sizeof sysp, "/sys/class/input/%s/dev",
				 de->d_name);
			if (read_sysfs_str(sysp, dev, sizeof dev) == 0 &&
			    sscanf(dev, "%u:%u", &maj, &min) == 2) {
				char node[64];

				snprintf(node, sizeof node, "/dev/input/%s",
					 de->d_name);
				if (access(node, F_OK) != 0)
					mknod(node, S_IFCHR | 0666,
					      makedev(maj, min));
				fprintf(stderr, "dp-touchpad: uinput node %s\n",
					node);
			}
			closedir(d);
			return;
		}
		closedir(d);
	}
}

static int setup_uinput(int src)
{
	struct uinput_setup uset;
	int i, fd;

	fd = mknod_uinput();
	if (fd < 0) {
		fprintf(stderr, "dp-touchpad: uinput open: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_ABS) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0)
		goto fail;
	if (ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH) < 0)
		goto fail;
	if (ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT) < 0)
		goto fail;
	/* copy every abs axis of the source 1:1 (incl. MT slot ranges) */
	for (i = 0; i <= ABS_MAX; i++) {
		struct input_absinfo ai;

		if (ioctl(src, EVIOCGABS(i), &ai) < 0)
			continue;
		if (ioctl(fd, UI_SET_ABSBIT, i) < 0)
			continue;
		{
			struct uinput_abs_setup as = {
				.code = (__u16)i,
				.absinfo = {
					.value = 0,
					.minimum = ai.minimum,
					.maximum = ai.maximum,
					.fuzz = ai.fuzz,
					.flat = ai.flat,
					.resolution = ai.resolution,
				},
			};
			ioctl(fd, UI_ABS_SETUP, &as);
		}
	}
	memset(&uset, 0, sizeof uset);
	uset.id.bustype = BUS_VIRTUAL;
	uset.id.vendor = 0x1;
	uset.id.product = 0x1;
	uset.id.version = 1;
	snprintf(uset.name, sizeof uset.name, "%s", UINPUT_NAME);
	if (ioctl(fd, UI_DEV_SETUP, &uset) < 0)
		goto fail;
	if (ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;
	settle_uinput();
	return fd;
fail:
	fprintf(stderr, "dp-touchpad: uinput setup: %s\n", strerror(errno));
	close(fd);
	return -1;
}

static int write_pid(void)
{
	char b[32];
	int n, fd;

	if (access("/run", W_OK) != 0)
		return 0;
	fd = open(PIDF, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0)
		return -1;
	n = snprintf(b, sizeof b, "%d\n", getpid());
	if (write(fd, b, (size_t)n) < 0) {
		close(fd);
		return -1;
	}
	close(fd);
	return 0;
}

static int cmd_on(const char *devpath)
{
	char found[128] = "";
	struct sigaction sa = { 0 };

	if (!devpath || !*devpath) {
		if (find_source(getenv("TP_NAME"), found, sizeof found) < 0) {
			fprintf(stderr,
				"dp-touchpad: no touchscreen found "
				"(set TP_NAME=/dev/input/eventN or TP_NAME=substring)\n");
			return 1;
		}
		devpath = found;
	}
	src_fd = open(devpath, O_RDONLY | O_NONBLOCK);
	if (src_fd < 0) {
		fprintf(stderr, "dp-touchpad: open %s: %s\n", devpath,
			strerror(errno));
		return 1;
	}
	if (ioctl(src_fd, EVIOCGRAB, 1) < 0) {
		fprintf(stderr, "dp-touchpad: EVIOCGRAB: %s\n", strerror(errno));
		close(src_fd);
		return 1;
	}
	uin_fd = setup_uinput(src_fd);
	if (uin_fd < 0) {
		ioctl(src_fd, EVIOCGRAB, 0);
		close(src_fd);
		return 1;
	}
	write_pid();
	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	fprintf(stderr, "dp-touchpad: grabbed %s -> %s (pid %d)\n", devpath,
		UINPUT_NAME, getpid());

	while (!stop_flag) {
		struct input_event ev;
		ssize_t r = read(src_fd, &ev, sizeof ev);

		if (r == (ssize_t)sizeof ev) {
			(void)write(uin_fd, &ev, sizeof ev);
		} else if (r < 0 && errno != EAGAIN && errno != EINTR) {
			break;
		} else {
			usleep(5000);
		}
	}

	ioctl(src_fd, EVIOCGRAB, 0);
	ioctl(uin_fd, UI_DEV_DESTROY);
	close(uin_fd);
	close(src_fd);
	unlink(PIDF);
	fprintf(stderr, "dp-touchpad: released\n");
	return 0;
}

static int cmd_off(void)
{
	char b[32];
	int fd, n, pid, i;

	fd = open(PIDF, O_RDONLY);
	if (fd < 0) {
		fprintf(stderr, "dp-touchpad: not running\n");
		return 1;
	}
	n = (int)read(fd, b, sizeof b - 1);
	close(fd);
	if (n <= 0)
		return 1;
	b[n] = 0;
	pid = atoi(b);
	if (pid <= 1)
		return 1;
	if (kill(pid, SIGTERM) < 0 && errno != ESRCH) {
		fprintf(stderr, "dp-touchpad: kill %d: %s\n", pid,
			strerror(errno));
		return 1;
	}
	for (i = 0; i < 30; i++) {
		if (kill(pid, 0) < 0)
			break;
		usleep(100000);
	}
	unlink(PIDF);
	fprintf(stderr, "dp-touchpad: off ok\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = NULL;
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: dp-touchpad on [--device PATH] | off\n");
		return 2;
	}
	for (i = 2; i < argc; i++) {
		if (!strcmp(argv[i], "--device") && i + 1 < argc)
			dev = argv[++i];
	}
	if (!strcmp(argv[1], "on"))
		return cmd_on(dev);
	if (!strcmp(argv[1], "off"))
		return cmd_off();
	fprintf(stderr, "usage: dp-touchpad on [--device PATH] | off\n");
	return 2;
}
