/* dp-screenctl — SCREEN_OFF policy for DP takeover sessions.
 *
 *   dp-screenctl display   keep system awake (wake_lock), blank the panel
 *                          backlight, swallow KEY_POWER, forward every
 *                          other key of the power device(s) through a
 *                          uinput clone (daemon, pidfile).
 *   dp-screenctl lock      keep system awake only — stock Android
 *                          lock/screen-off path stays in charge.
 *   dp-screenctl off       undo whichever mode is active (signal daemon,
 *                          restore brightness, wake_unlock).
 *
 * wake_lock is mandatory in BOTH modes: without it system_suspend tears
 * the Type-C DP link down (observed earlier: suspend -> DP disconnect).
 */
#define _GNU_SOURCE
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
#include <poll.h>

#define STATEF   "/run/dp-screenctl.state"
#define PIDF     "/run/dp-screenctl.pid"
#define WAKE_TOK "dp_takeover"
#define BL_PATH  "/sys/class/backlight/panel0-backlight/brightness"
#define FWD_NAME "dp-keyfwd"

#define BITS_PER_LONG (sizeof(unsigned long) * 8)
#define tbit(bit, array) \
	(((array)[(bit) / BITS_PER_LONG] >> ((bit) % BITS_PER_LONG)) & 1UL)

static int src_fd[16];
static int nsrc;
static int fwd_fd = -1;
static int saved_brightness = -1;
static volatile sig_atomic_t stop_flag;

static void on_term(int sig)
{
	(void)sig;
	stop_flag = 1;
}

static int wr_str(const char *path, const char *s)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	ssize_t w;

	if (fd < 0)
		return -1;
	w = write(fd, s, strlen(s));
	close(fd);
	return w < 0 ? -1 : 0;
}

static int rd_int(const char *path, int *out)
{
	char b[32];
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	ssize_t r;

	if (fd < 0)
		return -1;
	r = read(fd, b, sizeof b - 1);
	close(fd);
	if (r <= 0)
		return -1;
	b[r] = 0;
	*out = atoi(b);
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

static void wake_lock(void)
{
	if (wr_str("/sys/power/wake_lock", WAKE_TOK "\n") < 0)
		fprintf(stderr, "dp-screenctl: wake_lock: %s\n", strerror(errno));
}

static void wake_unlock(void)
{
	wr_str("/sys/power/wake_unlock", WAKE_TOK "\n");
}

static int write_state(const char *mode)
{
	FILE *f = fopen(STATEF, "w");

	if (!f)
		return -1;
	fprintf(f, "mode=%s\nbright=%d\n", mode, saved_brightness);
	fclose(f);
	chmod(STATEF, 0666);
	return 0;
}

static int read_state(char *mode, size_t mn, int *bright)
{
	FILE *f = fopen(STATEF, "r");
	char line[128];
	int got = -1;

	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		char m[32];

		if (sscanf(line, "mode=%31s", m) == 1) {
			snprintf(mode, mn, "%s", m);
			got = 0;
		}
		if (bright)
			(void)sscanf(line, "bright=%d", bright);
	}
	fclose(f);
	return got;
}

static void restore_brightness(void)
{
	char b[32];

	if (saved_brightness < 0)
		return;
	snprintf(b, sizeof b, "%d\n", saved_brightness);
	if (wr_str(BL_PATH, b) < 0)
		fprintf(stderr, "dp-screenctl: restore bright %d: %s\n",
			saved_brightness, strerror(errno));
	saved_brightness = -1;
}

static void ungrab_all(void)
{
	int i;

	for (i = 0; i < nsrc; i++)
		ioctl(src_fd[i], EVIOCGRAB, 0);
}

static void fwd_mknod_settle(void)
{
	DIR *d = opendir("/sys/class/input");
	struct dirent *de;
	int i;

	if (!d)
		return;
	for (i = 0; i < 10; i++) {
		rewinddir(d);
		while ((de = readdir(d))) {
			char sysp[300], name[256], dev[32];
			unsigned maj, min;

			if (strncmp(de->d_name, "event", 5))
				continue;
			snprintf(sysp, sizeof sysp,
				 "/sys/class/input/%s/device/name", de->d_name);
			if (read_sysfs_str(sysp, name, sizeof name) < 0)
				continue;
			if (strcmp(name, FWD_NAME))
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
			}
			closedir(d);
			return;
		}
		usleep(50000);
	}
	closedir(d);
}

