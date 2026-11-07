/* capture.c -- screenshots: ext-image-copy-capture-v1 over an output source.
 *
 * Globals exposed:
 *   ext_output_image_capture_source_manager_v1   (pick the output as a source)
 *   ext_image_copy_capture_manager_v1            (sessions + frames)
 *
 * How a capture runs:
 *   client: create_source(output) -> create_session(source) -> create_frame
 *           attach_buffer(wl_shm buffer) -> damage_buffer -> capture
 *   railwl: capture queues the frame and asks for a redraw. render_gl() calls
 *           rail_capture_service() while the freshly composed scene is still in
 *           the framebuffer; the pixels are read back once and converted into
 *           every queued client buffer, then `ready` is sent.
 *
 * Sessions that did not ask for paint_cursors are served *before* the pointer
 * is drawn, the others after, so the cursor is only in shots that want it.
 *
 * Only wl_shm buffers are offered (ARGB8888 / XRGB8888, output size). No dmabuf,
 * no toplevel sources, and the pointer-cursor session is a stub that stops. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server.h>
#include "rail.h"
#include "ext-image-capture-source-v1-protocol.h"
#include "ext-image-copy-capture-v1-protocol.h"

struct cap_session;

struct cap_frame {
    struct wl_resource *resource;
    struct cap_session *session;       /* NULL once the session is gone */
    struct wl_resource *buffer;        /* attached wl_buffer, or NULL */
    struct wl_listener buffer_destroy;
    bool captured;                     /* capture was requested */
    bool queued;                       /* on the pending list */
    bool done;                         /* ready or failed was sent */
    struct wl_list link;
};

struct cap_session {
    struct wl_resource *resource;
    struct rail *rail;
    bool paint_cursors;
    struct cap_frame *frame;           /* most recent frame, may be NULL */
};

/* frames waiting for the next composed scene */
/* statically initialised: rail_output_init() renders the first frame, and so
   calls rail_capture_service(), before rail_capture_init() ever runs */
static struct wl_list pending = { &pending, &pending };

#define FAIL_UNKNOWN     EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN
#define FAIL_CONSTRAINTS EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS
#define FAIL_STOPPED     EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED

/* ---------------- frame ---------------- */

static void frame_fail(struct cap_frame *f, uint32_t reason)
{
    if(f->done) return;
    f->done = true;
    ext_image_copy_capture_frame_v1_send_failed(f->resource, reason);
}

static void frame_ready(struct cap_frame *f, int w, int h)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t sec = (uint64_t)ts.tv_sec;
    f->done = true;
    ext_image_copy_capture_frame_v1_send_transform(f->resource, WL_OUTPUT_TRANSFORM_NORMAL);
    ext_image_copy_capture_frame_v1_send_damage(f->resource, 0, 0, w, h);
    ext_image_copy_capture_frame_v1_send_presentation_time(f->resource,
        (uint32_t)(sec >> 32), (uint32_t)(sec & 0xffffffffu), (uint32_t)ts.tv_nsec);
    ext_image_copy_capture_frame_v1_send_ready(f->resource);
}

static void frame_drop_buffer(struct cap_frame *f)
{
    if(!f->buffer) return;
    wl_list_remove(&f->buffer_destroy.link);
    f->buffer = NULL;
}

static void frame_buffer_destroyed(struct wl_listener *l, void *data)
{
    (void)data;
    struct cap_frame *f = wl_container_of(l, f, buffer_destroy);
    wl_list_remove(&f->buffer_destroy.link);
    f->buffer = NULL;
}

static void frame_resource_destroy(struct wl_resource *res)
{
    struct cap_frame *f = wl_resource_get_user_data(res);
    frame_drop_buffer(f);
    if(f->queued) wl_list_remove(&f->link);
    if(f->session && f->session->frame == f) f->session->frame = NULL;
    free(f);
}

