#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <math.h>
#include <wayland-server.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <gbm.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include "rail.h"

#ifndef GL_BGRA_EXT
#define GL_BGRA_EXT 0x80E1
#endif
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D8
#endif

static uint32_t now_ms(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec*1000 + ts.tv_nsec/1000000);
}

/* ================================================================
   GLES2 renderer: solid-color quads (background, borders) and
   textured quads (client surfaces, cursor).
   ================================================================ */

static const char *QUAD_VS =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_rect;\n"     /* x, y, w, h in px, top-left origin */
    "uniform vec2 u_screen;\n"
    "uniform float u_flip;\n"    /* 1 = buffer is bottom-up (dmabuf y_invert) */
    "uniform vec4 u_uv;\n"      /* sampled part of the buffer: x, y, w, h in 0..1, top-down */
    "varying vec2 v_uv;\n"
    "void main(){\n"
    "  vec2 px = u_rect.xy + a_pos * u_rect.zw;\n"
    "  gl_Position = vec4(px.x / u_screen.x * 2.0 - 1.0,\n"
    "                     1.0 - px.y / u_screen.y * 2.0, 0.0, 1.0);\n"
    "  vec2 uv = u_uv.xy + a_pos * u_uv.zw;\n"
    "  v_uv = vec2(uv.x, mix(uv.y, 1.0 - uv.y, u_flip));\n"
    "}\n";

static const char *SOLID_FS =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main(){ gl_FragColor = u_color; }\n";

static const char *TEX_FS =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform float u_opaque;\n"
    "void main(){\n"
    "  vec4 c = texture2D(u_tex, v_uv);\n"
    "  if(u_opaque > 0.5) c.a = 1.0;\n"  /* XRGB buffers: alpha byte is padding */
    "  gl_FragColor = c;\n"
    "}\n";

struct gprog {
    GLuint id;
    GLint a_pos, u_rect, u_screen, u_flip, u_uv, u_color, u_tex, u_opaque;
};
static struct gprog p_solid, p_tex;
static GLuint quad_vbo;
static GLuint cursor_tex;
static int gl_bgra; /* GL_EXT_bgra: upload shm BGRA bytes directly */

/* --- dmabuf (zwp_linux_dmabuf_v1) image import --- */
static PFNEGLCREATEIMAGEKHRPROC p_egl_create_image;
static PFNEGLDESTROYIMAGEKHRPROC p_egl_destroy_image;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC p_egl_image_to_texture;
static PFNEGLQUERYDMABUFMODIFIERSEXTPROC p_egl_query_modifiers;
static int dmabuf_import_ok;
static int dmabuf_modifiers_ok;

