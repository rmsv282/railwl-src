#ifndef RAIL_H
#define RAIL_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-server.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define RAIL_AMBER 0xFFBF00FFu /* ARGB */
#define RAIL_BORDER 2
#define RAIL_BAR_HEIGHT 28
/* empty space between a tile's amber frame and the screen edges / the bar */
#define RAIL_GAP 16
/* client content size of a strip tile: the output (below the bar) minus the
   gap and the amber frame on every side. This is the size clients are
   configured to, so their buffer fills the frame exactly and never overlaps it. */
#define RAIL_TILE_W(ow) ((ow) - 2 * (RAIL_GAP + RAIL_BORDER))
#define RAIL_TILE_H(oh) ((oh) - RAIL_BAR_HEIGHT - 2 * (RAIL_GAP + RAIL_BORDER))
/* floating launcher: the size its client draws at (utils/raillauncher.c W x H) */
#define RAIL_FLOAT_W 520
#define RAIL_FLOAT_H 320
#define RAIL_ANIM_MS 120

struct rail;
struct rail_view;

/* --- shm buffer --- */
struct rail_pool { void *data; size_t size; int refs; bool dead; struct rail *rail; int fd; /* kept open only where there is no mremap */ };

/* a dmabuf (zwp_linux_dmabuf_v1) buffer keeps its planes and the EGLImage the
   planes were imported into; it never has CPU-mapped pixels */
#define RAIL_MAX_PLANES 4

/* zwp_linux_buffer_params_v1 flags */
#define RAIL_DMABUF_Y_INVERT   1u
#define RAIL_DMABUF_INTERLACED 2u
#define RAIL_DMABUF_BOTTOM_FIRST 4u

struct rail_buffer {
    struct wl_resource *resource;
    struct rail *rail;
    struct rail_pool *pool;
    uint32_t format, w, h, stride;
    size_t pool_off;              /* offset of the pixels inside the shm pool */
    size_t size;
    int fd;
    bool busy;
    bool released;   /* wl_buffer.release already sent for last commit */
    /* dmabuf planes + imported image */
    bool is_dmabuf;
    int nplanes;
    int planes[RAIL_MAX_PLANES];
    uint32_t off[RAIL_MAX_PLANES], pstride[RAIL_MAX_PLANES];
    uint64_t mod;
    uint32_t flags;
    void *img;                    /* EGLImageKHR */
    /* dmabuf buffers stay locked until the frame sampling them is presented */
    bool rel_pending;
    struct rail_buffer *rel_next;
};

/* wl_shm pixels. Derived from the pool every time instead of cached: a pool
   resize may move the mapping, and a cached pointer would then dangle. NULL for
   dmabuf buffers and for pools whose mapping failed. */
static inline uint8_t *rail_buf_data(const struct rail_buffer *b)
{
    return (b->pool && b->pool->data) ? (uint8_t *)b->pool->data + b->pool_off : NULL;
}