static void frame_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static void frame_attach_buffer(struct wl_client *c, struct wl_resource *r,
                                struct wl_resource *buffer)
{
    (void)c;
    struct cap_frame *f = wl_resource_get_user_data(r);
    if(f->captured){
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "attach_buffer after capture");
        return;
    }
    frame_drop_buffer(f);
    f->buffer = buffer;
    f->buffer_destroy.notify = frame_buffer_destroyed;
    wl_resource_add_destroy_listener(buffer, &f->buffer_destroy);
}

static void frame_damage_buffer(struct wl_client *c, struct wl_resource *r,
                                int32_t x, int32_t y, int32_t w, int32_t h)
{
    (void)c; (void)x; (void)y;
    struct cap_frame *f = wl_resource_get_user_data(r);
    if(f->captured){
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "damage_buffer after capture");
        return;
    }
    if(w < 0 || h < 0)
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_INVALID_BUFFER_DAMAGE,
                               "negative damage size");
    /* every capture is a full frame, so the damage hint is not needed */
}

static void frame_capture(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    struct cap_frame *f = wl_resource_get_user_data(r);
    if(f->captured){
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_ALREADY_CAPTURED,
                               "capture requested twice");
        return;
    }
    if(!f->buffer){
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_FRAME_V1_ERROR_NO_BUFFER,
                               "capture without a buffer");
        return;
    }
    f->captured = true;
    if(!f->session){ frame_fail(f, FAIL_STOPPED); return; }

    struct rail *rail = f->session->rail;
    struct rail_output *o = rail->output;
    if(!o || !o->egl_ok || o->suspended){ frame_fail(f, FAIL_UNKNOWN); return; }

    wl_list_insert(pending.prev, &f->link);
    f->queued = true;
    rail_output_frame(rail);      /* make sure a scene gets composed */
}

static const struct ext_image_copy_capture_frame_v1_interface frame_impl = {
    .destroy       = frame_destroy,
    .attach_buffer = frame_attach_buffer,
    .damage_buffer = frame_damage_buffer,
    .capture       = frame_capture,
};

/* ---------------- session ---------------- */

static void session_resource_destroy(struct wl_resource *res)
{
    struct cap_session *s = wl_resource_get_user_data(res);
    struct cap_frame *f = s->frame;
    if(f){
        f->session = NULL;
        if(f->queued){            /* nothing will compose for it any more */
            wl_list_remove(&f->link);
            f->queued = false;
            frame_fail(f, FAIL_STOPPED);
        }
    }
    free(s);
}

static void session_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static void session_create_frame(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct cap_session *s = wl_resource_get_user_data(r);
    if(s->frame && !s->frame->done){
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_SESSION_V1_ERROR_DUPLICATE_FRAME,
                               "a frame is still in flight");
        return;
    }
    struct cap_frame *f = calloc(1, sizeof *f);
    if(!f){ wl_client_post_no_memory(client); return; }
    struct wl_resource *res = wl_resource_create(client,
        &ext_image_copy_capture_frame_v1_interface, wl_resource_get_version(r), id);
    if(!res){ free(f); wl_client_post_no_memory(client); return; }
    f->resource = res;
    f->session = s;
    s->frame = f;
    wl_resource_set_implementation(res, &frame_impl, f, frame_resource_destroy);
}

static const struct ext_image_copy_capture_session_v1_interface session_impl = {
    .create_frame = session_create_frame,
    .destroy      = session_destroy,
};

static struct cap_session *session_new(struct wl_client *client, uint32_t version,
                                       uint32_t id, struct rail *rail, bool cursors)
{
    struct cap_session *s = calloc(1, sizeof *s);
    if(!s){ wl_client_post_no_memory(client); return NULL; }
    struct wl_resource *res = wl_resource_create(client,
        &ext_image_copy_capture_session_v1_interface, version, id);
    if(!res){ free(s); wl_client_post_no_memory(client); return NULL; }
    s->resource = res;
    s->rail = rail;
    s->paint_cursors = cursors;
    wl_resource_set_implementation(res, &session_impl, s, session_resource_destroy);
    return s;
}

