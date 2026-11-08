#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#ifdef __FreeBSD__
#include <dev/evdev/input.h>
#else
#include <linux/input.h>
#endif
#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>
#include "rail.h"

/* ---------- keymap + layouts ----------
   layouts come from XKB_DEFAULT_LAYOUT (comma list), default "us,ua".
   the active one is the xkb "group", sent to clients in wl_keyboard.modifiers.
   the active layout code ("US", "UA") is also published to
   $XDG_RUNTIME_DIR/railwl/layout so railbar can show it. */
#define MAX_LAYOUTS 4
static char *keymap_str;
static struct xkb_keymap *km_keep;   /* kept alive: clients' keymap + our state */
static struct xkb_state *xkb_st;     /* real modifier/level state (AltGr, levels) */
static xkb_keycode_t km_max_kc;
static int n_layouts = 1, layout_group;
static char layout_code[MAX_LAYOUTS][8];

/* change only the active layout group; modifiers (esp. the locked NumLock and
   CapsLock) must survive it */
static void xkb_set_group(xkb_layout_index_t g)
{
    if(!xkb_st) return;
    xkb_state_update_mask(xkb_st,
        xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_DEPRESSED),
        xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LATCHED),
        xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LOCKED),
        0, 0, g);
}

static void layout_publish(void)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char dir[256], tmp[300], path[300];
    if(!rt) rt = "/tmp";
    snprintf(dir, sizeof dir, "%s/railwl", rt);
    mkdir(dir, 0700);
    snprintf(tmp, sizeof tmp, "%s/layout.tmp", dir);
    snprintf(path, sizeof path, "%s/layout", dir);
    FILE *f = fopen(tmp, "w");
    if(!f) return;
    fprintf(f, "%s\n", layout_code[layout_group]);
    fclose(f);
    rename(tmp, path); /* atomic: the bar sees a complete file */
}

static void keymap_init(void)
{
    if(keymap_str) return;
    const char *lay = getenv("XKB_DEFAULT_LAYOUT");
    if(!lay || !*lay) lay = "us,ua";
    struct xkb_rule_names names = {
        .layout = lay,
        .variant = getenv("XKB_DEFAULT_VARIANT"),
        .options = getenv("XKB_DEFAULT_OPTIONS"),
    };
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if(!km){ /* bad layout list: fall back to plain US */
        names.layout = "us"; names.variant = NULL; names.options = NULL;
        km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    }
    keymap_str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    n_layouts = (int)xkb_keymap_num_layouts(km);
    if(n_layouts > MAX_LAYOUTS) n_layouts = MAX_LAYOUTS;
    if(n_layouts < 1) n_layouts = 1;
    /* keep the keymap for our own state: clients resolve symbols from it, but
       we need it too so modifiers/levels (AltGr etc.) are reported correctly */
    km_keep = km;
    km_max_kc = xkb_keymap_max_keycode(km);
    xkb_st = xkb_state_new(km);
    xkb_context_unref(ctx);
    /* NumLock starts ON, like a normal desktop: with it off the keypad sends
       KP_Home/KP_Up/... instead of digits, which looks like a dead numpad */
    { xkb_mod_index_t num = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_NUM);
      if(num != XKB_MOD_INVALID)
          xkb_state_update_mask(xkb_st, 0, 0, 1u << num, 0, 0, 0); }

    /* short codes from the layout list: "us,ua" -> "US","UA"
       (a "us(intl)" style entry keeps just the part before the paren) */
    char buf[128]; snprintf(buf, sizeof buf, "%s", names.layout);
    char *save = NULL; int i = 0;
    for(char *t = strtok_r(buf, ",", &save); t && i < MAX_LAYOUTS; t = strtok_r(NULL, ",", &save), i++){
        int j = 0;
        for(; t[j] && t[j] != '(' && j < 2; j++)
            layout_code[i][j] = (t[j] >= 'a' && t[j] <= 'z') ? (char)(t[j] - 'a' + 'A') : t[j];
        layout_code[i][j] = 0;
    }
    for(; i < n_layouts; i++) snprintf(layout_code[i], sizeof layout_code[i], "%d", i + 1);
    layout_publish();
}

/* ---------- seat state ---------- */
struct rail_seat {
    struct rail *rail;
    struct wl_resource *res;
    struct wl_resource *kb, *ptr;
    struct rail_seat *next;
};