/* --- wl_surface --- */
struct rail_surface {
    struct wl_resource *resource;
    struct rail *rail;
    bool dying;                     /* wl_surface is being destroyed: no events */
    struct rail_buffer *front;      /* last committed buffer */
    struct rail_buffer *pending;
    int w, h;
    bool has_frame_cb;
    struct wl_resource *frame_cb;
    bool attach_null;               /* last attach passed a NULL buffer (unmap) */
    /* xdg */
    struct wl_resource *xdg_surface;
    struct wl_resource *toplevel;
    bool configured, mapped;
    char title[256], app_id[256];
    /* placement (target) */
    int x, y, tw, th;
    /* animation state (current) */
    int cx, cy, cw, ch;
    /* GPU texture cache: tex holds last uploaded buffer; dirty set on each commit */
    unsigned int tex;            /* GL texture name, 0 = not created */
    bool tex_opaque;             /* texture came from an XRGB buffer (force alpha 1) */
    bool tex_flip;               /* buffer is bottom-up (dmabuf y_invert) */
    bool tex_shm;                /* texture storage came from glTexImage2D (not an EGLImage) */
    int  tex_w, tex_h;           /* size of that storage, to update it in place */
    bool buffer_dirty;           /* new commit waiting for upload */
    /* damage reported since the last upload (buffer coords, x1/y1 exclusive);
       dmg_any false = the client reported none, so upload everything */
    bool dmg_any;
    int  dmg_x0, dmg_y0, dmg_x1, dmg_y1;
    /* strip links (circular) */
    struct rail_surface *prev, *next;
    bool is_bar;
    bool is_float;                  /* centred, un-tiled window (the launcher) */
    bool is_dialog;                 /* transient / fixed-size toplevel: floats centred above the strip at its own size */
    struct rail_surface *dlg_next;  /* link in rail_output.dialogs (oldest first, last = topmost) */
    int min_w, min_h, max_w, max_h; /* xdg_toplevel size hints (0 = unset) */
    struct rail_surface *g_next;
    /* xdg_popup */
    bool is_popup;
    bool fullscreen;                 /* client requested fullscreen */
    bool maximized;                  /* client requested maximize */
    struct wl_resource *popup;          /* xdg_popup resource */
    struct rail_surface *popup_parent;  /* toplevel or popup this floats over */
    struct rail_surface *popups;        /* linked list: popups parented to this surface */
    struct rail_surface *popup_next;    /* sibling link in parent's popups list */
    int anchor_x, anchor_y, anchor_w, anchor_h; /* anchor rect (surface-local) */
    int pop_x, pop_y;                   /* resolved popup position (surface-local) */
    uint32_t popup_grab_serial;
    /* wl_subsurface: a client-positioned child composited over its parent */
    bool is_sub;
    struct wl_resource *subsurface;
    int sub_x, sub_y;                   /* offset, parent-surface coordinates */
    int geo_x, geo_y;                   /* xdg window geometry origin inside the surface (CSD shadow margin) */
    int geo_w, geo_h;                   /* xdg window geometry size (0 = never set: the whole buffer is the window) */
};

/* a strip tile draws its content inset by the amber frame, so anything a
   client positions inside that tile must be offset by the same amount */
static inline int rail_child_inset(const struct rail_surface *par)
{
    if(!par || par->is_popup || par->is_sub || par->is_bar) return 0;
    return par->fullscreen ? 0 : RAIL_BORDER;
}

/* where a surface's own (0,0) lands on screen: tiles are drawn inset by the
   frame, and shifted back by the client's window-geometry origin so the
   geometry (not the invisible shadow margin) is what sits in the tile */
static inline int rail_inset_x(const struct rail_surface *par)
{
    int b = rail_child_inset(par);
    return (par && b && !par->is_sub && !par->is_popup) ? b - par->geo_x : b;
}
static inline int rail_inset_y(const struct rail_surface *par)
{
    int b = rail_child_inset(par);
    return (par && b && !par->is_sub && !par->is_popup) ? b - par->geo_y : b;
}

/* place a child (popup or subsurface) at its offset within its parent */
static inline void rail_child_place(struct rail_surface *s)
{
    if(!s->popup_parent) return;
    s->cx = s->popup_parent->cx + rail_inset_x(s->popup_parent) + s->sub_x;
    s->cy = s->popup_parent->cy + rail_inset_y(s->popup_parent) + s->sub_y;
}

void rail_pointer_reenter(struct rail_surface *s);

struct rail_view { struct rail_surface *s; };

/* xdg_positioner state (client-requested popup placement) */
struct rail_positioner {
    int width, height;
    int anchor_x, anchor_y, anchor_w, anchor_h;
    int32_t anchor, gravity, constraint;
    int offset_x, offset_y;
    bool reactive, unconstrained;
};

/* --- core clipboard (wl_data_device_manager) --- */
struct rail_data_source {
    struct wl_resource *resource;
    struct rail *rail;
    char mimes[8][64];
    int n_mime;
    bool dead;
};

struct rail_data_device {
    struct wl_resource *resource;
    struct rail *rail;
    /* the selection offer this device currently holds. Re-broadcasting the
       same selection reuses it instead of handing the client a brand new
       offer every time focus moves. */
    struct wl_resource *offer;
    struct rail_data_source *offer_src;
    struct rail_data_offer *offers;    /* every offer we handed this device */
    struct rail_data_device *next;
};

