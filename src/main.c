#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <execinfo.h>
#include <wayland-server.h>
#include "rail.h"

static struct rail R;
static unsigned long long g_t0;

static unsigned long long mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static void segv(int s){
    (void)s;
    rail_vt_restore(); /* give the console back first so the report is readable */
    fprintf(stderr, "=== CRASH signal %d after %llums ===\n", s, mono_ms() - g_t0);
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);   /* addr2line -f -e build/railwl <+0x..> for static functions */
    _exit(99);
}
/* SIGTERM/SIGINT arrive through the event loop's own signalfd. A plain
   signal handler is not enough: once any helper thread exists the signal
   can be delivered to that thread instead, and the main thread then stays
   parked in epoll_wait forever - the compositor would never exit. */
static int on_sig(int signum, void *data)
{
    (void)signum; (void)data;
    R.quit = 1;
    return 0;
}

/* ~/.config/railwl/autostart: one shell command per line, '#' comments and
   blank lines ignored. Each line is started once when railwl comes up. */
static void run_autostart(void)
{
    const char *home = getenv("HOME");
    if(!home) return;
    char path[512];
    snprintf(path, sizeof path, "%s/.config/railwl/autostart", home);
    FILE *f = fopen(path, "r");
    if(!f) return;
    char line[1024];
    while(fgets(line, sizeof line, f)){
        line[strcspn(line, "\r\n")] = 0;
        char *p = line;
        while(*p == ' ' || *p == '\t') p++;
        if(!*p || *p == '#') continue;
        rail_spawn(p);
    }
    fclose(f);
}

int main(void)
{
    g_t0 = mono_ms();
    { void *warm[1]; backtrace(warm, 1); }   /* load libgcc now, not inside the handler */
    signal(SIGSEGV, segv); signal(SIGABRT, segv);   /* before any init, so early crashes report too */
    R.display = wl_display_create();
    if(!R.display){ fprintf(stderr,"railwl: cannot create display\n"); return 1; }
    R.loop = wl_display_get_event_loop(R.display);
    rail_trace_init(R.display);   /* crash note for clients: /tmp/railwl-crash.txt */

    rail_lseat_init(&R);   /* seat daemon first: it opens the GPU for us */
    if(rail_output_init(&R) < 0){
        fprintf(stderr,"railwl: no DRM output, aborting\n");
        wl_display_destroy(R.display);
        return 1;
    }
    rail_compositor_init(&R);
    rail_xdg_init(&R);
    rail_seat_init(&R);
    rail_vt_init(&R);   /* before input init: input grabs devices only if we own a VT */
    rail_input_init(&R);
    rail_impl_check();
    rail_input_impl_check();

    const char *sock = wl_display_add_socket_auto(R.display);
    if(!sock){ fprintf(stderr,"railwl: no wayland socket\n"); return 1; }
    /* every program railwl spawns (alt+t, super+enter, launcher) inherits this */
    setenv("WAYLAND_DISPLAY", sock, 1);
    /* tell toolkits this is a Wayland session so they pick it without manual
       exports (Qt otherwise tries xcb and aborts). overwrite=0: anything the
       user already exported before starting railwl still wins. */
    setenv("XDG_SESSION_TYPE", "wayland", 0);
    setenv("XDG_CURRENT_DESKTOP", "railwl", 0);
    setenv("QT_QPA_PLATFORM", "wayland;xcb", 0);
    setenv("GDK_BACKEND", "wayland", 0);
    setenv("MOZ_ENABLE_WAYLAND", "1", 0);
    /* the bar is part of the session; RAIL_NO_AUTOSTART=1 skips it */
    if(!getenv("RAIL_NO_AUTOSTART")){
        rail_spawn_util("railbar");
        run_autostart();
    }

    wl_event_loop_add_signal(R.loop, SIGTERM, on_sig, &R);
    wl_event_loop_add_signal(R.loop, SIGINT, on_sig, &R);
    fprintf(stderr, "railwl: up WAYLAND_DISPLAY=%s %dx%d\n", sock, R.output->w, R.output->h);

    /* manual loop: wl_display_run ignores pre-set terminate in this libwayland */
    int fps_mode = getenv("RAIL_FPS") != NULL;
    uint32_t last_t = 0, last_f = 0;
    while(!R.quit){
        wl_event_loop_dispatch(R.loop, fps_mode ? 500 : -1);
        wl_display_flush_clients(R.display);
        if(fps_mode){
            struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
            uint32_t now = (uint32_t)(ts.tv_sec*1000 + ts.tv_nsec/1000000);
            if(!last_t){ last_t = now; last_f = R.drm_frame_count; }
            else if(now - last_t >= 1000){
                fprintf(stderr, "railwl: fps=%.1f frames=%u anim=%d\n",
                        (double)(R.drm_frame_count - last_f) * 1000.0 / (now - last_t),
                        R.drm_frame_count, R.output->anim_busy ? 1 : 0);
                last_t = now; last_f = R.drm_frame_count;
            }
        }
    }
    rail_vt_restore();
    /* Give the console its screen back even when we never owned a VT. */
    rail_output_suspend(&R);
    rail_lseat_finish();
    wl_display_destroy(R.display);
    return 0;
}
