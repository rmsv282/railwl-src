#define _GNU_SOURCE
#ifdef __FreeBSD__
#include <sys/types.h>
#include <sys/sysctl.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <wayland-server.h>
#include <time.h>
#include <drm_fourcc.h>
#include "rail.h"
#include "xdg-shell-protocol.h"
#include "xdg-decoration-protocol.h"
#include "primary-selection-unstable-v1-protocol.h"
#include "linux-dmabuf-v1-protocol.h"

/* ---------------- wl_shm ---------------- */

static void shm_pool_destroy(struct wl_resource *r)
{
    struct rail_pool *p = wl_resource_get_user_data(r);
    if(!p) return;
    p->dead = true;
    if(p->refs == 0){ if(p->data) munmap(p->data, p->size); if(p->fd >= 0) close(p->fd); free(p); }
}

static void shm_buffer_destroy(struct wl_resource *r)
{
    struct rail_buffer *b = wl_resource_get_user_data(r);
    if(!b) return;
    struct rail_pool *p = b->pool;
    /* detach from any surface still referencing this buffer */
    if(b->rail){
        for(struct rail_surface *s = b->rail->all_surfaces; s; s = s->g_next){
            if(s->front == b){
                s->front = NULL;
                /* a dmabuf's texture *is* the buffer's storage: it must go
                   with it (the next commit re-imports and re-creates it) */
                if(b->is_dmabuf) rail_output_forget_tex(s);
            }
            if(s->pending == b) s->pending = NULL;
        }
        /* a buffer waiting to be released may be gone before the flip */
        if(b->rel_pending){
            for(struct rail_buffer **pp = &b->rail->rel_pending; *pp; pp = &(*pp)->rel_next){
                if(*pp == b){ *pp = b->rel_next; break; }
            }
            b->rel_pending = false;
        }
    }
    if(b->is_dmabuf) rail_dmabuf_destroy(b);
    free(b);
    if(p && --p->refs == 0 && p->dead){ if(p->data) munmap(p->data, p->size); if(p->fd >= 0) close(p->fd); free(p); }
}

static void buf_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static const struct wl_buffer_interface buf_impl = { .destroy = buf_destroy };

static void pool_create_buffer(struct wl_client *client, struct wl_resource *r,
                               uint32_t id, int32_t off, int32_t w, int32_t h,
                               int32_t stride, uint32_t fmt)
{
    struct rail_pool *p = wl_resource_get_user_data(r);
    /* Everything below comes from the client and decides which bytes the
       renderer reads, so none of it is trusted: an offset, size or stride that
       runs past the pool would otherwise read outside the mapping. */
    if(fmt != WL_SHM_FORMAT_ARGB8888 && fmt != WL_SHM_FORMAT_XRGB8888){
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FORMAT, "unsupported format 0x%x", fmt);
        return;
    }
    if(!p->data || off < 0 || w <= 0 || h <= 0 || w > 16384 || h > 16384 ||
       stride < w * 4 || (int64_t)stride * h + off > (int64_t)p->size){
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_STRIDE,
                               "buffer %dx%d stride %d offset %d does not fit the pool (%zu bytes)",
                               w, h, stride, off, p->size);
        return;
    }
    struct rail_buffer *b = calloc(1, sizeof *b);
    if(!b){ wl_client_post_no_memory(client); return; }
    b->fd = -1;
    b->rail = p->rail;
    b->pool = p; p->refs++;
    b->format = fmt; b->w = w; b->h = h; b->stride = stride;
    b->size = (size_t)stride * h;
    b->pool_off = (size_t)off;
    struct wl_resource *res = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if(!res){ p->refs--; free(b); wl_client_post_no_memory(client); return; }
    b->resource = res;
    wl_resource_set_implementation(res, &buf_impl, b, shm_buffer_destroy);
}

static void pool_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }
/* A client may grow a pool (never shrink it) and then create buffers in the new
   part. Ignoring the request left those buffers outside the mapping, so the
   first draw of one read memory that is not there. mremap works on the shared
   mapping without the fd (which was closed at creation), and buffers find their
   pixels through rail_buf_data(), so a moved mapping needs no fix-up. */
static void pool_resize(struct wl_client *c, struct wl_resource *r, int32_t sz)
{
    (void)c;
    struct rail_pool *p = wl_resource_get_user_data(r);
    if(sz <= 0 || (size_t)sz < p->size || !p->data){
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "invalid pool resize to %d", sz);
        return;
    }
    if((size_t)sz == p->size) return;
#ifdef __linux__
    void *n = mremap(p->data, p->size, (size_t)sz, MREMAP_MAYMOVE);
#else
    /* no mremap outside Linux: map the (already grown) file again, then drop the old mapping */
    void *n = MAP_FAILED;
    if(p->fd >= 0){
        n = mmap(NULL, (size_t)sz, PROT_READ|PROT_WRITE, MAP_SHARED, p->fd, 0);
        if(n != MAP_FAILED) munmap(p->data, p->size);
    }
#endif
    if(n == MAP_FAILED){
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "cannot grow the pool to %d bytes", sz);
        return;
    }
    p->data = n;
    p->size = (size_t)sz;
}

static const struct wl_shm_pool_interface pool_impl = {
    .create_buffer = pool_create_buffer,
    .destroy = pool_destroy,
    .resize = pool_resize,
};

static void shm_create_pool(struct wl_client *client, struct wl_resource *r,
                            uint32_t id, int32_t fd, int32_t size)
{
    if(size <= 0){
        close(fd);
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_STRIDE, "invalid pool size %d", size);
        return;
    }
    struct rail_pool *p = calloc(1, sizeof *p);
    if(!p){ close(fd); wl_client_post_no_memory(client); return; }
    p->rail = wl_resource_get_user_data(r);
    p->size = size;
    p->data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
#ifdef __linux__
    close(fd);          /* mremap needs no fd */
    p->fd = -1;
#else
    p->fd = fd;         /* kept for pool_resize, closed when the pool is freed */
#endif
    if(p->data == MAP_FAILED){
        if(p->fd >= 0) close(p->fd);
        free(p);
        wl_resource_post_error(r, WL_SHM_ERROR_INVALID_FD, "cannot map the shm pool");
        return;
    }
    struct wl_resource *res = wl_resource_create(client, &wl_shm_pool_interface, 1, id);
    if(!res){ munmap(p->data, p->size); if(p->fd >= 0) close(p->fd); free(p); wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &pool_impl, p, shm_pool_destroy);
}

static void shm_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct wl_shm_interface shm_impl = {
    .create_pool = shm_create_pool,
    .release = shm_release,
};

static void bind_shm(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 1) ver = 1;
    struct wl_resource *res = wl_resource_create(client, &wl_shm_interface, ver, id);
    wl_resource_set_implementation(res, &shm_impl, rail, NULL);
    wl_shm_send_format(res, WL_SHM_FORMAT_ARGB8888);
    wl_shm_send_format(res, WL_SHM_FORMAT_XRGB8888);
}

/* ---------------- wl_compositor / wl_surface ---------------- */

static void surface_destroy_res(struct wl_resource *r)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->dying = true;           /* the wl_surface is going away: no more events */
    rail_focus_forget(s);      /* never leave a dangling focus pointer behind */
    if(s->mapped) rail_view_unmap(s);
    /* orphan any children: they outlive this surface object but must never
       keep pointing at freed memory */
    while(s->popups){
        struct rail_surface *ch = s->popups;
        s->popups = ch->popup_next;
        ch->popup_next = NULL;
        ch->popup_parent = NULL;
        ch->mapped = false;
    }
    if(s->is_sub && s->popup_parent){
        struct rail_surface **pp = &s->popup_parent->popups;
        while(*pp && *pp != s) pp = &(*pp)->popup_next;
        if(*pp == s) *pp = s->popup_next;
        s->popup_parent = NULL;
    }
    /* unlink from server's all_surfaces list */
    struct rail *rail = s->rail;
    if(rail){
        struct rail_surface **pp = &rail->all_surfaces;
        while(*pp && *pp != s) pp = &(*pp)->g_next;
        if(*pp == s) *pp = s->g_next;
    }
    if(rail && rail->output && rail->output->bar == s) rail->output->bar = NULL;
    if(rail && rail->output && rail->output->launcher == s) rail->output->launcher = NULL;
    if(rail && s->is_dialog){
        struct rail_surface **dp = rail->output ? &rail->output->dialogs : NULL;
        while(dp && *dp && *dp != s) dp = &(*dp)->dlg_next;
        if(dp && *dp == s) *dp = s->dlg_next;
        s->dlg_next = NULL;
    }
    if(rail && rail->cursor == s){ rail->cursor = NULL; rail->cursor_set = false; rail_output_frame(rail); }
    /* Role resources keep this surface as their user data. Destroy them here,
       while s is still alive: during client teardown libwayland walks the
       resource table in id order, so a wl_subsurface or xdg_* resource whose
       wl_surface was already freed would otherwise dereference freed memory.
       Every one of these destructors clears its own field on s, so a resource
       libwayland already destroyed is simply skipped. */
    if(s->popup){ wl_resource_destroy(s->popup); s->popup = NULL; }
    if(s->toplevel){ wl_resource_destroy(s->toplevel); s->toplevel = NULL; }
    if(s->subsurface){ wl_resource_destroy(s->subsurface); s->subsurface = NULL; }
    if(s->xdg_surface){ wl_resource_destroy(s->xdg_surface); s->xdg_surface = NULL; }
    if(s->frame_cb){ wl_resource_destroy(s->frame_cb); s->frame_cb = NULL; }
    free(s);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *buf_r, int32_t x, int32_t y)
{
    (void)c;(void)x;(void)y;
    struct rail_surface *s = wl_resource_get_user_data(r);
    s->attach_null = (buf_r == NULL);
    s->pending = buf_r ? wl_resource_get_user_data(buf_r) : NULL;
}
/* accumulate damage until the renderer uploads it: only that rectangle is
   re-sent to the GPU instead of the whole buffer.  Scale and transform are
   always 1/normal here, so surface and buffer coordinates are the same. */
static void dmg_add(struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s || w <= 0 || h <= 0) return;
    int64_t x1 = (int64_t)x + w, y1 = (int64_t)y + h;
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    if(x1 > INT32_MAX) x1 = INT32_MAX;
    if(y1 > INT32_MAX) y1 = INT32_MAX;
    if(!s->dmg_any){
        s->dmg_any = true;
        s->dmg_x0 = x; s->dmg_y0 = y; s->dmg_x1 = (int)x1; s->dmg_y1 = (int)y1;
        return;
    }
    if(x < s->dmg_x0) s->dmg_x0 = x;
    if(y < s->dmg_y0) s->dmg_y0 = y;
    if(x1 > s->dmg_x1) s->dmg_x1 = (int)x1;
    if(y1 > s->dmg_y1) s->dmg_y1 = (int)y1;
}
static void surface_damage(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; dmg_add(r, x, y, w, h); }

/* every wl_surface.frame callback MUST get its done event: a client that never
   gets one stops painting (the window looks frozen). dropping one silently is
   a bug, so replacing/unmapping fires it first. */
static void frame_cb_fire(struct rail_surface *s)
{
    if(!s->frame_cb) return;
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    struct wl_resource *cb = s->frame_cb;
    s->frame_cb = NULL;
    wl_callback_send_done(cb, (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000));
    wl_resource_destroy(cb);
}

static void frame_cb_destroy(struct wl_resource *r)
{
    /* client dropped the callback before we fired it: forget it, or the next
       send_frame_cbs would write into freed memory */
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(s && s->frame_cb == r) s->frame_cb = NULL;
    if(getenv("RAIL_DEBUG"))
        fprintf(stderr, "railwl: CB destroy surf=%p\n", (void *)s);
}

