/* crtcstate.c — dump the CONNECTED connector's CRTC + DPMS/brightness state.
 * IDs are runtime-discovered (conn67/crtc206 were piano boot IDs; canoe
 * re-enumerates every boot/service restart). Optional arg: connector id. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <xf86drm.h>
#include <drm.h>
#include <xf86drmMode.h>

int main(int argc, char **argv) {
    int fd = open("/dev/dri/card0", O_RDWR);
    if (fd < 0) { perror("open"); return 1; }
    drmModeRes *res = drmModeGetResources(fd);
    if (!res) { perror("GetResources"); return 2; }
    uint32_t want = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 0;
    drmModeConnector *n = NULL;
    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if (!c) continue;
        if (want ? c->connector_id == want
                 : (c->connection == DRM_MODE_CONNECTED && c->count_modes)) {
            n = c;
            break;
        }
        drmModeFreeConnector(c);
    }
    if (!n) { printf("no connected connector\n"); return 3; }
    printf("conn%u: type=%d connection=%d encoder=%u count_modes=%d\n",
           n->connector_id, n->connector_type, n->connection,
           n->encoder_id, n->count_modes);
    uint32_t crtc_id = 0;
    if (n->encoder_id) {
        drmModeEncoder *e = drmModeGetEncoder(fd, n->encoder_id);
        if (e) { crtc_id = e->crtc_id; drmModeFreeEncoder(e); }
    }
    if (!crtc_id && res->count_crtcs) crtc_id = res->crtcs[0];
    drmModeCrtc *c = crtc_id ? drmModeGetCrtc(fd, crtc_id) : NULL;
    if (c) {
        printf("crtc%u: mode_valid=%d mode=%s %dx%d\n",
               crtc_id, c->mode_valid, c->mode.name,
               c->mode.hdisplay, c->mode.vdisplay);
        drmModeFreeCrtc(c);
    } else printf("crtc%u: %s\n", crtc_id, strerror(errno));
    drmModeObjectPropertiesPtr p =
        drmModeObjectGetProperties(fd, n->connector_id, DRM_MODE_OBJECT_CONNECTOR);
    if (p) {
        for (uint32_t i = 0; i < p->count_props; i++) {
            drmModePropertyPtr pr = drmModeGetProperty(fd, p->props[i]);
            if (!pr) continue;
            if (!strcmp(pr->name, "DPMS") || !strcmp(pr->name, "brightness") ||
                !strcmp(pr->name, "power_mode"))
                printf("conn%u %s = %llu\n", n->connector_id, pr->name,
                       (unsigned long long)p->prop_values[i]);
            drmModeFreeProperty(pr);
        }
        drmModeFreeObjectProperties(p);
    }
    drmModeFreeConnector(n);
    drmModeFreeResources(res);
    return 0;
}
