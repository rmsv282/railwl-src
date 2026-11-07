/* clip_test: a from-scratch clipboard *reader*.
 *
 * Binds a data device, waits for the selection, then asks the owner for
 * text/plain over a pipe and prints what arrives. This is the receiving half
 * of the end-to-end clipboard check and, unlike wl-clipboard, it is ours: if
 * this passes while wl-paste crashes, the bug is not in railwl.
 *
 * The selection is only sent to the client holding keyboard focus
 * (wl_data_device.selection is documented as going out "immediately before
 * receiving keyboard focus"), so like every other clipboard reader we map a
 * throwaway toplevel to take focus; the offer then arrives on our device.
 *
 *   ./clip_test              read the regular selection
 *   ./clip_test --primary    read the primary selection
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <sys/mman.h>
#include <wayland-client.h>
#include "primary-selection-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

static struct wl_display *dp;
static struct wl_data_offer *pending_offer;
static int got_data;

static int read_all(int rfd)
{
    char buf[4096];
    for(;;){
        struct pollfd p = { rfd, POLLIN, 0 };
        if(poll(&p, 1, 2000) <= 0) break;      /* nothing more is coming */
        ssize_t n = read(rfd, buf, sizeof buf);
        if(n <= 0) break;
        fwrite(buf, 1, (size_t)n, stdout);
        got_data = 1;
    }
    fflush(stdout);
    return got_data;
}

/* ---------------- wl_data_offer ---------------- */
static void offer_offer(void *d, struct wl_data_offer *o, const char *mime)
{ (void)d; (void)o; fprintf(stderr, "clip_test: offered mime=%s\n", mime); }
static void offer_source_actions(void *d, struct wl_data_offer *o, uint32_t a)
{ (void)d; (void)o; (void)a; }
static void offer_action(void *d, struct wl_data_offer *o, uint32_t a)
{ (void)d; (void)o; (void)a; }
static const struct wl_data_offer_listener offer_listener = {
    .offer = offer_offer,
    .source_actions = offer_source_actions,
    .action = offer_action,
};

/* data_offer and selection both hand us the same object; libwayland complains
   if a listener is added twice, so only ever attach it once per offer */
static struct wl_data_offer *listened_offer;
static void listen_offer(struct wl_data_offer *o)
{
    if(o && o != listened_offer){
        listened_offer = o;
        wl_data_offer_add_listener(o, &offer_listener, NULL);
    }
}

static void request_data(const char *mime)
{
    int p[2];
    if(!pending_offer){ fprintf(stderr, "clip_test: no offer\n"); return; }
    if(pipe(p) < 0){ perror("pipe"); return; }
    /* the write end goes to the owner through the compositor */
    wl_data_offer_receive(pending_offer, mime, p[1]);
    close(p[1]);
    wl_display_flush(dp);
    read_all(p[0]);
    close(p[0]);
}

/* ---------------- wl_data_device ---------------- */
static void dev_data_offer(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
    (void)d; (void)dev;
    pending_offer = o;
    listen_offer(o);
}
static void dev_enter(void *d, struct wl_data_device *dev, uint32_t s, struct wl_surface *sf,
                      int32_t x, int32_t y, struct wl_data_offer *o)
{ (void)d;(void)dev;(void)s;(void)sf;(void)x;(void)y;(void)o; }
static void dev_leave(void *d, struct wl_data_device *dev){ (void)d;(void)dev; }
static void dev_motion(void *d, struct wl_data_device *dev, uint32_t t, int32_t x, int32_t y)
{ (void)d;(void)dev;(void)t;(void)x;(void)y; }
static void dev_drop(void *d, struct wl_data_device *dev){ (void)d;(void)dev; }

static void dev_selection(void *d, struct wl_data_device *dev, struct wl_data_offer *o)
{
    (void)d; (void)dev;
    pending_offer = o;
    if(!o){ fprintf(stderr, "clip_test: selection cleared\n"); return; }
    listen_offer(o);
    request_data("text/plain");
}

static const struct wl_data_device_listener dev_listener = {
    .data_offer = dev_data_offer,
    .enter = dev_enter,
    .leave = dev_leave,
    .motion = dev_motion,
    .drop = dev_drop,
    .selection = dev_selection,
};

/* ---------------- primary selection ---------------- */
static void poffer_offer(void *d, struct zwp_primary_selection_offer_v1 *o, const char *mime)
{ (void)d;(void)o; fprintf(stderr, "clip_test: offered primary mime=%s\n", mime); }
static const struct zwp_primary_selection_offer_v1_listener poffer_listener = {
    .offer = poffer_offer,
};
static struct zwp_primary_selection_offer_v1 *listened_poffer;
static void listen_poffer(struct zwp_primary_selection_offer_v1 *o)
{
    if(o && o != listened_poffer){
        listened_poffer = o;
        zwp_primary_selection_offer_v1_add_listener(o, &poffer_listener, NULL);
    }
}