/* ---------------- pointer cursor session (stub) ---------------- */

static void cursor_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

/* we never report the pointer entering the capture source, so the capture
   session this hands out simply stops */
static void cursor_get_capture_session(struct wl_client *client, struct wl_resource *r, uint32_t id)
{
    struct rail *rail = wl_resource_get_user_data(r);
    struct cap_session *s = session_new(client, wl_resource_get_version(r), id, rail, false);
    if(s) ext_image_copy_capture_session_v1_send_stopped(s->resource);
}

static const struct ext_image_copy_capture_cursor_session_v1_interface cursor_impl = {
    .destroy             = cursor_destroy,
    .get_capture_session = cursor_get_capture_session,
};

/* ---------------- managers ---------------- */

static void mgr_create_session(struct wl_client *client, struct wl_resource *r, uint32_t id,
                               struct wl_resource *source, uint32_t options)
{
    (void)source;
    struct rail *rail = wl_resource_get_user_data(r);
    if(options & ~(uint32_t)EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS){
        wl_resource_post_error(r, EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_ERROR_INVALID_OPTION,
                               "unknown option bits 0x%x", options);
        return;
    }
    struct cap_session *s = session_new(client, wl_resource_get_version(r), id, rail,
        (options & EXT_IMAGE_COPY_CAPTURE_MANAGER_V1_OPTIONS_PAINT_CURSORS) != 0);
    if(!s) return;

    struct rail_output *o = rail->output;
    ext_image_copy_capture_session_v1_send_buffer_size(s->resource,
        (uint32_t)(o ? o->w : 0), (uint32_t)(o ? o->h : 0));
    ext_image_copy_capture_session_v1_send_shm_format(s->resource, WL_SHM_FORMAT_XRGB8888);
    ext_image_copy_capture_session_v1_send_shm_format(s->resource, WL_SHM_FORMAT_ARGB8888);
    ext_image_copy_capture_session_v1_send_done(s->resource);
}

static void mgr_create_pointer_cursor_session(struct wl_client *client, struct wl_resource *r,
                                              uint32_t id, struct wl_resource *source,
                                              struct wl_resource *pointer)
{
    (void)source; (void)pointer;
    struct wl_resource *res = wl_resource_create(client,
        &ext_image_copy_capture_cursor_session_v1_interface, wl_resource_get_version(r), id);
    if(!res){ wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &cursor_impl, wl_resource_get_user_data(r), NULL);
}

static void mgr_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static const struct ext_image_copy_capture_manager_v1_interface mgr_impl = {
    .create_session                = mgr_create_session,
    .create_pointer_cursor_session = mgr_create_pointer_cursor_session,
    .destroy                       = mgr_destroy,
};

static void bind_mgr(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    if(ver > 1) ver = 1;
    struct wl_resource *res = wl_resource_create(client,
        &ext_image_copy_capture_manager_v1_interface, ver, id);
    if(!res){ wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &mgr_impl, data, NULL);
}

/* the source is just a marker: there is one output, it is always that one */
static void source_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static const struct ext_image_capture_source_v1_interface source_impl = {
    .destroy = source_destroy,
};

static void srcmgr_create_source(struct wl_client *client, struct wl_resource *r,
                                 uint32_t id, struct wl_resource *output)
{
    (void)output;
    struct wl_resource *res = wl_resource_create(client,
        &ext_image_capture_source_v1_interface, wl_resource_get_version(r), id);
    if(!res){ wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &source_impl, NULL, NULL);
}

static void srcmgr_destroy(struct wl_client *c, struct wl_resource *r)
{ (void)c; wl_resource_destroy(r); }

