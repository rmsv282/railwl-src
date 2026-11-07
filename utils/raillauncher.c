/* raillauncher: app launcher. type to filter, enter to run, esc to quit. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <sys/wait.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "railclient.h"
#include "xdg-shell-client-protocol.h"

#define W 520
#define H 320
#define MAX 512
#define AMBER 0xFFBF00u

struct app { char name[128]; char exec[256]; };

static struct app apps[MAX];
static int napps;
static char query[128];
static int sel;

/* Desktop-file IDs already handled. A file in an earlier (higher priority)
 * directory shadows the same name later on, even if it is hidden. */
#define MAXSEEN 2048
static char seen_ids[MAXSEEN][96];
static int nseen;

static int seen_id(const char *id)
{
    for(int i=0; i<nseen; i++) if(!strcmp(seen_ids[i], id)) return 1;
    if(nseen < MAXSEEN) snprintf(seen_ids[nseen++], sizeof seen_ids[0], "%s", id);
    return 0;
}

/* read one value after "Key=", trimmed of newline / CR */
static void get_val(const char *line, size_t klen, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s", line + klen);
    out[strcspn(out, "\r\n")] = 0;
}

static void load_file(const char *dirpath, const char *fname)
{
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dirpath, fname);
    FILE *f = fopen(p, "r");
    if(!f) return;
    char line[1024], name[128]="", exec[256]="", type[32]="Application";
    int hidden=0, noDisplay=0, in_entry=0;
    while(fgets(line, sizeof line, f)){
        if(line[0] == '['){ in_entry = !strncmp(line, "[Desktop Entry]", 15); continue; }
        if(!in_entry) continue;   /* ignore [Desktop Action ...] groups */
        if(!strncmp(line, "Name=", 5)) get_val(line, 5, name, sizeof name);
        else if(!strncmp(line, "Exec=", 5)) get_val(line, 5, exec, sizeof exec);
        else if(!strncmp(line, "Type=", 5)) get_val(line, 5, type, sizeof type);
        else if(!strncmp(line, "Hidden=true", 11)) hidden = 1;
        else if(!strncmp(line, "NoDisplay=true", 14)) noDisplay = 1;
    }
    fclose(f);
    if(hidden || noDisplay || !name[0] || !exec[0] || strcmp(type, "Application")) return;
    /* strip field codes %f %u etc */
    for(char *q = exec; *q; q++) if(*q == '%') { *q = 0; break; }
    for(size_t n = strlen(exec); n > 0 && exec[n-1] == ' '; ) exec[--n] = 0;
    if(!exec[0]) return;
    snprintf(apps[napps].name, sizeof apps[napps].name, "%s", name);
    snprintf(apps[napps].exec, sizeof apps[napps].exec, "%s", exec);
    napps++;
}

static void scan_dir(const char *dirpath)
{
    DIR *dir = opendir(dirpath);
    if(!dir) return;
    struct dirent *e;
    while((e = readdir(dir)) && napps < MAX){
        size_t l = strlen(e->d_name);
        if(l < 9 || strcmp(e->d_name + l - 8, ".desktop")) continue;
        if(seen_id(e->d_name)) continue;
        load_file(dirpath, e->d_name);
    }
    closedir(dir);
}

static int cmp_app(const void *a, const void *b)
{
    return strcasecmp(((const struct app *)a)->name, ((const struct app *)b)->name);
}

/* Scan <base>/applications for one XDG data dir, skipping repeats. */
static char scanned[32][512];
static int nscanned;

static void scan_base(const char *base)
{
    if(!base || !*base) return;
    char path[512]; snprintf(path, sizeof path, "%s/applications", base);
    for(int i=0; i<nscanned; i++) if(!strcmp(scanned[i], path)) return;
    if(nscanned < 32) snprintf(scanned[nscanned++], sizeof scanned[0], "%s", path);
    scan_dir(path);
}

/* Search order follows the XDG basedir spec: $XDG_DATA_HOME (default
 * ~/.local/share) first, then $XDG_DATA_DIRS (default /usr/local/share:
 * /usr/share). The two system prefixes are also always tried, since a
 * compositor-launched process often has no XDG_* variables set. */