static int setup_fwd_uinput(void)
{
	struct uinput_setup uset;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	int i;

	if (fd < 0 && errno == ENOENT) {
		char dev[32];

		if (read_sysfs_str("/sys/class/misc/uinput/dev", dev,
				   sizeof dev) == 0) {
			unsigned maj, min;

			if (sscanf(dev, "%u:%u", &maj, &min) == 2) {
				unlink("/dev/uinput");
				if (mknod("/dev/uinput", S_IFCHR | 0666,
					  makedev(maj, min)) == 0)
					fd = open("/dev/uinput",
						  O_WRONLY | O_NONBLOCK);
			}
		}
	}
	if (fd < 0) {
		fprintf(stderr, "dp-screenctl: uinput: %s\n", strerror(errno));
		return -1;
	}
	if (ioctl(fd, UI_SET_EVBIT, EV_KEY) < 0 ||
	    ioctl(fd, UI_SET_EVBIT, EV_SYN) < 0)
		goto fail;
	/* forward every key the grabbed devices emit except KEY_POWER */
	for (i = 0; i < nsrc; i++) {
		unsigned long keys[(KEY_MAX + 64) / 64] = { 0 };
		int k;

		if (ioctl(src_fd[i], EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0)
			continue;
		for (k = 0; k <= KEY_MAX; k++) {
			if (!tbit(k, keys) || k == KEY_POWER)
				continue;
			ioctl(fd, UI_SET_KEYBIT, k);
		}
	}
	memset(&uset, 0, sizeof uset);
	uset.id.bustype = BUS_VIRTUAL;
	uset.id.vendor = 0x1;
	uset.id.product = 0x2;
	snprintf(uset.name, sizeof uset.name, "%s", FWD_NAME);
	if (ioctl(fd, UI_DEV_SETUP, &uset) < 0)
		goto fail;
	if (ioctl(fd, UI_DEV_CREATE) < 0)
		goto fail;
	fwd_mknod_settle();
	return fd;
fail:
	fprintf(stderr, "dp-screenctl: uinput setup: %s\n", strerror(errno));
	close(fd);
	return -1;
}

static int find_power_sources(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *de;

	if (!d)
		return -1;
	while ((de = readdir(d)) && nsrc < 16) {
		char path[128];
		unsigned long evbits[(EV_MAX + 64) / 64] = { 0 };
		unsigned long keys[(KEY_MAX + 64) / 64] = { 0 };
		int fd;

		if (strncmp(de->d_name, "event", 5))
			continue;
		snprintf(path, sizeof path, "/dev/input/%s", de->d_name);
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		if (ioctl(fd, EVIOCGBIT(0, sizeof evbits), evbits) < 0 ||
		    !tbit(EV_KEY, evbits) ||
		    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) < 0 ||
		    !tbit(KEY_POWER, keys)) {
			close(fd);
			continue;
		}
		if (ioctl(fd, EVIOCGRAB, 1) < 0) {
			fprintf(stderr, "dp-screenctl: grab %s: %s\n", path,
				strerror(errno));
			close(fd);
			continue;
		}
		src_fd[nsrc++] = fd;
		fprintf(stderr, "dp-screenctl: grabbed power source %s\n", path);
	}
	closedir(d);
	return nsrc > 0 ? 0 : -1;
}

static void full_teardown(int keep_state_for_crash)
{
	ungrab_all();
	if (fwd_fd >= 0) {
		ioctl(fwd_fd, UI_DEV_DESTROY);
		close(fwd_fd);
		fwd_fd = -1;
	}
	restore_brightness();
	wake_unlock();
	if (!keep_state_for_crash)
		unlink(STATEF);
	unlink(PIDF);
}

static int cmd_lock(void)
{
	if (rd_int(BL_PATH, &saved_brightness) < 0)
		saved_brightness = -1;
	wake_lock();
	write_state("lock");
	fprintf(stderr, "dp-screenctl: lock mode (wake_lock=%s, keys untouched)\n",
		WAKE_TOK);
	return 0;
}

