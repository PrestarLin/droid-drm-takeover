/* dp-lease-helper — root helper for DP alt-mode takeover sessions.
 *
 * daemon: mint ONE DRM_MODE_LEASE_EXCL lease over the DP connector, its
 *         free CRTC and every plane reachable from that CRTC; keep the
 *         lessee fd alive; serve it to kwin (via kwin-drm-shim.so) over a
 *         unix socket with SCM_RIGHTS; revoke on demand so SurfaceFlinger
 *         gets the hotplug back (kernel patches in kernel-patches/).
 * revoke: ask the daemon to REVOKE_LEASE (fallback: direct ioctl).
 * status: dump state file + socket liveness.
 *
 * Raw DRM ioctls only (no libdrm) so it builds -static anywhere.
 * Runs container-side as root (shared kernel with Android).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <drm/drm.h>
#include <stdint.h>
#include <drm/drm_mode.h>

#ifndef DRM_MODE_LEASE_EXCL
#define DRM_MODE_LEASE_EXCL (1u << 30)
#endif

#define SOCK_NAME  "drm-lease.sock"
#define PID_NAME   "dp-lease.pid"
#define STATE_NAME "dp-lease.state"

static const char *card_path = "/dev/dri/card0";
static uint32_t opt_connector;
static uint32_t opt_type = DRM_MODE_CONNECTOR_DisplayPort;

static void die(const char *fmt, ...)
{
	va_list ap;
	int e = errno;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	if (e)
		fprintf(stderr, ": %s", strerror(e));
	fputc('\n', stderr);
	exit(1);
}

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);
	fflush(stdout);
}

/* ---- socket/state paths (mirror order in kwin-drm-shim.c) ---- */

static const char *sockdir(void)
{
	const char *e = getenv("DP_LEASE_DIR");
	if (e && *e)
		return e;
	if (access("/data/local/tmp", W_OK) == 0)
		return "/data/local/tmp";
	if (access("/run", W_OK) == 0)
		return "/run";
	return "/tmp";
}

static void sock_path(char *buf, size_t n)
{
	const char *e = getenv("DP_LEASE_SOCK");
	if (e && *e) {
		snprintf(buf, n, "%s", e);
		return;
	}
	snprintf(buf, n, "%s/%s", sockdir(), SOCK_NAME);
}

static void aux_path(char *buf, size_t n, const char *name)
{
	const char *e = getenv("DP_LEASE_SOCK");
	if (e && *e) {
		char dir[256];
		char *sl;
		snprintf(dir, sizeof dir, "%s", e);
		sl = strrchr(dir, '/');
		if (sl)
			*sl = 0;
		else
			snprintf(dir, sizeof dir, "%s", sockdir());
		snprintf(buf, n, "%s/%s", dir, name);
		return;
	}
	snprintf(buf, n, "%s/%s", sockdir(), name);
}

/* ---- DRM enumeration helpers (2-pass id arrays) ---- */

struct res {
	uint32_t crtcs[32];
	int ncrtcs;
	uint32_t connectors[32];
	int nconnectors;
};

static int get_res(int fd, struct res *r)
{
	struct drm_mode_card_res cr = { 0 };

	memset(r, 0, sizeof *r);
	if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &cr) < 0)
		return -1;
	if (cr.count_crtcs > 32 || cr.count_connectors > 32)
		return -1;
	r->ncrtcs = (int)cr.count_crtcs;
	r->nconnectors = (int)cr.count_connectors;
	if (cr.count_crtcs) {
		uint32_t tmp[32] = { 0 };
		cr.crtc_id_ptr = (uintptr_t)tmp;
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &cr) < 0)
			return -1;
		memcpy(r->crtcs, tmp, r->ncrtcs * 4);
	}
	if (cr.count_connectors) {
		uint32_t tmp[32] = { 0 };
		cr.connector_id_ptr = (uintptr_t)tmp;
		if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &cr) < 0)
			return -1;
		memcpy(r->connectors, tmp, r->nconnectors * 4);
	}
	return 0;
}

static int get_conn_type(int fd, uint32_t id)
{
	struct drm_mode_get_connector c = { .connector_id = id };

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
		return -1;
	return (int)c.connector_type;
}

static int get_conn_encs(int fd, uint32_t id, uint32_t *encs, int max_enc,
			 int *nenc, uint32_t *status)
{
	struct drm_mode_get_connector c = { .connector_id = id };

	if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
		return -1;
	if (status)
		*status = c.connection;
	if (c.count_encoders > (uint32_t)max_enc)
		return -1;
	if (c.count_encoders) {
		uint32_t tmp[16] = { 0 };
		c.encoders_ptr = (uintptr_t)tmp;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &c) < 0)
			return -1;
		memcpy(encs, tmp, c.count_encoders * 4);
	}
	*nenc = (int)c.count_encoders;
	return 0;
}

