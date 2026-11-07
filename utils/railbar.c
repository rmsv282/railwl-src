/* railbar: clock at leftmost, everything else at right. custom icons.
 * redrawn every second; wayland-driven. from scratch, no toolkit. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#if defined(__linux__) || __has_include(<sys/inotify.h>)
#include <sys/inotify.h>
#define RAIL_HAVE_INOTIFY 1
#endif
#include "railclient.h"
#include "xdg-shell-client-protocol.h"

#define BAR_H 28
#define AMBER 0xFFBF00u
#define STRIPE_W 3   /* px, amber stripe on each side edge */

static time_t last_sec;
static char layout_path[300];
static void draw(struct rc_client *c);

/* active layout code ("US", "UA") published by the compositor */
static bool read_layout(struct rc_client *c)
{
    char buf[8] = {0};
    FILE *f = fopen(layout_path, "r");
    if(!f) return false;
    bool ok = fgets(buf, sizeof buf, f) != NULL;
    fclose(f);
    if(!ok) return false;
    buf[strcspn(buf, "\r\n")] = 0;
    if(!buf[0] || !strcmp(buf, c->layout)) return false;
    snprintf(c->layout, sizeof c->layout, "%s", buf);
    return true;
}

/* compositor rewrote the layout file: re-read it and redraw right away */
static void on_watch(struct rc_client *c)
{
    char ev[512];
    while(read(c->watch_fd, ev, sizeof ev) > 0) {} /* drain (non-blocking) */
    if(read_layout(c)) draw(c);
}
static int hover_x = -1;

static void draw(struct rc_client *c)
{
    struct rc_window *w = c->window;
    if(!w->configured || !rc_pixels(w)) return;
    int W = rc_fb_w(w), H = rc_fb_h(w);
    if(W <= 0 || H <= 0) return;
    uint32_t *px = rc_pixels(w);
    rc_clear(px, W, H, 0xFF0A1428u);   /* app background, same as railfm */

    /* amber stripes down the left and right edge */
    rc_fill(px, W, H, 0, 0, STRIPE_W, H, AMBER);
    rc_fill(px, W, H, W - STRIPE_W, 0, STRIPE_W, H, AMBER);

    /* leftmost: clock */
    time_t t = time(NULL);
    struct tm tm; localtime_r(&t, &tm);
    char clock[64];
    strftime(clock, sizeof clock, "%a %b %d %H:%M", &tm);
    rc_text(px, W, H, 6, 10, clock, 0xFFFFFFu);

    /* right side: everything else, custom icons.
       icons draw rightward from their x, so step left by width first
       to keep the last one inside the 6px margin */
    int x = W - 6;

    /* keyboard layout (rightmost) */
    int lw = rc_text_width(c->layout);
    x -= lw; rc_text(px, W, H, x, 10, c->layout, 0xAAAAAAu); x -= 8;

    /* network */
    x -= 13; rc_icon_network(px, W, H, x, 8, true, 0x4EC94Eu); x -= 8;

    /* volume */
    x -= 14; rc_icon_volume(px, W, H, x, 8, 2, 0xFFFFFFu); x -= 8;

    rc_present(w);
}

static void on_redraw(struct rc_client *c)
{
    time_t t = time(NULL);
    if(t != last_sec){ last_sec = t; draw(c); }
}

static void on_motion(struct rc_client *c, double mx, double my)
{ (void)c; (void)my; hover_x = (int)mx; }

int main(void)
{
    struct rc_client c;
    if(!rc_connect(&c, "railbar")) return 1;
    c.on_redraw = on_redraw;
    c.tick_ms = 1000;   /* wake every second so the clock advances with no input */
    c.on_ptr_motion = on_motion;

    /* layout indicator: read now, then wake whenever the compositor rewrites the file */
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[256];
    snprintf(dir, sizeof dir, "%s/railwl", rt ? rt : "/tmp");
    snprintf(layout_path, sizeof layout_path, "%s/layout", dir);
    read_layout(&c);
#ifdef RAIL_HAVE_INOTIFY
    int in = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if(in >= 0 && inotify_add_watch(in, dir, IN_MOVED_TO | IN_CLOSE_WRITE | IN_CREATE) >= 0){
        c.watch_fd = in; c.on_watch = on_watch;
    }
#endif

    if(!rc_window_create(&c, 0, BAR_H)) return 1;
    draw(&c);
    return rc_run(&c);
}
