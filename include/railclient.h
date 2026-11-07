#ifndef RAILCLIENT_H
#define RAILCLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include <wayland-client.h>

#define RC_UNUSED(x) (void)(x)

struct rc_client;
struct rc_window;

struct rc_client {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct rc_window *window;
    const char *app_id;
    void (*on_key)(struct rc_client *c, uint32_t sym, bool pressed, uint32_t modmask);
    void (*on_ptr_motion)(struct rc_client *c, double x, double y);
    void (*on_ptr_button)(struct rc_client *c, uint32_t button, bool pressed, double x, double y);
    void (*on_ptr_axis)(struct rc_client *c, double dx, double dy);
    void (*on_redraw)(struct rc_client *c);
    int watch_fd;                          /* optional extra fd to poll (-1/0 = none) */
    void (*on_watch)(struct rc_client *c); /* called when watch_fd is readable */
    void (*on_configure)(struct rc_client *c, int w, int h);
    bool running;
    uint32_t modmask; /* 1 shift 2 caps 4 ctrl 8 alt 64 super */
    /* keyboard repeat (client-side per protocol) */
    int32_t repeat_rate, repeat_delay; /* rate = chars/sec, 0 = off */
    uint32_t repeat_sym; int repeat_key; uint32_t repeat_mod;
    uint64_t repeat_deadline_ms; /* when next repeat fires; 0 = inactive */
    bool repeat_pressed;
    /* optional periodic wake-up: if tick_ms > 0, rc_run wakes at least that
     * often (even with no input) and calls on_redraw. 0 = block until an event. */
    int tick_ms;
    uint64_t tick_deadline_ms;
    char layout[8]; /* current keyboard layout, 2-letter uppercase (e.g. "US") */
};

struct rc_window {
    struct rc_client *client;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    int w, h;
    bool configured, closed;
    /* double buffered shm */
    struct { struct wl_buffer *buf; void *data; size_t size; int fd; } fb[2];
    int cur;
    int fb_w, fb_h; /* allocated buffer size */
    uint32_t serial;
};

/* connect + bind; returns false on failure */
bool rc_connect(struct rc_client *c, const char *app_id);
/* create window of size w x h (bar height or 0 for default) */
bool rc_window_create(struct rc_client *c, int w, int h);
/* run until closed; returns exit code */
int rc_run(struct rc_client *c);
/* request redraw at next frame (compositor-driven) */
void rc_damage(struct rc_window *w);
/* get current draw target pixels (ARGB8888, stride = w*4) */
uint32_t *rc_pixels(struct rc_window *w);
void rc_present(struct rc_window *w);
int rc_fb_w(struct rc_window *w);
int rc_fb_h(struct rc_window *w);

/* ------- tiny 8x8 font + drawing (all from scratch) ------- */
void rc_clear(uint32_t *px, int w, int h, uint32_t color);
void rc_fill(uint32_t *px, int W, int H, int x, int y, int w, int h, uint32_t color);
/* draw text with 8x8 font, 1px spacing; returns width drawn */
int rc_text(uint32_t *px, int W, int H, int x, int y, const char *s, uint32_t color);
int rc_text_width(const char *s);
/* custom icons (12x12, hand-made glyphs) */
int rc_icon_battery(uint32_t *px, int W, int H, int x, int y, int pct, uint32_t color);
int rc_icon_volume(uint32_t *px, int W, int H, int x, int y, int lvl, uint32_t color);
int rc_icon_network(uint32_t *px, int W, int H, int x, int y, bool up, uint32_t color);
int rc_icon_cpu(uint32_t *px, int W, int H, int x, int y, int load, uint32_t color);

#endif