struct mint {
	uint32_t conn, crtc;
	uint32_t planes[32];
	int nplanes;
	uint32_t lessee_id, fd;
};

static int mint_lease(int fd, struct mint *m)
{
	struct res r;
	uint32_t encs[16];
	int nenc = 0, i, j;
	uint32_t possible = 0;
	int conn_idx = -1, chosen = -1, fallback = -1;
	int crtc_busy = 0;
	uint32_t status = 0;
	uint32_t objs[64];
	int nobj = 0;

	memset(m, 0, sizeof *m);

	{
		uint64_t cap[2] = { DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1 };
		if (ioctl(fd, DRM_IOCTL_SET_CLIENT_CAP, cap) < 0)
			logmsg("warn: UNIVERSAL_PLANES cap: %s\n", strerror(errno));
	}

	if (get_res(fd, &r) < 0)
		return -1;

	if (opt_connector) {
		for (i = 0; i < r.nconnectors; i++)
			if (r.connectors[i] == opt_connector)
				conn_idx = i;
		if (conn_idx < 0) {
			fprintf(stderr, "connector %u not found\n", opt_connector);
			return -1;
		}
	} else {
		for (i = 0; i < r.nconnectors; i++) {
			int t = get_conn_type(fd, r.connectors[i]);
			if (t == (int)opt_type) {
				conn_idx = i;
				break;
			}
		}
		if (conn_idx < 0) {
			fprintf(stderr, "no connector of type %u found\n", opt_type);
			return -1;
		}
	}
	m->conn = r.connectors[conn_idx];

	if (get_conn_encs(fd, m->conn, encs, 16, &nenc, &status) < 0)
		return -1;
	if (nenc == 0) {
		fprintf(stderr, "connector %u has no encoders\n", m->conn);
		return -1;
	}
	for (j = 0; j < nenc; j++) {
		struct drm_mode_get_encoder e = { .encoder_id = encs[j] };
		if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &e) < 0)
			continue;
		possible |= e.possible_crtcs;
	}
	if (!possible) {
		fprintf(stderr, "connector %u: no possible crtcs\n", m->conn);
		return -1;
	}

	/* pick a free CRTC inside possible_crtcs (bit i == crtcs[i]) */
	for (i = 0; i < r.ncrtcs; i++) {
		struct drm_mode_crtc c = { .crtc_id = r.crtcs[i] };

		if (!(possible & (1u << i)))
			continue;
		if (fallback < 0)
			fallback = i;
		if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) < 0 || !c.mode_valid) {
			chosen = i;
			break;
		}
	}
	if (chosen < 0 && fallback >= 0) {
		chosen = fallback;
		crtc_busy = 1;
	}
	if (chosen < 0) {
		fprintf(stderr, "no usable crtc for connector %u\n", m->conn);
		return -1;
	}
	m->crtc = r.crtcs[chosen];
	logmsg("conn=%u crtc=%u (idx %d, %s) encs=%d conn_status=%u\n",
	       m->conn, m->crtc, chosen,
	       crtc_busy ? "busy-fallback" : "free", nenc, status);

	/* every plane reachable from this crtc */
	{
		struct drm_mode_get_plane_res pr = { 0 };
		uint32_t pids[32] = { 0 };

		if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0)
			return -1;
		if (pr.count_planes > 32)
			return -1;
		if (pr.count_planes) {
			pr.plane_id_ptr = (uintptr_t)pids;
			if (ioctl(fd, DRM_IOCTL_MODE_GETPLANERESOURCES, &pr) < 0)
				return -1;
		}
		for (i = 0; i < (int)pr.count_planes && i < 32; i++) {
			struct drm_mode_get_plane p = { .plane_id = pids[i] };

			if (ioctl(fd, DRM_IOCTL_MODE_GETPLANE, &p) < 0)
				continue;
			if (p.possible_crtcs & (1u << chosen))
				m->planes[m->nplanes++] = pids[i];
		}
	}
	if (m->nplanes == 0) {
		fprintf(stderr, "no planes reachable from crtc %u\n", m->crtc);
		return -1;
	}

	objs[nobj++] = m->conn;
	objs[nobj++] = m->crtc;
	for (i = 0; i < m->nplanes; i++)
		objs[nobj++] = m->planes[i];

	{
		struct drm_mode_create_lease cl = {
			.object_ids = (uintptr_t)objs,
			.object_count = (uint32_t)nobj,
			.flags = DRM_MODE_LEASE_EXCL,
		};
		if (ioctl(fd, DRM_IOCTL_MODE_CREATE_LEASE, &cl) < 0)
			return -1;
		m->lessee_id = cl.lessee_id;
		m->fd = cl.fd;
	}
	logmsg("minted EXCL lease lessee=%u fd=%d objs=%d (conn %u, crtc %u, planes %d)\n",
	       m->lessee_id, m->fd, nobj, m->conn, m->crtc, m->nplanes);
	return 0;
}