static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t cb_id)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    /* a client may queue a new one before the old fires; replace, never clobber */
    frame_cb_fire(s);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, cb_id);
    s->frame_cb = cb;
    wl_resource_set_implementation(cb, NULL, s, frame_cb_destroy);
    if(getenv("RAIL_DEBUG"))
        fprintf(stderr, "railwl: CB request surf=%p sub=%d map=%d\n",
                (void *)s, s->is_sub, s->mapped);
}
static void surface_set_opaque(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg)
{ (void)c;(void)r;(void)reg; }
static void surface_set_input(struct wl_client *c, struct wl_resource *r, struct wl_resource *reg)
{ (void)c;(void)r;(void)reg; }
static void surface_commit(struct wl_client *c, struct wl_resource *r);
static void surface_set_transform(struct wl_client *c, struct wl_resource *r, int32_t t)
{ (void)c;(void)r;(void)t; }
static void surface_set_scale(struct wl_client *c, struct wl_resource *r, int32_t sc)
{ (void)c;(void)r;(void)sc; }
static void surface_damage_buffer(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; dmg_add(r, x, y, w, h); }
static void surface_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{ (void)c;(void)r;(void)x;(void)y; }

static void surface_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
/* v7: asks for a wl_callback fired when the surface may be released. We do not
   track buffer lifetime that precisely, so answer immediately - a client that
   waits for this callback would otherwise be stuck */
static void surface_get_release(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)r;
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    wl_resource_set_implementation(cb, NULL, NULL, NULL);
    wl_callback_send_done(cb, 0);
    wl_resource_destroy(cb);
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque,
    .set_input_region = surface_set_input,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_transform,
    .set_buffer_scale = surface_set_scale,
    .damage_buffer = surface_damage_buffer,
    .offset = surface_offset,
    .get_release = surface_get_release,
};

/* Centre a floating window (the launcher) at the size of its current buffer.
   The client first receives the screen-sized configure that xdg_get_toplevel
   sends, and its first buffer can have that size; the 520x320 configure only
   follows once the app_id is known. So the window is re-placed on every
   commit that changes its size, not just once at map time. */
/* the window's own size: its declared geometry when that fits in the buffer
   (the rest is a CSD shadow margin), else the whole buffer */
static void float_content_size(const struct rail_surface *s, int *w, int *h)
{
    *w = s->w > 0 ? s->w : RAIL_FLOAT_W;
    *h = s->h > 0 ? s->h : RAIL_FLOAT_H;
    if(s->geo_w > 0 && s->geo_h > 0 && s->w > 0 && s->h > 0 &&
       s->geo_x >= 0 && s->geo_y >= 0 &&
       s->geo_x + s->geo_w <= s->w && s->geo_y + s->geo_h <= s->h){
        *w = s->geo_w; *h = s->geo_h;
    }
}

static void float_place(struct rail_surface *s)
{
    struct rail_output *o = s->rail->output;
    int w, h;
    float_content_size(s, &w, &h);
    s->tw = w; s->th = h;
    s->cw = w + 2 * RAIL_BORDER; s->ch = h + 2 * RAIL_BORDER;
    s->cx = (o->w - s->cw) / 2;
    s->cy = (o->h - s->ch) / 2;
    if(s->cx < 0) s->cx = 0;
    if(s->cy < 0) s->cy = 0;
}

static bool float_size_changed(const struct rail_surface *s)
{
    int w, h;
    float_content_size(s, &w, &h);
    return s->tw != w || s->th != h;
}

static void surface_commit_inner(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(getenv("RAIL_DEBUG"))
        fprintf(stderr, "railwl: COMMIT surf=%p cl=%p sub=%d popup=%d bar=%d map=%d top=%p null=%d pend=%p frontw=%d fronth=%d\n",
                (void *)s, (void *)c, s->is_sub, s->is_popup, s->is_bar, s->mapped,
                (void *)s->toplevel, s->attach_null, (void *)s->pending,
                s->front ? s->front->w : -1, s->front ? s->front->h : -1);
    if(s->attach_null){
        /* attach(NULL) + commit = unmap/hide the surface */
        s->attach_null = false;
        s->pending = NULL;
        frame_cb_fire(s);
        if(s->mapped) rail_view_unmap(s);
        s->front = NULL;
        rail_output_frame(s->rail);
        return;
    }
    if(s->pending){
        struct rail_buffer *old = s->front;
        s->front = s->pending;
        s->pending = NULL;
        if(s->front){
            s->w = s->front->w; s->h = s->front->h;
            s->front->released = false;   /* every commit earns one release */
            s->buffer_dirty = true;       /* renderer must upload it */
            /* a replaced buffer we never uploaded still needs its release */
            if(old && old != s->front && !old->released){
                old->released = true;
                wl_buffer_send_release(old->resource);
            }
        }
    } else if(!s->front){
        return;
    } else {
        s->buffer_dirty = true; /* re-commit of current buffer (content may have changed) */
    }
    if(s->is_sub && !s->mapped){
        rail_view_map(s);
    } else if(s->is_sub){
        s->cw = s->w; s->ch = s->h;
        rail_child_place(s);
        rail_output_frame(s->rail);
    } else if(s->is_popup && s->xdg_surface && !s->mapped){
        rail_view_map(s);   /* menus, dropdowns, tooltips: first buffer maps them */
    } else if(s->xdg_surface && s->toplevel && !s->mapped){        rail_view_map(s);
    } else if(s->mapped && (s->is_float || s->is_dialog) && s->rail->output && float_size_changed(s)){
        float_place(s);
        rail_output_frame(s->rail);
    } else if(s->mapped){
        rail_output_frame(s->rail);
    } else if(s->rail->cursor == s){
        /* cursor surface: not a tile, still needs a redraw */
        rail_output_frame(s->rail);
    }
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    surface_commit_inner(c, r);
    /* a surface that asked for a frame callback (popups, not-yet-mapped
       surfaces...) must get a render so the callback is answered */
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(s && s->frame_cb) rail_output_frame(s->rail);
}

static void compositor_create_surface(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct rail *rail = wl_resource_get_user_data(r);
    struct rail_surface *s = calloc(1, sizeof *s);
    s->rail = rail;
    s->g_next = rail->all_surfaces; rail->all_surfaces = s;
    struct wl_resource *res = wl_resource_create(client, &wl_surface_interface,
                                                 wl_resource_get_version(r), id);
    s->resource = res;
    wl_resource_set_implementation(res, &surface_impl, s, surface_destroy_res);
}
static void region_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static void region_add(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h){ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static void region_sub(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h){ (void)c;(void)r;(void)x;(void)y;(void)w;(void)h; }
static const struct wl_region_interface region_impl = {
    .destroy = region_destroy, .add = region_add, .subtract = region_sub,
};

static void compositor_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    (void)r;
    struct wl_resource *res = wl_resource_create(c, &wl_region_interface, 1, id);
    wl_resource_set_implementation(res, &region_impl, NULL, NULL);
}
static void compositor_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
    .release = compositor_release,
};

static void bind_compositor(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    /* never announce more than the client asked for: a client bound at v1 has
       no handler for v6 events and libwayland will abort on them */
    if(ver > 4) ver = 4;
    struct wl_resource *res = wl_resource_create(client, &wl_compositor_interface, ver, id);
    wl_resource_set_implementation(res, &compositor_impl, rail, NULL);
}

/* ---------------- xdg-shell ---------------- */

static void toplevel_destroy_res(struct wl_resource *r)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(s) s->toplevel = NULL;
}

/* configure a toplevel: configure_bounds first (v4+), then the size and states.
   one helper so every configure site stays in sync. */

/* Every xdg_surface.configure carries a serial the client hands back through
   ack_configure. It is opaque to us - we accept any value - but it has to be a
   real, opaquely-unique serial rather than a constant, so use the display's
   counter (the same one input events draw from). */
static uint32_t configure_serial(struct rail *r)
{
    return r->display ? wl_display_next_serial(r->display) : 1;
}

/* ---- dialogs: file choosers, confirmations, anything transient or fixed-size ---- */
static struct rail_surface *top_dialog(struct rail *r)
{
    struct rail_surface *t = NULL;
    if(!r->output) return NULL;
    for(struct rail_surface *d = r->output->dialogs; d; d = d->dlg_next)
        if(d->mapped) t = d;
    return t;
}

static void dialog_unlink(struct rail *r, struct rail_surface *s)
{
    if(!r || !r->output) return;
    struct rail_surface **pp = &r->output->dialogs;
    while(*pp && *pp != s) pp = &(*pp)->dlg_next;
    if(*pp == s) *pp = s->dlg_next;
    s->dlg_next = NULL;
}

/* the root toplevel a popup/subsurface hangs off */
static struct rail_surface *surface_root(struct rail_surface *s)
{
    while(s && s->popup_parent) s = s->popup_parent;
    return s;
}

static void toplevel_configure(struct rail_surface *s, int w, int h)
{
    if(!s->toplevel){
        if(s->xdg_surface)
            xdg_surface_send_configure(s->xdg_surface, configure_serial(s->rail));
        return;
    }
    struct wl_array states;
    wl_array_init(&states);
    struct rail *r = s->rail;
    bool focused = r->output && r->output->active && r->output->active->focused == s;
    uint32_t *v;
    if(!s->is_bar && !s->is_popup && !s->is_float && !s->is_dialog){
        /* scrolling tile: tell the client it is tiled (no CSD shadows) */
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_TILED_LEFT;
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_TILED_RIGHT;
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_TILED_TOP;
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_TILED_BOTTOM;
    }
    if(s->is_dialog && r->output) focused = top_dialog(r) == s;   /* a dialog owns the keyboard */
    if(focused){
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_ACTIVATED;
    }
    if(s->fullscreen){
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_FULLSCREEN;
    }
    if(s->maximized){
        v = wl_array_add(&states, sizeof *v); *v = XDG_TOPLEVEL_STATE_MAXIMIZED;
    }
    if(wl_resource_get_version(s->toplevel) >= XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION){
        if(s->is_dialog && r->output)   /* size 0x0 = "pick your own", but never bigger than a tile */
            xdg_toplevel_send_configure_bounds(s->toplevel, RAIL_TILE_W(r->output->w), RAIL_TILE_H(r->output->h));
        else
            xdg_toplevel_send_configure_bounds(s->toplevel, w, h);
    }
    xdg_toplevel_send_configure(s->toplevel, w, h, &states);
    wl_array_release(&states);
    if(s->xdg_surface)
        xdg_surface_send_configure(s->xdg_surface, configure_serial(s->rail));
}

/* re-send configures for every tile in the active strip so cross-cutting
   state (ACTIVATED on the focused surface) stays correct after a focus move */
void rail_configure_all(struct rail *r)
{
    struct rail_ws *ws = r->output ? r->output->active : NULL;
    if(!ws || !ws->first) return;
    for(struct rail_surface *s = ws->first;; s = s->next){
        if(s->mapped && s->toplevel)
            toplevel_configure(s, s->tw, s->th);
        if(s->next == ws->first) break;
    }
}
/* A toplevel with a parent, or one that cannot be resized, is a dialog: it
   floats centred above the strip at the size its client chooses instead of
   being forced into a full-size tile. Both hints arrive before the first
   commit, so the client is told "choose your own size" (0x0) right away -
   it would otherwise keep the screen-sized configure xdg_get_toplevel sent. */
