/* Crash note for clients.  Keeps the last protocol messages of every client and,
   when a client disappears without a normal shutdown (it crashed, or railwl
   kicked it out with a protocol error), writes the tail to /tmp/railwl-crash.txt.
   Read it with:  cat /tmp/railwl-crash.txt   (about a dozen short lines) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-server.h>
#include "rail.h"

#define RING 512
#define KEEP 12
#define CRASH_FILE "/tmp/railwl-crash.txt"

static struct { struct wl_client *c; char line[72]; } ring[RING];
static unsigned head;

struct tclient {
    struct wl_listener l;
    struct wl_client *c;
    pid_t pid;
    bool clean;            /* last thing it did was a normal toplevel teardown */
    bool err;              /* railwl sent it a protocol error */
    char err_line[100];
    struct tclient *next;
};
static struct tclient *clients;

/* The crash note lives at a fixed, predictable path in /tmp. Open it without
 * following symlinks and with private permissions, so another local user
 * cannot plant a link there and have the compositor overwrite a victim file. */
static FILE *open_crash_file(void)
{
    int fd = open(CRASH_FILE, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
    if(fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if(!f) close(fd);
    return f;
}

static struct tclient *find(struct wl_client *c)
{
    for(struct tclient *t = clients; t; t = t->next) if(t->c == c) return t;
    return NULL;
}

static void logger(void *ud, enum wl_protocol_logger_type dir,
                   const struct wl_protocol_logger_message *m)
{
    (void)ud;
    if(!m->resource) return;
    struct wl_client *c = wl_resource_get_client(m->resource);
    const char *cls = wl_resource_get_class(m->resource);
    const char *name = m->message->name;
    struct tclient *t = find(c);
    bool req = dir == WL_PROTOCOL_LOGGER_REQUEST;

    if(t && req){
        if(!strcmp(cls, "xdg_toplevel") && !strcmp(name, "destroy")) t->clean = true;
        else if(strcmp(name, "destroy") && strcmp(name, "release") && strcmp(name, "sync")) t->clean = false;
    }
    if(t && !req && !strcmp(cls, "wl_display") && !strcmp(name, "error") && m->arguments_count >= 3){
        t->err = true;
        snprintf(t->err_line, sizeof t->err_line, "ERROR code %u: %s",
                 (unsigned)m->arguments[1].u, m->arguments[2].s ? m->arguments[2].s : "");
    }
    /* high-rate or uninteresting messages would flush the ring */
    if(!strcmp(name, "motion") || !strcmp(name, "frame") || !strcmp(name, "done") ||
       !strcmp(name, "release") || !strcmp(name, "axis") || !strcmp(name, "delete_id")) return;
    unsigned i = head++ % RING;
    ring[i].c = c;
    snprintf(ring[i].line, sizeof ring[i].line, "%s %s@%u.%s",
             req ? "rq" : "ev", cls, (unsigned)wl_resource_get_id(m->resource), name);
}

static void on_client_gone(struct wl_listener *l, void *data)
{
    (void)data;
    struct tclient *t = wl_container_of(l, t, l);
    if(!(t->clean && !t->err)){
        FILE *f = open_crash_file();
        if(f){
            fprintf(f, "client pid %d left %s\n", (int)t->pid, t->err ? "after a protocol error" : "abruptly");
            if(t->err) fprintf(f, "%s\n", t->err_line);
            const char *lines[KEEP]; int n = 0;
            for(unsigned k = 1; k <= RING && k <= head && n < KEEP; k++){
                unsigned i = (head - k) % RING;
                if(ring[i].c == t->c) lines[n++] = ring[i].line;
            }
            for(int k = n - 1; k >= 0; k--) fprintf(f, "%s\n", lines[k]);
            fclose(f);
        }
    }
    /* forget this client's ring entries: the next client may be allocated at the
     * same address and must not inherit them */
    for(unsigned i = 0; i < RING; i++)
        if(ring[i].c == t->c) ring[i].c = NULL;
    for(struct tclient **pp = &clients; *pp; pp = &(*pp)->next)
        if(*pp == t){ *pp = t->next; break; }
    free(t);
}

static void on_client_new(struct wl_listener *l, void *data)
{
    (void)l;
    struct wl_client *c = data;
    struct tclient *t = calloc(1, sizeof *t);
    if(!t) return;
    t->c = c;
    uid_t u; gid_t g; pid_t p = 0;
    wl_client_get_credentials(c, &p, &u, &g);
    t->pid = p;
    t->l.notify = on_client_gone;
    wl_client_add_destroy_listener(c, &t->l);
    t->next = clients;
    clients = t;
}

static struct wl_listener new_client_listener = { .notify = on_client_new };

void rail_trace_init(struct wl_display *d)
{
    wl_display_add_client_created_listener(d, &new_client_listener);
    wl_display_add_protocol_logger(d, logger, NULL);
}
