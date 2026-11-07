/* test: alt+shift+e must terminate the display */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef __FreeBSD__
#include <dev/evdev/input.h>
#else
#include <linux/input.h>
#endif
#include <wayland-server.h>
#include "rail.h"

/* stubs for compositor hooks used by input.c */
static int strip_next_calls, strip_prev_calls;
void rail_strip_focus_next(struct rail *r){ (void)r; strip_next_calls++; }
void rail_strip_focus_prev(struct rail *r){ (void)r; strip_prev_calls++; }
void rail_ws_switch(struct rail *r, int i){ (void)r;(void)i; }
void rail_spawn(const char *c){ (void)c; }
void rail_spawn_util(const char *c){ (void)c; }
void rail_close_focused(struct rail *r){ (void)r; }
void rail_output_frame(struct rail *r){ (void)r; }
void rail_drag_begin(struct rail_surface *s, int mode, int px, int py){ (void)s;(void)mode;(void)px;(void)py; }
void rail_drag_motion(struct rail *r, int px, int py){ (void)r;(void)px;(void)py; }
void rail_drag_end(struct rail *r){ (void)r; }
void rail_dismiss_popups(struct rail *r){ (void)r; }
struct rail_surface *rail_pick_surface(struct rail *r, int px, int py){ (void)r;(void)px;(void)py; return NULL; }
void rail_click_focus(struct rail *r, int px, int py){ (void)r;(void)px;(void)py; }
void rail_selection_broadcast(struct rail *r){ (void)r; }
void rail_primary_broadcast(struct rail *r){ (void)r; }
void rail_check_impl(const struct wl_interface *i, const void *impl){ (void)i;(void)impl; }
bool rail_active = true;
bool rail_vt_ok(void){ return false; }
void rail_vt_switch(int vt){ (void)vt; }
int rail_lseat_open(const char *p, int f){ (void)p; (void)f; return -1; }

#include "input.c"

static void feed(struct rail *r, uint16_t code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    input_key(r, &ev);
}

int main(void)
{
    alarm(3); /* if terminate fails, wl_display_run blocks and alarm kills us */
    struct wl_display *d = wl_display_create();
    struct rail r; memset(&r, 0, sizeof r);
    r.display = d;

    /* alt+left / alt+right must move the strip */
    feed(&r, KEY_LEFTALT, 1);
    feed(&r, KEY_LEFT, 1);
    feed(&r, KEY_LEFT, 0);
    feed(&r, KEY_RIGHT, 1);
    feed(&r, KEY_RIGHT, 0);
    if(strip_prev_calls != 1 || strip_next_calls != 1){
        fprintf(stderr, "key test: FAIL - alt+arrows strip calls prev=%d next=%d (want 1,1)\n",
                strip_prev_calls, strip_next_calls);
        return 1;
    }
    fprintf(stderr, "key test: PASS - alt+left/right move the strip\n");

    /* alt+shift+e must terminate the display */
    feed(&r, KEY_LEFTSHIFT, 1);
    feed(&r, KEY_E, 1);
    feed(&r, KEY_E, 0);

    fprintf(stderr, "key test: entering manual loop (should exit instantly)\n");
    struct wl_event_loop *loop = wl_display_get_event_loop(d);
    while(!r.quit){ wl_event_loop_dispatch(loop, 0); }
    wl_display_destroy(d);
    fprintf(stderr, "key test: PASS - alt+shift+e terminated the loop\n");
    return 0;
}