static void mark_dialog(struct rail_surface *s)
{
    if(!s || s->is_dialog || s->is_bar || s->is_float || s->mapped) return;
    s->is_dialog = true;
    toplevel_configure(s, 0, 0);
}
static void dialog_check_fixed(struct rail_surface *s)
{
    if(s->min_w > 0 && s->min_h > 0 && s->min_w == s->max_w && s->min_h == s->max_h)
        mark_dialog(s);
}
static void t_set_parent(struct wl_client *c, struct wl_resource *r, struct wl_resource *p)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(p) mark_dialog(s);
}
static void mark_dialog(struct rail_surface *s);
static void t_set_title(struct wl_client *c, struct wl_resource *r, const char *t)
{ (void)c; struct rail_surface *s = wl_resource_get_user_data(r); if(s) snprintf(s->title,sizeof s->title,"%s",t); }
static void t_set_app_id(struct wl_client *c, struct wl_resource *r, const char *a)
{ (void)c; struct rail_surface *s = wl_resource_get_user_data(r);
  if(s){ snprintf(s->app_id,sizeof s->app_id,"%s",a);
         s->is_bar = (strcmp(a, "railbar") == 0);
         s->is_float = (strcmp(a, "raillauncher") == 0);
         /* a file chooser / confirmation made by xdg-desktop-portal runs in its
            own process: it has no wl_surface parent we could see (no xdg_foreign),
            so it is recognised by who it is */
         if(strstr(a, "portal")) mark_dialog(s);
         /* xdg_get_toplevel already sent a screen-sized configure, before the
            app_id was known. The client takes the last size it is given, so send
            the floating size now. */
         if(s->is_float) toplevel_configure(s, RAIL_FLOAT_W, RAIL_FLOAT_H); } }
static void t_show_menu(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, int32_t x, int32_t y){(void)c;(void)r;(void)seat;(void)serial;(void)x;(void)y;}
static void t_move(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial)
{
    (void)c;(void)seat;(void)serial;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    /* a strip tile never moves. the client is now waiting for the compositor
       to take the pointer and give it back, and ignores clicks until it does:
       leave + enter ends that wait */
    if(s->prev){ rail_pointer_reenter(s); return; }
    rail_drag_begin(s, 1, 0, 0); /* grab pos set on next motion */
}
static void t_resize(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial, uint32_t edges)
{
    (void)c;(void)seat;(void)serial;
    struct rail_surface *s = wl_resource_get_user_data(r);
    /* xdg edges: 1=top 2=bottom 4=left 8=right -> rail drag bits: 2=right 8=bottom 16=left 32=top */
    int mode = 0;
    if(edges & 1) mode |= 32;
    if(edges & 2) mode |= 8;
    if(edges & 4) mode |= 16;
    if(edges & 8) mode |= 2;
    if(!s) return;
    if(s->prev){ rail_pointer_reenter(s); return; }
    rail_drag_begin(s, mode, 0, 0);
}
static void t_set_max_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->max_w = w; s->max_h = h;
    dialog_check_fixed(s);
}
static void t_set_min_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->min_w = w; s->min_h = h;
    dialog_check_fixed(s);
}
/* tiles already fill the work area, so maximize is a state change, not a resize */
static void t_set_maximized(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->maximized = true;
    toplevel_configure(s, s->tw, s->th);
    rail_output_frame(s->rail);
}
static void t_unset_maximized(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->maximized = false;
    toplevel_configure(s, s->tw, s->th);
    rail_output_frame(s->rail);
}
static void t_set_fullscreen(struct wl_client *c, struct wl_resource *r, struct wl_resource *o)
{
    (void)c;(void)o;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s || !s->rail->output) return;
    struct rail_output *out = s->rail->output;
    s->fullscreen = true;
    /* cover the whole output, bar included, and snap (no slide) */
    s->x = 0; s->y = 0; s->tw = out->w; s->th = out->h;
    s->cx = 0; s->cy = 0; s->cw = out->w; s->ch = out->h;
    toplevel_configure(s, out->w, out->h);
    rail_output_frame(s->rail);
}
static void t_unset_fullscreen(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s || !s->rail->output) return;
    struct rail_output *o = s->rail->output;
    s->fullscreen = false;
    /* back into the strip, below the bar */
    s->x = RAIL_GAP; s->y = RAIL_BAR_HEIGHT + RAIL_GAP;
    s->tw = RAIL_TILE_W(o->w); s->th = RAIL_TILE_H(o->h);
    toplevel_configure(s, s->tw, s->th);
    rail_output_frame(s->rail);
}
static void t_set_minimized(struct wl_client *c, struct wl_resource *r){(void)c;(void)r;}
static void t_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = t_destroy,
    .set_parent = t_set_parent,
    .set_title = t_set_title,
    .set_app_id = t_set_app_id,
    .show_window_menu = t_show_menu,
    .move = t_move,
    .resize = t_resize,
    .set_max_size = t_set_max_size,
    .set_min_size = t_set_min_size,
    .set_maximized = t_set_maximized,
    .unset_maximized = t_unset_maximized,
    .set_fullscreen = t_set_fullscreen,
    .unset_fullscreen = t_unset_fullscreen,
    .set_minimized = t_set_minimized,
};

static void xdg_get_toplevel(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    struct wl_resource *res = wl_resource_create(c, &xdg_toplevel_interface,
                                                 wl_resource_get_version(r), id);
    s->toplevel = res;
    wl_resource_set_implementation(res, &toplevel_impl, s, toplevel_destroy_res);
    /* initial configure with tiled geometry */
    struct rail *rail = s->rail;
    if(rail->output){
        s->tw = rail->output->w; s->th = rail->output->h;
        toplevel_configure(s, s->tw, s->th);
    }
}

/* ---- xdg_positioner ---- */
static void pos_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static void pos_set_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{ (void)c; struct rail_positioner *p = wl_resource_get_user_data(r); if(p){ p->width = w; p->height = h; } }
static void pos_set_anchor_rect(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{ (void)c; struct rail_positioner *p = wl_resource_get_user_data(r); if(p){ p->anchor_x=x; p->anchor_y=y; p->anchor_w=w; p->anchor_h=h; } }
static void pos_set_anchor(struct wl_client *c, struct wl_resource *r, uint32_t a)
{ (void)c; struct rail_positioner *p = wl_resource_get_user_data(r); if(p) p->anchor = (int32_t)a; }
static void pos_set_gravity(struct wl_client *c, struct wl_resource *r, uint32_t g)
{ (void)c; struct rail_positioner *p = wl_resource_get_user_data(r); if(p) p->gravity = (int32_t)g; }
static void pos_set_constraint(struct wl_client *c, struct wl_resource *r, uint32_t cons)
{ (void)c; struct rail_positioner *p = wl_resource_get_user_data(r); if(p) p->constraint = (int32_t)cons; }
static void pos_set_offset(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{ (void)c; struct rail_positioner *p = wl_resource_get_user_data(r); if(p){ p->offset_x=x; p->offset_y=y; } }
static void pos_set_reactive(struct wl_client *c, struct wl_resource *r)
{ (void)c; (void)r; }
static void pos_set_parent_size(struct wl_client *c, struct wl_resource *r, int32_t w, int32_t h)
{ (void)c;(void)r;(void)w;(void)h; }
static void pos_set_parent_configure(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{ (void)c;(void)r;(void)serial; }

static const struct xdg_positioner_interface positioner_impl = {
    .destroy = pos_destroy, .set_size = pos_set_size, .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor, .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint, .set_offset = pos_set_offset,
    .set_reactive = pos_set_reactive, .set_parent_size = pos_set_parent_size,
    .set_parent_configure = pos_set_parent_configure,
};

static void positioner_res_destroy(struct wl_resource *r)
{ struct rail_positioner *p = wl_resource_get_user_data(r); free(p); }

static void xdg_create_positioner(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct rail_positioner *p = calloc(1, sizeof *p);
    p->width = p->height = 1;
    struct wl_resource *res = wl_resource_create(c, &xdg_positioner_interface,
                                                 wl_resource_get_version(r), id);
    wl_resource_set_implementation(res, &positioner_impl, p, positioner_res_destroy);
}

/* ---- xdg_popup ---- */
static void popup_destroy_res(struct wl_resource *r);

/* screen position of the parent's window-geometry origin: the xdg_popup
   positioner and the xdg_popup.configure position are both relative to it.
   for a tile that is where its content is inset to; for a popup, its own origin. */
static void popup_parent_origin(struct rail_surface *s, int *px, int *py)
{
    *px = s->popup_parent ? s->popup_parent->cx + rail_child_inset(s->popup_parent) : 0;
    *py = s->popup_parent ? s->popup_parent->cy + rail_child_inset(s->popup_parent) : 0;
}

/* xdg_positioner anchor/gravity are an enum, not flags:
   0 none, 1 top, 2 bottom, 3 left, 4 right, 5 top_left, 6 bottom_left,
   7 top_right, 8 bottom_right */
static int pos_is_left(int e)   { return e == 3 || e == 5 || e == 6; }
static int pos_is_right(int e)  { return e == 4 || e == 7 || e == 8; }
static int pos_is_top(int e)    { return e == 1 || e == 5 || e == 7; }
static int pos_is_bottom(int e) { return e == 2 || e == 6 || e == 8; }
static int pos_flip_h(int e)
{ switch(e){ case 3: return 4; case 4: return 3; case 5: return 7; case 7: return 5;
             case 6: return 8; case 8: return 6; default: return e; } }
static int pos_flip_v(int e)
{ switch(e){ case 1: return 2; case 2: return 1; case 5: return 6; case 6: return 5;
             case 7: return 8; case 8: return 7; default: return e; } }

/* top-left of the popup relative to the parent geometry origin */
static void pos_resolve(const struct rail_positioner *p, int anchor, int gravity, int *x, int *y)
{
    int ax = p->anchor_x, ay = p->anchor_y;
    if(pos_is_right(anchor))       ax += p->anchor_w;
    else if(!pos_is_left(anchor))  ax += p->anchor_w / 2;
    if(pos_is_bottom(anchor))      ay += p->anchor_h;
    else if(!pos_is_top(anchor))   ay += p->anchor_h / 2;
    ax += p->offset_x; ay += p->offset_y;
    if(pos_is_left(gravity))       ax -= p->width;
    else if(!pos_is_right(gravity)) ax -= p->width / 2;
    if(pos_is_top(gravity))        ay -= p->height;
    else if(!pos_is_bottom(gravity)) ay -= p->height / 2;
    *x = ax; *y = ay;
}

/* resolve popup position from positioner + parent geometry, clamp to output */
static void popup_place(struct rail_surface *s, struct rail_positioner *p)
{
    struct rail *rail = s->rail;
    int px, py; popup_parent_origin(s, &px, &py);
    int W = rail->output->w, H = rail->output->h;

    int rx, ry;
    pos_resolve(p, p->anchor, p->gravity, &rx, &ry);
    int x = px + rx, y = py + ry;

    /* constraint_adjustment bits: slide_x=1 slide_y=2 flip_x=4 flip_y=8 resize_x=16 resize_y=32 */
    if((p->constraint & 4) && (x < 0 || x + p->width > W)){
        int fx, fy; pos_resolve(p, pos_flip_h(p->anchor), pos_flip_h(p->gravity), &fx, &fy);
        if(px + fx >= 0 && px + fx + p->width <= W) x = px + fx;
    }
    if((p->constraint & 8) && (y < 0 || y + p->height > H)){
        int fx, fy; pos_resolve(p, pos_flip_v(p->anchor), pos_flip_v(p->gravity), &fx, &fy);
        if(py + fy >= 0 && py + fy + p->height <= H) y = py + fy;
    }
    if((p->constraint & 1) && x + p->width > W) x = W - p->width;
    if((p->constraint & 2) && y + p->height > H) y = H - p->height;
    /* clamp hard to output bounds */
    if(x + p->width > W) x = W - p->width;
    if(y + p->height > H) y = H - p->height;
    if(x < 0) x = 0;
    if(y < 0) y = 0;

    s->pop_x = x; s->pop_y = y;
    s->tw = p->width; s->th = p->height;
}

/* tell the client where its popup ended up: position is relative to the
   parent's window geometry, NOT the screen */
static void popup_send_configure(struct rail_surface *s, struct wl_resource *popup_res,
                                 struct wl_resource *xdg_surf)
{
    int px, py; popup_parent_origin(s, &px, &py);
    xdg_popup_send_configure(popup_res, s->pop_x - px, s->pop_y - py, s->tw, s->th);
    xdg_surface_send_configure(xdg_surf, configure_serial(s->rail));
}

static void popup_grab(struct wl_client *c, struct wl_resource *r, struct wl_resource *seat, uint32_t serial)
{ (void)c;(void)seat; struct rail_surface *s = wl_resource_get_user_data(r); if(s) s->popup_grab_serial = serial; }
static void popup_reposition(struct wl_client *c, struct wl_resource *r, struct wl_resource *positioner, uint32_t token)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    struct rail_positioner *p = positioner ? wl_resource_get_user_data(positioner) : NULL;
    if(s && p){
        popup_place(s, p);
        if(wl_resource_get_version(r) >= XDG_POPUP_REPOSITIONED_SINCE_VERSION)
            xdg_popup_send_repositioned(r, token);
        if(s->xdg_surface) popup_send_configure(s, r, s->xdg_surface);
        rail_output_frame(s->rail);
    }
}
static void popup_destroy_cb(struct wl_client *c, struct wl_resource *r);

static const struct xdg_popup_interface popup_impl = {
    .destroy = popup_destroy_cb, .grab = popup_grab, .reposition = popup_reposition,
};

static void popup_destroy_res(struct wl_resource *r)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    /* unlink from parent's popup list */
    if(s->popup_parent){
        struct rail_surface **pp = &s->popup_parent->popups;
        while(*pp && *pp != s) pp = &(*pp)->popup_next;
        if(*pp == s) *pp = s->popup_next;
    }
    s->popup = NULL; s->is_popup = false;
    if(s->mapped){ s->mapped = false; rail_output_frame(s->rail); }
}

static void popup_destroy_cb(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static void xdg_get_popup(struct wl_client *c, struct wl_resource *r, uint32_t id,
                          struct wl_resource *parent, struct wl_resource *pos)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    struct rail_positioner *p = pos ? wl_resource_get_user_data(pos) : NULL;
    struct wl_resource *res = wl_resource_create(c, &xdg_popup_interface,
                                                 wl_resource_get_version(r), id);
    s->is_popup = true;
    s->popup = res;

    /* resolve parent surface: parent is xdg_surface or xdg_popup */
    struct rail_surface *parent_s = NULL;
    if(parent){
        void *ud = wl_resource_get_user_data(parent);
        /* if parent is an xdg_surface its user_data is the surface itself */
        parent_s = (struct rail_surface*)ud;
    }
    s->popup_parent = parent_s;
    if(parent_s){
        s->popup_next = parent_s->popups;
        parent_s->popups = s;
    }
    if(p) popup_place(s, p);

    wl_resource_set_implementation(res, &popup_impl, s, popup_destroy_res);

    /* configure popup at placed size */
    if(p) popup_send_configure(s, res, r);
}

static void xdg_pong(struct wl_client *c, struct wl_resource *r, uint32_t serial){(void)c;(void)r;(void)serial;}
static void xdg_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static void xdg_surface_ack(struct wl_client *c, struct wl_resource *r, uint32_t serial)
{ (void)c;(void)serial; struct rail_surface *s = wl_resource_get_user_data(r); if(s) s->configured = true; }
static void xdg_surface_set_geometry(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s || s->is_popup) return;
    if(s->geo_x != x || s->geo_y != y || s->geo_w != w || s->geo_h != h){
        s->geo_x = x; s->geo_y = y; s->geo_w = w; s->geo_h = h;
        /* a floating window's frame follows its geometry */
        if(s->mapped && (s->is_float || s->is_dialog) && s->rail->output && float_size_changed(s))
            float_place(s);
        rail_output_frame(s->rail);
    }
}
static void xdg_surface_destroy_res(struct wl_resource *r)
{ struct rail_surface *s = wl_resource_get_user_data(r); if(s) s->xdg_surface = NULL; }
static void xdg_surface_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_get_toplevel,
    .get_popup = xdg_get_popup,
    .set_window_geometry = xdg_surface_set_geometry,
    .ack_configure = xdg_surface_ack,
};

