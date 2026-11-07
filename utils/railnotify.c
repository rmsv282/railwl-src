/* railnotify: notifier. shows a popup for a few seconds.
 * usage: railnotify "title" "body"   (or piped: echo "title|body" | railnotify) */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "railclient.h"
#include "xdg-shell-client-protocol.h"

#define W 340
#define H 96
#define AMBER 0xFFBF00u

static char title[128] = "notification";
static char body[256] = "";
static time_t shown_at;
static bool have_input;

static void draw(struct rc_client *c)
{
    struct rc_window *w = c->window;
    if(!w->configured || !rc_pixels(w)) return;
    int fw = rc_fb_w(w), fh = rc_fb_h(w);
    if(fw <= 0 || fh <= 0) return;
    uint32_t *px = rc_pixels(w);
    rc_clear(px, fw, fh, 0xFF0A1428u);   /* app background, same as railfm */
    rc_fill(px, fw, fh, 0, 0, w->w, 2, AMBER);
    rc_fill(px, fw, fh, 0, w->h-2, w->w, 2, AMBER);
    rc_fill(px, fw, fh, 0, 0, 2, w->h, AMBER);
    rc_fill(px, fw, fh, w->w-2, 0, 2, w->h, AMBER);
    rc_text(px, fw, fh, 10, 12, title, AMBER);
    rc_text(px, fw, fh, 10, 34, body, 0xCCCCCCu);
    rc_present(w);
}

static void on_key(struct rc_client *c, uint32_t sym, bool pressed, uint32_t mod)
{
    (void)mod;
    if(pressed && (sym == XKB_KEY_Escape || sym == XKB_KEY_Return)) c->running = false;
}

int main(int argc, char **argv)
{
    if(argc >= 2) snprintf(title, sizeof title, "%s", argv[1]);
    if(argc >= 3) snprintf(body, sizeof body, "%s", argv[2]);
    else if(!isatty(0)){
        /* piped: "title|body" */
        char line[400];
        if(fgets(line, sizeof line, stdin)){
            char *bar = strchr(line, '|');
            if(bar){ *bar = 0; snprintf(body, sizeof body, "%s", bar+1); }
            else snprintf(body, sizeof body, "%s", line);
            size_t l = strlen(body); if(l && body[l-1]=='\n') body[l-1]=0;
            if(argc < 2) snprintf(title, sizeof title, "%s", line);
        }
        have_input = true;
    }

    struct rc_client c;
    if(!rc_connect(&c, "railnotify")) return 1;
    c.on_key = on_key;
    if(!rc_window_create(&c, W, H)) return 1;
    draw(&c);
    shown_at = time(NULL);

    /* run loop with 5s timeout */
    while(c.running){
        struct pollfd pfd = { .fd = wl_display_get_fd(c.display), .events = POLLIN };
        int r = poll(&pfd, 1, 200);
        if(r > 0) wl_display_dispatch(c.display);
        else wl_display_flush(c.display);
        if(time(NULL) - shown_at >= 5) break;
    }
    return 0;
}
