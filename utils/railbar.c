/* railbar: clock at leftmost, everything else at right. custom icons.
 * redrawn every second; wayland-driven. from scratch, no toolkit.
 *
 * right side is interactive (hover highlights the item):
 *   keyboard layout  click = next layout (asks railwl over its control fifo)
 *   network          click = run $RAILBAR_NET_CMD if set; icon follows link state
 *   volume           click = mute, wheel = +/-5%  (wpctl, pactl, FreeBSD mixer or amixer)
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
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

/* ---- interactive items on the right ---- */
enum { IT_LAYOUT, IT_NET, IT_VOL, IT_N };
struct rect { int x0, x1; };
static struct rect rects[IT_N];     /* where draw() put each item (bar x coords) */
static int hover = -1;
#define BTN_LEFT 0x110
#define HIT_PAD 4                   /* half of the 8px gap between items */

/* ---- network: link state from /sys on Linux, assumed up elsewhere ---- */
static bool net_up = true;
static void net_read(void)
{
#ifdef __linux__
    DIR *d = opendir("/sys/class/net");
    if(!d) return;
    bool up = false;
    struct dirent *e;
    while((e = readdir(d))){
        if(e->d_name[0] == '.' || !strcmp(e->d_name, "lo")) continue;
        char p[300], st[16] = {0};
        snprintf(p, sizeof p, "/sys/class/net/%s/operstate", e->d_name);
        FILE *f = fopen(p, "r");
        if(!f) continue;
        if(fgets(st, sizeof st, f) && !strncmp(st, "up", 2)) up = true;
        fclose(f);
    }
    closedir(d);
    net_up = up;
#endif
}

static void net_click(void)
{
    const char *cmd = getenv("RAILBAR_NET_CMD");
    if(cmd && *cmd){
        char buf[600];
        snprintf(buf, sizeof buf, "(%s) >/dev/null 2>&1 &", cmd);
        if(system(buf)){}
    }
    net_read();
}

/* ---- volume: shell out to whichever mixer exists, read state back ---- */
enum { VB_NONE, VB_WPCTL, VB_PACTL, VB_AMIXER, VB_MIXER };
static int vb = VB_NONE;
static int vol_pct = 50;
static bool vol_muted;

/* [backend][0=up, 1=down, 2=toggle mute] */
static const char *vol_cmds[][3] = {
    { NULL, NULL, NULL },
    { "wpctl set-volume -l 1.0 @DEFAULT_AUDIO_SINK@ 5%+", "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-",
      "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle" },
    { "pactl set-sink-volume @DEFAULT_SINK@ +5%", "pactl set-sink-volume @DEFAULT_SINK@ -5%",
      "pactl set-sink-mute @DEFAULT_SINK@ toggle" },
    { "amixer -q sset Master 5%+", "amixer -q sset Master 5%-", "amixer -q sset Master toggle" },
    /* FreeBSD / OSS mixer(8): values are fractions, 0.05 = 5% */
    { "mixer vol=+0.05", "mixer vol=-0.05", "mixer vol.mute=^" },
};

/* program each backend needs, indexed like vol_cmds */
static const char *vb_bin[] = { NULL, "wpctl", "pactl", "amixer", "mixer" };

static bool have_cmd(const char *name)
{
    const char *p = getenv("PATH");
    char buf[512];
    while(p && *p){
        const char *e = strchr(p, ':');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if(n && n < 300){
            snprintf(buf, sizeof buf, "%.*s/%s", (int)n, p, name);
            if(access(buf, X_OK) == 0) return true;
        }
        if(!e) break;
        p = e + 1;
    }
    return false;
}

static bool vol_read(void)
{
    const char *cmd;
    switch(vb){
    case VB_WPCTL: cmd = "LC_ALL=C wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null"; break;
    case VB_PACTL: cmd = "export LC_ALL=C; pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null; "
                         "pactl get-sink-mute @DEFAULT_SINK@ 2>/dev/null"; break;
    case VB_AMIXER: cmd = "LC_ALL=C amixer sget Master 2>/dev/null"; break;
    case VB_MIXER:  cmd = "LC_ALL=C mixer 2>/dev/null"; break;
    default: return false;
    }
    FILE *f = popen(cmd, "r");
    if(!f) return false;
    char out[512];
    size_t n = fread(out, 1, sizeof out - 1, f);
    out[n] = 0;
    pclose(f);
    if(!n) return false;
    int pct = -1;
    bool mixer_muted = false;
    if(vb == VB_WPCTL){                       /* "Volume: 0.45" or "Volume: 0.45 [MUTED]" */
        const char *v = strstr(out, "Volume:");
        if(v) pct = (int)(atof(v + 7) * 100.0 + 0.5);
    } else if(vb == VB_MIXER){
        /* mixer(8) lists every control; find the line "    vol  = 0.89:0.89  pbk mute"
           (the value is a fraction of full volume, " mute" is appended when muted) */
        for(const char *ln = out; ln && *ln; ){
            const char *nl = strchr(ln, '\n');
            size_t len = nl ? (size_t)(nl - ln) : strlen(ln);
            const char *p = ln;
            while(p < ln + len && (*p == ' ' || *p == '\t')) p++;
            if(ln + len - p > 3 && !strncmp(p, "vol", 3) &&
               (p[3] == ' ' || p[3] == '\t' || p[3] == '=')){
                char line[160];
                size_t l = (size_t)(ln + len - p);
                if(l >= sizeof line) l = sizeof line - 1;
                memcpy(line, p, l);
                line[l] = 0;
                const char *eq = strchr(line, '=');
                if(eq){
                    pct = (int)(atof(eq + 1) * 100.0 + 0.5);
                    mixer_muted = strstr(line, " mute") != NULL;
                }
                break;
            }
            ln = nl ? nl + 1 : NULL;
        }
    } else {                                  /* first "NN%" in the output */
        for(const char *q = out; *q; q++){
            if(*q == '%' && q > out && q[-1] >= '0' && q[-1] <= '9'){
                const char *d = q - 1;
                while(d > out && d[-1] >= '0' && d[-1] <= '9') d--;
                pct = atoi(d);
                break;
            }
        }
    }
    if(pct >= 0) vol_pct = pct;
    vol_muted = strstr(out, "[MUTED]") || strstr(out, "Mute: yes") || strstr(out, "[off]")
             || mixer_muted;
    return pct >= 0;
}