static void xdg_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surf_r)
{
    struct rail_surface *s = wl_resource_get_user_data(surf_r);
    struct wl_resource *res = wl_resource_create(c, &xdg_surface_interface,
                                                 wl_resource_get_version(r), id);
    s->xdg_surface = res;
    wl_resource_set_implementation(res, &xdg_surface_impl, s, xdg_surface_destroy_res);
}

static const struct xdg_wm_base_interface wm_base_full = {
    .destroy = xdg_destroy,
    .create_positioner = xdg_create_positioner,
    .get_xdg_surface = xdg_get_xdg_surface,
    .pong = xdg_pong,
};

static void bind_wm_base(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 6) ver = 6;
    struct wl_resource *res = wl_resource_create(client, &xdg_wm_base_interface, ver, id);
    wl_resource_set_implementation(res, &wm_base_full, rail, NULL);
}


/* ---------------- xdg-decoration: force server-side (no client decorations) ---------------- */

static void dec_manager_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static void dec_set_mode(struct wl_client *c, struct wl_resource *r, uint32_t mode)
{ (void)c; (void)mode; /* client preference ignored: railwl is always server-side */ (void)r; }
static void dec_unset_mode(struct wl_client *c, struct wl_resource *r)
{ (void)c;(void)r; }
static void dec_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct zxdg_toplevel_decoration_v1_interface dec_toplevel_impl = {
    .destroy = dec_destroy,
    .set_mode = dec_set_mode,
    .unset_mode = dec_unset_mode,
};

static void dec_get_toplevel_decoration(struct wl_client *c, struct wl_resource *r,
                                        uint32_t id, struct wl_resource *toplevel)
{
    (void)r;
    struct rail_surface *s = wl_resource_get_user_data(toplevel);
    struct wl_resource *res = wl_resource_create(c, &zxdg_toplevel_decoration_v1_interface, 1, id);
    wl_resource_set_implementation(res, &dec_toplevel_impl, s, NULL);
    /* tell client: server-side. no CSD, ever. */
    zxdg_toplevel_decoration_v1_send_configure(res,
        ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static const struct zxdg_decoration_manager_v1_interface dec_manager_impl = {
    .destroy = dec_manager_destroy,
    .get_toplevel_decoration = dec_get_toplevel_decoration,
};

static void bind_decoration_manager(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 1) ver = 1;
    struct wl_resource *res = wl_resource_create(client, &zxdg_decoration_manager_v1_interface, ver, id);
    wl_resource_set_implementation(res, &dec_manager_impl, rail, NULL);
}

/* ---------------- core clipboard: wl_data_device_manager ---------------- */

/* a data source offers mime types and, on receive, sends data over an fd */
static void src_offer(struct wl_client *c, struct wl_resource *r, const char *mime)
{
    (void)c;
    struct rail_data_source *s = wl_resource_get_user_data(r);
    if(s && mime && s->n_mime < 8){ snprintf(s->mimes[s->n_mime++], 64, "%s", mime); }
}
static void src_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static void src_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t a)
{ (void)c;(void)r;(void)a; }

static const struct wl_data_source_interface src_impl = {
    .offer = src_offer, .destroy = src_destroy, .set_actions = src_set_actions,
};

static void src_res_destroy(struct wl_resource *r)
{
    struct rail_data_source *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->dead = true;
    /* if this was the active selection, clear it everywhere */
    if(s->rail && s->rail->selection_source == s){
        s->rail->selection_source = NULL;
        rail_selection_broadcast(s->rail);
    }
    free(s);
}

static void dm_create_data_source(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct rail_data_source *s = calloc(1, sizeof *s);
    s->rail = wl_resource_get_user_data(r);
    struct wl_resource *res = wl_resource_create(c, &wl_data_source_interface,
                                                 wl_resource_get_version(r), id);
    s->resource = res;
    wl_resource_set_implementation(res, &src_impl, s, src_res_destroy);
}

/* --- data offer: bridge between a source and a receiving client --- */
static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime)
{ (void)c;(void)r;(void)serial;(void)mime; }
static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)c;
    struct rail_data_offer *o = wl_resource_get_user_data(r);
    if(o && o->source && !o->source->dead && o->source->resource){
        /* hand the fd to the source; it writes the selection data */
        wl_data_source_send_send(o->source->resource, mime ? mime : "text/plain", fd);
    }
    if(fd >= 0) close(fd);
}
static void offer_finish(struct wl_client *c, struct wl_resource *r){ (void)c;(void)r; }
static void offer_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t d, uint32_t p)
{ (void)c;(void)r;(void)d;(void)p; }
static void offer_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static void offer_res_destroy(struct wl_resource *r)
{
    struct rail_data_offer *o = wl_resource_get_user_data(r);
    if(!o) return;
    if(o->device){
        if(o->device->offer == o->resource){
            o->device->offer = NULL;
            o->device->offer_src = NULL;
        }
        struct rail_data_offer **pp = &o->device->offers;
        while(*pp && *pp != o) pp = &(*pp)->dev_next;
        if(*pp == o) *pp = o->dev_next;
    }
    free(o);
}

static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept, .receive = offer_receive, .destroy = offer_destroy,
    .finish = offer_finish, .set_actions = offer_set_actions,
};

/* Push the current selection to whichever client holds keyboard focus.
 *
 * The protocol is explicit about this (wl_data_device.selection): the event is
 * sent "immediately before receiving keyboard focus and when a new selection
 * is set while the client has keyboard focus", and the offer stays valid only
 * until the client loses focus. Pushing it to every device instead looks more
 * generous, but it hands out offers at arbitrary times - including during
 * another client's startup - which is not what clients expect, and clients
 * that map a throwaway surface just to receive the selection (wl-paste) are
 * entitled to not be told about it before that surface exists.
 *
 * A device that already holds an offer for this exact source is left alone, so
 * moving focus around does not churn through offer objects. */
void rail_selection_broadcast(struct rail *r)
{
    struct rail_ws *ws = r->output ? r->output->active : NULL;
    struct rail_surface *focus = ws ? ws->focused : NULL;
    struct wl_client *target = focus && focus->resource
        ? wl_resource_get_client(focus->resource) : NULL;
    struct rail_data_source *src = r->selection_source;
    if(src && src->dead) src = NULL;
    for(struct rail_data_device *d = r->data_devices; d; d = d->next){
        if(!d->resource) continue;
        if(!target || wl_resource_get_client(d->resource) != target){
            /* not focused: any offer it holds is no longer valid, and focus
               will hand it a fresh one */
            d->offer = NULL; d->offer_src = NULL;
            continue;
        }
        if(!src){
            if(d->offer) wl_data_device_send_selection(d->resource, NULL);
            d->offer = NULL; d->offer_src = NULL;
            continue;
        }
        if(d->offer && d->offer_src == src) continue;   /* already has this one */
        /* build the offer, advertise the mime types, then send selection */
        struct rail_data_offer *o = calloc(1, sizeof *o);
        o->source = src;
        o->device = d;
        o->dev_next = d->offers;
        d->offers = o;
        struct wl_resource *ores = wl_resource_create(
            wl_resource_get_client(d->resource), &wl_data_offer_interface,
            wl_resource_get_version(d->resource), 0);
        o->resource = ores;
        wl_resource_set_implementation(ores, &offer_impl, o, offer_res_destroy);
        wl_data_device_send_data_offer(d->resource, ores);
        for(int i=0;i<src->n_mime;i++)
            wl_data_offer_send_offer(ores, src->mimes[i]);
        wl_data_device_send_selection(d->resource, ores);
        d->offer = ores; d->offer_src = src;
    }
}