static const float QUAD_VERTS[12] = {
    0,0, 1,0, 0,1,
    0,1, 1,0, 1,1,
};

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if(!ok){
        char log[1024];
        glGetShaderInfoLog(s, sizeof log, NULL, log);
        fprintf(stderr, "railwl: shader compile: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static int gprog_init(struct gprog *p, const char *fs_src)
{
    GLuint v = compile_shader(GL_VERTEX_SHADER, QUAD_VS);
    GLuint f = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if(!v || !f) return -1;
    GLuint id = glCreateProgram();
    glAttachShader(id, v);
    glAttachShader(id, f);
    glBindAttribLocation(id, 0, "a_pos");
    glLinkProgram(id);
    GLint ok = 0;
    glGetProgramiv(id, GL_LINK_STATUS, &ok);
    if(!ok){
        char log[1024];
        glGetProgramInfoLog(id, sizeof log, NULL, log);
        fprintf(stderr, "railwl: program link: %s\n", log);
        return -1;
    }
    glDeleteShader(v);
    glDeleteShader(f);
    p->id = id;
    p->a_pos = 0;
    p->u_rect   = glGetUniformLocation(id, "u_rect");
    p->u_screen = glGetUniformLocation(id, "u_screen");
    p->u_flip   = glGetUniformLocation(id, "u_flip");
    p->u_uv     = glGetUniformLocation(id, "u_uv");
    p->u_color  = glGetUniformLocation(id, "u_color");
    p->u_tex    = glGetUniformLocation(id, "u_tex");
    p->u_opaque = glGetUniformLocation(id, "u_opaque");
    return 0;
}

/* bind program, set position uniforms, draw the unit quad */
static float quad_uv[4] = {0.0f, 0.0f, 1.0f, 1.0f};   /* which part of the texture the next quad shows */

static void draw_quad(struct gprog *p, int x, int y, int w, int h, int sw, int sh)
{
    glUseProgram(p->id);
    if(p->u_uv >= 0) glUniform4f(p->u_uv, quad_uv[0], quad_uv[1], quad_uv[2], quad_uv[3]);
    glUniform4f(p->u_rect, (float)x, (float)y, (float)w, (float)h);
    glUniform2f(p->u_screen, (float)sw, (float)sh);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glEnableVertexAttribArray((GLuint)p->a_pos);
    glVertexAttribPointer((GLuint)p->a_pos, 2, GL_FLOAT, GL_FALSE, 0, (const void*)0);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

/* ---- cursor: the drawn pointer, embedded verbatim at build time ---- */
#include "cursor_image.h"

static void make_cursor_texture(void)
{
    glGenTextures(1, &cursor_tex);
    glBindTexture(GL_TEXTURE_2D, cursor_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, CUR_W, CUR_H,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, CUR_RGBA);
}

static int gl_init(void)
{
    if(gprog_init(&p_solid, SOLID_FS) || gprog_init(&p_tex, TEX_FS))
        return -1;

    glGenBuffers(1, &quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof QUAD_VERTS, QUAD_VERTS, GL_STATIC_DRAW);

    make_cursor_texture();
    return 0;
}

/* ================================================================
   GBM + EGL setup
   ================================================================ */

static int egl_init(struct rail_output *o)
{
    o->gbm = gbm_create_device(o->fd);
    if(!o->gbm){
        fprintf(stderr, "railwl: gbm_create_device failed\n");
        return -1;
    }

    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if(get_platform)
        dpy = get_platform(EGL_PLATFORM_GBM_KHR, o->gbm, NULL);
    if(dpy == EGL_NO_DISPLAY)
        dpy = eglGetDisplay((EGLNativeDisplayType)(uintptr_t)o->gbm);
    if(dpy == EGL_NO_DISPLAY){
        fprintf(stderr, "railwl: eglGetDisplay failed (0x%x)\n", eglGetError());
        return -1;
    }

    EGLint maj = 0, min = 0;
    if(!eglInitialize(dpy, &maj, &min)){
        fprintf(stderr, "railwl: eglInitialize failed (0x%x)\n", eglGetError());
        return -1;
    }
    if(!eglBindAPI(EGL_OPENGL_ES_API)){
        fprintf(stderr, "railwl: eglBindAPI(ES) failed\n");
        return -1;
    }

    /* EGL_NATIVE_VISUAL_ID is not reliably matchable in eglChooseConfig,
       so enumerate and pick the config whose visual equals our GBM format */
    EGLint base_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig candidates[64];
    EGLint ncfg = 0;
    if(!eglChooseConfig(dpy, base_attribs, candidates, 64, &ncfg) || ncfg < 1){
        fprintf(stderr, "railwl: no EGL window/ES2 config (0x%x)\n", eglGetError());
        return -1;
    }
    EGLConfig cfg = NULL;
    for(EGLint i = 0; i < ncfg; i++){
        EGLint vis = 0;
        eglGetConfigAttrib(dpy, candidates[i], EGL_NATIVE_VISUAL_ID, &vis);
        if((uint32_t)vis == GBM_FORMAT_XRGB8888){ cfg = candidates[i]; break; }
    }
    if(!cfg){
        fprintf(stderr, "railwl: no XR24 EGL config among %d\n", (int)ncfg);
        return -1;
    }

    EGLint ctx_attribs[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if(ctx == EGL_NO_CONTEXT){
        fprintf(stderr, "railwl: eglCreateContext failed (0x%x)\n", eglGetError());
        return -1;
    }

    o->gbm_surf = gbm_surface_create(o->gbm, (uint32_t)o->w, (uint32_t)o->h,
                                     GBM_FORMAT_XRGB8888,
                                     GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if(!o->gbm_surf){
        fprintf(stderr, "railwl: gbm_surface_create failed\n");
        return -1;
    }

    EGLSurface surf = eglCreateWindowSurface(dpy, cfg,
                        (EGLNativeWindowType)o->gbm_surf, NULL);
    if(surf == EGL_NO_SURFACE){
        fprintf(stderr, "railwl: eglCreateWindowSurface failed (0x%x)\n", eglGetError());
        return -1;
    }
    if(!eglMakeCurrent(dpy, surf, surf, ctx)){
        fprintf(stderr, "railwl: eglMakeCurrent failed (0x%x)\n", eglGetError());
        return -1;
    }
    /* we sync to vblank through drm page flips, not swap queues */
    eglSwapInterval(dpy, 0);

    o->egl_dpy = (void*)dpy;
    o->egl_ctx = (void*)ctx;
    o->egl_surf = (void*)surf;
    o->egl_ok = 1;

    const char *ren = (const char*)glGetString(GL_RENDERER);
    const char *ext = (const char*)glGetString(GL_EXTENSIONS);
    /* desktop GL says GL_EXT_bgra, GLES says GL_EXT_texture_format_BGRA8888;
       without either, every shm commit is swizzled on the CPU */
    gl_bgra = (ext && (strstr(ext, "GL_EXT_bgra") ||
                       strstr(ext, "GL_EXT_texture_format_BGRA8888"))) ? 1 : 0;

    /* can we turn a client's dmabuf planes into something the GPU samples? */
    const char *eext = eglQueryString(dpy, EGL_EXTENSIONS);
    dmabuf_import_ok       = eext && strstr(eext, "EGL_EXT_image_dma_buf_import");
    dmabuf_modifiers_ok    = eext && strstr(eext, "EGL_EXT_image_dma_buf_import_modifiers");
    if(dmabuf_import_ok){
        p_egl_create_image     = (PFNEGLCREATEIMAGEKHRPROC)
                                 eglGetProcAddress("eglCreateImageKHR");
        p_egl_destroy_image    = (PFNEGLDESTROYIMAGEKHRPROC)
                                 eglGetProcAddress("eglDestroyImageKHR");
        p_egl_image_to_texture = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
                                 eglGetProcAddress("glEGLImageTargetTexture2DOES");
        if(!p_egl_create_image || !p_egl_destroy_image || !p_egl_image_to_texture)
            dmabuf_import_ok = 0;
    }
    if(dmabuf_modifiers_ok)
        p_egl_query_modifiers = (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)
                                eglGetProcAddress("eglQueryDmaBufModifiersEXT");
    fprintf(stderr, "railwl: GL renderer: %s (bgra=%d, EGL %d.%d, dmabuf=%s%s)\n",
            ren ? ren : "?", gl_bgra, (int)maj, (int)min,
            dmabuf_import_ok ? "yes" : "no",
            dmabuf_modifiers_ok ? "+modifiers" : "");
    return 0;
}

/* ================================================================
   dmabuf buffers: the client's planes are imported into an EGLImage
   which the GPU samples directly; no CPU mapping is ever involved.
   ================================================================ */

static const uint32_t DMABUF_FORMATS[] = {
    DRM_FORMAT_ARGB8888,
    DRM_FORMAT_XRGB8888,
};

int rail_dmabuf_supported(void)
{
    return dmabuf_import_ok;
}

const uint32_t *rail_dmabuf_formats(int *n)
{
    if(!dmabuf_import_ok){ if(n) *n = 0; return NULL; }
    if(n) *n = (int)(sizeof DMABUF_FORMATS / sizeof DMABUF_FORMATS[0]);
    return DMABUF_FORMATS;
}

/* modifiers this GPU can import for one fourcc (caller frees) */
uint64_t *rail_dmabuf_modifiers(struct rail *r, uint32_t fourcc, int *n)
{
    if(n) *n = 0;
    if(!dmabuf_modifiers_ok || !p_egl_query_modifiers || !r || !r->output)
        return NULL;
    EGLDisplay dpy = (EGLDisplay)r->output->egl_dpy;
    EGLint cnt = 0;
    if(!p_egl_query_modifiers(dpy, (EGLint)fourcc, 0, NULL, NULL, &cnt) || cnt <= 0)
        return NULL;
    uint64_t *mods = calloc((size_t)cnt, sizeof *mods);
    if(!mods) return NULL;
    EGLint got = 0;
    if(!p_egl_query_modifiers(dpy, (EGLint)fourcc, cnt, (EGLuint64KHR*)mods, NULL, &got)){
        free(mods);
        return NULL;
    }
    if(n) *n = (int)got;
    return mods;
}

int rail_dmabuf_import(struct rail_buffer *b)
{
    if(!dmabuf_import_ok || !b || !b->rail || !b->rail->output) return -1;
    if(b->nplanes <= 0 || b->nplanes > RAIL_MAX_PLANES) return -1;

    static const EGLint FD_N[RAIL_MAX_PLANES] = {
        EGL_DMA_BUF_PLANE0_FD_EXT, EGL_DMA_BUF_PLANE1_FD_EXT,
        EGL_DMA_BUF_PLANE2_FD_EXT, EGL_DMA_BUF_PLANE3_FD_EXT };
    static const EGLint OFF_N[RAIL_MAX_PLANES] = {
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, EGL_DMA_BUF_PLANE1_OFFSET_EXT,
        EGL_DMA_BUF_PLANE2_OFFSET_EXT, EGL_DMA_BUF_PLANE3_OFFSET_EXT };
    static const EGLint PITCH_N[RAIL_MAX_PLANES] = {
        EGL_DMA_BUF_PLANE0_PITCH_EXT, EGL_DMA_BUF_PLANE1_PITCH_EXT,
        EGL_DMA_BUF_PLANE2_PITCH_EXT, EGL_DMA_BUF_PLANE3_PITCH_EXT };
    static const EGLint MOD_LO_N[RAIL_MAX_PLANES] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT };
    static const EGLint MOD_HI_N[RAIL_MAX_PLANES] = {
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT,
        EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT, EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT };

    EGLint attrs[4 + 8*RAIL_MAX_PLANES + 1];
    int i = 0;
    attrs[i++] = EGL_WIDTH;  attrs[i++] = (EGLint)b->w;
    attrs[i++] = EGL_HEIGHT; attrs[i++] = (EGLint)b->h;
    attrs[i++] = EGL_LINUX_DRM_FOURCC_EXT; attrs[i++] = (EGLint)b->format;
    for(int p = 0; p < b->nplanes; p++){
        attrs[i++] = FD_N[p];    attrs[i++] = b->planes[p];
        attrs[i++] = OFF_N[p];   attrs[i++] = (EGLint)b->off[p];
        attrs[i++] = PITCH_N[p]; attrs[i++] = (EGLint)b->pstride[p];
        if(dmabuf_modifiers_ok){
            attrs[i++] = MOD_LO_N[p]; attrs[i++] = (EGLint)(b->mod & 0xffffffffu);
            attrs[i++] = MOD_HI_N[p]; attrs[i++] = (EGLint)(b->mod >> 32);
        }
    }
    attrs[i++] = EGL_NONE;

    b->img = p_egl_create_image((EGLDisplay)b->rail->output->egl_dpy,
                                EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, NULL, attrs);
    if(!b->img){
        if(getenv("RAIL_DEBUG")){
            char f[5] = { (char)(b->format), (char)(b->format >> 8),
                          (char)(b->format >> 16), (char)(b->format >> 24), 0 };
            fprintf(stderr, "railwl: dmabuf import failed fourcc=%s mod=%#llx mods=%d planes=%d (egl 0x%x)\n",
                    f, (unsigned long long)b->mod, dmabuf_modifiers_ok ? 1 : 0,
                    b->nplanes, eglGetError());
        }
        return -1;
    }
    /* the image holds its own dups; our copies of the fds are done with */
    for(int p = 0; p < b->nplanes; p++){
        if(b->planes[p] >= 0){ close(b->planes[p]); b->planes[p] = -1; }
    }
    return 0;
}

void rail_dmabuf_destroy(struct rail_buffer *b)
{
    if(!b) return;
    if(b->img){
        if(p_egl_destroy_image && b->rail && b->rail->output)
            p_egl_destroy_image((EGLDisplay)b->rail->output->egl_dpy,
                                (EGLImageKHR)b->img);
        b->img = NULL;
    }
    for(int p = 0; p < b->nplanes; p++){
        if(b->planes[p] >= 0){ close(b->planes[p]); b->planes[p] = -1; }
    }
}

/* a dmabuf is still being read by the GPU; hand it back once the frame
   sampling it has actually reached the screen */
void rail_buffer_release_later(struct rail *r, struct rail_buffer *b)
{
    if(!r || !b || b->released || b->rel_pending) return;
    b->rel_pending = true;
    b->rel_next = r->rel_pending;
    r->rel_pending = b;
}

static void flush_buffer_releases(struct rail *r)
{
    struct rail_buffer *b = r->rel_pending;
    r->rel_pending = NULL;
    while(b){
        struct rail_buffer *next = b->rel_next;
        b->rel_next = NULL;
        b->rel_pending = false;
        if(b->resource && !b->released){
            b->released = true;
            wl_buffer_send_release(b->resource);
        }
        b = next;
    }
}

/* the buffer a surface's texture was built from is going away */
void rail_output_forget_tex(struct rail_surface *s)
{
    if(!s) return;
    if(s->tex){ glDeleteTextures(1, &s->tex); s->tex = 0; }
    s->tex_flip = false;
    s->tex_shm = false;
    s->buffer_dirty = false;
}

/* ================================================================
   strip animation: frame-rate independent exponential easing,
   settles within RAIL_ANIM_MS regardless of fps
   ================================================================ */

static int rel_distance(struct rail_ws *ws, struct rail_surface *s)
{
    if(!ws->first) return 0;
    /* circular distance from focused, wrap-around aware */
    int i=0; struct rail_surface *it = ws->focused ? ws->focused : ws->first;
    for(int k=0;k<64;k++){ if(it==s) return i; it = it->next; i++; }
    i=0; it = ws->focused ? ws->focused : ws->first;
    for(int k=0;k<64;k++){ if(it==s) return -i; it = it->prev; i++; }
    return 0;
}

static bool anim_toward(int *cur, int target, double k)
{
    int d = target - *cur;
    if(!d) return false;
    int m = (int)((double)d * k);
    if(!m) m = (d > 0) ? 1 : -1;          /* guarantee progress when close */
    if((d > 0 && m >= d) || (d < 0 && m <= d)){ *cur = target; return false; }
    *cur += m;
    return true;
}

static bool anim_step(struct rail_output *o)
{
    struct rail_ws *ws = o->active;
    if(!ws || !ws->first){ o->last_anim_ms = 0; return false; }

    uint32_t t = now_ms();
    uint32_t dt = o->last_anim_ms ? (t - o->last_anim_ms) : 16;
    if(dt < 1) dt = 1;
    if(dt > 100) dt = 100;                 /* clamp stalls (first frame, hiccups) */
    o->last_anim_ms = t;

    /* time constant = RAIL_ANIM_MS/4 => ~95% there after RAIL_ANIM_MS */
    double k = 1.0 - exp(-(double)dt * 4.0 / (double)RAIL_ANIM_MS);
    if(k > 1.0) k = 1.0;

    bool busy = false;
    int tile_w = RAIL_TILE_W(o->w), tile_h = RAIL_TILE_H(o->h);
    for(struct rail_surface *s = ws->first;; s = s->next){
        if(s->fullscreen){
            /* fullscreen tiles ignore the strip: fill the output, no offset */
            s->tw = o->w; s->th = o->h;
            busy |= anim_toward(&s->cx, 0, k);
            busy |= anim_toward(&s->cy, 0, k);
            busy |= anim_toward(&s->cw, o->w, k);
            busy |= anim_toward(&s->ch, o->h, k);
            if(s->next == ws->first) break;
            continue;
        }
        int rel = rel_distance(ws, s);
        s->tw = tile_w; s->th = tile_h;   /* content size sent to the client */
        /* target: framed tile below the bar, RAIL_GAP away from every edge */
        busy |= anim_toward(&s->cx, rel * o->w + RAIL_GAP, k);
        busy |= anim_toward(&s->cy, RAIL_BAR_HEIGHT + RAIL_GAP, k);
        busy |= anim_toward(&s->cw, tile_w + 2 * RAIL_BORDER, k);
        busy |= anim_toward(&s->ch, tile_h + 2 * RAIL_BORDER, k);
        if(s->next == ws->first) break;
    }
    if(!busy) o->last_anim_ms = 0;
    return busy;
}

/* ================================================================
   scene drawing
   ================================================================ */

/* upload a committed shm buffer into the surface's GL texture once,
   then release the buffer back to the client */
static void upload_surface(struct rail_surface *s)
{
    struct rail_buffer *b = s->front;
    if(!b || b->w <= 0 || b->h <= 0) return;

    if(b->is_dmabuf){
        /* GPU buffer: bind its EGLImage straight into the surface's texture.
           Nothing is copied, so the buffer stays locked until the frame that
           samples it has been presented (flush_buffer_releases). */
        s->dmg_any = false;
        if(!b->img){ s->buffer_dirty = false; return; }
        if(!s->tex) glGenTextures(1, &s->tex);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        p_egl_image_to_texture(GL_TEXTURE_2D, (GLeglImageOES)b->img);
        s->tex_opaque = (b->format == (uint32_t)DRM_FORMAT_XRGB8888);
        s->tex_flip = (b->flags & RAIL_DMABUF_Y_INVERT) != 0;
        s->tex_shm = false;
        s->buffer_dirty = false;
        if(getenv("RAIL_DEBUG"))
            fprintf(stderr, "railwl: DMABUF surf=%p %dx%d fourcc=%c%c%c%c flip=%d\n",
                    (void *)s, b->w, b->h,
                    (char)b->format, (char)(b->format >> 8),
                    (char)(b->format >> 16), (char)(b->format >> 24),
                    s->tex_flip ? 1 : 0);
        rail_buffer_release_later(s->rail, b);
        return;
    }

    const uint8_t *bdata = rail_buf_data(b);
    if(!bdata) return;

    if(!s->tex) glGenTextures(1, &s->tex);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    int w = b->w, h = b->h;

    /* same-size buffer + reported damage: send only the damaged rectangle */
    bool had_dmg = s->dmg_any;
    int rx = s->dmg_x0, ry = s->dmg_y0, rx1 = s->dmg_x1, ry1 = s->dmg_y1;
    s->dmg_any = false;
    if(had_dmg && s->tex_shm && s->tex_w == w && s->tex_h == h){
        if(rx1 > w) rx1 = w;
        if(ry1 > h) ry1 = h;
        int rw = rx1 - rx, rh = ry1 - ry;
        if(rw > 0 && rh > 0 && (rw < w || rh < h)){
            static uint8_t *pscratch;
            static size_t pcap;
            size_t rneed = (size_t)rw * 4, want = rneed * (size_t)rh;
            if(want > pcap){
                uint8_t *ns = realloc(pscratch, want);
                if(!ns) return;
                pscratch = ns; pcap = want;
            }
            for(int y = 0; y < rh; y++){
                const uint8_t *src = bdata + (size_t)(ry + y) * (size_t)b->stride + (size_t)rx * 4;
                uint8_t *dst = pscratch + (size_t)y * rneed;
                if(gl_bgra) memcpy(dst, src, rneed);
                else for(int x = 0; x < rw; x++){
                    dst[x*4]   = src[x*4+2];
                    dst[x*4+1] = src[x*4+1];
                    dst[x*4+2] = src[x*4];
                    dst[x*4+3] = src[x*4+3];
                }
            }
            glTexSubImage2D(GL_TEXTURE_2D, 0, rx, ry, rw, rh,
                            gl_bgra ? GL_BGRA_EXT : GL_RGBA, GL_UNSIGNED_BYTE, pscratch);
            s->tex_opaque = (b->format == WL_SHM_FORMAT_XRGB8888);
            s->tex_flip = false;
            s->buffer_dirty = false;
            if(!b->released){
                b->released = true;
                wl_buffer_send_release(b->resource);
            }
            return;
        }
    }
    size_t need = (size_t)w * 4;
    const void *data = bdata;
    uint8_t *tmp = NULL;
    if((size_t)b->stride != need || !gl_bgra){
        /* stride mismatch or no BGRA: pack (and swizzle) once per commit,
           into one scratch buffer that is reused between commits */
        static uint8_t *scratch;
        static size_t scratch_cap;
        size_t want = need * (size_t)h;
        if(want > scratch_cap){
            uint8_t *ns = realloc(scratch, want);
            if(!ns) return;
            scratch = ns; scratch_cap = want;
        }
        tmp = scratch;
        for(int y = 0; y < h; y++){
            const uint8_t *src = bdata + (size_t)y * (size_t)b->stride;
            uint8_t *dst = tmp + (size_t)y * need;
            if(gl_bgra) memcpy(dst, src, need);
            else for(int x = 0; x < w; x++){          /* BGRA -> RGBA */
                dst[x*4]   = src[x*4+2];
                dst[x*4+1] = src[x*4+1];
                dst[x*4+2] = src[x*4];
                dst[x*4+3] = src[x*4+3];
            }
        }
        data = tmp;
    }
    if(getenv("RAIL_DEBUG")){
        const uint8_t *q = data;
        /* count reddish pixels so we can tell whether the client ever painted
           the page (sampled; the buffers here are multi-megabyte) */
        int reds = 0, nonblack = 0, tot = 0;
        size_t np = (size_t)w * (size_t)h;
        for(size_t i = 0; i < np; i += 997){
            const uint8_t *p = q + i*4;
            uint8_t r0 = gl_bgra ? p[2] : p[0];
            uint8_t g0 = p[1];
            uint8_t b0 = gl_bgra ? p[0] : p[2];
            tot++;
            if(r0 > 180 && g0 < 80 && b0 < 80) reds++;
            if(r0 > 8 || g0 > 8 || b0 > 8) nonblack++;
        }
        fprintf(stderr, "railwl: UPLOAD surf=%p sub=%d popup=%d w=%d h=%d fmt=%u px0=%02x%02x%02x%02x reds=%d/%d nonblack=%d\n",
                (void *)s, s->is_sub, s->is_popup, w, h, b->format,
                q[0], q[1], q[2], q[3], reds, tot, nonblack);
    }
    GLenum fmt = gl_bgra ? GL_BGRA_EXT : GL_RGBA;
    if(s->tex_shm && s->tex_w == w && s->tex_h == h){
        /* same size as last commit: update the existing storage in place */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, fmt, GL_UNSIGNED_BYTE, data);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)fmt, w, h, 0, fmt, GL_UNSIGNED_BYTE, data);
        s->tex_shm = true; s->tex_w = w; s->tex_h = h;
    }
    (void)tmp;

    s->tex_opaque = (b->format == WL_SHM_FORMAT_XRGB8888);
    s->tex_flip = false;
    s->buffer_dirty = false;
    if(!b->released){
        b->released = true;
        wl_buffer_send_release(b->resource);
    }
}

static void draw_surface(struct rail_output *o, struct rail_surface *s)
{
    if(!s->mapped) return;
    if(s->buffer_dirty && s->front) upload_surface(s);
    if(!s->tex) return;
    if(getenv("RAIL_DEBUG"))
        fprintf(stderr, "railwl: DRAW  surf=%p sub=%d popup=%d bar=%d at %d,%d %dx%d\n",
                (void *)s, s->is_sub, s->is_popup, s->is_bar, s->cx, s->cy, s->cw, s->ch);
    glUseProgram(p_tex.id);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(p_tex.u_tex, 0);
    glUniform1f(p_tex.u_opaque, s->tex_opaque ? 1.0f : 0.0f);
    glUniform1f(p_tex.u_flip, s->tex_flip ? 1.0f : 0.0f);
    draw_quad(&p_tex, s->cx, s->cy, s->cw, s->ch, o->w, o->h);
}

/* tile content, inset by the amber frame so the border never covers the
   client's first rows/columns (fullscreen has no frame) */
static void draw_tile(struct rail_output *o, struct rail_surface *s)
{
    if(!s->mapped) return;
    if(s->buffer_dirty && s->front) upload_surface(s);
    if(!s->tex) return;
    int b = s->fullscreen ? 0 : RAIL_BORDER;
    int w = s->cw - 2*b, h = s->ch - 2*b;
    if(w <= 0 || h <= 0) return;
    glUseProgram(p_tex.id);
    glBindTexture(GL_TEXTURE_2D, s->tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(p_tex.u_tex, 0);
    glUniform1f(p_tex.u_opaque, s->tex_opaque ? 1.0f : 0.0f);
    glUniform1f(p_tex.u_flip, s->tex_flip ? 1.0f : 0.0f);
    /* a client that declared a window geometry smaller than its buffer (CSD
       shadow margin around a dialog) shows only that part, so the amber frame
       hugs the window instead of the invisible shadow */
    bool sub = !s->fullscreen && s->geo_w > 0 && s->geo_h > 0 && s->w > 0 && s->h > 0 &&
               s->geo_x >= 0 && s->geo_y >= 0 &&
               s->geo_x + s->geo_w <= s->w && s->geo_y + s->geo_h <= s->h &&
               (s->geo_w < s->w || s->geo_h < s->h);
    int qx = s->cx + rail_inset_x(s), qy = s->cy + rail_inset_y(s);
    if(sub){
        quad_uv[0] = (float)s->geo_x / (float)s->w;  quad_uv[1] = (float)s->geo_y / (float)s->h;
        quad_uv[2] = (float)s->geo_w / (float)s->w;  quad_uv[3] = (float)s->geo_h / (float)s->h;
        qx = s->cx + b; qy = s->cy + b;
    }
    draw_quad(&p_tex, qx, qy, w, h, o->w, o->h);
    quad_uv[0] = 0.0f; quad_uv[1] = 0.0f; quad_uv[2] = 1.0f; quad_uv[3] = 1.0f;
}

static void draw_border(struct rail_output *o, struct rail_surface *s)
{
    int x = s->cx, y = s->cy, w = s->cw, h = s->ch, bw = RAIL_BORDER;
    if(w <= 0 || h <= 0) return;
    glUseProgram(p_solid.id);
    glUniform4f(p_solid.u_color, 1.0f, 191.0f/255.0f, 0.0f, 1.0f); /* amber */
    draw_quad(&p_solid, x,        y,        w,   bw, o->w, o->h);
    draw_quad(&p_solid, x,        y + h-bw, w,   bw, o->w, o->h);
    draw_quad(&p_solid, x,        y,        bw,  h,  o->w, o->h);
    draw_quad(&p_solid, x + w-bw, y,        bw,  h,  o->w, o->h);
}

/* popups and subsurfaces hanging off one toplevel, parent-first */
static void draw_popup_tree(struct rail_output *o, struct rail_surface *s)
{
    struct rail_surface *stack[64]; int n = 0;
    for(struct rail_surface *p = s->popups; p && n < 64; p = p->popup_next) stack[n++] = p;
    while(n > 0){
        struct rail_surface *pu = stack[--n];
        if(pu->is_sub) rail_child_place(pu);   /* children track their parent */
        if(pu->mapped) draw_surface(o, pu);
        for(struct rail_surface *q = pu->popups; q && n < 64; q = q->popup_next) stack[n++] = q;
    }
}

/* popup children and their children, parent-first */
static void draw_popups(struct rail_output *o, struct rail_ws *ws)
{
    if(!ws) return;
    for(struct rail_surface *s = ws->first; s; s = s->next){
        if(!s->mapped){ if(!s->next || s->next == ws->first) break; continue; }
        draw_popup_tree(o, s);
        if(!s->next || s->next == ws->first) break;
    }
}

static void draw_cursor(struct rail *r)
{
    struct rail_output *o = r->output;
    /* a client that called wl_pointer.set_cursor owns the pointer now */
    if(r->cursor_set){
        struct rail_surface *s = r->cursor;
        if(!s) return;                         /* set_cursor(NULL): hide */
        if(s->buffer_dirty) upload_surface(s); /* cursor surfaces skip rail_view_map */
        if(!s->tex || !s->front) return;
        int w = s->front->w, h = s->front->h;
        if(w <= 0 || h <= 0) return;
        glUseProgram(p_tex.id);
        glBindTexture(GL_TEXTURE_2D, s->tex);
        glActiveTexture(GL_TEXTURE0);
        glUniform1i(p_tex.u_tex, 0);
        glUniform1f(p_tex.u_opaque, s->tex_opaque ? 1.0f : 0.0f);
    glUniform1f(p_tex.u_flip, s->tex_flip ? 1.0f : 0.0f);
        draw_quad(&p_tex, o->px - r->cursor_hx, o->py - r->cursor_hy, w, h, o->w, o->h);
        return;
    }
    /* nobody set a cursor: the built-in arrow */
    glUseProgram(p_tex.id);
    glBindTexture(GL_TEXTURE_2D, cursor_tex);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(p_tex.u_tex, 0);
    glUniform1f(p_tex.u_opaque, 0.0f);
    glUniform1f(p_tex.u_flip, 0.0f);
    draw_quad(&p_tex, o->px - CUR_HOT_X, o->py - CUR_HOT_Y, CUR_W, CUR_H, o->w, o->h);
}

static void send_frame_cbs(struct rail *r)
{
    uint32_t t = now_ms();
    for(struct rail_surface *s = r->all_surfaces; s; s = s->g_next){
        if(s->frame_cb){
            if(getenv("RAIL_DEBUG"))
                fprintf(stderr, "railwl: CB fire surf=%p sub=%d map=%d\n",
                        (void *)s, s->is_sub, s->mapped);
            wl_callback_send_done(s->frame_cb, t);
            wl_resource_destroy(s->frame_cb);
            s->frame_cb = NULL;
        }
    }
}

int rail_output_read_rgba(struct rail *r, uint8_t *dst)
{
    struct rail_output *o = r->output;
    size_t row = (size_t)o->w * 4;
    uint8_t *tmp = malloc(row);
    if(!tmp) return -1;
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, o->w, o->h, GL_RGBA, GL_UNSIGNED_BYTE, dst);
    for(int y = 0; y < o->h / 2; y++){          /* GL is bottom-up: flip in place */
        uint8_t *a = dst + (size_t)y * row;
        uint8_t *b = dst + (size_t)(o->h - 1 - y) * row;
        memcpy(tmp, a, row); memcpy(a, b, row); memcpy(b, tmp, row);
    }
    free(tmp);
    return 0;
}

/* draw the whole scene into the EGL surface, return animating */
static bool render_gl(struct rail *r)
{
    struct rail_output *o = r->output;
    bool busy = anim_step(o);

    glViewport(0, 0, o->w, o->h);
    glClearColor(20.0f/255.0f, 20.0f/255.0f, 20.0f/255.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_DEPTH_TEST);

    struct rail_ws *ws = o->active;
    /* when the focused tile is fullscreen the bar must sit *under* it */
    bool fs_focus = ws && ws->focused && ws->focused->fullscreen;
    if(!fs_focus && o->bar) draw_surface(o, o->bar);
    if(ws && ws->first){
        /* draw non-focused first (so focused lands on top) */
        for(struct rail_surface *s = ws->first;; s = s->next){
            if(s != ws->focused && abs(s->cx) < o->w) draw_tile(o, s);
            if(s->next == ws->first) break;
        }
        if(ws->focused) draw_tile(o, ws->focused);
        for(struct rail_surface *s = ws->first;; s = s->next){
            if(s != ws->focused && abs(s->cx) < o->w) draw_border(o, s);
            if(s->next == ws->first) break;
        }
        if(ws->focused && !ws->focused->fullscreen) draw_border(o, ws->focused);
    }
    /* bar only on top of a windowed scene */
    if(fs_focus && o->bar) draw_surface(o, o->bar);
    /* popups float above everything, parent first then children */
    draw_popups(o, ws);
    /* dialogs (file choosers, confirmations): framed, centred, above the strip; newest on top */
    for(struct rail_surface *d = o->dialogs; d; d = d->dlg_next){
        if(!d->mapped) continue;
        draw_tile(o, d); draw_border(o, d);
        draw_popup_tree(o, d);
    }
    /* the floating launcher, above tiles, popups and the bar */
    if(o->launcher){ draw_tile(o, o->launcher); draw_border(o, o->launcher); }
    rail_capture_service(r, false);   /* screenshots that want no pointer */
    if(rail_sel.active){              /* region selection: dim everything, amber frame on the box */
        int x = rail_sel.x0 < rail_sel.x1 ? rail_sel.x0 : rail_sel.x1;
        int y = rail_sel.y0 < rail_sel.y1 ? rail_sel.y0 : rail_sel.y1;
        int w = abs(rail_sel.x1 - rail_sel.x0) + 1, h = abs(rail_sel.y1 - rail_sel.y0) + 1;
        glUseProgram(p_solid.id);
        glUniform4f(p_solid.u_color, 0.0f, 0.0f, 0.0f, 0.45f);
        if(rail_sel.dragging){
            draw_quad(&p_solid, 0,     0,     o->w,         y,             o->w, o->h);
            draw_quad(&p_solid, 0,     y + h, o->w,         o->h - y - h,  o->w, o->h);
            draw_quad(&p_solid, 0,     y,     x,            h,             o->w, o->h);
            draw_quad(&p_solid, x + w, y,     o->w - x - w, h,             o->w, o->h);
            glUniform4f(p_solid.u_color, 1.0f, 191.0f/255.0f, 0.0f, 1.0f);
            draw_quad(&p_solid, x,         y,         w, 1, o->w, o->h);
            draw_quad(&p_solid, x,         y + h - 1, w, 1, o->w, o->h);
            draw_quad(&p_solid, x,         y,         1, h, o->w, o->h);
            draw_quad(&p_solid, x + w - 1, y,         1, h, o->w, o->h);
        } else {
            draw_quad(&p_solid, 0, 0, o->w, o->h, o->w, o->h);
        }
    }
    draw_cursor(r);
    rail_capture_service(r, true);    /* screenshots with the pointer */

    /* debug: RAIL_SHOT=<path> dumps the composed frame as PPM every frame */
    {
        const char *shot = getenv("RAIL_SHOT");
        if(shot && *shot){
            size_t npx = (size_t)o->w * (size_t)o->h;
            uint8_t *buf = malloc(npx * 4);
            if(buf){
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, o->w, o->h, GL_RGBA, GL_UNSIGNED_BYTE, buf);
                char tmp[512];
                snprintf(tmp, sizeof tmp, "%s.tmp", shot);
                FILE *f = fopen(tmp, "wb");
                if(f){
                    fprintf(f, "P6\n%d %d\n255\n", o->w, o->h);
                    for(int y = o->h - 1; y >= 0; y--)      /* GL is bottom-up */
                        for(int x = 0; x < o->w; x++){
                            const uint8_t *p = buf + ((size_t)y * (size_t)o->w + (size_t)x) * 4;
                            fputc(p[0], f); fputc(p[1], f); fputc(p[2], f);
                        }
                    fclose(f);
                    rename(tmp, shot);      /* atomic: readers never see a partial file */
                }
                free(buf);
            }
        }
    }

    send_frame_cbs(r);
    return busy;
}

