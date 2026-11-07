/* Linux virtual terminal handling.
   While railwl runs: the console is in graphics mode (no text drawn over our
   screen), keyboards are grabbed (keys don't leak into the shell), and
   ctrl+alt+F1..F12 switch away from / back to railwl cleanly. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#ifdef __FreeBSD__
#include <sys/consio.h>
#include <sys/kbio.h>
/* vt(4) has no K_OFF; seatd silences the console keyboard with K_RAW instead */
#define RAIL_KB_OFF K_RAW
#else
#include <linux/vt.h>
#include <linux/kd.h>
#endif
#include <wayland-server.h>
#include "rail.h"

bool rail_active = true;
static int tty_fd = -1;
static bool vt_ok;
#ifdef __FreeBSD__
static int old_kbmode = -1;   /* keyboard mode to give back to the console */
#endif

bool rail_vt_ok(void){ return vt_ok; }

/* only ioctls in here: it is also called from the crash handler */
void rail_vt_restore(void)
{
    if(tty_fd < 0) return;
    struct vt_mode m = { .mode = VT_AUTO };
    ioctl(tty_fd, VT_SETMODE, &m);
    ioctl(tty_fd, KDSETMODE, KD_TEXT);
#ifdef __FreeBSD__
    if(old_kbmode >= 0) ioctl(tty_fd, KDSKBMODE, old_kbmode);
#endif
}

/* kernel asks us to give up the screen (user switched to another VT) */
static int on_release(int sig, void *data)
{
    (void)sig;
    struct rail *r = data;
    rail_active = false;
    rail_input_grab(false);
    rail_input_reset();
    rail_output_suspend(r);
#ifdef __FreeBSD__
    if(old_kbmode >= 0) ioctl(tty_fd, KDSKBMODE, old_kbmode);
#endif
    ioctl(tty_fd, VT_RELDISP, 1);
    return 0;
}

/* we are the active VT again */
static int on_acquire(int sig, void *data)
{
    (void)sig;
    struct rail *r = data;
    ioctl(tty_fd, VT_RELDISP, VT_ACKACQ);
#ifdef __FreeBSD__
    ioctl(tty_fd, KDSKBMODE, RAIL_KB_OFF);
#endif
    rail_input_reset();
    rail_input_grab(true);
    rail_active = true;
    rail_output_resume(r);
    return 0;
}

int rail_vt_init(struct rail *r)
{
    if(getenv("RAIL_NO_VT") || rail_lseat_active()) return -1;   /* the seat daemon owns the VT */
    tty_fd = open("/dev/tty", O_RDWR | O_CLOEXEC);
#ifdef __FreeBSD__
    int active_vt;
    if(tty_fd < 0 || ioctl(tty_fd, VT_GETACTIVE, &active_vt) < 0){
#else
    struct vt_stat st;
    if(tty_fd < 0 || ioctl(tty_fd, VT_GETSTATE, &st) < 0){
#endif
        fprintf(stderr, "railwl: not on a VT, VT switching disabled\n");
        if(tty_fd >= 0) close(tty_fd);
        tty_fd = -1;
        return -1;
    }
    wl_event_loop_add_signal(r->loop, SIGUSR1, on_release, r);
    wl_event_loop_add_signal(r->loop, SIGUSR2, on_acquire, r);
    struct vt_mode m = { .mode = VT_PROCESS, .relsig = SIGUSR1, .acqsig = SIGUSR2 };
#ifdef __FreeBSD__
    /* vt(4) rejects VT_PROCESS unless frsig is a valid signal too (Xorg uses SIGIO) */
    m.frsig = SIGIO;
    signal(SIGIO, SIG_IGN);
    /* kernel keyboard driver must stop feeding the console behind us, or every
       key typed in railwl also lands in the shell on this VT */
    if(ioctl(tty_fd, KDGKBMODE, &old_kbmode) < 0) old_kbmode = -1;
    ioctl(tty_fd, KDSKBMODE, RAIL_KB_OFF);
#endif
    if(ioctl(tty_fd, VT_SETMODE, &m) < 0 || ioctl(tty_fd, KDSETMODE, KD_GRAPHICS) < 0){
        perror("railwl: vt setup");
        rail_vt_restore();
        close(tty_fd);
        tty_fd = -1;
        return -1;
    }
    vt_ok = true;
    atexit(rail_vt_restore);
    return 0;
}

void rail_vt_switch(int vt)
{
    if(rail_lseat_active()){ if(vt >= 1 && vt <= 12) rail_lseat_switch(vt); return; }
    if(vt_ok && vt >= 1 && vt <= 12) ioctl(tty_fd, VT_ACTIVATE, vt);
}