struct rail_data_offer {
    struct wl_resource *resource;
    struct rail_data_source *source;
    struct rail_data_device *device;   /* owner, so a destroyed offer can unlink */
    struct rail_data_offer *dev_next;
};

/* --- workspace: a circular strip --- */
struct rail_ws {
    int index;
    struct rail_surface *first, *focused;
    struct rail_ws *prev, *next; /* workspaces list on output */
};

/* --- output (DRM/KMS) --- */
struct gbm_device;
struct gbm_surface;
struct gbm_bo;
struct rail_output {
    struct rail *rail;
    uint32_t conn_id;
    drmModeConnector *conn;
    drmModeCrtc *crtc_saved;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    /* GPU compositing: GBM device + EGL window surface over a GBM scanout surface */
    struct gbm_device *gbm;
    struct gbm_surface *gbm_surf;
    struct gbm_bo *disp_bo;   /* BO currently scanned out; released when replaced */
    struct gbm_bo *flip_bo;   /* BO the in-flight page flip targets */
    void *egl_dpy;            /* EGLDisplay */
    void *egl_ctx;            /* EGLContext */
    void *egl_surf;           /* EGLSurface (window) */
    int egl_ok;
    bool flip_pending;        /* page-flip in flight */
    bool suspended;           /* switched away to another VT: no drawing, no DRM master */
    bool needs_frame;         /* a redraw was requested while flip pending */
    bool anim_busy;           /* last composite still animating */
    uint32_t last_anim_ms;    /* timestamp of previous anim step (time-based easing) */
    int w, h;
    int fd; /* drm fd */
    /* strip animation */
    int offset, target;     /* strip scroll offset in px */
    struct rail_ws *workspaces, *active;
    struct rail_surface *bar;
    struct rail_surface *launcher;   /* the floating launcher while it is open */
    struct rail_surface *dialogs;    /* mapped dialogs, oldest first; the last one is on top and has the keyboard */
    /* pointer cursor */
    int px, py;
};

/* --- server --- */
/* one wl_output object a client bound from our global */
struct rail_out_res {
    struct wl_resource *resource;
    struct rail *rail;
    struct rail_out_res *next;
};

struct rail {
    struct wl_display *display;
    struct wl_event_loop *loop;
    struct wl_event_source *drm_src;
    bool quit;
    uint32_t drm_frame_count;

    struct rail_output *output; /* v0: single output */
    struct rail_surface *all_surfaces; /* linked via g_next for safe teardown */
    struct rail_out_res *outputs;      /* bound wl_output objects (enter/leave) */

    /* core clipboard: current selection source + list of data devices */
    struct rail_data_source *selection_source;
    struct rail_data_device *data_devices;
    /* primary selection (middle-click / select-to-copy) */
    struct rail_data_source *primary_source;
    struct rail_data_device *primary_devices;

    /* interactive move/resize drag */
    bool dragging;                 /* pointer grab for move/resize */
    int drag_mode;                 /* 0=none,1=move, else resize edge bitmask */
    struct rail_surface *drag_surf;
    int drag_grab_x, drag_grab_y;  /* pointer pos at grab */
    int drag_start_x, drag_start_y, drag_start_w, drag_start_h;

    uint32_t pending_serial;

    /* globals */
    struct wl_global *compositor_global;
    struct wl_global *shm_global;
    struct wl_global *xdg_wm_base_global;
    struct wl_global *seat_global;
    struct wl_global *output_global;

    struct wl_list seats;
    bool pointer_down;

    /* dmabuf buffers sampled by the frame being presented: released back to
       their client once that frame is on screen */
    struct rail_buffer *rel_pending;

    /* pointer cursor owned by the focused client via wl_pointer.set_cursor */
    struct rail_surface *cursor; /* NULL while no client has taken the cursor */
    bool cursor_set;             /* a client called set_cursor (even with NULL) */
    int cursor_hx, cursor_hy;    /* hotspot, surface-local */
};