/* ================================================================
   present loop: render -> eglSwapBuffers -> lock BO -> page flip
   ================================================================ */

static uint32_t fb_for_bo(struct rail_output *o, struct gbm_bo *bo)
{
    uint32_t fb = (uint32_t)(uintptr_t)gbm_bo_get_user_data(bo);
    if(fb) return fb;
    uint32_t handle = gbm_bo_get_handle(bo).u32;
    uint32_t stride = gbm_bo_get_stride(bo);
    if(drmModeAddFB(o->fd, (uint32_t)o->w, (uint32_t)o->h, 24, 32,
                    stride, handle, &fb) < 0){
        fprintf(stderr, "railwl: addfb failed: %s\n", strerror(errno));
        return 0;
    }
    gbm_bo_set_user_data(bo, (void*)(uintptr_t)fb, NULL);
    return fb;
}

/* render one frame and page-flip to it; never touch the scanned-out BO */
static void render_frame(struct rail *r)
{
    struct rail_output *o = r->output;
    if(o->suspended){ o->needs_frame = true; return; }
    if(o->flip_pending){ o->needs_frame = true; return; }
    if(!o->egl_ok) return;

    o->anim_busy = render_gl(r);
    o->needs_frame = false;

    eglSwapBuffers((EGLDisplay)o->egl_dpy, (EGLSurface)o->egl_surf);
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(o->gbm_surf);
    if(!bo){
        fprintf(stderr, "railwl: gbm lock_front_buffer failed\n");
        return;
    }
    uint32_t fb = fb_for_bo(o, bo);
    if(!fb){ gbm_surface_release_buffer(o->gbm_surf, bo); return; }
    o->flip_bo = bo;
    if(drmModePageFlip(o->fd, o->crtc_id, fb, DRM_MODE_PAGE_FLIP_EVENT, r) == 0){
        o->flip_pending = true;
    } else {
        static int warned = 0;
        if(!warned++){
            fprintf(stderr, "railwl: pageflip failed: %s (no DRM master?)\n",
                    strerror(errno));
        }
        gbm_surface_release_buffer(o->gbm_surf, bo);
        o->flip_bo = NULL;
        flush_buffer_releases(r);   /* nothing will present: unblock clients */
    }
}