/* --- data device (one per seat binding) --- */
static void dev_start_drag(struct wl_client *c, struct wl_resource *r,
                           struct wl_resource *src, struct wl_resource *origin,
                           struct wl_resource *icon, uint32_t serial)
{ (void)c;(void)r;(void)src;(void)origin;(void)icon;(void)serial; /* dnd: no-op */ }

static void dev_set_selection(struct wl_client *c, struct wl_resource *r,
                              struct wl_resource *src, uint32_t serial)
{
    (void)c;(void)serial;
    struct rail_data_device *d = wl_resource_get_user_data(r);
    if(!d || !d->rail) return;
    struct rail *rail = d->rail;
    struct rail_data_source *s = src ? wl_resource_get_user_data(src) : NULL;
    struct rail_data_source *old = rail->selection_source;
    rail->selection_source = s;
    /* the source we are replacing must be told its data is no longer wanted */
    if(old && old != s && old->resource && !old->dead)
        wl_data_source_send_cancelled(old->resource);
    rail_selection_broadcast(rail);
}

static void dev_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct wl_data_device_interface dev_impl = {
    .start_drag = dev_start_drag, .set_selection = dev_set_selection, .release = dev_release,
};

static void dev_res_destroy(struct wl_resource *r)
{
    struct rail_data_device *d = wl_resource_get_user_data(r);
    if(!d || !d->rail) return;
    /* offers outlive the device resource object: stop them pointing at
       memory that is about to be freed */
    for(struct rail_data_offer *o = d->offers; o; o = o->dev_next) o->device = NULL;
    d->offers = NULL;
    struct rail_data_device **pp = &d->rail->data_devices;
    while(*pp && *pp != d) pp = &(*pp)->next;
    if(*pp) *pp = d->next;
    free(d);
}

static void dm_get_data_device(struct wl_client *c, struct wl_resource *r, uint32_t id,
                               struct wl_resource *seat)
{
    (void)seat;
    struct rail *rail = wl_resource_get_user_data(r);
    struct rail_data_device *d = calloc(1, sizeof *d);
    d->rail = rail;
    struct wl_resource *res = wl_resource_create(c, &wl_data_device_interface,
                                                 wl_resource_get_version(r), id);
    d->resource = res;
    wl_resource_set_implementation(res, &dev_impl, d, dev_res_destroy);
    d->next = rail->data_devices; rail->data_devices = d;
    /* a client that binds late still needs the current selection */
    rail_selection_broadcast(rail);
}

static void dm_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct wl_data_device_manager_interface dm_impl = {
    .create_data_source = dm_create_data_source,
    .get_data_device = dm_get_data_device,
    .release = dm_release,
};

static void bind_data_manager(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 3) ver = 3;
    struct wl_resource *res = wl_resource_create(client, &wl_data_device_manager_interface, ver, id);
    wl_resource_set_implementation(res, &dm_impl, rail, NULL);
}

/* ---------------- primary selection (zwp_primary_selection_device_manager_v1) ----------------
   select-to-copy / middle-click paste. mirrors the clipboard above but on the
   primary-selection protocol terminals and toolkits actually use. */

static void psrc_offer(struct wl_client *c, struct wl_resource *r, const char *mime)
{
    (void)c;
    struct rail_data_source *s = wl_resource_get_user_data(r);
    if(s && mime && s->n_mime < 8) snprintf(s->mimes[s->n_mime++], 64, "%s", mime);
}
static void psrc_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static const struct zwp_primary_selection_source_v1_interface psrc_impl = {
    .offer = psrc_offer, .destroy = psrc_destroy,
};

static void psrc_res_destroy(struct wl_resource *r)
{
    struct rail_data_source *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->dead = true;
    if(s->rail && s->rail->primary_source == s){
        s->rail->primary_source = NULL;
        rail_primary_broadcast(s->rail);
    }
    free(s);
}

static void pdm_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct rail_data_source *s = calloc(1, sizeof *s);
    s->rail = wl_resource_get_user_data(r);
    struct wl_resource *res = wl_resource_create(c, &zwp_primary_selection_source_v1_interface, 1, id);
    s->resource = res;
    wl_resource_set_implementation(res, &psrc_impl, s, psrc_res_destroy);
}

static void poffer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)c;
    struct rail_data_offer *o = wl_resource_get_user_data(r);
    if(o && o->source && !o->source->dead && o->source->resource)
        zwp_primary_selection_source_v1_send_send(o->source->resource,
            mime ? mime : "text/plain;charset=utf-8", fd);
    if(fd >= 0) close(fd);
}
static void poffer_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static const struct zwp_primary_selection_offer_v1_interface poffer_impl = {
    .receive = poffer_receive, .destroy = poffer_destroy,
};

/* Same rules as the core selection (see rail_selection_broadcast): the offer is
 * only valid while the client holds keyboard focus, so only the focused
 * client's device is served, and an identical offer is not re-sent. */
void rail_primary_broadcast(struct rail *r)
{
    if(!r->output) return;
    struct rail_ws *ws = r->output->active;
    struct rail_surface *focus = ws ? ws->focused : NULL;
    struct wl_client *target = focus && focus->resource
        ? wl_resource_get_client(focus->resource) : NULL;
    struct rail_data_source *src = r->primary_source;
    if(src && src->dead) src = NULL;
    for(struct rail_data_device *d = r->primary_devices; d; d = d->next){
        if(!d->resource) continue;
        if(!target || wl_resource_get_client(d->resource) != target){
            d->offer = NULL; d->offer_src = NULL;   /* only valid while focused */
            continue;
        }
        if(!src){
            if(d->offer) zwp_primary_selection_device_v1_send_selection(d->resource, NULL);
            d->offer = NULL; d->offer_src = NULL;
            continue;
        }
        if(d->offer && d->offer_src == src) continue;   /* already has this one */
        struct rail_data_offer *o = calloc(1, sizeof *o);
        o->source = src;
        o->device = d;
        o->dev_next = d->offers;
        d->offers = o;
        struct wl_resource *ores = wl_resource_create(
            wl_resource_get_client(d->resource), &zwp_primary_selection_offer_v1_interface, 1, 0);
        o->resource = ores;
        wl_resource_set_implementation(ores, &poffer_impl, o, offer_res_destroy);
        zwp_primary_selection_device_v1_send_data_offer(d->resource, ores);
        for(int i=0;i<src->n_mime;i++)
            zwp_primary_selection_offer_v1_send_offer(ores, src->mimes[i]);
        zwp_primary_selection_device_v1_send_selection(d->resource, ores);
        d->offer = ores; d->offer_src = src;
    }
}

static void pdev_set_selection(struct wl_client *c, struct wl_resource *r,
                               struct wl_resource *src, uint32_t serial)
{
    (void)c;(void)serial;
    struct rail_data_device *d = wl_resource_get_user_data(r);
    if(!d || !d->rail) return;
    struct rail_data_source *s = src ? wl_resource_get_user_data(src) : NULL;
    d->rail->primary_source = s;
    rail_primary_broadcast(d->rail);
}
static void pdev_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static const struct zwp_primary_selection_device_v1_interface pdev_impl = {
    .set_selection = pdev_set_selection, .destroy = pdev_destroy,
};

static void pdev_res_destroy(struct wl_resource *r)
{
    struct rail_data_device *d = wl_resource_get_user_data(r);
    if(!d || !d->rail) return;
    struct rail_data_device **pp = &d->rail->primary_devices;
    while(*pp && *pp != d) pp = &(*pp)->next;
    if(*pp) *pp = d->next;
    free(d);
}

static void pdm_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id,
                           struct wl_resource *seat)
{
    (void)seat;
    struct rail *rail = wl_resource_get_user_data(r);
    struct rail_data_device *d = calloc(1, sizeof *d);
    d->rail = rail;
    struct wl_resource *res = wl_resource_create(c, &zwp_primary_selection_device_v1_interface, 1, id);
    d->resource = res;
    wl_resource_set_implementation(res, &pdev_impl, d, pdev_res_destroy);
    d->next = rail->primary_devices; rail->primary_devices = d;
    rail_primary_broadcast(rail);
}

static void pdm_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct zwp_primary_selection_device_manager_v1_interface pdm_impl = {
    .create_source = pdm_create_source,
    .get_device = pdm_get_device,
    .destroy = pdm_destroy,
};

static void bind_primary_manager(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 1) ver = 1;
    struct wl_resource *res = wl_resource_create(client, &zwp_primary_selection_device_manager_v1_interface, ver, id);
    wl_resource_set_implementation(res, &pdm_impl, rail, NULL);
}

/* ---------------- wl_subcompositor / wl_subsurface ---------------- */
static void sub_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

/* a subsurface is a client-positioned child of another surface: it lives in
   the parent's child list and is composited right after it */
static void sub_surf_destroy(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static void sub_surf_set_position(struct wl_client *c, struct wl_resource *r, int32_t x, int32_t y)
{
    (void)c;
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    s->sub_x = x; s->sub_y = y;
    rail_child_place(s);
    rail_output_frame(s->rail);
}
static void sub_surf_place(struct wl_resource *r, struct wl_resource *sib_r, bool above)
{
    (void)sib_r;
    struct rail_surface *s = wl_resource_get_user_data(r);
    struct rail_surface *par = s ? s->popup_parent : NULL;
    if(!par) return;
    if(above && par->popups == s) return;      /* already on top */
    struct rail_surface **pp = &par->popups;
    while(*pp && *pp != s) pp = &(*pp)->popup_next;
    if(*pp != s) return;
    *pp = s->popup_next;
    if(above){                                  /* head of list draws last, on top */
        s->popup_next = par->popups;
        par->popups = s;
    } else {                                    /* tail draws first, underneath */
        struct rail_surface **t = &par->popups;
        while(*t) t = &(*t)->popup_next;
        *t = s;
        s->popup_next = NULL;
    }
    rail_output_frame(s->rail);
}
static void sub_above(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib)
{ (void)c; sub_surf_place(r, sib, true); }
static void sub_below(struct wl_client *c, struct wl_resource *r, struct wl_resource *sib)
{ (void)c; sub_surf_place(r, sib, false); }
static void sub_sync(struct wl_client *c, struct wl_resource *r){ (void)c;(void)r; }
static void sub_desync(struct wl_client *c, struct wl_resource *r){ (void)c;(void)r; }

static const struct wl_subsurface_interface subsurface_impl = {
    .destroy = sub_surf_destroy,
    .set_position = sub_surf_set_position,
    .place_above = sub_above,
    .place_below = sub_below,
    .set_sync = sub_sync,
    .set_desync = sub_desync,
};

static void sub_res_destroy(struct wl_resource *r)
{
    struct rail_surface *s = wl_resource_get_user_data(r);
    if(!s) return;
    if(s->popup_parent){
        struct rail_surface **pp = &s->popup_parent->popups;
        while(*pp && *pp != s) pp = &(*pp)->popup_next;
        if(*pp == s) *pp = s->popup_next;
    }
    s->popup_parent = NULL; s->popup_next = NULL;
    s->is_sub = false; s->subsurface = NULL;
    s->mapped = false;
    rail_output_frame(s->rail);
}

static void sub_get_subsurface(struct wl_client *c, struct wl_resource *r, uint32_t id,
                               struct wl_resource *surf_r, struct wl_resource *parent_r)
{
    struct rail_surface *s = wl_resource_get_user_data(surf_r);
    struct rail_surface *par = wl_resource_get_user_data(parent_r);
    if(!s || !par) return;
    /* the id MUST get a real object: a client that keeps using it would
       otherwise desynchronise the whole connection */
    struct wl_resource *res = wl_resource_create(c, &wl_subsurface_interface,
                                                 wl_resource_get_version(r), id);
    s->subsurface = res;
    s->is_sub = true;
    s->popup_parent = par;
    s->popup_next = par->popups;
    par->popups = s;
    wl_resource_set_implementation(res, &subsurface_impl, s, sub_res_destroy);
    rail_child_place(s);
}
static const struct wl_subcompositor_interface sub_impl = {
    .destroy = sub_destroy, .get_subsurface = sub_get_subsurface,
};
static void bind_sub(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 1) ver = 1;
    struct wl_resource *res = wl_resource_create(client, &wl_subcompositor_interface, ver, id);
    wl_resource_set_implementation(res, &sub_impl, rail, NULL);
}