static struct rail_seat *seats;

static void kb_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }
static const struct wl_keyboard_interface kb_impl = { .release = kb_release };
static void ptr_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

/* the focused client owns the cursor: its wl_surface is drawn at the pointer
   minus the hotspot, and passing NULL hides the cursor entirely */
static struct rail_surface *pointer_surface;   /* defined with the focus state below */
static void ptr_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial,
                           struct wl_resource *surf, int32_t hx, int32_t hy)
{
    (void)serial;
    struct rail_seat *seat = wl_resource_get_user_data(r);
    struct rail *rail = seat ? seat->rail : NULL;
    if(!rail) return;
    /* only the client the pointer is currently over may change the cursor;
       requests from any other client are ignored (per wl_pointer.set_cursor) */
    if(!pointer_surface || !pointer_surface->resource ||
       wl_resource_get_client(pointer_surface->resource) != c) return;
    struct rail_surface *s = surf ? wl_resource_get_user_data(surf) : NULL;
    rail->cursor = s;
    rail->cursor_hx = hx;
    rail->cursor_hy = hy;
    rail->cursor_set = true;
    rail_output_frame(rail);
}
static const struct wl_pointer_interface ptr_impl = {
    .release = ptr_release,
    .set_cursor = ptr_set_cursor,
};
/* a client may destroy its keyboard/pointer without dropping the seat */
static void kb_res_destroy(struct wl_resource *r)
{
    struct rail_seat *s = wl_resource_get_user_data(r);
    if(s && s->kb == r) s->kb = NULL;
}
static void ptr_res_destroy(struct wl_resource *r)
{
    struct rail_seat *s = wl_resource_get_user_data(r);
    if(s && s->ptr == r) s->ptr = NULL;
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct rail_seat *seat = wl_resource_get_user_data(r);
    struct wl_resource *kb = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    seat->kb = kb;
    wl_resource_set_implementation(kb, &kb_impl, seat, kb_res_destroy);
    /* send keymap: compiled by xkbcommon, xkb v1 via memfd */
    keymap_init();
    size_t len = strlen(keymap_str) + 1; /* include the trailing NUL: clients map and parse this region */
    int fd = memfd_create("rail-keymap", 0);
    ftruncate(fd, len);
    write(fd, keymap_str, len);
    lseek(fd, 0, SEEK_SET);
    wl_keyboard_send_keymap(kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);
    /* repeat_info is a v4 event: never send it to a client that bound an
       older wl_keyboard (libwayland-client aborts on an unknown opcode) */
    if(wl_resource_get_version(kb) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(kb, 25, 600);
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct rail_seat *seat = wl_resource_get_user_data(r);
    struct wl_resource *ptr = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    seat->ptr = ptr;
    wl_resource_set_implementation(ptr, &ptr_impl, seat, ptr_res_destroy);
}

static void seat_get_touch(struct wl_client *c, struct wl_resource *r, uint32_t id){ (void)c;(void)r;(void)id; }
static void seat_release(struct wl_client *c, struct wl_resource *r){ (void)c; wl_resource_destroy(r); }

static const struct wl_seat_interface seat_impl = {
    .get_keyboard = seat_get_keyboard,
    .get_pointer = seat_get_pointer,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void seat_res_destroy(struct wl_resource *r)
{
    struct rail_seat *s = wl_resource_get_user_data(r);
    if(!s) return;
    if(s == seats) seats = s->next;
    else for(struct rail_seat *i=seats;i&&i->next;i=i->next) if(i->next==s){ i->next=s->next; break; }
    free(s);
}

static void bind_seat(struct wl_client *client, void *data, uint32_t ver, uint32_t id)
{
    struct rail *rail = data;
    struct rail_seat *seat = calloc(1, sizeof *seat);
    seat->rail = rail;
    if(ver > 7) ver = 7;
    struct wl_resource *res = wl_resource_create(client, &wl_seat_interface, ver, id);
    seat->res = res;
    wl_resource_set_implementation(res, &seat_impl, seat, seat_res_destroy);
    seat->next = seats; seats = seat;
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER);
    if(wl_resource_get_version(res) >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(res, "seat0");
}

void rail_seat_init(struct rail *r)
{
    r->seat_global = wl_global_create(r->display, &wl_seat_interface, 7, r, bind_seat);
}

/* ---------- focus tracking ---------- */
static struct rail_surface *focused_surface;   /* keyboard focus: the tile */
static struct rail_surface *pointer_surface;   /* pointer focus: whatever is under the cursor (tile or popup) */
static bool meta_down, shift_down, ctrl_down, alt_down, cap_locked;
static uint32_t serial(void){ static uint32_t s; return ++s; }

/* route input to the CLIENT owning the surface: every client binds its own
   seat resources, so never send another client's keyboard/pointer */
/* A client may bind wl_seat several times (Firefox: GTK and Gecko each do) and
   every wl_keyboard/wl_pointer it created must receive the events: the one
   that actually handles input is not necessarily the newest binding. */
#define FOR_KB(cl, K) \
    for(struct rail_seat *K##_s = seats; K##_s; K##_s = K##_s->next) \
        if(K##_s->kb && wl_resource_get_client(K##_s->kb) == (cl)) \
            for(struct wl_resource *K = K##_s->kb, *K##_once = K; K##_once; K##_once = NULL)
#define FOR_PTR(cl, P) \
    for(struct rail_seat *P##_s = seats; P##_s; P##_s = P##_s->next) \
        if(P##_s->ptr && wl_resource_get_client(P##_s->ptr) == (cl)) \
            for(struct wl_resource *P = P##_s->ptr, *P##_once = P; P##_once; P##_once = NULL)
static struct wl_client *surf_client(struct rail_surface *sf)
{ return (sf && sf->resource) ? wl_resource_get_client(sf->resource) : NULL; }

static void ptr_frame(struct wl_resource *p)
{
    if(p && wl_resource_get_version(p) >= WL_POINTER_FRAME_SINCE_VERSION)
        wl_pointer_send_frame(p);
}

static uint32_t mod_depressed(void)
{
    /* prefer the xkb state: it knows level-3 (AltGr), latching, etc. */
    if(xkb_st) return xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_DEPRESSED);
    uint32_t d = 0;
    if(shift_down) d |= 1;      /* shift */
    if(cap_locked) d |= 2;      /* caps */
    if(ctrl_down)  d |= 4;      /* ctrl */
    if(alt_down)   d |= 8;      /* mod1 */
    if(meta_down)  d |= 64;     /* mod4 */
    return d;
}
static uint32_t mod_latched(void)
{
    return xkb_st ? xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LATCHED) : 0;
}
static uint32_t mod_locked(void)
{
    return xkb_st ? xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LOCKED)
                  : (cap_locked ? 2u : 0u);
}

/* Pointer focus follows the cursor: a menu has to see the pointer enter and
   move over it before a click can activate an item. Keyboard focus is separate
   and stays on the window the menu belongs to. */
static void ptr_focus(struct rail_surface *s)
{
    /* a popup that was dismissed/unlinked is not a valid pointer target */
    if(pointer_surface && pointer_surface->is_popup &&
       (!pointer_surface->mapped || !pointer_surface->popup_parent))
        pointer_surface = NULL;
    if(pointer_surface == s) return;
    /* focus is changing: the old client's cursor (or its "hidden" state) must
       not follow the pointer. Fall back to the built-in arrow until the new
       client calls set_cursor itself. */
    {
        struct rail *cr = s ? s->rail : (pointer_surface ? pointer_surface->rail : NULL);
        if(cr){ cr->cursor = NULL; cr->cursor_set = false; }
    }
    if(pointer_surface && pointer_surface->resource){
        FOR_PTR(surf_client(pointer_surface), p){
            wl_pointer_send_leave(p, serial(), pointer_surface->resource);
            ptr_frame(p);
        }
    }
    pointer_surface = s;
    if(s && s->resource && s->rail && s->rail->output){
        struct rail_output *o = s->rail->output;
        /* the pointer enters with coords in the surface's own space, 24.8 fixed */
        FOR_PTR(surf_client(s), p){
            wl_pointer_send_enter(p, serial(), s->resource,
                                  wl_fixed_from_int(o->px - s->cx - rail_inset_x(s)),
                                  wl_fixed_from_int(o->py - s->cy - rail_inset_y(s)));
            ptr_frame(p);
        }
    }
}

void rail_seat_set_focus(struct rail_surface *s)
{
    /* a popup takes the pointer but never the keyboard: moving keyboard focus
       off the window makes GTK close its own menus */
    if(s && s->is_popup){ ptr_focus(s); return; }
    if(focused_surface == s){ ptr_focus(s); return; }
    /* leave goes to the OLD surface's own client keyboard */
    if(focused_surface && focused_surface->resource){
        struct wl_client *ocl = surf_client(focused_surface);
        FOR_KB(ocl, k) wl_keyboard_send_leave(k, serial(), focused_surface->resource);
    }
    /* the selection goes out immediately *before* keyboard focus: the offer is
       only valid while the client is focused, and clients that take focus just
       to read the clipboard (wl-paste maps a throwaway surface for this) pick
       a mime type from it as soon as they are told about it. */
    focused_surface = s;
    if(s && s->rail){ rail_selection_broadcast(s->rail); rail_primary_broadcast(s->rail); }
    /* enter goes to the NEW surface's own client keyboard */
    if(s && s->resource){
        struct wl_client *ncl = surf_client(s);
        FOR_KB(ncl, k){
            struct wl_array keys; wl_array_init(&keys);
            wl_keyboard_send_enter(k, serial(), s->resource, &keys);
            wl_array_release(&keys);
            /* freshly focused client needs current modifier state */
            wl_keyboard_send_modifiers(k, serial(), mod_depressed(), mod_latched(),
                                       mod_locked(), (uint32_t)layout_group);
        }
    }
    /* without wl_pointer.enter a client never accepts motion/button/axis */
    ptr_focus(s);
}

/* ---------- evdev ---------- */
static void input_btn(struct rail *r, struct input_event *e);

static void send_mods(void)
{
    FOR_KB(surf_client(focused_surface), k)
        wl_keyboard_send_modifiers(k, serial(), mod_depressed(), mod_latched(),
                                   mod_locked(), (uint32_t)layout_group);
}

/* next keyboard layout: Super+Space, or "layout-next" on the control fifo */
static void layout_next(void)
{
    keymap_init();
    layout_group = (layout_group + 1) % n_layouts;
    /* keep our xkb state's layout in step with what we publish */
    xkb_set_group((xkb_layout_index_t)layout_group);
    layout_publish();
    send_mods(); /* focused client switches layout now */
}

/* control fifo $XDG_RUNTIME_DIR/railwl/ctl: one command per line, written by
   our own utilities (railbar asks for the next layout when its indicator is
   clicked). the directory is 0700, so only this user can write to it. */
static int ctl_fd = -1, ctl_keep = -1;
static char ctl_buf[128];
static size_t ctl_len;

static int ctl_cb(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    char tmp[64];
    ssize_t n;
    while((n = read(fd, tmp, sizeof tmp)) > 0){
        for(ssize_t i = 0; i < n; i++){
            if(tmp[i] == '\n'){
                ctl_buf[ctl_len] = 0; ctl_len = 0;
                if(!strcmp(ctl_buf, "layout-next")) layout_next();
            } else if(ctl_len < sizeof ctl_buf - 1) ctl_buf[ctl_len++] = tmp[i];
        }
    }
    return 0;
}

static void ctl_init(struct rail *r)
{
    const char *rt = getenv("XDG_RUNTIME_DIR");
    char path[300];
    snprintf(path, sizeof path, "%s/railwl/ctl", rt ? rt : "/tmp");
    unlink(path);                               /* stale fifo from a crashed run */
    if(mkfifo(path, 0600) < 0) return;
    ctl_fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(ctl_fd < 0) return;
    /* hold a write end open so the read end never sees EOF between writers */
    ctl_keep = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    wl_event_loop_add_fd(r->loop, ctl_fd, WL_EVENT_READABLE, ctl_cb, NULL);
}

struct rail_sel rail_sel;

static void sel_end(struct rail *r, bool take)
{
    int x = rail_sel.x0 < rail_sel.x1 ? rail_sel.x0 : rail_sel.x1;
    int y = rail_sel.y0 < rail_sel.y1 ? rail_sel.y0 : rail_sel.y1;
    int w = abs(rail_sel.x1 - rail_sel.x0) + 1, h = abs(rail_sel.y1 - rail_sel.y0) + 1;
    bool ok = take && rail_sel.dragging && w > 2 && h > 2;
    rail_sel.active = rail_sel.dragging = false;
    rail_output_frame(r);                 /* redraw without the overlay first */
    if(ok){
        char cmd[320];
        snprintf(cmd, sizeof cmd, "mkdir -p ~/Pictures && shot -g %d,%d,%dx%d "
                 "~/Pictures/shot-$(date +%%Y%%m%%d-%%H%%M%%S).png", x, y, w, h);
        rail_spawn(cmd);
    }
}

static void leds_sync(bool force);

static void input_key(struct rail *r, struct input_event *e)
{
    uint16_t code = e->code;
    bool down = e->value != 0;

    /* value 2 is the kernel's own autorepeat.  Clients repeat held keys
       themselves from wl_keyboard.repeat_info, so forwarding these as fresh
       presses gives two repeat engines and duplicates every held key. */
    if(e->value == 2) return;

    if(code >= BTN_LEFT && code <= BTN_TASK){ input_btn(r, e); return; }

    /* feed every key to xkb so its modifier/level state stays real, and
       remember whether it changed what clients must be told (NumLock,
       ScrollLock, AltGr...: they only learn modifiers from wl_keyboard.modifiers) */
    bool mods_changed = false;
    if(xkb_st && (xkb_keycode_t)(code + 8) <= km_max_kc){
        uint32_t d0 = mod_depressed(), l0 = mod_latched(), k0 = mod_locked();
        xkb_state_update_key(xkb_st, (xkb_keycode_t)(code + 8),
                             down ? XKB_KEY_DOWN : XKB_KEY_UP);
        mods_changed = d0 != mod_depressed() || l0 != mod_latched() || k0 != mod_locked();
        if(k0 != mod_locked()) leds_sync(false);   /* keyboard LEDs follow the locks */
    }

    if(code == KEY_LEFTMETA || code == KEY_RIGHTMETA){ meta_down = down; send_mods(); return; }
    if(code == KEY_LEFTSHIFT || code == KEY_RIGHTSHIFT){ shift_down = down; send_mods(); return; }
    if(code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL){ ctrl_down = down; send_mods(); return; }
    if(code == KEY_LEFTALT || code == KEY_RIGHTALT){ alt_down = down; send_mods(); return; }
    if(code == KEY_CAPSLOCK && down){ cap_locked = !cap_locked; send_mods(); return; }

    /* region selection owns the keyboard: Esc cancels, nothing reaches clients */
    if(rail_sel.active){
        if(down && code == KEY_ESC) sel_end(r, false);
        return;
    }

    /* alt+shift+e = exit compositor */
    if(down && alt_down && shift_down && code == KEY_E){
        r->quit = true; /* main loop checks this after the current dispatch returns */
        return;
    }

    /* ctrl+alt+F1..F12: switch virtual terminal */
    if(ctrl_down && alt_down){
        int vt = 0;
        if(code >= KEY_F1 && code <= KEY_F10) vt = code - KEY_F1 + 1;
        else if(code == KEY_F11) vt = 11;
        else if(code == KEY_F12) vt = 12;
        if(vt){ if(down) rail_vt_switch(vt); return; }
    }

    /* Print = screenshot -> ~/Pictures/shot-YYYYmmdd-HHMMSS.png */
    if(code == KEY_SYSRQ){
        if(down) rail_spawn("mkdir -p ~/Pictures && shot -c ~/Pictures/shot-$(date +%Y%m%d-%H%M%S).png");
        return; /* swallow the release too */
    }

    /* compositor bindings (super held) */
    if(down && meta_down){
        if(code == KEY_S && shift_down){
            rail_sel.active = true; rail_sel.dragging = false;
            rail_output_frame(r);
            return;
        }
        if(code == KEY_S){ rail_spawn("mkdir -p ~/Pictures && shot -c ~/Pictures/shot-$(date +%Y%m%d-%H%M%S).png"); return; }
        if(code == KEY_TAB){ if(shift_down) rail_strip_focus_prev(r); else rail_strip_focus_next(r); return; }
        if(code >= KEY_1 && code <= KEY_9){ rail_ws_switch(r, code - KEY_1); return; }
        if(code == KEY_0){ rail_ws_switch(r, 9); return; }
        if(code == KEY_ENTER){ rail_spawn("term"); return; }
        if(code == KEY_Q){ rail_close_focused(r); return; }
        if(code == KEY_D){ rail_spawn_util("raillauncher"); return; }
        if(code == KEY_SPACE){ layout_next(); return; }
    }
    if(!down && meta_down && (code == KEY_SPACE || code == KEY_D || code == KEY_S)) return; /* swallow its release */

    /* alt+left / alt+right: move left/right through the strip */
    if(alt_down && (code == KEY_LEFT || code == KEY_RIGHT)){
        if(down){
            if(code == KEY_LEFT) rail_strip_focus_prev(r);
            else rail_strip_focus_next(r);
        }
        return; /* swallow the matching key release too */
    }

    /* alt+t = open a terminal */
    if(alt_down && !meta_down && !shift_down && !ctrl_down && code == KEY_T){
        if(down) rail_spawn("term");
        return; /* swallow the matching key release too */
    }

    /* forward to the focused client's OWN keyboard.
       wl_keyboard.key carries the raw evdev code.  Clients add the 8 offset
       themselves when they feed it to their xkb keymap; sending evdev + 8
       here makes every client (term, railfm, the utilities) resolve a key
       that is 8 positions away - Enter arrives as 'j'. */
    FOR_KB(surf_client(focused_surface), k)
        wl_keyboard_send_key(k, serial(), e->time.tv_sec*1000 + e->time.tv_usec/1000,
                             code, down ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
    if(mods_changed) send_mods();   /* e.g. NumLock toggled: tell the client */
}

static void input_rel(struct rail *r, struct input_event *e)
{
    struct rail_output *o = r->output;
    if(!o) return;
    int dx=0, dy=0;
    switch(e->code){
    case REL_X: dx = e->value; break;
    case REL_Y: dy = e->value; break;
    case REL_WHEEL:
    case REL_HWHEEL:
        if(meta_down && e->code == REL_WHEEL){
            if(e->value > 0) rail_strip_focus_prev(r);
            else if(e->value < 0) rail_strip_focus_next(r);
        } else {
            uint32_t axis = (e->code == REL_HWHEEL)
                ? WL_POINTER_AXIS_HORIZONTAL_SCROLL : WL_POINTER_AXIS_VERTICAL_SCROLL;
            /* wheel up (value > 0) = scroll up = negative axis value */
            wl_fixed_t v = wl_fixed_from_int(e->value > 0 ? -10 : 10);
            FOR_PTR(surf_client(pointer_surface), p){
                if(wl_resource_get_version(p) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION)
                    wl_pointer_send_axis_source(p, WL_POINTER_AXIS_SOURCE_WHEEL);
                if(wl_resource_get_version(p) >= WL_POINTER_AXIS_DISCRETE_SINCE_VERSION)
                    wl_pointer_send_axis_discrete(p, axis, e->value > 0 ? -1 : 1);
                wl_pointer_send_axis(p, e->time.tv_sec*1000 + e->time.tv_usec/1000, axis, v);
                ptr_frame(p);
            }
        }
        return;
    default: return;
    }
    o->px += dx; o->py += dy;
    if(o->px < 0) o->px = 0;
    if(o->py < 0) o->py = 0;
    if(o->px >= o->w) o->px = o->w-1;
    if(o->py >= o->h) o->py = o->h-1;
    if(rail_sel.active){
        if(rail_sel.dragging){ rail_sel.x1 = o->px; rail_sel.y1 = o->py; }
        rail_output_frame(r);
        return;
    }
    /* interactive drag overrides normal forwarding */
    if(r->dragging){
        rail_drag_motion(r, o->px, o->py);
        return;
    }
    /* pointer focus tracks whatever is under the cursor: popup or tile */
    { struct rail_surface *hv = rail_pick_surface(r, o->px, o->py);
      if(hv) ptr_focus(hv); }   /* the bar gets pointer focus too, or hover/click/wheel never reach railbar */
    if(pointer_surface)
    FOR_PTR(surf_client(pointer_surface), p)
    { /* tile content is inset by the frame, shifted by the client's geometry origin */
      wl_pointer_send_motion(p, e->time.tv_sec*1000 + e->time.tv_usec/1000,
                             wl_fixed_from_int(o->px - pointer_surface->cx - rail_inset_x(pointer_surface)),
                             wl_fixed_from_int(o->py - pointer_surface->cy - rail_inset_y(pointer_surface)));
      ptr_frame(p); }
    rail_output_frame(r);
}

/* leave + enter on the focused surface: tells a client that asked for an
   interactive move/resize we refused that the grab is over */
void rail_pointer_reenter(struct rail_surface *s)
{
    if(!s || s != pointer_surface || !s->resource || !s->rail || !s->rail->output) return;
    struct rail_output *o = s->rail->output;
    FOR_PTR(surf_client(s), p){
        wl_pointer_send_leave(p, serial(), s->resource);
        ptr_frame(p);
        wl_pointer_send_enter(p, serial(), s->resource,
                              wl_fixed_from_int(o->px - s->cx - rail_inset_x(s)),
                              wl_fixed_from_int(o->py - s->cy - rail_inset_y(s)));
        ptr_frame(p);
    }
}

/* which resize edge the cursor sits in, for a surface; 0 = none, 1 = move-ish interior */
static int edge_at(struct rail_surface *s, int px, int py, int grip)
{
    if(!s) return 0;
    int x = s->cx, y = s->cy, w = s->cw, h = s->ch;
    bool in = px >= x && px < x+w && py >= y && py < y+h;
    if(!in) return 0;
    int e = 0;
    if(px >= x+w-grip) e |= 2;              /* right */
    else if(px < x+grip) e |= 16;           /* left */
    if(py >= y+h-grip) e |= 8;              /* bottom */
    else if(py < y+grip) e |= 32;           /* top */
    return e; /* 0 = interior (move), nonzero = resize edge */
}

static void input_btn(struct rail *r, struct input_event *e)
{
    struct rail_output *o = r->output;
    bool pressed = e->value != 0;
    /* wl_pointer.button state is only ever 0 (released) or 1 (pressed), for every button */
    uint32_t b = pressed ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED;

    if(rail_sel.active && o){
        if(e->code == BTN_LEFT && pressed){
            rail_sel.dragging = true;
            rail_sel.x0 = rail_sel.x1 = o->px; rail_sel.y0 = rail_sel.y1 = o->py;
            rail_output_frame(r);
        } else if(e->code == BTN_LEFT) sel_end(r, true);
        else if(pressed) sel_end(r, false);   /* any other button cancels */
        return;
    }

    if(e->code == BTN_LEFT && o){
        if(pressed){
            /* focus-follows-click: target whatever is under the cursor */
            rail_click_focus(r, o->px, o->py);
            struct rail_surface *t = rail_pick_surface(r, o->px, o->py);
            /* dismiss popups unless the click landed inside one */
            if(!(t && t->is_popup)) rail_dismiss_popups(r);
            t = rail_pick_surface(r, o->px, o->py); /* re-resolve after dismiss */
            if(t && !t->is_bar){
                int grip = 8;
                int edge = edge_at(t, o->px, o->py, grip);
                /* super held => move; edge grip => resize; interior => the client's */
                /* strip tiles fill the screen, so their "edge" is the screen edge
                   where tabs, toolbars and scrollbars live: only Super may drag
                   a tile. edge-resize is for floating windows. */
                bool no_resize = t->prev != NULL || t->is_popup;
                if(meta_down || (edge && !no_resize)){
                    rail_drag_begin(t, meta_down ? 1 : edge, o->px, o->py);
                    /* swallow the press only if a drag really started; a menu
                       or a strip tile refuses it and the client gets its click */
                    if(r->dragging || meta_down) return;
                }
            }
            /* interior click falls through and is forwarded below */
        } else {
            if(r->dragging){ rail_drag_end(r); return; }
        }
    }
    FOR_PTR(surf_client(pointer_surface), p){
        wl_pointer_send_button(p, serial(), e->time.tv_sec*1000 + e->time.tv_usec/1000, e->code, b);
        ptr_frame(p);
    }
}

int rail_input_fds[64];
int rail_input_nfds = 0;

/* Keyboard LEDs.  With the keyboards grabbed (EVIOCGRAB) nothing else drives
   them, so after every lock change we write EV_LED events to the devices.
   Devices that are not keyboards simply reject the write. */
static void leds_sync(bool force)
{
    static int last[3] = { -1, -1, -1 };
    if(!xkb_st) return;
    int now[3] = {
        xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_NUM,  XKB_STATE_MODS_LOCKED) > 0,
        xkb_state_mod_name_is_active(xkb_st, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_LOCKED) > 0,
        xkb_state_led_name_is_active(xkb_st, XKB_LED_NAME_SCROLL) > 0,
    };
    static const uint16_t code[3] = { LED_NUML, LED_CAPSL, LED_SCROLLL };
    for(int l = 0; l < 3; l++){
        if(!force && last[l] == now[l]) continue;
        last[l] = now[l];
        struct input_event ev[2];
        memset(ev, 0, sizeof ev);
        ev[0].type = EV_LED; ev[0].code = code[l]; ev[0].value = now[l];
        ev[1].type = EV_SYN; ev[1].code = SYN_REPORT;
        for(int i = 0; i < rail_input_nfds; i++)
            (void)!write(rail_input_fds[i], ev, sizeof ev);
    }
}

static int input_fd_cb(int fd, uint32_t mask, void *data)
{
    (void)fd;
    (void)mask;
    struct rail *r = data;
    for(int i=0;i<rail_input_nfds;i++){
        struct input_event ev;
        while(read(rail_input_fds[i], &ev, sizeof ev) == sizeof ev){
            if(!rail_active) continue; /* on another VT: drain and ignore */
            if(ev.type == EV_KEY) input_key(r, &ev);
            else if(ev.type == EV_REL) input_rel(r, &ev);
            else if(ev.type == EV_MSC && ev.code == MSC_SCAN) continue;
            else if(ev.type == EV_ABS){
                struct rail_output *o = r->output;
                if(o){
                    if(ev.code == ABS_X) o->px = ev.value;
                    if(ev.code == ABS_Y) o->py = ev.value;
                    rail_output_frame(r);
                }
            } else if(ev.type == EV_KEY && ev.code >= BTN_LEFT && ev.code <= BTN_TASK){
                input_btn(r, &ev);
            }
        }
    }
    return 0;
}

int rail_input_init(struct rail *r)
{
    keymap_init(); /* also writes the layout state file before any client starts */
    ctl_init(r);
    DIR *d = opendir("/dev/input");
    if(!d){ perror("opendir /dev/input"); return -1; }
    struct dirent *e;
    while((e = readdir(d)) && rail_input_nfds < 64){
        if(strncmp(e->d_name, "event", 5)) continue;
        char path[256];
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = rail_lseat_open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);   /* RW: LED writes */
        if(fd < 0) fd = rail_lseat_open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if(fd < 0) continue;
        /* grab: keys/mouse go only to railwl, not to the console behind it */
        if(rail_vt_ok()) ioctl(fd, EVIOCGRAB, 1);
        wl_event_loop_add_fd(r->loop, fd, WL_EVENT_READABLE, input_fd_cb, r);
        rail_input_fds[rail_input_nfds++] = fd;
    }
    closedir(d);
    leds_sync(true);   /* NumLock starts on: light the LED to match */
    fprintf(stderr, "railwl: %d input devices\n", rail_input_nfds);
    return 0;
}

/* grab or release every input device (released while another VT is shown) */
void rail_input_grab(bool on)
{
    for(int i=0;i<rail_input_nfds;i++) ioctl(rail_input_fds[i], EVIOCGRAB, on ? 1 : 0);
    if(on) leds_sync(true);   /* back from another VT: the LEDs may have been reset */
}

/* forget held modifiers: their release happened while we were not looking */
void rail_input_reset(void)
{
    meta_down = shift_down = ctrl_down = alt_down = false;
    /* drop any modifier xkb still thinks is held (the release happened elsewhere) */
    if(xkb_st)  /* clear held/latched, keep locked (NumLock, CapsLock) */
        xkb_state_update_mask(xkb_st, 0, 0,
            xkb_state_serialize_mods(xkb_st, XKB_STATE_MODS_LOCKED), 0, 0,
            (xkb_layout_index_t)layout_group);
}

/* the surface that owns focus is going away: forget it. Leaving a dangling
   pointer is worse than it looks - the freed block is usually handed straight
   back out to the next surface, which then compares equal to the stale focus
   and quietly never receives enter/leave or the clipboard. */
void rail_focus_forget(struct rail_surface *s)
{
    if(focused_surface == s) focused_surface = NULL;
    if(pointer_surface == s) pointer_surface = NULL;
}

/* see rail_impl_check(): seat/pointer/keyboard live here */
void rail_input_impl_check(void)
{
    rail_check_impl(&wl_seat_interface, &seat_impl);
    rail_check_impl(&wl_pointer_interface, &ptr_impl);
    rail_check_impl(&wl_keyboard_interface, &kb_impl);
}
