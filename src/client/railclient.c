#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <time.h>
#include <poll.h>
#include <xkbcommon/xkbcommon.h>
#include "railclient.h"
#include "xdg-shell-client-protocol.h"

static struct xkb_context *xkb_ctx;

/* forward decls (defined near keyboard handlers) */
static uint64_t now_ms(void);
static bool repeatable_key(uint32_t key);
static struct xkb_keymap *g_keymap;
static struct xkb_state *g_state;

/* ---------- shm helpers ---------- */
static int create_shm_file(size_t size)
{
    char name[64];
    snprintf(name, sizeof name, "/railwl-shm-%d", getpid());
    int fd = memfd_create(name, 0);
    if(fd < 0) return -1;
    ftruncate(fd, size);
    return fd;
}

static struct wl_buffer *shm_alloc(struct rc_window *w, int idx, int width, int height)
{
    int stride = width * 4;
    size_t size = (size_t)stride * height;
    int fd = create_shm_file(size);
    if(fd < 0) return NULL;
    void *data = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
    if(data == MAP_FAILED){ close(fd); return NULL; }
    struct wl_shm_pool *pool = wl_shm_create_pool(w->client->shm, fd, size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    w->fb[idx].buf = buf;
    w->fb[idx].data = data;
    w->fb[idx].size = size;
    w->fb_w = width; w->fb_h = height;
    return buf;
}

/* ---------- xdg events ---------- */
static void wm_base_ping(void *d, struct xdg_wm_base *b, uint32_t s)
{ (void)d; xdg_wm_base_pong(b, s); }
static const struct xdg_wm_base_listener wm_base_listener = { .ping = wm_base_ping };

static void toplevel_configure(void *d, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *st)
{ (void)t;(void)st; struct rc_window *win = d;
  if(w>0&&h>0){ win->w = w; win->h = h; } }
static void toplevel_close(void *d, struct xdg_toplevel *t)
{ (void)t; struct rc_window *win = d; win->closed = true; win->client->running = false; }
static void toplevel_config_bounds(void *d, struct xdg_toplevel *t, int32_t w, int32_t h)
{ (void)d;(void)t;(void)w;(void)h; }
static void toplevel_wm_state(void *d, struct xdg_toplevel *t, struct wl_array *s)
{ (void)d;(void)t;(void)s; }
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure, .close = toplevel_close,
    .configure_bounds = toplevel_config_bounds, .wm_capabilities = toplevel_wm_state,
};

static void xdg_surface_configure(void *d, struct xdg_surface *s, uint32_t serial)
{
    struct rc_window *win = d;
    xdg_surface_ack_configure(s, serial);
    win->configured = true;
    /* match buffer size to what the compositor configured (e.g. bar 800->1360);
       buffers are idle here: released right after the last texture upload */
    if(win->w > 0 && win->h > 0 && (win->fb_w != win->w || win->fb_h != win->h)){
        for(int i = 0; i < 2; i++){
            if(win->fb[i].data) munmap(win->fb[i].data, win->fb[i].size);
            if(win->fb[i].buf) wl_buffer_destroy(win->fb[i].buf);
            win->fb[i].buf = NULL; win->fb[i].data = NULL; win->fb[i].size = 0;
            shm_alloc(win, i, win->w, win->h);
        }
        win->cur = 0;
    }
    /* attach current buffer to become visible */
    if(win->fb[win->cur].buf)
        wl_surface_attach(win->surface, win->fb[win->cur].buf, 0, 0);
    wl_surface_commit(win->surface);
}
static const struct xdg_surface_listener xdg_surface_listener = { .configure = xdg_surface_configure };