static void load_apps(void)
{
    char home_share[512] = "";
    const char *dh = getenv("XDG_DATA_HOME");
    if(dh && *dh) snprintf(home_share, sizeof home_share, "%s", dh);
    else {
        const char *home = getenv("HOME");
        if(!home || !*home){ struct passwd *pw = getpwuid(getuid()); home = pw ? pw->pw_dir : NULL; }
        if(home) snprintf(home_share, sizeof home_share, "%s/.local/share", home);
    }
    scan_base(home_share);

    const char *dd = getenv("XDG_DATA_DIRS");
    char dirs[1024];
    snprintf(dirs, sizeof dirs, "%s", (dd && *dd) ? dd : "/usr/local/share:/usr/share");
    for(char *save = NULL, *tok = strtok_r(dirs, ":", &save); tok; tok = strtok_r(NULL, ":", &save))
        scan_base(tok);

    scan_base("/usr/local/share");
    scan_base("/usr/share");

    qsort(apps, (size_t)napps, sizeof apps[0], cmp_app);
}

static int match(const struct app *a)
{
    if(!query[0]) return 1;
    return strcasestr(a->name, query) != NULL;
}

static void launch(const struct app *a)
{
    pid_t pid = fork();
    if(pid == 0){
        setsid();
        execl("/bin/sh", "/bin/sh", "-c", a->exec, (char*)NULL);
        _exit(1);
    }
}

static void draw(struct rc_client *c)
{
    struct rc_window *w = c->window;
    if(!w->configured || !rc_pixels(w)) return;
    int fw = rc_fb_w(w), fh = rc_fb_h(w);
    if(fw <= 0 || fh <= 0) return;
    uint32_t *px = rc_pixels(w);
    rc_clear(px, fw, fh, 0xFF0A1428u);   /* app background, same as railfm */

    /* amber border */
    rc_fill(px, fw, fh, 0, 0, w->w, 2, AMBER);
    rc_fill(px, fw, fh, 0, w->h-2, w->w, 2, AMBER);
    rc_fill(px, fw, fh, 0, 0, 2, w->h, AMBER);
    rc_fill(px, fw, fh, w->w-2, 0, 2, w->h, AMBER);

    /* query line */
    char buf[160]; snprintf(buf, sizeof buf, "> %s_", query);
    rc_text(px, fw, fh, 12, 12, buf, 0xFFFFFFu);
    rc_fill(px, fw, fh, 8, 26, w->w-16, 1, 0x444444u);

    /* results */
    int y = 34, shown = 0;
    for(int i=0; i<napps && shown < 12; i++){
        if(!match(&apps[i])) continue;
        if(shown == sel) rc_fill(px, fw, fh, 8, y-2, w->w-16, 22, 0x2A2A2Au);
        rc_text(px, fw, fh, 14, y, apps[i].name,
                shown == sel ? AMBER : 0xCCCCCCu);
        y += 22; shown++;
    }
    rc_present(w);
}

static void on_key(struct rc_client *c, uint32_t sym, bool pressed, uint32_t mod)
{
    (void)mod;
    if(!pressed) return;
    size_t l = strlen(query);
    if(sym == XKB_KEY_Escape){ c->running = false; return; }
    if(sym == XKB_KEY_Return){
        int n = 0;
        for(int i=0; i<napps; i++) if(match(&apps[i])){ if(n == sel){ launch(&apps[i]); c->running = false; return; } n++; }
        return;
    }
    if(sym == XKB_KEY_Up){ if(sel > 0) sel--; draw(c); return; }
    if(sym == XKB_KEY_Down){
        int cnt = 0; for(int i=0;i<napps;i++) if(match(&apps[i])) cnt++;
        if(sel < cnt-1) sel++;
        draw(c); return;
    }
    if(sym == XKB_KEY_BackSpace && l > 0){ query[l-1] = 0; sel = 0; draw(c); return; }
    if(sym >= 32 && sym < 127 && l < sizeof query - 1){
        query[l] = (char)sym; query[l+1] = 0; sel = 0; draw(c);
    }
}

int main(void)
{
    load_apps();
    struct rc_client c;
    if(!rc_connect(&c, "raillauncher")) return 1;
    c.on_key = on_key;
    if(!rc_window_create(&c, W, H)) return 1;
    draw(&c);
    return rc_run(&c);
}