static void write_state(const struct mint *m, const char *sp)
{
	char p[300];
	FILE *f;

	aux_path(p, sizeof p, STATE_NAME);
	f = fopen(p, "w");
	if (!f)
		return;
	fprintf(f, "sock=%s\nlessee=%u\nconn=%u\ncrtc=%u\nplanes=%d\n",
		sp, m->lessee_id, m->conn, m->crtc, m->nplanes);
	fclose(f);
	chmod(p, 0666);
}

static int read_state_lessee(uint32_t *lessee, char *sock, size_t sockn)
{
	char p[300];
	char line[320];
	FILE *f;
	int got = -1;

	aux_path(p, sizeof p, STATE_NAME);
	f = fopen(p, "r");
	if (!f)
		return -1;
	while (fgets(line, sizeof line, f)) {
		unsigned v;
		char s[300];

		if (sscanf(line, "lessee=%u", &v) == 1) {
			*lessee = v;
			got = 0;
		}
		if (sock && sscanf(line, "sock=%299s", s) == 1)
			snprintf(sock, sockn, "%s", s);
	}
	fclose(f);
	return got;
}

/* ---- socket serving ---- */

static int srv_fd = -1, card_fd = -1, lease_fd = -1, pidfd = -1;
static uint32_t g_lessee;
static char spath[256];

static void cleanup_and_exit(int code)
{
	if (srv_fd >= 0)
		close(srv_fd);
	if (lease_fd >= 0)
		close(lease_fd);
	if (card_fd >= 0)
		close(card_fd);
	if (spath[0])
		unlink(spath);
	if (pidfd >= 0) {
		char p[300];
		aux_path(p, sizeof p, PID_NAME);
		unlink(p);
	}
	exit(code);
}

static void on_term(int sig)
{
	(void)sig;
	cleanup_and_exit(0);
}

static void send_lease_fd(int cl)
{
	char buf[1] = { 'F' };
	char cbuf[CMSG_SPACE(sizeof(int))];
	struct iovec iov = { .iov_base = buf, .iov_len = 1 };
	struct msghdr msg = { 0 };
	struct cmsghdr *c;

	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = sizeof cbuf;
	c = CMSG_FIRSTHDR(&msg);
	c->cmsg_level = SOL_SOCKET;
	c->cmsg_type = SCM_RIGHTS;
	c->cmsg_len = CMSG_LEN(sizeof(int));
	memcpy(CMSG_DATA(c), &lease_fd, sizeof(int));
	if (sendmsg(cl, &msg, MSG_NOSIGNAL) < 0)
		fprintf(stderr, "sendmsg fd: %s\n", strerror(errno));
	else
		logmsg("served lease fd to client\n");
}

static void do_revoke_live(int cl)
{
	if (card_fd >= 0 && g_lessee) {
		struct drm_mode_revoke_lease r = { .lessee_id = g_lessee };

		if (ioctl(card_fd, DRM_IOCTL_MODE_REVOKE_LEASE, &r) < 0)
			fprintf(stderr, "revoke: %s\n", strerror(errno));
		else
			logmsg("revoked lessee=%u (hotplug fired)\n", g_lessee);
		g_lessee = 0;
	}
	if (cl >= 0) {
		char ack = 'K';

		send(cl, &ack, 1, MSG_NOSIGNAL);
	}
	cleanup_and_exit(0);
}

