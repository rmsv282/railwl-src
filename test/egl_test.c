#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gbm.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

int main(void)
{
    int fd = open("/dev/dri/card0", O_RDWR);
    if(fd < 0){ puts("open card0 fail"); return 1; }
    struct gbm_device *gbm = gbm_create_device(fd);
    if(!gbm){ puts("gbm_create_device fail"); return 1; }

    EGLDisplay dpy = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gp =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    printf("getPlatformDisplayEXT=%p\n", (void*)gp);
    if(gp) dpy = gp(EGL_PLATFORM_GBM_KHR, gbm, NULL);
    if(dpy == EGL_NO_DISPLAY){
        printf("platform display fail 0x%x, trying eglGetDisplay\n", eglGetError());
        dpy = eglGetDisplay((EGLNativeDisplayType)(uintptr_t)gbm);
    }
    if(dpy == EGL_NO_DISPLAY){ printf("display fail 0x%x\n", eglGetError()); return 1; }

    EGLint maj = 0, min = 0;
    if(!eglInitialize(dpy, &maj, &min)){ printf("init fail 0x%x\n", eglGetError()); return 1; }
    printf("egl %d.%d  APIs=%s\n", (int)maj, (int)min, eglQueryString(dpy, EGL_CLIENT_APIS));

    eglBindAPI(EGL_OPENGL_ES_API);

    EGLint attr[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfgs[64]; EGLint n = 0;
    if(!eglChooseConfig(dpy, attr, cfgs, 64, &n) || n < 1){
        printf("chooseconfig fail 0x%x n=%d\n", eglGetError(), (int)n);
        return 1;
    }
    EGLConfig cfg = NULL;
    EGLint cid = 0;
    for(EGLint i = 0; i < n; i++){
        EGLint vid = 0;
        eglGetConfigAttrib(dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid);
        if((uint32_t)vid == GBM_FORMAT_XRGB8888){
            cfg = cfgs[i];
            eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_ID, &cid);
            break;
        }
    }
    if(!cfg){ printf("no XR24 config of %d\n", (int)n); return 1; }
    printf("picked config id=%d visual=XR24 (0x%x) of %d configs\n",
           (int)cid, (unsigned)GBM_FORMAT_XRGB8888, (int)n);

    EGLint ctxattr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctxattr);
    if(ctx == EGL_NO_CONTEXT){ printf("context fail 0x%x\n", eglGetError()); return 1; }

    struct gbm_surface *s = gbm_surface_create(gbm, 1360, 768,
                                GBM_FORMAT_XRGB8888,
                                GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING);
    if(!s){ puts("gbm_surface_create fail"); return 1; }

    EGLSurface es = eglCreateWindowSurface(dpy, cfg, (EGLNativeWindowType)s, NULL);
    printf("window surface=%p err=0x%x\n", (void*)es,
           es == EGL_NO_SURFACE ? eglGetError() : 0);
    if(es == EGL_NO_SURFACE){
        /* retry: maybe a plain XRGB config without visual filter behaves differently */
        EGLint attr2[] = {
            EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
            EGL_NONE
        };
        EGLConfig c2; EGLint n2 = 0;
        eglChooseConfig(dpy, attr2, &c2, 1, &n2);
        EGLint v2 = 0;
        if(n2){ eglGetConfigAttrib(dpy, c2, EGL_NATIVE_VISUAL_ID, &v2); }
        printf("unfiltered: n=%d visual=0x%x\n", (int)n2, (unsigned)v2);
        if(n2){
            es = eglCreateWindowSurface(dpy, c2, (EGLNativeWindowType)s, NULL);
            printf("retry surface=%p err=0x%x\n", (void*)es,
                   es == EGL_NO_SURFACE ? eglGetError() : 0);
        }
        if(es == EGL_NO_SURFACE) return 1;
    }
    if(!eglMakeCurrent(dpy, es, es, ctx)){
        printf("makecurrent fail 0x%x\n", eglGetError());
        return 1;
    }
    printf("renderer=%s\nversion=%s\nextensions_has_bgra=%d\n",
           (const char*)glGetString(GL_RENDERER),
           (const char*)glGetString(GL_VERSION),
           strstr((const char*)glGetString(GL_EXTENSIONS), "GL_EXT_bgra") ? 1 : 0);
    puts("OK");
    return 0;
}