static void pdev_selection(void *d, struct zwp_primary_selection_device_v1 *dev,
                           struct zwp_primary_selection_offer_v1 *o)
{
    (void)d; (void)dev;
    int p[2];
    if(!o){ fprintf(stderr, "clip_test: primary selection cleared\n"); return; }
    listen_poffer(o);
    if(pipe(p) < 0){ perror("pipe"); return; }
    zwp_primary_selection_offer_v1_receive(o, "text/plain", p[1]);
    close(p[1]);
    wl_display_flush(dp);
    read_all(p[0]);
    close(p[0]);
}
static void pdev_data_offer(void *d, struct zwp_primary_selection_device_v1 *dev,
                            struct zwp_primary_selection_offer_v1 *o)
{
    (void)d; (void)dev;
    listen_poffer(o);
}
static const struct zwp_primary_selection_device_v1_listener pdev_listener = {
    .data_offer = pdev_data_offer,
    .selection = pdev_selection,
};

/* ---------------- registry ---------------- */
static struct wl_data_device_manager *ddev_mgr;
static struct zwp_primary_selection_device_manager_v1 *pmgr;
static struct wl_seat *seat;
static struct wl_compositor *comp;
static struct xdg_wm_base *wm_base;
static struct wl_shm *shm;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver)
{
    (void)d;
    if(!strcmp(iface, "wl_data_device_manager"))
        ddev_mgr = wl_registry_bind(reg, name, &wl_data_device_manager_interface,
                                    ver < 3 ? ver : 3);
    else if(!strcmp(iface, "zwp_primary_selection_device_manager_v1"))
        pmgr = wl_registry_bind(reg, name,
                                &zwp_primary_selection_device_manager_v1_interface, 1);
    else if(!strcmp(iface, "wl_seat"))
        seat = wl_registry_bind(reg, name, &wl_seat_interface, ver < 5 ? ver : 5);
    else if(!strcmp(iface, "wl_compositor"))
        comp = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    else if(!strcmp(iface, "xdg_wm_base"))
        wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, ver < 6 ? ver : 6);
    else if(!strcmp(iface, "wl_shm"))
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *reg, uint32_t name)
{ (void)d;(void)reg;(void)name; }
static const struct wl_registry_listener reg_listener = { reg_global, reg_remove };

/* ---------------- throwaway toplevel (to take focus) ---------------- */
static struct wl_surface *surf;
static struct xdg_surface *xsurf;
static struct xdg_toplevel *toplevel;

static void wm_ping(void *d, struct xdg_wm_base *b, uint32_t serial)
{ (void)d; xdg_wm_base_pong(b, serial); }
static const struct xdg_wm_base_listener wm_listener = { .ping = wm_ping };

static void xsurf_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    (void)d;
    xdg_surface_ack_configure(s, serial);
    /* commit with no buffer: enough for the compositor to map and focus us */
    wl_surface_commit(surf);
}
static const struct xdg_surface_listener xsurf_listener = { .configure = xsurf_configure };

static void make_focus_surface(void)
{
    if(!comp || !wm_base || !shm) return;
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    surf = wl_compositor_create_surface(comp);
    xsurf = xdg_wm_base_get_xdg_surface(wm_base, surf);
    xdg_surface_add_listener(xsurf, &xsurf_listener, NULL);
    toplevel = xdg_surface_get_toplevel(xsurf);
    xdg_toplevel_set_title(toplevel, "clip_test");
    /* a toplevel only becomes mapped - and so only takes focus - once it has a
       buffer; a bufferless commit is ignored, exactly like it is on wlroots */
    int stride = 4;
    int fd = memfd_create("clip_test", MFD_CLOEXEC);
    if(fd < 0){ wl_surface_commit(surf); return; }
    if(ftruncate(fd, stride) < 0){ /* ignore */ }
    void *px = mmap(NULL, stride, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(px != MAP_FAILED) memset(px, 0, (size_t)stride);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, stride);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, 1, 1, stride,
                                                      WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);              /* the buffer holds its own reference */
    wl_surface_attach(surf, buf, 0, 0);
    wl_surface_damage(surf, 0, 0, 1, 1);
    wl_surface_commit(surf);
}

int main(int argc, char **argv)
{
    int primary = argc > 1 && !strcmp(argv[1], "--primary");
    dp = wl_display_connect(NULL);
    if(!dp){ fprintf(stderr, "clip_test: cannot connect\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dp);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(dp);
    wl_display_roundtrip(dp);
    if(!seat){ fprintf(stderr, "clip_test: no wl_seat\n"); return 1; }

    if(primary){
        if(!pmgr){ fprintf(stderr, "clip_test: no primary manager\n"); return 1; }
        struct zwp_primary_selection_device_v1 *pd =
            zwp_primary_selection_device_manager_v1_get_device(pmgr, seat);
        zwp_primary_selection_device_v1_add_listener(pd, &pdev_listener, NULL);
    } else {
        if(!ddev_mgr){ fprintf(stderr, "clip_test: no data device manager\n"); return 1; }
        struct wl_data_device *dev = wl_data_device_manager_get_data_device(ddev_mgr, seat);
        wl_data_device_add_listener(dev, &dev_listener, NULL);
    }
    /* now take focus, which is when the compositor sends us the selection */
    make_focus_surface();
    for(int i = 0; i < 200 && !got_data; i++){
        wl_display_flush(dp);
        struct pollfd pfd = { wl_display_get_fd(dp), POLLIN, 0 };
        if(poll(&pfd, 1, 20) > 0) wl_display_dispatch(dp);
        else wl_display_dispatch_pending(dp);
    }
    fprintf(stderr, "clip_test: %s\n", got_data ? "OK" : "TIMEOUT");
    wl_display_disconnect(dp);
    return got_data ? 0 : 1;
}