static int cmd_daemon(void)
{
	struct mint m;
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	char p[300];
	int one = 1;

	signal(SIGPIPE, SIG_IGN);
	signal(SIGTERM, on_term);
	signal(SIGINT, on_term);

	card_fd = open(card_path, O_RDWR | O_CLOEXEC);
	if (card_fd < 0)
		die("open %s", card_path);

	if (mint_lease(card_fd, &m) < 0)
		die("mint_lease");
	lease_fd = (int)m.fd;
	g_lessee = m.lessee_id;

	/* belt-and-braces: a plain client never sits on master anyway */
	ioctl(card_fd, DRM_IOCTL_DROP_MASTER, 0);

	sock_path(spath, sizeof spath);
	unlink(spath);
	srv_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (srv_fd < 0)
		die("socket");
	setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	snprintf(sa.sun_path, sizeof sa.sun_path, "%s", spath);
	if (bind(srv_fd, (struct sockaddr *)&sa, sizeof sa) < 0)
		die("bind %s", spath);
	chmod(spath, 0666);
	if (listen(srv_fd, 8) < 0)
		die("listen");

	aux_path(p, sizeof p, PID_NAME);
	pidfd = open(p, O_WRONLY | O_CREAT | O_CLOEXEC, 0666);
	if (pidfd >= 0) {
		if (flock(pidfd, LOCK_EX | LOCK_NB) < 0) {
			fprintf(stderr,
				"another dp-lease-helper daemon is running (%s)\n", p);
			exit(1);
		}
		if (ftruncate(pidfd, 0) == 0) {
			char b[32];
			int n = snprintf(b, sizeof b, "%d\n", getpid());

			(void)write(pidfd, b, (size_t)n);
		}
	}
	write_state(&m, spath);
	logmsg("daemon up: sock=%s lessee=%u\n", spath, m.lessee_id);

	for (;;) {
		int cl = accept(srv_fd, NULL, NULL);
		char c = 0;
		ssize_t n;

		if (cl < 0) {
			if (errno == EINTR)
				continue;
			die("accept");
		}
		n = recv(cl, &c, 1, 0);
		if (n == 1) {
			if (c == 'F')
				send_lease_fd(cl);
			else if (c == 'R')
				do_revoke_live(cl);
			else {
				char nak = 'N';

				send(cl, &nak, 1, MSG_NOSIGNAL);
			}
		}
		close(cl);
	}
}

static int connect_sock(const char *path)
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

static int cmd_revoke(void)
{
	char p[256];
	int s;
	char c = 'R', ack = 0;

	sock_path(p, sizeof p);
	s = connect_sock(p);
	if (s >= 0) {
		if (send(s, &c, 1, MSG_NOSIGNAL) == 1)
			recv(s, &ack, 1, 0);
		close(s);
		if (ack == 'K') {
			logmsg("revoke: daemon acknowledged\n");
			return 0;
		}
		fprintf(stderr, "revoke: daemon nack/timeout\n");
		return 1;
	}
	/* fallback: direct revoke (root) using state file */
	{
		uint32_t lessee = 0;
		char sock[256] = "";
		int fd;
		struct drm_mode_revoke_lease rv;
		char aux[300];

		if (read_state_lessee(&lessee, sock, sizeof sock) < 0 || !lessee) {
			fprintf(stderr, "revoke: no daemon and no state\n");
			return 1;
		}
		fd = open(card_path, O_RDWR | O_CLOEXEC);
		if (fd < 0)
			die("open %s", card_path);
		rv.lessee_id = lessee;
		if (ioctl(fd, DRM_IOCTL_MODE_REVOKE_LEASE, &rv) < 0) {
			fprintf(stderr, "revoke direct: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		close(fd);
		logmsg("revoke: direct ioctl lessee=%u ok\n", lessee);
		aux_path(aux, sizeof aux, STATE_NAME);
		unlink(aux);
		aux_path(aux, sizeof aux, PID_NAME);
		unlink(aux);
		return 0;
	}
}

static int cmd_status(void)
{
	char p[300], sp[256] = "";
	uint32_t lessee = 0;
	int have_state = read_state_lessee(&lessee, sp, sizeof sp) == 0;
	int s;

	aux_path(p, sizeof p, STATE_NAME);
	if (have_state) {
		FILE *f = fopen(p, "r");
		char line[320];

		printf("state file %s:\n", p);
		if (f) {
			while (fgets(line, sizeof line, f))
				fputs(line, stdout);
			fclose(f);
		}
	} else {
		printf("state file %s: missing\n", p);
	}
	sock_path(sp, sizeof sp);
	printf("socket %s: ", sp);
	s = connect_sock(sp);
	if (s >= 0) {
		printf("alive (connected)\n");
		close(s);
	} else {
		printf("dead (%s)\n", strerror(errno));
	}
	return have_state ? 0 : 1;
}

int main(int argc, char **argv)
{
	int i;
	const char *cmd = NULL;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--card") && i + 1 < argc)
			card_path = argv[++i];
		else if (!strcmp(argv[i], "--connector") && i + 1 < argc)
			opt_connector = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--type") && i + 1 < argc)
			opt_type = (uint32_t)strtoul(argv[++i], NULL, 0);
		else if (argv[i][0] != '-')
			cmd = argv[i];
	}
	if (!cmd)
		cmd = "daemon";

	if (!strcmp(cmd, "daemon"))
		return cmd_daemon();
	if (!strcmp(cmd, "revoke"))
		return cmd_revoke();
	if (!strcmp(cmd, "status"))
		return cmd_status();

	fprintf(stderr,
		"usage: dp-lease-helper [daemon|revoke|status] [--card PATH]\n"
		"                        [--connector ID] [--type N]\n"
		"env: DP_LEASE_SOCK, DP_LEASE_DIR\n");
	return 2;
}