static void drm_page_flip(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *data)
{
    (void)fd;(void)seq;(void)tv_sec;(void)tv_usec;
    struct rail *r = data;
    struct rail_output *o = r->output;
    o->flip_pending = false;
    /* the buffer that was on screen is free now */
    if(o->disp_bo) gbm_surface_release_buffer(o->gbm_surf, o->disp_bo);
    o->disp_bo = o->flip_bo;
    o->flip_bo = NULL;
    r->drm_frame_count++;
    /* the frame is on screen: dmabufs it sampled are the client's again */
    flush_buffer_releases(r);
    /* keep going if animating or a redraw was requested during the flip */
    if(!o->suspended && (o->anim_busy || o->needs_frame)) render_frame(r);
}

static int drm_on_fd(int fd, uint32_t mask, void *data)
{
    (void)mask; (void)data;
    drmEventContext ev = { .version = 2, .page_flip_handler = drm_page_flip };
    drmHandleEvent(fd, &ev);
    return 0;
}

void rail_output_frame(struct rail *r)
{
    /* request a redraw; runs now if no flip in flight, else on flip completion */
    struct rail_output *o = r->output;
    o->needs_frame = true;
    if(!o->flip_pending) render_frame(r);
}

/* switched to another VT: stop drawing and let the other VT have the screen */
void rail_output_suspend(struct rail *r)
{
    struct rail_output *o = r->output;
    if(!o) return;
    o->suspended = true;
    drmDropMaster(o->fd);
}