/* ---------- pointer/keyboard ---------- */
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf, wl_fixed_t x, wl_fixed_t y)
{ (void)d;(void)p;(void)s;(void)sf;(void)x;(void)y; }
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *sf)
{ (void)d;(void)p;(void)s;(void)sf; }
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y)
{
    (void)p;(void)t;
    struct rc_client *c = d;
    if(c->on_ptr_motion) c->on_ptr_motion(c, wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t s, uint32_t t, uint32_t b, uint32_t st)
{
    (void)p;(void)s;(void)t;
    struct rc_client *c = d;
    if(c->on_ptr_button) c->on_ptr_button(c, b, st == WL_POINTER_BUTTON_STATE_PRESSED,
                                          c->window ? c->window->w/2.0 : 0, 0);
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v)
{
    (void)p;(void)t;(void)a;
    struct rc_client *c = d;
    if(c->on_ptr_axis) c->on_ptr_axis(c, 0, wl_fixed_to_double(v));
}
static void ptr_frame(void *d, struct wl_pointer *p){ (void)d;(void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s){ (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a){ (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v){ (void)d;(void)p;(void)a;(void)v; }
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
};

static void modmask_sync(struct rc_client *c)
{
    c->modmask = 0;
    if(!g_state) return;
    if(xkb_state_mod_name_is_active(g_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0) c->modmask |= 1;
    if(xkb_state_mod_name_is_active(g_state, XKB_MOD_NAME_CAPS, XKB_STATE_MODS_EFFECTIVE) > 0) c->modmask |= 2;
    if(xkb_state_mod_name_is_active(g_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0) c->modmask |= 4;
    if(xkb_state_mod_name_is_active(g_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0) c->modmask |= 8;
    if(xkb_state_mod_name_is_active(g_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0) c->modmask |= 64;
}

/* Layout names come from xkeyboard-config, e.g. "English (US)" or plain
 * "Ukrainian".  The parenthesised form carries the code; the plain ones need
 * a small table (taking the first two letters gives "UK" for Ukrainian). */
static const struct { const char *name, *code; } layout_alias[] = {
    { "ukrainian", "UA" }, { "russian", "RU" },   { "german", "DE" },
    { "french", "FR" },    { "spanish", "ES" },   { "italian", "IT" },
    { "polish", "PL" },    { "portuguese", "PT" },{ "turkish", "TR" },
    { "czech", "CZ" },     { "greek", "GR" },     { "swedish", "SE" },
    { "dutch", "NL" },     { "danish", "DK" },    { "norwegian", "NO" },
    { "finnish", "FI" },   { "hungarian", "HU" }, { "romanian", "RO" },
    { "bulgarian", "BG" }, { "serbian", "RS" },   { "croatian", "HR" },
    { "slovak", "SK" },    { "slovenian", "SI" }, { "lithuanian", "LT" },
    { "latvian", "LV" },   { "estonian", "EE" },  { "belarusian", "BY" },
    { "english", "US" },
};

static uint32_t g_layout_group;
static bool g_layout_group_valid;

/* "English (US)" -> "US"; "Ukrainian" -> "UA" from the table above. */
static void layout_sync(struct rc_client *c, uint32_t group)
{
    if(g_layout_group_valid && group == g_layout_group) return;
    g_layout_group = group;
    g_layout_group_valid = true;

    char code[8] = "US";
    const char *name = g_keymap ? xkb_keymap_layout_get_name(g_keymap, (xkb_layout_index_t)group) : NULL;
    if(name && *name){
        const char *o = strchr(name, '(');
        if(o && o[1] && o[2] && o[3] == ')'){
            code[0] = o[1]; code[1] = o[2]; code[2] = 0;
        } else {
            bool found = false;
            for(size_t i = 0; i < sizeof layout_alias / sizeof layout_alias[0]; i++){
                if(strcasestr(name, layout_alias[i].name)){
                    snprintf(code, sizeof code, "%s", layout_alias[i].code);
                    found = true;
                    break;
                }
            }
            if(!found){
                int n = 0;
                for(const char *p = name; *p && n < 2; p++){
                    char ch = *p;
                    if(ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
                    if(ch >= 'A' && ch <= 'Z') code[n++] = ch;
                }
                code[n] = 0;
                if(n == 0) snprintf(code, sizeof code, "%s", "US");
            }
        }
    }
    if(strcmp(c->layout, code) != 0)
        snprintf(c->layout, sizeof c->layout, "%s", code);
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t size)
{
    struct rc_client *c = d;
    (void)k;
    if(fmt != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1){ close(fd); return; }
    char *map = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if(map == MAP_FAILED) return;
    struct xkb_keymap *nk = xkb_keymap_new_from_string(xkb_ctx, map,
        XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if(nk){
        if(g_state) xkb_state_unref(g_state);
        if(g_keymap) xkb_keymap_unref(g_keymap);
        g_keymap = nk;
        g_state = xkb_state_new(nk);
        g_layout_group_valid = false;
        modmask_sync(c);
        layout_sync(c, 0);
    }
}
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf, struct wl_array *ks)
{ (void)d;(void)k;(void)s;(void)sf;(void)ks; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *sf)
{ (void)d;(void)k;(void)s;(void)sf; }
static void kb_key(void *d, struct wl_keyboard *k, uint32_t s, uint32_t t, uint32_t key, uint32_t st)
{
    (void)k;(void)s;(void)t;
    struct rc_client *c = d;
    bool pressed = st == WL_KEYBOARD_KEY_STATE_PRESSED;
    uint32_t sym = 0;
    if(g_state){
        xkb_state_update_key(g_state, key + 8, pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
        sym = xkb_state_key_get_one_sym(g_state, key + 8);
        modmask_sync(c);
    }
    if(c->on_key) c->on_key(c, sym, pressed, c->modmask);
    /* repeat: start on press of a repeating key, stop on release */
    if(repeatable_key(key)){
        if(pressed){
            c->repeat_sym = sym; c->repeat_key = key; c->repeat_mod = c->modmask;
            c->repeat_pressed = true;
            c->repeat_deadline_ms = now_ms() + (uint64_t)c->repeat_delay;
        } else if((uint32_t)c->repeat_key == key){
            c->repeat_pressed = false;
            c->repeat_deadline_ms = 0;
        }
    }
}
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep, uint32_t lat, uint32_t lock, uint32_t grp)
{
    (void)k;(void)s;
    struct rc_client *c = d;
    /* railwl does not forward modifier keys as key events: the modifier
       state arrives only here, so this handler is not optional. */
    if(g_state)
        xkb_state_update_mask(g_state, dep, lat, lock, 0, 0, grp);
    modmask_sync(c);
    layout_sync(c, grp);
}
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)k; struct rc_client *c = d; c->repeat_rate = rate; c->repeat_delay = delay > 0 ? delay : 400; }

static uint64_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* keys that repeat when held (letters, digits, edit keys); not modifiers */
static bool repeatable_key(uint32_t key)
{
    switch(key){
    case 1 ... 13: return true;          /* esc, digits, minus, equal */
    case 14: return true;                /* backspace */
    case 15: return true;                /* tab */
    case 16 ... 27: return true;         /* qwertyuiop */
    case 28: return true;                /* enter */
    case 30 ... 41: return true;         /* asdfghjkl */
    case 43: return true;                /* backslash */
    case 44 ... 53: return true;         /* zxcvbnm */
    default: return false;
    }
}
static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = kb_keymap, .enter = kb_enter, .leave = kb_leave,
    .key = kb_key, .modifiers = kb_modifiers, .repeat_info = kb_repeat,
};

/* ---------- registry ---------- */
static void registry_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver)
{
    struct rc_client *c = d;
    if(!strcmp(iface, wl_compositor_interface.name)){
        c->compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if(!strcmp(iface, wl_shm_interface.name)){
        c->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if(!strcmp(iface, xdg_wm_base_interface.name)){
        c->wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 6);
        xdg_wm_base_add_listener(c->wm_base, &wm_base_listener, c);
    } else if(!strcmp(iface, wl_seat_interface.name)){
        c->seat = wl_registry_bind(r, name, &wl_seat_interface, 7);
        c->pointer = wl_seat_get_pointer(c->seat);
        wl_pointer_add_listener(c->pointer, &pointer_listener, c);
        c->keyboard = wl_seat_get_keyboard(c->seat);
        wl_keyboard_add_listener(c->keyboard, &keyboard_listener, c);
    }
    (void)ver;
}
static void registry_global_remove(void *d, struct wl_registry *r, uint32_t name)
{ (void)d;(void)r;(void)name; }
static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

bool rc_connect(struct rc_client *c, const char *app_id)
{
    memset(c, 0, sizeof *c);
    c->app_id = app_id;
    memcpy(c->layout, "US", 3);   /* until the compositor's keymap says otherwise */
    if(!xkb_ctx) xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    c->display = wl_display_connect(NULL);
    if(!c->display){ fprintf(stderr, "%s: cannot connect\n", app_id); return false; }
    c->registry = wl_display_get_registry(c->display);
    wl_registry_add_listener(c->registry, &registry_listener, c);
    wl_display_roundtrip(c->display);
    if(!c->compositor || !c->shm || !c->wm_base){
        fprintf(stderr, "%s: missing globals\n", app_id);
        return false;
    }
    c->running = true;
    return true;
}

bool rc_window_create(struct rc_client *c, int w, int h)
{
    struct rc_window *win = calloc(1, sizeof *win);
    win->client = c;
    win->w = w > 0 ? w : 800;
    win->h = h > 0 ? h : 600;
    win->surface = wl_compositor_create_surface(c->compositor);
    win->xdg_surface = xdg_wm_base_get_xdg_surface(c->wm_base, win->surface);
    xdg_surface_add_listener(win->xdg_surface, &xdg_surface_listener, win);
    win->toplevel = xdg_surface_get_toplevel(win->xdg_surface);
    xdg_toplevel_add_listener(win->toplevel, &toplevel_listener, win);
    xdg_toplevel_set_app_id(win->toplevel, c->app_id);
    xdg_toplevel_set_title(win->toplevel, c->app_id);
    shm_alloc(win, 0, win->w, win->h);
    shm_alloc(win, 1, win->w, win->h);
    c->window = win;
    wl_surface_commit(win->surface);
    wl_display_roundtrip(c->display);
    return true;
}

uint32_t *rc_pixels(struct rc_window *w)
{
    return (uint32_t*)w->fb[w->cur].data;
}

int rc_fb_w(struct rc_window *w){ return w->fb_w; }
int rc_fb_h(struct rc_window *w){ return w->fb_h; }

void rc_present(struct rc_window *w)
{
    wl_surface_attach(w->surface, w->fb[w->cur].buf, 0, 0);
    wl_surface_damage(w->surface, 0, 0, w->w, w->h);
    struct wl_callback *cb = wl_surface_frame(w->surface);
    (void)cb; /* fire-and-forget; bar redraws on timer */
    wl_surface_commit(w->surface);
    w->cur ^= 1;
}

int rc_run(struct rc_client *c)
{
    while(c->running){
        /* always flush queued requests before waiting, else we deadlock
           with commits sitting unflushed in the outgoing buffer */
        wl_display_flush(c->display);

        /* timeout = ms until the next key repeat or tick, or -1 to block */
        int timeout = -1;
        uint64_t now = now_ms();
        if(c->repeat_pressed && c->repeat_rate > 0 && c->repeat_deadline_ms)
            timeout = c->repeat_deadline_ms > now ? (int)(c->repeat_deadline_ms - now) : 0;
        if(c->tick_ms > 0){
            if(!c->tick_deadline_ms) c->tick_deadline_ms = now + (uint64_t)c->tick_ms;
            int tt = c->tick_deadline_ms > now ? (int)(c->tick_deadline_ms - now) : 0;
            if(timeout < 0 || tt < timeout) timeout = tt;
        }
        struct pollfd pfds[2] = {
            { .fd = wl_display_get_fd(c->display), .events = POLLIN },
            { .fd = c->watch_fd > 0 ? c->watch_fd : -1, .events = POLLIN },
        };
        int r = poll(pfds, 2, timeout);
        if(r < 0) break;
        if(r > 0 && (pfds[0].revents & POLLIN)) wl_display_dispatch(c->display);
        if(r > 0 && (pfds[1].revents & POLLIN) && c->on_watch) c->on_watch(c);

        /* fire key repeat if deadline passed */
        if(c->repeat_pressed && c->repeat_rate > 0 && c->repeat_deadline_ms
           && now_ms() >= c->repeat_deadline_ms && c->on_key){
            c->on_key(c, c->repeat_sym, true, c->repeat_mod);
            c->repeat_deadline_ms = now_ms() + (uint64_t)(1000 / c->repeat_rate);
        }
        if(c->tick_ms > 0 && now_ms() >= c->tick_deadline_ms)
            c->tick_deadline_ms = now_ms() + (uint64_t)c->tick_ms;
        if(c->on_redraw) c->on_redraw(c);
    }
    return 0;
}
