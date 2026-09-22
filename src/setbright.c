/* setbright.c — write connector 67 "brightness" prop (vendor SDE prop 87).
 * usage: setbright [value]   (no arg = print range only) */
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

int main(int argc, char **argv) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    uint64_t val = (uint64_t)-1;
    if (argc > 1) val = strtoull(argv[1], NULL, 0);

    drmModePropertyPtr p = drmModeGetProperty(fd, 87);
    if (!p) { printf("prop 87: %s\n", strerror(errno)); return 2; }
    printf("prop87 %s flags 0x%x range [%lu..%lu]\n",
           p->name, p->flags, (unsigned long)p->values[0], (unsigned long)p->values[1]);
    drmModeFreeProperty(p);
    if (argc <= 1) return 0;

    struct drm_mode_obj_set_property req = {
        .obj_id = 67, .obj_type = DRM_MODE_OBJECT_CONNECTOR,
        .prop_id = 87, .value = val,
    };
    errno = 0;
    int ret = drmIoctl(fd, DRM_IOCTL_MODE_OBJ_SETPROPERTY, &req);
    printf("set %llu -> %d (%s)\n", (unsigned long long)val, ret,
           ret ? strerror(errno) : "ok");
    return ret != 0;
}