/* back on our VT: take the screen again, restore our mode, redraw */
void rail_output_resume(struct rail *r)
{
    struct rail_output *o = r->output;
    if(!o) return;
    if(drmSetMaster(o->fd) < 0)
        fprintf(stderr, "railwl: drmSetMaster failed: %s\n", strerror(errno));
    o->suspended = false;
    if(o->disp_bo){
        uint32_t fb = fb_for_bo(o, o->disp_bo);
        if(fb && drmModeSetCrtc(o->fd, o->crtc_id, fb, 0, 0, &o->conn->connector_id, 1, &o->mode) < 0)
            fprintf(stderr, "railwl: setcrtc on resume failed: %s\n", strerror(errno));
    }
    rail_output_frame(r);
}

/* ================================================================
   KMS setup
   ================================================================ */

static uint32_t pick_connector(int fd, drmModeRes *res)
{
    for(int i=0;i<res->count_connectors;i++){
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if(!c) continue;
        if(c->connection == DRM_MODE_CONNECTED && c->count_modes > 0){
            drmModeFreeConnector(c);
            return res->connectors[i];
        }
        drmModeFreeConnector(c);
    }
    /* nothing reports "connected": take any connector that still has modes */
    for(int i=0;i<res->count_connectors;i++){
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if(!c) continue;
        bool has_modes = c->count_modes > 0;
        drmModeFreeConnector(c);
        if(has_modes) return res->connectors[i];
    }
    return 0; /* no usable connector on this card */
}