/* pick the first backend that is installed AND actually answers: a leftover
   pactl with no PulseAudio server running must not hide the real mixer */
static void vol_init(void)
{
    static const int order[] = { VB_WPCTL, VB_PACTL, VB_MIXER, VB_AMIXER };
    vb = VB_NONE;
    for(unsigned i = 0; i < sizeof order / sizeof *order; i++){
        if(!have_cmd(vb_bin[order[i]])) continue;
        vb = order[i];
        if(vol_read()) return;
        vb = VB_NONE;
    }
}

static void vol_do(int what)   /* 0 up, 1 down, 2 toggle mute */
{
    if(vb == VB_NONE) return;
    char buf[256];
    snprintf(buf, sizeof buf, "%s >/dev/null 2>&1", vol_cmds[vb][what]);
    if(system(buf)){}
    vol_read();
}

/* ---- layout: ask railwl for the next one ---- */
static void layout_request(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[300];
    snprintf(path, sizeof path, "%s/railwl/ctl", rt ? rt : "/tmp");
    int fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if(fd < 0) return;
    if(write(fd, "layout-next\n", 12) < 0){}
    close(fd);
}

static void hl(uint32_t *px, int W, int H, int item)
{
    if(hover != item) return;
    rc_fill(px, W, H, rects[item].x0 - HIT_PAD, 3,
            rects[item].x1 - rects[item].x0 + 2 * HIT_PAD, H - 6, 0x1E2F55);
}

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
    for(int i = 0; i < IT_N; i++) rects[i].x0 = rects[i].x1 = 0;

    /* keyboard layout (rightmost) */
    int lw = rc_text_width(c->layout);
    x -= lw;
    if(lw > 0){
        rects[IT_LAYOUT].x0 = x; rects[IT_LAYOUT].x1 = x + lw;
        hl(px, W, H, IT_LAYOUT);
    }
    rc_text(px, W, H, x, 10, c->layout, hover == IT_LAYOUT ? 0xFFFFFFu : 0xAAAAAAu);
    x -= 8;

    /* network */
    x -= 13;
    rects[IT_NET].x0 = x; rects[IT_NET].x1 = x + 13;
    hl(px, W, H, IT_NET);
    rc_icon_network(px, W, H, x, 8, net_up, net_up ? 0x4EC94Eu : 0xC94E4Eu);
    x -= 8;

    /* volume */
    x -= 14;
    rects[IT_VOL].x0 = x; rects[IT_VOL].x1 = x + 14;
    hl(px, W, H, IT_VOL);
    int lvl = vol_muted || vol_pct <= 0 ? 0 : vol_pct < 34 ? 1 : vol_pct < 67 ? 2 : 3;
    rc_icon_volume(px, W, H, x, 8, lvl, vol_muted ? 0x777777u : 0xFFFFFFu);
    x -= 8;

    rc_present(w);
}

static void on_redraw(struct rc_client *c)
{
    time_t t = time(NULL);
    if(t != last_sec){
        last_sec = t;
        read_layout(c);                 /* cheap; also covers builds without inotify */
        if(t % 5 == 0) net_read();
        draw(c);
    }
}

static int hit_item(double mx, double my)
{
    if(my < 0 || my >= BAR_H) return -1;
    for(int i = 0; i < IT_N; i++)
        if(rects[i].x1 > rects[i].x0 && mx >= rects[i].x0 - HIT_PAD && mx < rects[i].x1 + HIT_PAD)
            return i;
    return -1;
}

static void on_motion(struct rc_client *c, double mx, double my)
{
    int h = hit_item(mx, my);
    if(h != hover){ hover = h; draw(c); }
}

static void on_leave(struct rc_client *c)
{
    if(hover != -1){ hover = -1; draw(c); }
}

static void on_button(struct rc_client *c, uint32_t button, bool pressed, double mx, double my)
{
    if(!pressed || button != BTN_LEFT) return;
    switch(hit_item(mx, my)){
    case IT_LAYOUT: layout_request(); break;
    case IT_NET:    net_click(); break;
    case IT_VOL:    vol_do(2); break;
    default: return;
    }
    draw(c);
}

static void on_axis(struct rc_client *c, double dx, double dy)
{
    (void)dx;
    if(dy == 0 || hit_item(c->ptr_x, c->ptr_y) != IT_VOL) return;
    vol_do(dy < 0 ? 0 : 1);             /* wayland: negative = wheel up */
    draw(c);
}

int main(void)
{
    struct rc_client c;
    if(!rc_connect(&c, "railbar")) return 1;
    c.on_redraw = on_redraw;
    c.tick_ms = 1000;   /* wake every second so the clock advances with no input */
    c.on_ptr_motion = on_motion;
    c.on_ptr_leave = on_leave;
    c.on_ptr_button = on_button;
    c.on_ptr_axis = on_axis;

    vol_init();
    net_read();

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
