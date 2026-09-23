/* kwin-drm-shim — LD_PRELOAD companion for dp-lease-helper.
 *
 * Intercepts open/open64/openat/openat64 of /dev/dri/card* and swaps the
 * result for the DRM_MODE_LEASE_EXCL lessee fd served by the helper over
 * $DP_LEASE_SOCK (SCM_RIGHTS). Every other path transparently falls
 * through to libc. Render nodes stay untouched (GPU only, no modeset).
 *
 * Build: gcc -shared -fPIC  (container/glibc side, loaded into kwin_wayland)
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);

static open_fn real_open_fn;
static open_fn real_open64_fn;
static openat_fn real_openat_fn;
static openat_fn real_openat64_fn;

static int lease_fd = -1;
static int shim_verbose = -1;

static void shim_log(const char *fmt, ...)
{
	va_list ap;

	if (!shim_verbose) {
		if (!getenv("DP_LEASE_SHIM_LOG"))
			return;
		if (!strcmp(getenv("DP_LEASE_SHIM_LOG"), "0"))
			return;
	}
	if (shim_verbose < 0)
		shim_verbose = 1;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

__attribute__((constructor))
static void shim_init(void)
{
	real_open_fn = (open_fn)dlsym(RTLD_NEXT, "open");
	real_open64_fn = (open_fn)dlsym(RTLD_NEXT, "open64");
	real_openat_fn = (openat_fn)dlsym(RTLD_NEXT, "openat");
	real_openat64_fn = (openat_fn)dlsym(RTLD_NEXT, "openat64");
}

static int is_card_path(const char *p)
{
	return p && strstr(p, "/dri/card") != NULL;
}

static int try_connect(const char *path)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

	if (s < 0)
		return -1;
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", path);
	if (connect(s, (struct sockaddr *)&sa, sizeof sa) < 0) {
		close(s);
		return -1;
	}
	return s;
}

static int recv_lease_fd(int s)
{
	char buf[1];
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = buf, .iov_len = 1 };
	struct msghdr msg = { 0 };
	struct cmsghdr *c;
	ssize_t n;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof cbuf;
	n = recvmsg(s, &msg, 0);
	if (n <= 0)
		return -1;
	if (buf[0] != 'F')
		return -1;
	c = CMSG_FIRSTHDR(&msg);
	if (!c || c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
		return -1;
	{
		int fd = -1;

		memcpy(&fd, CMSG_DATA(c), sizeof(fd));
		return fd;
	}
}

static int fetch_lease(void)
{
	static const char *fallbacks[] = {
		"/data/local/tmp/drm-lease.sock",
		"/run/drm-lease.sock",
		"/tmp/drm-lease.sock",
		NULL,
	};
	const char *env = getenv("DP_LEASE_SOCK");
	int i;

	if (lease_fd >= 0)
		return lease_fd;

	if (env && *env) {
		int s = try_connect(env);

		if (s >= 0) {
			lease_fd = recv_lease_fd(s);
			close(s);
			if (lease_fd >= 0) {
				shim_log("kwin-drm-shim: lease fd %d from %s\n",
					 lease_fd, env);
				return lease_fd;
			}
		}
		shim_log("kwin-drm-shim: %s failed (%s), trying fallbacks\n",
			 env, strerror(errno));
	}
	for (i = 0; fallbacks[i]; i++) {
		int s = try_connect(fallbacks[i]);

		if (s < 0)
			continue;
		lease_fd = recv_lease_fd(s);
		close(s);
		if (lease_fd >= 0) {
			shim_log("kwin-drm-shim: lease fd %d from %s\n",
				 lease_fd, fallbacks[i]);
			return lease_fd;
		}
	}
	shim_log("kwin-drm-shim: no lease available, falling back to real open "
		 "(modeset will fail without DP alt-mode kernel patches)\n");
	return -1;
}

static mode_t va_mode(int flags, va_list ap)
{
	return (flags & O_CREAT) ? va_arg(ap, mode_t) : 0;
}

int open(const char *path, int flags, ...)
{
	va_list ap;
	mode_t mode;

	va_start(ap, flags);
	mode = va_mode(flags, ap);
	va_end(ap);

	if (is_card_path(path)) {
		int l = fetch_lease();

		if (l >= 0) {
			int d = dup(l);

			if (d >= 0)
				return d;
			shim_log("kwin-drm-shim: dup failed (%s)\n", strerror(errno));
		}
	}
	return real_open_fn(path, flags, mode);
}

int open64(const char *path, int flags, ...)
{
	va_list ap;
	mode_t mode;

	va_start(ap, flags);
	mode = va_mode(flags, ap);
	va_end(ap);

	if (is_card_path(path)) {
		int l = fetch_lease();

		if (l >= 0) {
			int d = dup(l);

			if (d >= 0)
				return d;
		}
	}
	if (real_open64_fn)
		return real_open64_fn(path, flags, mode);
	return real_open_fn(path, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
	va_list ap;
	mode_t mode;

	va_start(ap, flags);
	mode = va_mode(flags, ap);
	va_end(ap);

	if (is_card_path(path)) {
		int l = fetch_lease();

		if (l >= 0) {
			int d = dup(l);

			if (d >= 0)
				return d;
		}
	}
	return real_openat_fn(dirfd, path, flags, mode);
}

int openat64(int dirfd, const char *path, int flags, ...)
{
	va_list ap;
	mode_t mode;

	va_start(ap, flags);
	mode = va_mode(flags, ap);
	va_end(ap);

	if (is_card_path(path)) {
		int l = fetch_lease();

		if (l >= 0) {
			int d = dup(l);

			if (d >= 0)
				return d;
		}
	}
	if (real_openat64_fn)
		return real_openat64_fn(dirfd, path, flags, mode);
	return real_openat_fn(dirfd, path, flags, mode);
}