/* can this DRM device show anything? needs a connector with a mode */
static bool card_has_display(int fd)
{
    drmModeRes *res = drmModeGetResources(fd);
    if(!res) return false;
    bool ok = false;
    for(int i=0;i<res->count_connectors && !ok;i++){
        drmModeConnector *c = drmModeGetConnector(fd, res->connectors[i]);
        if(!c) continue;
        ok = c->connection == DRM_MODE_CONNECTED && c->count_modes > 0;
        drmModeFreeConnector(c);
    }
    drmModeFreeResources(res);
    return ok;
}

/* RAIL_DRM_CARD=/dev/dri/cardN forces a device; otherwise the first card
   (card0..card7) that has a connected display wins. Laptops and hybrid setups
   often have the screen on card1, not card0. */
static int open_card(void)
{
    const char *want = getenv("RAIL_DRM_CARD");
    if(want){
        int fd = rail_lseat_open(want, O_RDWR | O_CLOEXEC);
        if(fd < 0) perror(want);
        return fd;
    }
    int fallback = -1;
    for(int i=0;i<8;i++){
        char path[32];
        snprintf(path, sizeof path, "/dev/dri/card%d", i);
        int fd = rail_lseat_open(path, O_RDWR | O_CLOEXEC);
        if(fd < 0) continue;
        if(card_has_display(fd)){
            fprintf(stderr, "railwl: using %s\n", path);
            if(fallback >= 0) rail_lseat_close(fallback);
            return fd;
        }
        if(fallback < 0) fallback = fd; else rail_lseat_close(fd);
    }
    return fallback; /* none has a display: the caller reports it */
}