/* ---------------- strip (circular, wraps) ---------------- */

/* wl_surface.enter/leave: which outputs a surface is on. Clients (browsers in
   particular) use this to decide that a window is really visible and to pick
   the output's scale/refresh rate; a surface that never "enters" an output can
   be treated as hidden and left unpainted. */
static void surface_enter_outputs(struct rail_surface *s)
{
    if(!s->resource || s->dying) return;
    struct wl_client *cl = wl_resource_get_client(s->resource);
    for(struct rail_out_res *o = s->rail->outputs; o; o = o->next)
        if(wl_resource_get_client(o->resource) == cl)
            wl_surface_send_enter(s->resource, o->resource);
}

static void surface_leave_outputs(struct rail_surface *s)
{
    if(!s->resource || s->dying) return;
    struct wl_client *cl = wl_resource_get_client(s->resource);
    for(struct rail_out_res *o = s->rail->outputs; o; o = o->next)
        if(wl_resource_get_client(o->resource) == cl)
            wl_surface_send_leave(s->resource, o->resource);
}

void rail_view_map(struct rail_surface *s)
{
    struct rail *r = s->rail;
    struct rail_ws *ws = r->output->active;
    s->mapped = true;
    surface_enter_outputs(s);

    if(s->is_sub){
        /* client-positioned child of another surface */
        s->cw = s->tw = s->w;
        s->ch = s->th = s->h;
        rail_child_place(s);
        rail_output_frame(r);
        return;
    }
    if(s->is_popup){
        /* popup floats over its parent; position resolved by popup_place */
        /* draw at the size the client actually rendered, not the requested one */
        s->cx = s->pop_x; s->cy = s->pop_y;
        s->cw = s->w > 0 ? s->w : s->tw; s->ch = s->h > 0 ? s->h : s->th;
        if(s->xdg_surface)
            xdg_surface_send_configure(s->xdg_surface, configure_serial(r));
        rail_output_frame(r);
        return;
    }
    fprintf(stderr, "railwl: map toplevel app_id='%s' title='%s' size=%dx%d dialog=%d float=%d bar=%d\n",
            s->app_id, s->title, s->w, s->h, s->is_dialog, s->is_float, s->is_bar);
    if(s->is_dialog){
        /* centred at the size the client drew, above the strip and its popups,
           and it takes the keyboard until it closes */
        float_place(s);
        s->dlg_next = NULL;
        struct rail_surface **pp = &r->output->dialogs;
        while(*pp) pp = &(*pp)->dlg_next;
        *pp = s;
        rail_seat_set_focus(s);
        toplevel_configure(s, 0, 0);   /* delivers the ACTIVATED state */
        rail_output_frame(r);
        return;
    }
    if(s->is_float){
        /* a centred window at its own size: never enters the strip, sits above
           everything, and takes the keyboard until it closes */
        struct rail_output *o = r->output;
        if(o->launcher && o->launcher != s && o->launcher->toplevel)
            xdg_toplevel_send_close(o->launcher->toplevel);   /* only one at a time */
        int w = s->w > 0 ? s->w : RAIL_FLOAT_W;
        int h = s->h > 0 ? s->h : RAIL_FLOAT_H;
        s->tw = w; s->th = h;
        /* the frame is drawn around the content, so the window rect is the
           content grown by the border on every side */
        s->cw = w + 2 * RAIL_BORDER; s->ch = h + 2 * RAIL_BORDER;
        s->cx = (o->w - s->cw) / 2;
        s->cy = (o->h - s->ch) / 2;
        o->launcher = s;
        rail_seat_set_focus(s);
        rail_output_frame(r);
        return;
    }
    if(s->is_bar){
        s->x = 0; s->y = 0;
        s->tw = r->output->w; s->th = RAIL_BAR_HEIGHT;
        s->cx = 0; s->cy = 0; s->cw = s->tw; s->ch = s->th;
    } else {
        s->x = RAIL_GAP; s->y = RAIL_BAR_HEIGHT + RAIL_GAP;
        s->tw = RAIL_TILE_W(r->output->w); s->th = RAIL_TILE_H(r->output->h);
        s->cx = r->output->w; s->cy = s->y; s->cw = s->tw; s->ch = s->th;
    }
    if(s->is_bar){
        /* bar pins to top-left; never slides, never enters the strip */
        s->rail->output->bar = s;
        toplevel_configure(s, s->tw, s->th);
        rail_output_frame(r);
        return;
    }
    /* tiles: start offscreen right, slide to center */
    s->cx = r->output->w; s->cy = 0;
    s->cw = s->tw + 2 * RAIL_BORDER; s->ch = s->th + 2 * RAIL_BORDER;

    if(!ws->first){ ws->first = s; s->prev = s->next = s; }
    else{
        struct rail_surface *last = ws->first->prev;
        last->next = s; s->prev = last;
        s->next = ws->first; ws->first->prev = s;
    }
    ws->focused = s;
    rail_seat_set_focus(s);
    toplevel_configure(s, s->tw, s->th);
    rail_output_frame(r);
}

void rail_view_unmap(struct rail_surface *s)
{
    struct rail *r = s->rail;
    struct rail_ws *ws = r->output->active;
    s->mapped = false;
    surface_leave_outputs(s);
    if(s->is_bar){ if(r->output->bar == s) r->output->bar = NULL; rail_output_frame(r); return; }
    if(s->is_dialog){
        dialog_unlink(r, s);
        struct rail_surface *next = top_dialog(r);
        rail_seat_set_focus(next ? next : (ws ? ws->focused : NULL));   /* back to the next dialog or the strip */
        if(next) toplevel_configure(next, 0, 0);
        rail_output_frame(r);
        return;
    }
    if(s->is_float){
        if(r->output->launcher == s) r->output->launcher = NULL;
        rail_seat_set_focus(ws ? ws->focused : NULL);   /* back to the strip */
        rail_output_frame(r);
        return;
    }
    if(s->is_popup || s->is_sub){
        /* children float over a parent and are not in the strip list */
        rail_output_frame(r);
        return;
    }
    if(ws->first){
        if(s->next == s){ ws->first = NULL; ws->focused = NULL; }
        else{
            s->prev->next = s->next;
            s->next->prev = s->prev;
            if(ws->first == s) ws->first = s->next;
            if(ws->focused == s) ws->focused = s->next;
        }
        s->prev = s->next = NULL;
    }
    /* follow the strip: whoever is highlighted now owns the keyboard. when the
       surface is being destroyed the caller has already forgotten it, so no
       leave is sent to a dead resource - an unmap (attach(NULL)) does tell the
       client to leave. */
    rail_seat_set_focus(ws->focused);
    rail_output_frame(r);
}

void rail_strip_focus_next(struct rail *r)
{
    struct rail_ws *ws = r->output->active;
    if(!ws->first) return;
    ws->focused = ws->focused->next ? ws->focused->next : ws->first;
    rail_seat_set_focus(ws->focused);
    rail_configure_all(r);
    rail_output_frame(r);
}
void rail_strip_focus_prev(struct rail *r)
{
    struct rail_ws *ws = r->output->active;
    if(!ws->first) return;
    ws->focused = ws->focused->prev ? ws->focused->prev : ws->first;
    rail_seat_set_focus(ws->focused);
    rail_configure_all(r);
    rail_output_frame(r);
}

void rail_ws_switch(struct rail *r, int index)
{
    struct rail_ws *ws = r->output->workspaces;
    while(ws && ws->index != index) ws = ws->next;
    if(!ws){
        ws = calloc(1, sizeof *ws);
        ws->index = index;
        ws->next = r->output->workspaces;
        if(ws->next) ws->next->prev = ws;
        r->output->workspaces = ws;
    }
    r->output->active = ws;
    rail_seat_set_focus(ws->focused);
    rail_configure_all(r);
    rail_output_frame(r);
}

void rail_close_focused(struct rail *r)
{
    struct rail_ws *ws = r->output->active;
    struct rail_surface *dlg = top_dialog(r);
    if(dlg && dlg->toplevel){ xdg_toplevel_send_close(dlg->toplevel); return; }
    if(ws->focused && ws->focused->toplevel)
        xdg_toplevel_send_close(ws->focused->toplevel);
}

/* topmost mapped surface at output coords (popups > bar > focused tile) */
static bool hit(struct rail_surface *s, int px, int py)
{
    return s && s->mapped && px >= s->cx && px < s->cx + s->cw
                        && py >= s->cy && py < s->cy + s->ch;
}

/* a wl_subsurface is part of its parent: input focus, drags and resize edges
   belong to the root surface (the toplevel or popup it is attached to) */
static struct rail_surface *sub_root(struct rail_surface *s)
{
    while(s && s->is_sub && s->popup_parent) s = s->popup_parent;
    return s;
}

/* Topmost popup under (px,py) among the popups and subsurfaces hanging off
   `owner`. Candidates are listed in the order the scene is drawn (draw_popup_tree)
   and scanned from the top, so a menu opened later wins over whatever was
   created earlier. A subsurface of the toplevel itself (a browser's content
   area covers the whole window and was created first) is the toplevel, not a
   popup: it must never shadow a menu that is drawn over it. */
static struct rail_surface *pick_popup_tree(struct rail_surface *owner, int px, int py)
{
    struct rail_surface *order[128]; int cnt = 0;
    struct rail_surface *stack[64]; int n = 0;
    for(struct rail_surface *p = owner->popups; p && n < 64; p = p->popup_next) stack[n++] = p;
    while(n > 0 && cnt < 128){
        struct rail_surface *pu = stack[--n];
        if(pu->is_sub) rail_child_place(pu);   /* children track their parent */
        order[cnt++] = pu;
        for(struct rail_surface *q = pu->popups; q && n < 64; q = q->popup_next) stack[n++] = q;
    }
    for(int i = cnt - 1; i >= 0; i--){
        if(!hit(order[i], px, py)) continue;
        struct rail_surface *root = sub_root(order[i]);
        if(root && root->is_popup) return root;
    }
    return NULL;
}

struct rail_surface *rail_pick_surface(struct rail *r, int px, int py)
{
    struct rail_ws *ws = r->output->active;
    if(!ws) return NULL;
    /* 0) the floating launcher sits above everything */
    if(hit(r->output->launcher, px, py)) return r->output->launcher;
    /* 0b) dialogs sit above the strip: topmost dialog first, its popups before it */
    struct rail_surface *dhit = NULL;
    for(struct rail_surface *d = r->output->dialogs; d; d = d->dlg_next){
        struct rail_surface *h = pick_popup_tree(d, px, py);
        if(!h && hit(d, px, py)) h = d;
        if(h) dhit = h;                 /* later dialogs are on top */
    }
    if(dhit) return dhit;
    /* 1) popups of the strip tiles */
    for(struct rail_surface *s = ws->first; s; s = s->next){
        struct rail_surface *h = pick_popup_tree(s, px, py);
        if(h) return h;
        if(s->next == ws->first || !s->next) break;
    }
    /* 2) bar */
    if(hit(r->output->bar, px, py)) return r->output->bar;
    /* 3) focused tile */
    if(hit(ws->focused, px, py)) return ws->focused;
    return NULL;
}