static int cmd_display(void)
{
	struct sigaction sa = { 0 };
	int i;

	wake_lock();
	if (rd_int(BL_PATH, &saved_brightness) == 0) {
		char b[32];

		snprintf(b, sizeof b, "0\n");
		if (wr_str(BL_PATH, b) < 0)
			fprintf(stderr, "dp-screenctl: blank: %s\n",
				strerror(errno));
	} else {
		fprintf(stderr, "dp-screenctl: cannot read %s (%s); panel stays lit\n",
			BL_PATH, strerror(errno));
		saved_brightness = -1;
	}
	if (find_power_sources() < 0) {
		fprintf(stderr,
			"dp-screenctl: no KEY_POWER device found; power key not swallowed\n");
	} else {
		fwd_fd = setup_fwd_uinput();
		if (fwd_fd < 0)
			fprintf(stderr, "dp-screenctl: key forwarding disabled\n");
	}
	write_state("display");

	{
		char b[32];
		int n, fd = open(PIDF, O_WRONLY | O_CREAT | O_TRUNC, 0666);

		if (fd >= 0) {
			n = snprintf(b, sizeof b, "%d\n", getpid());
			(void)write(fd, b, (size_t)n);
			close(fd);
		}
	}

	sa.sa_handler = on_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	fprintf(stderr, "dp-screenctl: display mode (wake_lock, bright=0, "
			"power swallowed, pid %d)\n", getpid());

	while (!stop_flag) {
		struct pollfd pfd[16];
		int timeout = 5000, pr;

		/* Android services may fight over brightness — re-assert 0 */
		if (saved_brightness >= 0) {
			int cur = -1;

			if (rd_int(BL_PATH, &cur) == 0 && cur != 0)
				wr_str(BL_PATH, "0\n");
		}
		for (i = 0; i < nsrc; i++) {
			pfd[i].fd = src_fd[i];
			pfd[i].events = POLLIN;
		}
		pr = poll(pfd, (nfds_t)nsrc, timeout);
		if (pr <= 0)
			continue;
		for (i = 0; i < nsrc; i++) {
			struct input_event ev;

			if (!(pfd[i].revents & POLLIN))
				continue;
			while (read(src_fd[i], &ev, sizeof ev) ==
			       (ssize_t)sizeof ev) {
				if (ev.type == EV_KEY && ev.code == KEY_POWER)
					continue; /* swallowed */
				if (fwd_fd >= 0)
					(void)write(fwd_fd, &ev, sizeof ev);
			}
		}
	}

	full_teardown(0);
	fprintf(stderr, "dp-screenctl: off (restored bright/wake)\n");
	return 0;
}

static int cmd_off(void)
{
	char mode[32] = "";
	int bright = -1, pid = -1, i;
	char b[32];
	int fd, n;

	if (read_state(mode, sizeof mode, &bright) < 0) {
		/* still make sure no stray wake_lock lingers */
		wake_unlock();
		fprintf(stderr, "dp-screenctl: no state, wake_unlock attempted\n");
		return 1;
	}

	fd = open(PIDF, O_RDONLY);
	if (fd >= 0) {
		n = (int)read(fd, b, sizeof b - 1);
		close(fd);
		if (n > 0) {
			b[n] = 0;
			pid = atoi(b);
		}
	}
	if (pid > 1 && kill(pid, SIGTERM) == 0) {
		for (i = 0; i < 30; i++) {
			if (kill(pid, 0) < 0)
				break;
			usleep(100000);
		}
	}
	/* dead daemon or lock-mode (no daemon): restore from state */
	if (bright >= 0) {
		char bb[32];

		snprintf(bb, sizeof bb, "%d\n", bright);
		wr_str(BL_PATH, bb);
	}
	wake_unlock();
	unlink(STATEF);
	unlink(PIDF);
	fprintf(stderr, "dp-screenctl: off ok (mode=%s bright=%d)\n", mode,
		bright);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: dp-screenctl display|lock|off\n");
		return 2;
	}
	if (!strcmp(argv[1], "display"))
		return cmd_display();
	if (!strcmp(argv[1], "lock"))
		return cmd_lock();
	if (!strcmp(argv[1], "off"))
		return cmd_off();
	fprintf(stderr, "usage: dp-screenctl display|lock|off\n");
	return 2;
}
