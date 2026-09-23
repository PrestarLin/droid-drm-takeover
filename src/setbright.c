/* setbright.c — backlight brightness control (canoe adaptation).
 * Primary path: sysfs /sys/class/backlight/panel0-backlight (works on both
 * piano and canoe; canoe has NO DRM "brightness" connector prop — its
 * prop 87 is avr_step_state [0..1], piano's setbright hardcoded 87/67).
 * Fallback: DRM connector prop found BY NAME "brightness".
 * usage: setbright [value]   (no arg = print sysfs range + current) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <xf86drm.h>
#include <drm.h>
#include <xf86drmMode.h>

#define BL_SYS "/sys/class/backlight/panel0-backlight/brightness"
#define BL_MAX "/sys/class/backlight/panel0-backlight/max_brightness"

static long read_long(const char *path) {
    char buf[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;
    return strtol(buf, NULL, 10);
}

static int drm_brightness_fallback(long val) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) return -1;
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { close(fd); return -1; }
    uint32_t conn = 0;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (c->connection == DRM_MODE_CONNECTED && c->count_modes) {
            conn = c->connector_id;
            drmModeFreeConnector(c);
            break;
        }
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    if (!conn) { close(fd); return -1; }
    drmModeConnector *c = drmModeGetConnector(fd, conn);
    if (!c) { close(fd); return -1; }
    uint32_t prop_id = 0;
    for (int i = 0; i < c->count_props; i++) {
        drmModePropertyPtr p = drmModeGetProperty(fd, c->props[i]);
        if (!p) continue;
        if (!strcmp(p->name, "brightness")) { prop_id = p->prop_id; drmModeFreeProperty(p); break; }
        drmModeFreeProperty(p);
    }
    drmModeFreeConnector(c);
    if (!prop_id) { close(fd); return -1; }
    struct drm_mode_obj_set_property req = {
        .obj_id = conn, .obj_type = DRM_MODE_OBJECT_CONNECTOR,
        .prop_id = prop_id, .value = (uint64_t)val,
    };
    int ret = drmIoctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &req);
    printf("drm conn%u brightness=%ld -> %d (%s)\n", conn, val, ret,
           ret ? strerror(errno) : "ok");
    close(fd);
    return ret;
}

int main(int argc, char **argv) {
    long max = read_long(BL_MAX);
    long cur = read_long(BL_SYS);
    if (argc <= 1) {
        if (max < 0) { printf("no sysfs backlight: %s\n", strerror(errno)); return 1; }
        printf("sysfs %s range [0..%ld] current=%ld\n", BL_SYS, max, cur);
        return 0;
    }
    long val = strtol(argv[1], NULL, 0);
    if (max > 0 && val > max) val = max;
    if (val < 0) val = 0;
    int fd = open(BL_SYS, O_WRONLY);
    if (fd >= 0) {
        char b[32];
        int n = snprintf(b, sizeof b, "%ld\n", val);
        ssize_t w = write(fd, b, n);
        close(fd);
        long now = read_long(BL_SYS);
        printf("sysfs set %ld -> write=%zd now=%ld (%s)\n", val, w, now,
               w == n && now == val ? "ok" : strerror(errno));
        if (w == n && now == val) return 0;
    }
    if (drm_brightness_fallback(val) == 0) return 0;
    return 1;
}