/* focus-follows-click: route keyboard focus to surface under cursor */
void rail_click_focus(struct rail *r, int px, int py)
{
    struct rail_surface *t = rail_pick_surface(r, px, py);
    struct rail_surface *lau = r->output->launcher;
    if(lau && t != lau && lau->toplevel)
        xdg_toplevel_send_close(lau->toplevel);   /* click away = dismiss */
    if(!t || t->is_bar) return;
    /* an open dialog keeps the keyboard: a click on the window behind it does not take it away */
    { struct rail_surface *dlg = top_dialog(r);
      if(dlg && t != lau && surface_root(t) != dlg) return; }
    /* only strip tiles change the strip's focused window */
    struct rail_ws *ws = r->output->active;
    if(!t->is_popup && ws && t->mapped && t->prev){
        ws->focused = t;
        rail_configure_all(r);
    }
    rail_seat_set_focus(t);
}

/* ---------------- interactive move / resize ---------------- */

void rail_drag_begin(struct rail_surface *s, int mode, int px, int py)
{
    if(!s || s->is_popup || s->is_bar) return;
    /* strip tiles are laid out by the strip, never dragged or resized: clients
       (browsers) ask for an interactive move on any press in their titlebar
       area, and honouring it would fling the tile off-centre */
    if(s->prev) return;
    struct rail *r = s->rail;
    r->dragging = true;
    r->drag_mode = mode;
    r->drag_surf = s;
    r->drag_grab_x = px; r->drag_grab_y = py;
    r->drag_start_x = s->cx; r->drag_start_y = s->cy;
    r->drag_start_w = s->cw; r->drag_start_h = s->ch;
}

void rail_drag_motion(struct rail *r, int px, int py)
{
    if(!r->dragging || !r->drag_surf) return;
    struct rail_surface *s = r->drag_surf;
    int dx = px - r->drag_grab_x, dy = py - r->drag_grab_y;
    int edge = r->drag_mode;
    int minw = 80, minh = 60;

    if(edge == 1){ /* move */
        s->cx = r->drag_start_x + dx;
        s->cy = r->drag_start_y + dy;
    } else {
        int w = r->drag_start_w, h = r->drag_start_h;
        int x = r->drag_start_x, y = r->drag_start_y;
        if(edge & 2){ w = r->drag_start_w + dx; }              /* right */
        if(edge & 8){ h = r->drag_start_h + dy; }              /* bottom */
        if(edge & 16){ x = r->drag_start_x + dx; w = r->drag_start_w - dx; } /* left */
        if(edge & 32){ y = r->drag_start_y + dy; h = r->drag_start_h - dy; } /* top */
        if(w < minw){ if(edge & 16) x -= (minw - w); w = minw; }
        if(h < minh){ if(edge & 32) y -= (minh - h); h = minh; }
        s->cx = x; s->cy = y; s->cw = w; s->ch = h;
    }
    rail_output_frame(r);
}

void rail_drag_end(struct rail *r)
{
    if(!r->dragging) return;
    r->dragging = false;
    /* re-tile: snap back into strip layout, and send configure to client */
    struct rail_surface *s = r->drag_surf;
    r->drag_surf = NULL;
    r->drag_mode = 0;
    if(s && s->mapped && s->toplevel){
        /* for move: keep floating position; for resize: send new size */
        /* a strip tile always goes back to its slot size, never the dragged one */
        if(s->prev) toplevel_configure(s, s->tw, s->th);
        else if(s->is_dialog) toplevel_configure(s, s->cw - 2 * RAIL_BORDER, s->ch - 2 * RAIL_BORDER);
        else toplevel_configure(s, s->cw, s->ch);
    }
    rail_output_frame(r);
}

void rail_dismiss_popups(struct rail *r)
{
    struct rail_ws *ws = r->output->active;
    if(!ws) return;
    /* only real xdg_popups are dismissed. wl_subsurface children share the
       same parent->popups list (they are the browser's actual content), so
       they must stay linked or the window is left as an empty frame. */
    for(struct rail_surface *s = r->all_surfaces; s; s = s->g_next){
        struct rail_surface **pp = &s->popups;
        while(*pp){
            struct rail_surface *pu = *pp;
            if(pu->popup && !pu->is_sub){
                xdg_popup_send_popup_done(pu->popup);
                *pp = pu->popup_next;
                pu->popup_next = NULL;
                pu->popup_parent = NULL;
            } else {
                pp = &pu->popup_next;
            }
        }
    }
    rail_output_frame(r);
}

void rail_spawn_util(const char *name)
{
    char exe[512], cmd[640];
#ifdef __FreeBSD__
    /* no /proc by default on FreeBSD: ask the kernel for our own path */
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1 };
    size_t exe_len = sizeof exe - 1;
    ssize_t n = (sysctl(mib, 4, exe, &exe_len, NULL, 0) == 0 && exe_len > 1)
                ? (ssize_t)exe_len - 1 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
#endif
    if(n > 0){
        exe[n] = '\0';
        char *slash = strrchr(exe, '/');
        if(slash){
            *slash = '\0';
            snprintf(cmd, sizeof cmd, "%s/%s", exe, name);
            if(access(cmd, X_OK) == 0){
                snprintf(cmd, sizeof cmd, "'%s/%s'", exe, name);
                rail_spawn(cmd);
                return;
            }
        }
    }
    rail_spawn(name);
}

void rail_spawn(const char *cmd)
{
    pid_t pid = fork();
    if(pid == 0){
        setsid();
        /* railwl blocks some signals for its event loop; children must not inherit that */
        sigset_t none; sigemptyset(&none); sigprocmask(SIG_SETMASK, &none, NULL);
        execl("/bin/sh","/bin/sh","-c",cmd,(char*)NULL);
        _exit(1);
    }
}

/* ---------------- wl_output ---------------- */

static void out_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static const struct wl_output_interface out_impl = { .release = out_release };

static void out_res_destroy(struct wl_resource *r)
{
    struct rail_out_res *o = wl_resource_get_user_data(r);
    if(!o) return;
    struct rail *rail = o->rail;
    if(rail){
        struct rail_out_res **pp = &rail->outputs;
        while(*pp && *pp != o) pp = &(*pp)->next;
        if(*pp == o) *pp = o->next;
    }
    free(o);
}

static void bind_output(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    if(ver > 4) ver = 4;
    struct wl_resource *res = wl_resource_create(client, &wl_output_interface, ver, id);
    struct rail_out_res *o = calloc(1, sizeof *o);
    o->resource = res;
    o->rail = rail;
    o->next = rail->outputs;
    rail->outputs = o;
    wl_resource_set_implementation(res, &out_impl, o, out_res_destroy);
    wl_output_send_geometry(res, 0, 0, 0, 0, WL_OUTPUT_SUBPIXEL_UNKNOWN,
                            "railwl", "primary", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(res, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED,
                        rail->output->w, rail->output->h, rail->output->mode.vrefresh * 1000);
    if(wl_resource_get_version(res) >= WL_OUTPUT_SCALE_SINCE_VERSION)
        wl_output_send_scale(res, 1);
    if(wl_resource_get_version(res) >= WL_OUTPUT_NAME_SINCE_VERSION){
        wl_output_send_name(res, "RAIL-1");
        wl_output_send_description(res, "railwl primary output");
    }
    if(wl_resource_get_version(res) >= WL_OUTPUT_DONE_SINCE_VERSION)
        wl_output_send_done(res);
    /* surfaces this client mapped before binding the output are already on it */
    for(struct rail_surface *s = rail->all_surfaces; s; s = s->g_next)
        if(s->mapped && !s->dying && s->resource &&
           wl_resource_get_client(s->resource) == client)
            wl_surface_send_enter(s->resource, res);
    (void)ver;
}

void rail_output_global_init(struct rail *r)
{
    wl_global_create(r->display, &wl_output_interface, 4, r, bind_output);
}

/* ---------------- zwp_linux_dmabuf_v1 ----------------
   GPU clients (browsers, GL apps) render into dmabuf planes and hand them
   over here.  The planes are imported into an EGLImage that the GPU samples
   directly, so nothing is ever copied through the CPU.  Such a buffer stays
   the client's only once the frame sampling it has reached the screen.
   -------------------------------------------------------------------- */

struct rail_params {
    struct rail *rail;
    int nplanes;
    int fd[RAIL_MAX_PLANES];
    uint32_t off[RAIL_MAX_PLANES], stride[RAIL_MAX_PLANES];
    uint64_t mod;
};

static void params_res_destroy(struct wl_resource *r)
{
    struct rail_params *p = wl_resource_get_user_data(r);
    if(!p) return;
    for(int i = 0; i < RAIL_MAX_PLANES; i++)
        if(p->fd[i] >= 0) close(p->fd[i]);
    free(p);
}

static void params_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void params_add(struct wl_client *c, struct wl_resource *r, int32_t fd,
                       uint32_t plane_idx, uint32_t offset, uint32_t stride,
                       uint32_t mod_hi, uint32_t mod_lo)
{
    (void)c;
    struct rail_params *p = wl_resource_get_user_data(r);
    if(!p || plane_idx >= RAIL_MAX_PLANES){ close(fd); return; }
    if(p->fd[plane_idx] >= 0) close(p->fd[plane_idx]);
    p->fd[plane_idx] = fd;
    p->off[plane_idx] = offset;
    p->stride[plane_idx] = stride;
    p->mod = ((uint64_t)mod_hi << 32) | mod_lo;
    if((int)plane_idx + 1 > p->nplanes) p->nplanes = (int)plane_idx + 1;
}

/* move the added planes into a buffer and import them; the fds always leave
   the params object, and NULL means the import (or the request) failed */
static struct wl_resource *dmabuf_make_buffer(struct wl_client *client,
                                              struct rail_params *p, uint32_t id,
                                              int32_t w, int32_t h,
                                              uint32_t fourcc, uint32_t flags)
{
    if(!client || !p || p->nplanes <= 0 || w <= 0 || h <= 0) return NULL;
    if(flags & (RAIL_DMABUF_INTERLACED | RAIL_DMABUF_BOTTOM_FIRST)) return NULL;

    struct rail_buffer *b = calloc(1, sizeof *b);
    if(!b) return NULL;
    b->fd = -1;
    for(int i = 0; i < RAIL_MAX_PLANES; i++) b->planes[i] = -1;
    b->rail = p->rail;
    b->is_dmabuf = true;
    b->format = fourcc;
    b->w = (uint32_t)w; b->h = (uint32_t)h;
    b->stride = p->stride[0];
    b->flags = flags;
    b->mod = p->mod;
    b->nplanes = p->nplanes;
    for(int i = 0; i < p->nplanes; i++){
        b->planes[i] = p->fd[i]; p->fd[i] = -1;
        b->off[i] = p->off[i];
        b->pstride[i] = p->stride[i];
    }
    if(rail_dmabuf_import(b) < 0){
        rail_dmabuf_destroy(b);     /* closes the planes we took */
        free(b);
        return NULL;
    }
    struct wl_resource *res = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if(!res){ rail_dmabuf_destroy(b); free(b); return NULL; }
    b->resource = res;
    wl_resource_set_implementation(res, &buf_impl, b, shm_buffer_destroy);
    return res;
}

static void params_create(struct wl_client *client, struct wl_resource *r,
                          int32_t w, int32_t h, uint32_t fourcc, uint32_t flags)
{
    struct rail_params *p = wl_resource_get_user_data(r);
    struct wl_resource *buf = dmabuf_make_buffer(client, p, 0, w, h, fourcc, flags);
    if(buf) zwp_linux_buffer_params_v1_send_created(r, buf);
    else    zwp_linux_buffer_params_v1_send_failed(r);
}

static void params_create_immed(struct wl_client *client, struct wl_resource *r,
                                uint32_t id, int32_t w, int32_t h,
                                uint32_t fourcc, uint32_t flags)
{
    struct rail_params *p = wl_resource_get_user_data(r);
    struct wl_resource *buf = dmabuf_make_buffer(client, p, id, w, h, fourcc, flags);
    if(buf) return;
    /* the id is the client's own: it gets an inert buffer plus the failure */
    struct wl_resource *res = wl_resource_create(client, &wl_buffer_interface, 1, id);
    if(res) wl_resource_set_implementation(res, &buf_impl, NULL, NULL);
    zwp_linux_buffer_params_v1_send_failed(r);
}

static void params_set_sampling_device(struct wl_client *c, struct wl_resource *r,
                                       struct wl_array *dev_id)
{ (void)c; (void)r; (void)dev_id; }  /* v6: never advertised */

static const struct zwp_linux_buffer_params_v1_interface params_impl = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
    .set_sampling_device = params_set_sampling_device,
};