int rail_output_init(struct rail *r)
{
    int fd = open_card();
    if(fd < 0){
        fprintf(stderr, "railwl: cannot open a DRM device in /dev/dri "
                        "(permissions? are you in the video group? or set RAIL_DRM_CARD)\n");
        return -1;
    }
    drmModeRes *res = drmModeGetResources(fd);
    if(!res){ perror("drmModeGetResources"); close(fd); return -1; }

    struct rail_output *o = calloc(1, sizeof *o);
    if(!o){ drmModeFreeResources(res); close(fd); return -1; }
    o->rail = r; o->fd = fd;
    o->conn_id = pick_connector(fd, res);
    o->conn = o->conn_id ? drmModeGetConnector(fd, o->conn_id) : NULL;
    if(!o->conn || o->conn->count_modes < 1){
        fprintf(stderr, "railwl: no display with a video mode on this GPU "
                        "(%d connectors) - is a monitor connected to it? "
                        "try RAIL_DRM_CARD=/dev/dri/card1\n", res->count_connectors);
        if(o->conn) drmModeFreeConnector(o->conn);
        drmModeFreeResources(res);
        free(o);
        close(fd);
        return -1;
    }
    drmModeModeInfo *m = &o->conn->modes[0];
    o->mode = *m;
    o->w = m->hdisplay; o->h = m->vdisplay;

    /* find crtc */
    drmModeEncoder *enc = drmModeGetEncoder(fd, o->conn->encoder_id);
    uint32_t crtc_id = enc ? enc->crtc_id : 0;
    if(!crtc_id){
        for(int i=0;i<res->count_crtcs;i++){
            bool ok=false;
            for(int j=0;j<o->conn->count_encoders;j++){
                drmModeEncoder *e = drmModeGetEncoder(fd, o->conn->encoders[j]);
                if(e && e->possible_crtcs & (1u<<i)) ok=true;
                if(e) drmModeFreeEncoder(e);
            }
            if(ok){ crtc_id = res->crtcs[i]; break; }
        }
    }
    o->crtc_id = crtc_id;
    o->crtc_saved = drmModeGetCrtc(fd, crtc_id);
    drmModeFreeResources(res);

    /* GPU pipeline: GBM scanout surface + EGL context + GLES2 programs */
    if(egl_init(o) < 0) return -1;
    if(gl_init() < 0) return -1;

    r->output = o;
    r->output->px = o->w/2; r->output->py = o->h/2;

    /* workspace 0 */
    o->active = o->workspaces = calloc(1, sizeof *o->active);
    o->active->index = 0;

    /* first frame: draw, swap, show */
    o->anim_busy = render_gl(r);
    eglSwapBuffers((EGLDisplay)o->egl_dpy, (EGLSurface)o->egl_surf);
    struct gbm_bo *bo = gbm_surface_lock_front_buffer(o->gbm_surf);
    if(bo){
        uint32_t fb = fb_for_bo(o, bo);
        if(fb){
            if(drmModeSetCrtc(fd, crtc_id, fb, 0, 0, &o->conn->connector_id, 1, m) < 0)
                perror("setcrtc"); /* continue anyway */
            o->disp_bo = bo;
        } else {
            gbm_surface_release_buffer(o->gbm_surf, bo);
        }
    }

    rail_output_global_init(r);

    r->drm_src = wl_event_loop_add_fd(r->loop, fd, WL_EVENT_READABLE, drm_on_fd, r);
    return 0;
}
