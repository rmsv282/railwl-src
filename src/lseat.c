/* Seat access through libseat (seatd on FreeBSD/Linux, or logind).
   With it railwl runs as a normal user: the seat daemon opens the DRM and
   evdev input for us, owns the VT (graphics mode, keyboard off, VT
   switching) and tells us when to give the screen up or take it back.
   Without a seat daemon (or built without libseat) every function falls back
   to the old behaviour: open the devices directly, railwl's own vt.c handles
   the VT. RAIL_NO_SEAT=1 forces that fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-server.h>
#include "rail.h"

#ifdef HAVE_LIBSEAT
#include <libseat.h>

#define MAX_DEVS 96
static struct libseat *seat;
static struct rail *seat_rail;
static bool seat_enabled;
static struct { int fd, id; } devs[MAX_DEVS];
static int ndevs;

static void on_enable(struct libseat *s, void *data)
{
    (void)s;
    struct rail *r = data;
    seat_enabled = true;
    if(r->output){            /* not the first activation: we are back on our VT */
        rail_active = true;
        rail_input_reset();
        rail_output_resume(r);
    }
}

static void on_disable(struct libseat *s, void *data)
{
    struct rail *r = data;
    seat_enabled = false;
    rail_active = false;
    rail_input_reset();
    if(r->output) rail_output_suspend(r);
    libseat_disable_seat(s);
}

static const struct libseat_seat_listener seat_listener = {
    .enable_seat = on_enable,
    .disable_seat = on_disable,
};

static int seat_fd_cb(int fd, uint32_t mask, void *data)
{
    (void)fd;
    (void)data;
    if((mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) || libseat_dispatch(seat, 0) < 0){
        fprintf(stderr, "railwl: lost the seat daemon, quitting\n");
        seat_rail->quit = 1;
    }
    return 0;
}

bool rail_lseat_init(struct rail *r)
{
    if(getenv("RAIL_NO_SEAT")) return false;
    libseat_set_log_level(getenv("RAIL_SEAT_DEBUG") ? LIBSEAT_LOG_LEVEL_DEBUG
                                                    : LIBSEAT_LOG_LEVEL_SILENT);
    seat_rail = r;
    seat = libseat_open_seat(&seat_listener, r);
    if(!seat){
        fprintf(stderr, "railwl: no seat daemon (is seatd running? RAIL_SEAT_DEBUG=1 for details), "
                        "opening devices directly\n");
        return false;
    }
    /* the seat becomes active asynchronously: wait for it (max ~5 s) */
    for(int i = 0; i < 50 && !seat_enabled; i++){
        struct pollfd p = { .fd = libseat_get_fd(seat), .events = POLLIN };
        if(poll(&p, 1, 100) < 0 && errno != EINTR) break;
        if(libseat_dispatch(seat, 0) < 0) break;
    }
    if(!seat_enabled){
        fprintf(stderr, "railwl: the seat never became active, opening devices directly\n");
        libseat_close_seat(seat);
        seat = NULL;
        return false;
    }
    wl_event_loop_add_fd(r->loop, libseat_get_fd(seat), WL_EVENT_READABLE, seat_fd_cb, r);
    fprintf(stderr, "railwl: using seat %s via libseat\n", libseat_seat_name(seat));
    return true;
}

bool rail_lseat_active(void){ return seat != NULL; }

int rail_lseat_open(const char *path, int flags)
{
    if(!seat) return open(path, flags);
    int fd = -1;
    int id = libseat_open_device(seat, path, &fd);
    if(id < 0) return -1;
    if(flags & O_NONBLOCK) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if(ndevs < MAX_DEVS){ devs[ndevs].fd = fd; devs[ndevs].id = id; ndevs++; }
    return fd;
}

void rail_lseat_close(int fd)
{
    if(seat){
        for(int i = 0; i < ndevs; i++){
            if(devs[i].fd == fd){
                libseat_close_device(seat, devs[i].id);
                devs[i] = devs[--ndevs];
                break;
            }
        }
    }
    close(fd);
}

void rail_lseat_switch(int vt){ if(seat) libseat_switch_session(seat, vt); }

void rail_lseat_finish(void)
{
    if(seat){ libseat_close_seat(seat); seat = NULL; }
}

#else /* built without libseat */

bool rail_lseat_init(struct rail *r){ (void)r; return false; }
bool rail_lseat_active(void){ return false; }
int rail_lseat_open(const char *path, int flags){ return open(path, flags); }
void rail_lseat_close(int fd){ close(fd); }
void rail_lseat_switch(int vt){ (void)vt; }
void rail_lseat_finish(void){}

#endif