/* compositor.c */
void rail_compositor_init(struct rail *r);
void rail_xdg_init(struct rail *r);
void rail_output_global_init(struct rail *r);
void rail_seat_init(struct rail *r);
void rail_dmabuf_init(struct rail *r);

/* view/strip management */
void rail_view_map(struct rail_surface *s);
void rail_view_unmap(struct rail_surface *s);
void rail_strip_focus_next(struct rail *r);
void rail_strip_focus_prev(struct rail *r);
void rail_ws_switch(struct rail *r, int index);
void rail_spawn(const char *cmd);
/* run one of railwl's own utilities: next to the railwl binary if it is there, else via PATH */
void rail_spawn_util(const char *name);
void rail_close_focused(struct rail *r);
void rail_seat_set_focus(struct rail_surface *s);
void rail_configure_all(struct rail *r);
/* interactive move/resize */
void rail_drag_begin(struct rail_surface *s, int mode, int px, int py);
void rail_drag_motion(struct rail *r, int px, int py);
void rail_drag_end(struct rail *r);
/* dismiss popups on click outside */
void rail_dismiss_popups(struct rail *r);
/* focus-follows-click */
struct rail_surface *rail_pick_surface(struct rail *r, int px, int py);
void rail_click_focus(struct rail *r, int px, int py);
/* core clipboard */
void rail_selection_broadcast(struct rail *r);
void rail_primary_broadcast(struct rail *r);

/* capture.c: ext-image-copy-capture-v1 (screenshots) */
void rail_capture_init(struct rail *r);
void rail_capture_impl_check(void);
/* called from render_gl() with the GL context current, scene composed up to
   (after_cursor=false) or including (true) the pointer */
void rail_capture_service(struct rail *r, bool after_cursor);

/* output.c */
int rail_output_init(struct rail *r);
/* top-down RGBA copy of the framebuffer being composed (GL context current);
   dst needs w*h*4 bytes; -1 on failure */
int rail_output_read_rgba(struct rail *r, uint8_t *dst);
void rail_output_frame(struct rail *r);
/* region screenshot selection (Super+Shift+S): drawn by output.c, driven by input.c */
extern struct rail_sel { bool active, dragging; int x0, y0, x1, y1; } rail_sel;
void rail_output_suspend(struct rail *r);
void rail_output_resume(struct rail *r);
/* dmabuf support (output.c): import planes into an EGLImage, and hand the
   buffer back to its client once the frame sampling it has been presented */
int rail_dmabuf_supported(void);
int rail_dmabuf_import(struct rail_buffer *b);
void rail_dmabuf_destroy(struct rail_buffer *b);
void rail_buffer_release_later(struct rail *r, struct rail_buffer *b);
/* drop a surface's texture (its storage may already be gone) */
void rail_output_forget_tex(struct rail_surface *s);
/* formats railwl can import, as DRM fourccs; modifiers importable for one */
const uint32_t *rail_dmabuf_formats(int *n);
uint64_t *rail_dmabuf_modifiers(struct rail *r, uint32_t fourcc, int *n);

/* input.c */
int rail_input_init(struct rail *r);
void rail_input_grab(bool on);
void rail_input_reset(void);

/* startup self-check: a NULL request handler aborts the whole compositor,
   so every interface we expose is walked at startup (compositor.c) */
void rail_check_impl(const struct wl_interface *iface, const void *impl);
void rail_impl_check(void);
void rail_input_impl_check(void);
void rail_focus_forget(struct rail_surface *s);

/* vt.c */
extern bool rail_active;   /* false while another VT is shown */
int rail_vt_init(struct rail *r);
void rail_vt_restore(void);
void rail_vt_switch(int vt);
bool rail_vt_ok(void);

/* lseat.c: libseat (seatd/logind) session; falls back to direct open() */
bool rail_lseat_init(struct rail *r);
bool rail_lseat_active(void);
int rail_lseat_open(const char *path, int flags);
void rail_lseat_close(int fd);
void rail_lseat_switch(int vt);
void rail_lseat_finish(void);

/* trace.c: crash note for clients, /tmp/railwl-crash.txt */
void rail_trace_init(struct wl_display *d);

#endif