static const struct ext_output_image_capture_source_manager_v1_interface srcmgr_impl = {
    .create_source = srcmgr_create_source,
    .destroy       = srcmgr_destroy,
};

static void bind_srcmgr(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    if(ver > 1) ver = 1;
    struct wl_resource *res = wl_resource_create(client,
        &ext_output_image_capture_source_manager_v1_interface, ver, id);
    if(!res){ wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &srcmgr_impl, data, NULL);
}

/* ---------------- servicing, called from render_gl() ---------------- */

/* is this client buffer something we can write the scene into? */
static bool buffer_ok(struct wl_resource *res, const struct rail_output *o)
{
    if(strcmp(wl_resource_get_class(res), "wl_buffer") != 0) return false;
    const struct rail_buffer *b = wl_resource_get_user_data(res);
    if(!b || b->is_dmabuf || !rail_buf_data(b)) return false;
    if(b->format != WL_SHM_FORMAT_ARGB8888 && b->format != WL_SHM_FORMAT_XRGB8888) return false;
    if(b->w != (uint32_t)o->w || b->h != (uint32_t)o->h) return false;
    if(b->stride < b->w * 4u) return false;
    /* the whole buffer must lie inside its pool mapping */
    size_t off = b->pool_off;
    size_t need = (size_t)b->stride * (b->h - 1) + (size_t)b->w * 4;
    return off <= b->pool->size && need <= b->pool->size - off;
}

void rail_capture_service(struct rail *r, bool after_cursor)
{
    if(wl_list_empty(&pending)) return;
    struct rail_output *o = r->output;
    uint8_t *rgba = NULL;
    bool read_failed = false;

    struct cap_frame *f, *tmp;
    wl_list_for_each_safe(f, tmp, &pending, link){
        if(f->session && f->session->paint_cursors != after_cursor) continue;

        wl_list_remove(&f->link);
        f->queued = false;

        if(!f->buffer){ frame_fail(f, FAIL_UNKNOWN); continue; }
        if(!buffer_ok(f->buffer, o)){ frame_fail(f, FAIL_CONSTRAINTS); continue; }

        if(!rgba && !read_failed){
            rgba = malloc((size_t)o->w * (size_t)o->h * 4);
            if(!rgba || rail_output_read_rgba(r, rgba) < 0){
                free(rgba); rgba = NULL; read_failed = true;
            }
        }
        if(!rgba){ frame_fail(f, FAIL_UNKNOWN); continue; }

        const struct rail_buffer *b = wl_resource_get_user_data(f->buffer);
        for(int y = 0; y < o->h; y++){
            const uint8_t *s = rgba + (size_t)y * (size_t)o->w * 4;
            uint8_t *d = rail_buf_data(b) + (size_t)y * b->stride;
            for(int x = 0; x < o->w; x++, s += 4, d += 4){
                d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = 0xff; /* B G R A */
            }
        }
        frame_ready(f, o->w, o->h);
    }
    free(rgba);
}

/* ---------------- init ---------------- */

void rail_capture_init(struct rail *r)
{
    wl_list_init(&pending);
    wl_global_create(r->display, &ext_output_image_capture_source_manager_v1_interface,
                     1, r, bind_srcmgr);
    wl_global_create(r->display, &ext_image_copy_capture_manager_v1_interface,
                     1, r, bind_mgr);
}

void rail_capture_impl_check(void)
{
    rail_check_impl(&ext_output_image_capture_source_manager_v1_interface, &srcmgr_impl);
    rail_check_impl(&ext_image_capture_source_v1_interface, &source_impl);
    rail_check_impl(&ext_image_copy_capture_manager_v1_interface, &mgr_impl);
    rail_check_impl(&ext_image_copy_capture_session_v1_interface, &session_impl);
    rail_check_impl(&ext_image_copy_capture_frame_v1_interface, &frame_impl);
    rail_check_impl(&ext_image_copy_capture_cursor_session_v1_interface, &cursor_impl);
}