static void dmabuf_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void dmabuf_create_params(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct rail *rail = wl_resource_get_user_data(r);
    struct wl_resource *res = wl_resource_create(client, &zwp_linux_buffer_params_v1_interface,
                                                 wl_resource_get_version(r), id);
    if(!res){ wl_client_post_no_memory(client); return; }
    struct rail_params *p = calloc(1, sizeof *p);
    if(!p){ wl_resource_destroy(res); wl_client_post_no_memory(client); return; }
    p->rail = rail;
    for(int i = 0; i < RAIL_MAX_PLANES; i++) p->fd[i] = -1;
    wl_resource_set_implementation(res, &params_impl, p, params_res_destroy);
}

/* ---- dmabuf feedback (v4) ----
   Mesa's EGL Wayland platform learns which GPU to open from main_device and
   which format+modifier pairs are importable from the format table.  With
   neither it has no DRM device ("failed to get driver name for fd -1"), the
   EGL display fails to initialise, and GL clients (SDL, browsers) die. */

struct ft_entry { uint32_t format; uint32_t pad; uint64_t modifier; };

static int ft_fd = -1;          /* anonymous file holding the table; kept open */
static size_t ft_size;
static uint32_t ft_n;           /* entries in the table */

static int feedback_table_build(struct rail *r)
{
    if(ft_fd >= 0) return 0;
    int nf = 0;
    const uint32_t *fmts = rail_dmabuf_formats(&nf);
    if(nf <= 0) return -1;

    struct ft_entry *e = NULL;
    size_t n = 0, cap = 0;
    for(int i = 0; i < nf; i++){
        int nm = 0;
        uint64_t *mods = rail_dmabuf_modifiers(r, fmts[i], &nm);
        for(int k = -1; k < nm; k++){
            uint64_t m = k < 0 ? DRM_FORMAT_MOD_LINEAR : mods[k];
            if(k >= 0 && m == DRM_FORMAT_MOD_LINEAR) continue;   /* already first */
            if(n == cap){
                size_t nc = cap ? cap * 2 : 16;
                struct ft_entry *ne = realloc(e, nc * sizeof *e);
                if(!ne){ free(e); free(mods); return -1; }
                e = ne; cap = nc;
            }
            e[n].format = fmts[i]; e[n].pad = 0; e[n].modifier = m;
            n++;
        }
        free(mods);
    }
    if(n == 0 || n > 65535){ free(e); return -1; }

    int fd = memfd_create("rail-dmabuf-table", 0);
    if(fd < 0){ free(e); return -1; }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    size_t len = n * sizeof *e, done = 0;
    while(done < len){
        ssize_t w = write(fd, (const char *)e + done, len - done);
        if(w <= 0){ close(fd); free(e); return -1; }
        done += (size_t)w;
    }
    free(e);
    ft_fd = fd; ft_size = len; ft_n = (uint32_t)n;
    return 0;
}

static void feedback_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct zwp_linux_dmabuf_feedback_v1_interface feedback_impl = {
    .destroy = feedback_destroy,
};

/* one tranche: every table entry, targeting the GPU railwl composes on */
static void feedback_send(struct wl_client *client, struct wl_resource *dmabuf,
                          uint32_t id)
{
    struct rail *r = wl_resource_get_user_data(dmabuf);
    struct wl_resource *fb = wl_resource_create(client,
        &zwp_linux_dmabuf_feedback_v1_interface, wl_resource_get_version(dmabuf), id);
    if(!fb){ wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(fb, &feedback_impl, r, NULL);

    struct stat st;
    if(!r || !r->output || r->output->fd < 0 ||
       fstat(r->output->fd, &st) < 0 || feedback_table_build(r) < 0){
        wl_client_post_no_memory(client);
        return;
    }
    dev_t dev = st.st_rdev;

    struct wl_array devarr, idx;
    wl_array_init(&devarr);
    wl_array_init(&idx);
    void *dp = wl_array_add(&devarr, sizeof dev);
    uint16_t *ip = wl_array_add(&idx, ft_n * sizeof(uint16_t));
    if(!dp || !ip){
        wl_array_release(&devarr); wl_array_release(&idx);
        wl_client_post_no_memory(client);
        return;
    }
    memcpy(dp, &dev, sizeof dev);
    for(uint32_t i = 0; i < ft_n; i++) ip[i] = (uint16_t)i;

    zwp_linux_dmabuf_feedback_v1_send_main_device(fb, &devarr);
    zwp_linux_dmabuf_feedback_v1_send_format_table(fb, ft_fd, (uint32_t)ft_size);
    zwp_linux_dmabuf_feedback_v1_send_tranche_target_device(fb, &devarr);
    zwp_linux_dmabuf_feedback_v1_send_tranche_formats(fb, &idx);
    zwp_linux_dmabuf_feedback_v1_send_tranche_flags(fb, 0);
    zwp_linux_dmabuf_feedback_v1_send_tranche_done(fb);
    zwp_linux_dmabuf_feedback_v1_send_done(fb);

    wl_array_release(&devarr);
    wl_array_release(&idx);
}

static void dmabuf_feedback_default(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    feedback_send(c, r, id);
}

static void dmabuf_feedback_surface(struct wl_client *c, struct wl_resource *r,
                                    uint32_t id, struct wl_resource *surface)
{
    (void)surface;      /* railwl composes everything on one GPU: same answer */
    feedback_send(c, r, id);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_impl = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
    .get_default_feedback = dmabuf_feedback_default,
    .get_surface_feedback = dmabuf_feedback_surface,
};

static void bind_dmabuf(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *r = data;
    if(ver > 4) ver = 4;    /* v4 = feedback (device + format table); v3 = modifiers */
    struct wl_resource *res = wl_resource_create(client, &zwp_linux_dmabuf_v1_interface,
                                                 (int)ver, id);
    if(!res){ wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &dmabuf_impl, r, NULL);

    int nf = 0;
    const uint32_t *fmts = rail_dmabuf_formats(&nf);
    for(int i = 0; i < nf; i++){
        if(ver >= 4) break;     /* v4 clients use feedback, not these events */
        zwp_linux_dmabuf_v1_send_format(res, fmts[i]);
        if(ver < 3) continue;
        /* linear is always importable; add whatever else the GPU accepts */
        zwp_linux_dmabuf_v1_send_modifier(res, fmts[i],
            (uint32_t)(DRM_FORMAT_MOD_LINEAR >> 32),
            (uint32_t)(DRM_FORMAT_MOD_LINEAR & 0xffffffffu));
        int nm = 0;
        uint64_t *mods = rail_dmabuf_modifiers(r, fmts[i], &nm);
        for(int k = 0; k < nm; k++){
            if(mods[k] == DRM_FORMAT_MOD_LINEAR) continue;
            zwp_linux_dmabuf_v1_send_modifier(res, fmts[i],
                (uint32_t)(mods[k] >> 32), (uint32_t)(mods[k] & 0xffffffffu));
        }
        free(mods);
    }
}

void rail_dmabuf_init(struct rail *r)
{
    if(!rail_dmabuf_supported()){
        fprintf(stderr, "railwl: dmabuf not importable: GPU clients fall back to shm\n");
        return;
    }
    wl_global_create(r->display, &zwp_linux_dmabuf_v1_interface, 4, r, bind_dmabuf);
}

/* ---------------- init ---------------- */

void rail_compositor_init(struct rail *r)
{
    wl_global_create(r->display, &wl_compositor_interface, 4, r, bind_compositor);
    wl_global_create(r->display, &wl_subcompositor_interface, 1, r, bind_sub);
    wl_global_create(r->display, &zxdg_decoration_manager_v1_interface, 1, r, bind_decoration_manager);
    wl_global_create(r->display, &wl_data_device_manager_interface, 3, r, bind_data_manager);
    wl_global_create(r->display, &zwp_primary_selection_device_manager_v1_interface, 1, r, bind_primary_manager);
    wl_global_create(r->display, &wl_shm_interface, 1, r, bind_shm);
    rail_dmabuf_init(r);
    rail_capture_init(r);
}

void rail_xdg_init(struct rail *r)
{
    wl_global_create(r->display, &xdg_wm_base_interface, 6, r, bind_wm_base);
}

/* ---------------- startup self-check ----------------
   libwayland-server *aborts the whole compositor* when a client sends a
   request whose handler is NULL (it cannot post an error for a request it
   has no entry for), and the initializer that forgot it looks perfectly
   fine. Walking every interface we expose turns that class of bug into a
   loud message at startup instead of a crash weeks later. */
void rail_check_impl(const struct wl_interface *iface, const void *impl)
{
    const void *const *m = impl;
    for(int i = 0; i < iface->method_count; i++)
        if(!m[i])
            fprintf(stderr, "railwl: BUG: %s.%s has no handler (opcode %d)\n",
                    iface->name, iface->methods[i].name, i);
}

void rail_impl_check(void)
{
    rail_check_impl(&wl_surface_interface, &surface_impl);
    rail_check_impl(&wl_compositor_interface, &compositor_impl);
    rail_check_impl(&wl_region_interface, &region_impl);
    rail_check_impl(&wl_shm_interface, &shm_impl);
    rail_check_impl(&wl_shm_pool_interface, &pool_impl);
    rail_check_impl(&wl_subcompositor_interface, &sub_impl);
    rail_check_impl(&wl_subsurface_interface, &subsurface_impl);
    rail_check_impl(&xdg_wm_base_interface, &wm_base_full);
    rail_check_impl(&xdg_positioner_interface, &positioner_impl);
    rail_check_impl(&xdg_surface_interface, &xdg_surface_impl);
    rail_check_impl(&xdg_toplevel_interface, &toplevel_impl);
    rail_check_impl(&xdg_popup_interface, &popup_impl);
    rail_check_impl(&zxdg_decoration_manager_v1_interface, &dec_manager_impl);
    rail_check_impl(&zxdg_toplevel_decoration_v1_interface, &dec_toplevel_impl);
    rail_check_impl(&wl_data_device_manager_interface, &dm_impl);
    rail_check_impl(&wl_data_device_interface, &dev_impl);
    rail_check_impl(&wl_data_source_interface, &src_impl);
    rail_check_impl(&wl_data_offer_interface, &offer_impl);
    rail_check_impl(&zwp_primary_selection_device_manager_v1_interface, &pdm_impl);
    rail_check_impl(&zwp_primary_selection_device_v1_interface, &pdev_impl);
    rail_check_impl(&zwp_primary_selection_source_v1_interface, &psrc_impl);
    rail_check_impl(&zwp_primary_selection_offer_v1_interface, &poffer_impl);
    rail_check_impl(&wl_output_interface, &out_impl);
    rail_check_impl(&zwp_linux_dmabuf_v1_interface, &dmabuf_impl);
    rail_check_impl(&zwp_linux_dmabuf_feedback_v1_interface, &feedback_impl);
    rail_check_impl(&zwp_linux_buffer_params_v1_interface, &params_impl);
    rail_capture_impl_check();
}
