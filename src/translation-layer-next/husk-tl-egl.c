/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-egl.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "husk-tl-bionic.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

void *tl_nwindow_native(void *window);
int tl_nwindow_width(void *window);
void *tl_nwindow_texture(void *window);
void tl_nwindow_post(void *window, const void *rgba, int w, int h, bool flip);
int tl_nwindow_height(void *window);

typedef void *EGLDisplay, *EGLSurface, *EGLContext, *EGLConfig, *EGLNativeWindowType;
typedef int32_t EGLint;
typedef unsigned EGLBoolean, EGLenum;
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_PLATFORM_ANGLE_ANGLE 0x3202
#define EGL_PLATFORM_ANGLE_TYPE_ANGLE 0x3203
#define EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE 0x3489
#define EGL_FALSE 0
#define EGL_TRUE 1

static struct {
    char egl_path[600], gles_path[600];
    void *egl, *gles;
    bool ready;
    char frame_dir[512];
    int frame_every;
    bool latest_only;           /* overwrite latest.bmp instead of keeping every frame */
    atomic_ulong presented;
    EGLDisplay display;
    pthread_mutex_t lock;
} E = { .lock = PTHREAD_MUTEX_INITIALIZER };

/* ANGLE's entry points */
static void *(*a_eglGetProcAddress)(const char *);
static EGLDisplay (*a_eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *);
static EGLBoolean (*a_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static EGLBoolean (*a_eglTerminate)(EGLDisplay);
static const char *(*a_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean (*a_eglGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*a_eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
static EGLBoolean (*a_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLContext (*a_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean (*a_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLSurface (*a_eglCreateWindowSurface)(EGLDisplay, EGLConfig, EGLNativeWindowType, const EGLint *);
static EGLSurface (*a_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean (*a_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean (*a_eglQuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint *);
static EGLBoolean (*a_eglSurfaceAttrib)(EGLDisplay, EGLSurface, EGLint, EGLint);
static EGLBoolean (*a_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean (*a_eglSwapBuffers)(EGLDisplay, EGLSurface);
static EGLBoolean (*a_eglSwapInterval)(EGLDisplay, EGLint);
static EGLContext (*a_eglGetCurrentContext)(void);
static EGLSurface (*a_eglGetCurrentSurface)(EGLint);
static EGLDisplay (*a_eglGetCurrentDisplay)(void);
static EGLBoolean (*a_eglBindAPI)(EGLenum);
static EGLenum (*a_eglQueryAPI)(void);
static EGLint (*a_eglGetError)(void);
static EGLBoolean (*a_eglWaitGL)(void);
static EGLBoolean (*a_eglWaitNative)(EGLint);
static EGLBoolean (*a_eglReleaseThread)(void);
static void (*a_glReadPixels)(int, int, int, int, unsigned, unsigned, void *);
static void (*a_glGetIntegerv)(unsigned, int *);
static void (*a_glBindFramebuffer)(unsigned, unsigned);
static void (*a_glPixelStorei)(unsigned, int);

static void *gl_proc(const char *name)
{
    void *p = a_eglGetProcAddress ? a_eglGetProcAddress(name) : NULL;
    if (!p && E.gles) p = dlsym(E.gles, name);
    return p;
}

static unsigned gl_proc_get_error(void)
{
    static unsigned (*fn)(void);
    if (!fn) fn = gl_proc("glGetError");
    return fn ? fn() : 0;
}

static void adapt_init(void);
#define LOAD(lib, name) do { a_##name = dlsym(lib, #name); if (!a_##name) tl_log_line("egl: ANGLE does not export " #name); } while (0)

bool tl_egl_init(const char *egl_path, const char *gles_path, const char *frame_dir, int frame_every)
{
    pthread_mutex_lock(&E.lock);
    if (E.ready) { pthread_mutex_unlock(&E.lock); return true; }
    snprintf(E.egl_path, sizeof(E.egl_path), "%s", egl_path);
    snprintf(E.gles_path, sizeof(E.gles_path), "%s", gles_path ? gles_path : egl_path);
    E.egl = dlopen(egl_path, RTLD_NOW | RTLD_LOCAL);
    if (!E.egl) { tl_log_line("egl: cannot load %s: %s", egl_path, dlerror()); pthread_mutex_unlock(&E.lock); return false; }
    E.gles = gles_path ? dlopen(gles_path, RTLD_NOW | RTLD_LOCAL) : E.egl;
    if (!E.gles) { tl_log_line("egl: cannot load %s: %s", gles_path, dlerror()); pthread_mutex_unlock(&E.lock); return false; }
    void *g = E.egl;
    LOAD(g, eglGetProcAddress); LOAD(g, eglInitialize); LOAD(g, eglTerminate); LOAD(g, eglQueryString); LOAD(g, eglGetConfigs);
    LOAD(g, eglChooseConfig); LOAD(g, eglGetConfigAttrib); LOAD(g, eglCreateContext); LOAD(g, eglDestroyContext);
    LOAD(g, eglCreateWindowSurface); LOAD(g, eglCreatePbufferSurface); LOAD(g, eglDestroySurface); LOAD(g, eglQuerySurface);
    LOAD(g, eglSurfaceAttrib); LOAD(g, eglMakeCurrent); LOAD(g, eglSwapBuffers); LOAD(g, eglSwapInterval);
    LOAD(g, eglGetCurrentContext); LOAD(g, eglGetCurrentSurface); LOAD(g, eglGetCurrentDisplay); LOAD(g, eglBindAPI);
    LOAD(g, eglQueryAPI); LOAD(g, eglGetError); LOAD(g, eglWaitGL); LOAD(g, eglWaitNative); LOAD(g, eglReleaseThread);
    a_eglGetPlatformDisplayEXT = a_eglGetProcAddress ? a_eglGetProcAddress("eglGetPlatformDisplayEXT") : NULL;
    if (!a_eglGetPlatformDisplayEXT) { tl_log_line("egl: ANGLE has no eglGetPlatformDisplayEXT"); pthread_mutex_unlock(&E.lock); return false; }
    /* GLES functions this layer itself calls (frame capture) */
    a_glReadPixels = a_eglGetProcAddress("glReadPixels");
    a_glGetIntegerv = a_eglGetProcAddress("glGetIntegerv");
    a_glBindFramebuffer = a_eglGetProcAddress("glBindFramebuffer");
    a_glPixelStorei = a_eglGetProcAddress("glPixelStorei");
    if (frame_dir) { snprintf(E.frame_dir, sizeof(E.frame_dir), "%s", frame_dir); E.latest_only = frame_every < 0; E.frame_every = frame_every != 0 ? (frame_every < 0 ? -frame_every : frame_every) : 60; }
    E.ready = true;
    adapt_init();
    pthread_mutex_unlock(&E.lock);
    tl_log_line("egl: ANGLE loaded%s%s (core UBO: range=%p, base=%p, blockbind=%p)",
                frame_dir ? ", off-screen, frames to " : "", frame_dir ? frame_dir : "",
                gl_proc("glBindBufferRange"), gl_proc("glBindBufferBase"), gl_proc("glUniformBlockBinding"));
    return true;
}

unsigned long tl_egl_frames_presented(void) { return atomic_load(&E.presented); }

/* ------------------------------------------------------------ EGL wrappers */

static EGLDisplay w_eglGetDisplay(void *native)
{
    (void)native;
    pthread_mutex_lock(&E.lock);
    if (!E.display && E.ready) {
        const EGLint attribs[] = { EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, EGL_NONE };
        E.display = a_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL, attribs);
        tl_log_line("egl: display %p (ANGLE over Metal)", E.display);
    }
    EGLDisplay d = E.display;
    pthread_mutex_unlock(&E.lock);
    return d;
}

#define PASS_BOOL(name, params, args) static EGLBoolean w_##name params { return a_##name args; }
PASS_BOOL(eglInitialize, (EGLDisplay d, EGLint *a, EGLint *b), (d, a, b))
/* eglTerminate of the display everything here shares (the app's GL, Husk's own render thread): Android's counts initializations and
 * tears down at the last; here Husk holds one for as long as it runs, so a library terminating its own use (Castle's GPU probe in X)
 * leaves the display, and everyone else's surfaces, standing */
static EGLBoolean w_eglTerminate(EGLDisplay d)
{
    if (d && d == E.display) { if (getenv("TL_EGL_TRACE")) tl_log_line("egl: eglTerminate of the shared display: kept"); return EGL_TRUE; }
    return a_eglTerminate(d);
}
PASS_BOOL(eglGetConfigs, (EGLDisplay d, EGLConfig *c, EGLint n, EGLint *r), (d, c, n, r))
/* ANGLE's configs with depth and stencil all have alpha. Android's also come without (RGBX), and an engine that asks for no alpha
 * picks one of those (King's): such a request also gets each 8-bit RGBA config again as a "no alpha" alias -- its handle with the
 * low bit set, which reports alpha 0 and is the real config everywhere else. */
static inline EGLConfig cfg_real(EGLConfig c) { return (EGLConfig)((uintptr_t)c & ~(uintptr_t)1); }
static inline bool cfg_alias(EGLConfig c) { return ((uintptr_t)c & 1) != 0; }
static EGLBoolean w_eglChooseConfig(EGLDisplay d, const EGLint *at, EGLConfig *c, EGLint n, EGLint *r)
{
    /* Android-only attributes (EGL_RECORDABLE_ANDROID, EGL_FRAMEBUFFER_TARGET_ANDROID) match nothing in ANGLE: every
     * config would be refused over a property it cannot have. They are dropped from the request. */
    EGLint filtered[128]; int nf = 0;
    bool alpha_wanted = false;
    for (int i = 0; at && at[i] != EGL_NONE && nf < 124; i += 2) {
        if (at[i] == 0x3142 || at[i] == 0x3147) continue;
        if (at[i] == 0x3021 /* EGL_ALPHA_SIZE */ && at[i + 1] > 0) alpha_wanted = true;
        filtered[nf++] = at[i]; filtered[nf++] = at[i + 1];
    }
    filtered[nf] = EGL_NONE;
    static EGLConfig tmp[256];
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    EGLint k = 0;
    EGLBoolean ok = a_eglChooseConfig(d, at ? filtered : NULL, tmp, 256, &k);
    EGLint total = 0;
    if (ok) {
        EGLConfig all[512];
        for (int i = 0; i < k; i++) all[total++] = tmp[i];
        static int no_alias = -1;
        if (no_alias < 0) no_alias = getenv("TL_EGL_NO_ALIAS") ? 1 : 0;
        for (int i = 0; i < k && !alpha_wanted && !no_alias && total < 512; i++) {
            EGLint a = 0, rr = 0, g = 0, b = 0;
            a_eglGetConfigAttrib(d, tmp[i], 0x3021, &a); a_eglGetConfigAttrib(d, tmp[i], 0x3024, &rr);
            a_eglGetConfigAttrib(d, tmp[i], 0x3023, &g); a_eglGetConfigAttrib(d, tmp[i], 0x3022, &b);
            if (a == 8 && rr == 8 && g == 8 && b == 8) all[total++] = (EGLConfig)((uintptr_t)tmp[i] | 1);
        }
        if (c) { EGLint m = total < n ? total : n; for (int i = 0; i < m; i++) c[i] = all[i]; total = m; }
        if (r) *r = total;
    }
    pthread_mutex_unlock(&mu);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace || !ok || (r && *r == 0)) {
        char buf[512]; size_t kk = 0;
        for (int i = 0; at && at[i] != EGL_NONE && i < 60 && kk + 24 < sizeof(buf); i += 2) kk += (size_t)snprintf(buf + kk, sizeof(buf) - kk, " %#x=%d", at[i], at[i + 1]);
        tl_log_line("egl: eglChooseConfig(%s ) -> %s, %d config(s)", buf, ok ? "true" : "false", r ? *r : -1);
    }
    return ok;
}
static EGLBoolean w_eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint at, EGLint *v)
{
    if (at == 0x3142 /* EGL_RECORDABLE_ANDROID */) { if (v) *v = 1; return EGL_TRUE; }
    bool alias = cfg_alias(c);
    EGLBoolean ok = a_eglGetConfigAttrib(d, cfg_real(c), at, v);
    if (ok && alias && v) { if (at == 0x3021 /* ALPHA_SIZE */) *v = 0; else if (at == 0x3020 /* BUFFER_SIZE */) *v = 24; else if (at == 0x302E /* NATIVE_VISUAL_ID */) *v = 2; }
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace) tl_log_line("egl: eglGetConfigAttrib(%p, %#x) -> %s %d", c, at, ok ? "true" : "false", v ? *v : -1);
    return ok;
}
PASS_BOOL(eglDestroyContext, (EGLDisplay d, EGLContext c), (d, c))
PASS_BOOL(eglQuerySurface, (EGLDisplay d, EGLSurface s, EGLint at, EGLint *v), (d, s, at, v))
PASS_BOOL(eglSurfaceAttrib, (EGLDisplay d, EGLSurface s, EGLint at, EGLint v), (d, s, at, v))
static EGLBoolean w_eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    EGLBoolean ok = E.ready ? a_eglMakeCurrent(d, dr, rd, c) : EGL_FALSE;
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace || !ok) {
        char who[32] = ""; pthread_getname_np(pthread_self(), who, sizeof(who));
        const void *lr = __builtin_return_address(0); const char *lib = NULL; const void *base = NULL;
        const char *sym = tl_ld_symbol_at(lr, &lib, &base);
        tl_log_line("egl: eglMakeCurrent(draw %p, read %p, context %p) -> %s%s%#x  [thread '%s', called from %s %s+%#lx]", dr, rd, c, ok ? "true" : "false", ok ? "" : ", eglGetError ", ok ? 0 : (E.ready ? a_eglGetError() : 0x3001),
                    who, lib ? lib : "?", sym ? sym : "?", base ? (unsigned long)((const char *)lr - (const char *)base) : 0ul);
    }
    return ok;
}
PASS_BOOL(eglSwapInterval, (EGLDisplay d, EGLint i), (d, i))
PASS_BOOL(eglBindAPI, (EGLenum api), (api))
PASS_BOOL(eglWaitGL, (void), ())
PASS_BOOL(eglWaitNative, (EGLint e), (e))
PASS_BOOL(eglReleaseThread, (void), ())

static const char *w_eglQueryString(EGLDisplay d, EGLint name) { return a_eglQueryString(d, name); }
static bool g_es31_shim;           /* defined with the rest of the shim, below */
static EGLContext w_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *at)
{
    /* A game that insists on ES 3.1 or 3.2 (Unreal Engine tries 3.2, then 3.1) is given the 3.0 context ANGLE can make: the version it is then told is the shim's. */
    EGLint patched[40];
    if (g_es31_shim && at) {
        int n = 0;
        for (; at[n] != EGL_NONE && n < 36; n += 2) {
            patched[n] = at[n]; patched[n + 1] = at[n + 1];
            if (at[n] == 0x30fb /* EGL_CONTEXT_MINOR_VERSION */ && at[n + 1] > 0) patched[n + 1] = 0;
        }
        patched[n] = EGL_NONE;
        at = patched;
    }
    EGLContext ctx = a_eglCreateContext(d, cfg_real(c), share, at);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace || !ctx) {
        char buf[200]; size_t k = 0;
        for (int i = 0; at && at[i] != EGL_NONE && i < 20 && k + 24 < sizeof(buf); i += 2) k += (size_t)snprintf(buf + k, sizeof(buf) - k, " %#x=%d", at[i], at[i + 1]);
        tl_log_line("egl: eglCreateContext(share %p,%s ) -> %p%s", share, buf, ctx, ctx ? "" : " (failed)");
    }
    return ctx;
}
static EGLContext w_eglGetCurrentContext(void) { return a_eglGetCurrentContext(); }
static EGLSurface w_eglGetCurrentSurface(EGLint w) { return a_eglGetCurrentSurface(w); }
static EGLDisplay w_eglGetCurrentDisplay(void) { return a_eglGetCurrentDisplay(); }
static EGLenum w_eglQueryAPI(void) { return a_eglQueryAPI(); }
static EGLint w_eglGetError(void) { return E.ready ? a_eglGetError() : 0x3001; }

/* The guest's ANativeWindow: a Metal layer on the phone, an off-screen buffer for tests. */
static bool g_offscreen_windows;
/* A game that draws with Vulkan on the window's layer still makes an OpenGL ES context first, to read what the device offers. The layer is MoltenVK's, so ANGLE gets a buffer instead. */
void tl_egl_offscreen_windows(bool on) { g_offscreen_windows = on; }
/* surfaces made on a TextureView's window: off-screen, read back into its SurfaceTexture at each swap */
#define MAX_TEX_SURFACES 16
static struct { EGLSurface s; void *win; } g_tex_surf[MAX_TEX_SURFACES];
static pthread_mutex_t g_tex_lock = PTHREAD_MUTEX_INITIALIZER;
static void *tex_window_of(EGLSurface s)
{
    void *w = NULL;
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < MAX_TEX_SURFACES; i++) if (g_tex_surf[i].s == s && s) { w = g_tex_surf[i].win; break; }
    pthread_mutex_unlock(&g_tex_lock);
    return w;
}
/* The Java runtime's compositor (husk-tl-dvm-android.c), where it is linked: it holds the screen's surface, so an app that makes
 * an EGL window of its own (SDL, Unity on a SurfaceView) draws off screen and each swap hands the frame to it, to be put on the
 * screen under the app's views. Two window surfaces on one layer is what a phone refuses (EGL_BAD_ALLOC). */
__attribute__((weak)) bool tl_javaapp_compositing(void);
__attribute__((weak)) void tl_javaapp_app_pixels(const void *px, int w, int h);
#define MAX_COMP_SURFACES 8
static EGLSurface g_comp_surf[MAX_COMP_SURFACES];
static bool comp_surface(EGLSurface s)
{
    bool r = false;
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < MAX_COMP_SURFACES && s; i++) if (g_comp_surf[i] == s) r = true;
    pthread_mutex_unlock(&g_tex_lock);
    return r;
}
/* The current frame of a surface, read back (bottom-up RGBA, in a buffer of this thread's). */
static const uint8_t *read_back(EGLDisplay d, EGLSurface s, EGLint *pw, EGLint *ph)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w); a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return NULL;
    static __thread uint8_t *buf; static __thread size_t cap;
    size_t need = (size_t)w * h * 4;
    if (need > cap) { free(buf); buf = malloc(need); cap = buf ? need : 0; }
    if (!buf) return NULL;
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, buf);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    *pw = w; *ph = h;
    return buf;
}
static EGLSurface w_eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg, void *win, const EGLint *at)
{
    cfg = cfg_real(cfg);
    if (tl_nwindow_texture(win)) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (s) {
            pthread_mutex_lock(&g_tex_lock);
            for (int i = 0; i < MAX_TEX_SURFACES; i++) if (!g_tex_surf[i].s) { g_tex_surf[i].s = s; g_tex_surf[i].win = win; break; }
            pthread_mutex_unlock(&g_tex_lock);
        }
        tl_log_line("egl: TextureView window %dx%d -> off-screen surface %p", pb[1], pb[3], s);
        return s;
    }
    if (tl_javaapp_compositing && tl_javaapp_compositing()) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (s) {
            pthread_mutex_lock(&g_tex_lock);
            for (int i = 0; i < MAX_COMP_SURFACES; i++) if (!g_comp_surf[i]) { g_comp_surf[i] = s; break; }
            pthread_mutex_unlock(&g_tex_lock);
        }
        tl_log_line("egl: window %dx%d -> surface %p, composited under the app's views", pb[1], pb[3], s);
        return s;
    }
    if (E.frame_dir[0] || g_offscreen_windows) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (!s) {
            /* the config the game chose may draw only to windows: the closest one that also makes buffers */
            EGLint r = 8, g = 8, b = 8, a = 0, dp = 0, st = 0, rt = 0;
            a_eglGetConfigAttrib(d, cfg, 0x3024 /* EGL_RED_SIZE */, &r); a_eglGetConfigAttrib(d, cfg, 0x3023 /* EGL_GREEN_SIZE */, &g); a_eglGetConfigAttrib(d, cfg, 0x3022 /* EGL_BLUE_SIZE */, &b);
            a_eglGetConfigAttrib(d, cfg, 0x3021 /* EGL_ALPHA_SIZE */, &a); a_eglGetConfigAttrib(d, cfg, 0x3025 /* EGL_DEPTH_SIZE */, &dp); a_eglGetConfigAttrib(d, cfg, 0x3026 /* EGL_STENCIL_SIZE */, &st);
            a_eglGetConfigAttrib(d, cfg, 0x3040 /* EGL_RENDERABLE_TYPE */, &rt);
            tl_log_line("egl: off-screen surface for config %p failed (%#x); rgba %d%d%d%d depth %d stencil %d", cfg, a_eglGetError(), r, g, b, a, dp, st);
        }
        tl_log_line("egl: window %dx%d -> off-screen surface %p", pb[1], pb[3], s);
        return s;
    }
    void *layer = tl_nwindow_native(win);
    EGLSurface s = a_eglCreateWindowSurface(d, cfg, layer, at);
    tl_log_line("egl: window surface %p on layer %p (%dx%d), eglGetError %#x", s, layer, tl_nwindow_width(win), tl_nwindow_height(win), a_eglGetError());
    return s;
}
static EGLBoolean w_eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < MAX_TEX_SURFACES; i++) if (g_tex_surf[i].s == s) g_tex_surf[i].s = NULL;
    bool comp = false;
    for (int i = 0; i < MAX_COMP_SURFACES && s; i++) if (g_comp_surf[i] == s) { g_comp_surf[i] = NULL; comp = true; }
    pthread_mutex_unlock(&g_tex_lock);
    if (comp && tl_javaapp_app_pixels) tl_javaapp_app_pixels(NULL, 0, 0);
    return a_eglDestroySurface(d, s);
}
static EGLSurface w_eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *at) { return a_eglCreatePbufferSurface(d, cfg_real(c), at); }

/* A frame as a BMP: bottom-up BGR from GL's bottom-up RGBA, so no flip is needed. */
static void save_frame(EGLDisplay d, EGLSurface s, unsigned long n)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w);
    a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return;
    uint8_t *rgba = malloc((size_t)w * h * 4);
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, rgba);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    char path[700], final_path[700] = "";
    if (E.latest_only) {
        snprintf(final_path, sizeof(final_path), "%s/latest.bmp", E.frame_dir);
        snprintf(path, sizeof(path), "%s/latest.tmp", E.frame_dir);
    } else {
        snprintf(path, sizeof(path), "%s/frame-%05lu.bmp", E.frame_dir, n);
    }
    FILE *f = fopen(path, "wb");
    if (f) {
        uint32_t rowbytes = ((uint32_t)w * 3 + 3) & ~3u, size = 54 + rowbytes * (uint32_t)h;
        uint8_t hdr[54] = { 'B', 'M' };
        memcpy(hdr + 2, &size, 4); uint32_t off = 54; memcpy(hdr + 10, &off, 4);
        uint32_t dib = 40; memcpy(hdr + 14, &dib, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
        hdr[26] = 1; hdr[28] = 24; uint32_t img = rowbytes * (uint32_t)h; memcpy(hdr + 34, &img, 4);
        fwrite(hdr, 1, 54, f);
        uint8_t *row = calloc(1, rowbytes);
        for (int y = 0; y < h; y++) {
            const uint8_t *src = rgba + (size_t)y * w * 4;
            for (int x = 0; x < w; x++) { row[x * 3] = src[x * 4 + 2]; row[x * 3 + 1] = src[x * 4 + 1]; row[x * 3 + 2] = src[x * 4]; }
            fwrite(row, 1, rowbytes, f);
        }
        free(row); fclose(f);
        if (final_path[0]) rename(path, final_path);
    }
    free(rgba);
}

#define GLT_RING 256
static atomic_ulong g_glt_n, g_glt_draws, g_glt_uploads;
void tl_egl_gl_histogram(char *out, size_t cap);
void tl_egl_recent_calls(char *out, size_t n, int count);
static void tex_swap(EGLDisplay d, EGLSurface s, void *win)
{
    EGLint w = 0, h = 0;
    a_eglQuerySurface(d, s, EGL_WIDTH, &w); a_eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0 || !a_glReadPixels) return;
    static __thread uint8_t *buf; static __thread size_t cap;
    size_t need = (size_t)w * h * 4;
    if (need > cap) { free(buf); buf = malloc(need); cap = buf ? need : 0; }
    if (!buf) return;
    int old_fb = 0, old_pack = 0;
    a_glGetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_fb);
    a_glGetIntegerv(0x0D05 /* GL_PACK_ALIGNMENT */, &old_pack);
    a_glBindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    a_glPixelStorei(0x0D05, 1);
    a_glReadPixels(0, 0, w, h, 0x1908 /* GL_RGBA */, 0x1401 /* GL_UNSIGNED_BYTE */, buf);
    a_glPixelStorei(0x0D05, old_pack);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_fb);
    tl_nwindow_post(win, buf, w, h, true);
}
static EGLBoolean w_eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    void *texwin = tex_window_of(s);
    if (texwin) { tex_swap(d, s, texwin); return EGL_TRUE; }
    unsigned long n = atomic_fetch_add(&E.presented, 1) + 1;
    if (comp_surface(s)) {
        EGLint w = 0, h = 0;
        const uint8_t *px = read_back(d, s, &w, &h);
        if (px && tl_javaapp_app_pixels) tl_javaapp_app_pixels(px, w, h);
        if (n <= 3 || n % 600 == 0) {
            unsigned err = gl_proc_get_error();
            if (err) tl_log_line("gl: swap #%lu (composited) GL error: %#x", n, err);
            else tl_log_line("egl: swap #%lu (composited)", n);
        }
        return EGL_TRUE;
    }
    if (n <= 3 || n % 600 == 0) {
        if (getenv("TL_GL_TRACE")) tl_log_line("egl: swap #%lu (%lu GL calls, %lu draws, %lu uploads so far)", n, (unsigned long)atomic_load(&g_glt_n), (unsigned long)atomic_load(&g_glt_draws), (unsigned long)atomic_load(&g_glt_uploads));
        else tl_log_line("egl: swap #%lu", n);
    }
    if ((n == 300 || n == 3000) && getenv("TL_GL_TRACE")) {
        static char gl[GLT_RING * 40];
        tl_egl_recent_calls(gl, sizeof(gl), GLT_RING);
        fprintf(stderr, "egl: the calls before swap #%lu: %s\n", n, gl);
        static char hist[1024 * 48];
        tl_egl_gl_histogram(hist, sizeof(hist));
        fprintf(stderr, "egl: calls so far, by function: %s\n", hist);
    }
    if (E.frame_dir[0] && (n % (unsigned long)E.frame_every == 0 || n <= 3)) save_frame(d, s, n);
    return E.frame_dir[0] ? EGL_TRUE : a_eglSwapBuffers(d, s);
}

/* The damage-region swaps (Flutter's engine, HWUI): the same as a whole swap here -- through the wrapper, so a frame reaches the
 * compositor, the frame dump, or the screen as a plain swap would. */
static EGLBoolean w_eglSwapBuffersWithDamage(EGLDisplay d, EGLSurface s, const EGLint *rects, EGLint n) { (void)rects; (void)n; return w_eglSwapBuffers(d, s); }
static EGLBoolean w_eglSetDamageRegion(EGLDisplay d, EGLSurface s, EGLint *rects, EGLint n) { (void)d; (void)s; (void)rects; (void)n; return EGL_TRUE; }

/* Android extensions this ANGLE does not have: Swappy and Unity probe for them. */
static EGLBoolean w_eglPresentationTimeANDROID(EGLDisplay d, EGLSurface s, int64_t t) { (void)d; (void)s; (void)t; return EGL_TRUE; }
static EGLBoolean w_eglGetNextFrameIdANDROID(EGLDisplay d, EGLSurface s, uint64_t *id) { (void)d; (void)s; if (id) *id = 0; return EGL_FALSE; }
static EGLBoolean w_eglGetFrameTimestampsANDROID(EGLDisplay d, EGLSurface s, uint64_t id, EGLint n, const EGLint *t, int64_t *v) { (void)d; (void)s; (void)id; (void)n; (void)t; (void)v; return EGL_FALSE; }
static EGLBoolean w_eglGetCompositorTimingANDROID(EGLDisplay d, EGLSurface s, EGLint n, const EGLint *t, int64_t *v) { (void)d; (void)s; (void)n; (void)t; (void)v; return EGL_FALSE; }

/* ------------------------------------------------- GLES: stack-argument fixes */

/*
 * AAPCS64 gives each stack argument its own 8-byte slot; Apple's ABI packs small ones. A call
 * with two 4-byte stack arguments therefore lays them out differently, and the callee reads
 * the second from the wrong place. Each adapter takes its stack arguments as 8-byte values,
 * which is how the guest wrote them, and passes them on properly typed.
 */
static void (*r_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static void w_glBlitFramebuffer(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, uint64_t mask, uint64_t filter)
{ r_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, (unsigned)mask, (unsigned)filter); }

static void (*r_glTexImage3D)(unsigned, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexImage3D(unsigned target, int level, int internalformat, int width, int height, int depth, int border, unsigned format, uint64_t type, const void *pixels)
{
    if (!r_glTexImage3D) r_glTexImage3D = gl_proc("glTexImage3D");
    if (r_glTexImage3D) r_glTexImage3D(target, level, internalformat, width, height, depth, border, format, (unsigned)type, pixels);
    tl_log_line("gl: glTexImage3D(target %#x, lvl %d, intfmt %#x, %dx%dx%d, fmt %#x, type %#x) -> err %#x",
                target, level, internalformat, width, height, depth, format, (unsigned)type, gl_proc_get_error());
}

static void (*r_glCompressedTexImage3D)(unsigned, int, unsigned, int, int, int, int, int, const void *);
static void w_glCompressedTexImage3D(unsigned target, int level, unsigned internalformat, int width, int height, int depth, int border, int imageSize, const void *data)
{
    if (!r_glCompressedTexImage3D) r_glCompressedTexImage3D = gl_proc("glCompressedTexImage3D");
    if (r_glCompressedTexImage3D) r_glCompressedTexImage3D(target, level, internalformat, width, height, depth, border, imageSize, data);
    tl_log_line("gl: glCompressedTexImage3D(target %#x, lvl %d, intfmt %#x, %dx%dx%d, sz %d) -> err %#x",
                target, level, internalformat, width, height, depth, imageSize, gl_proc_get_error());
}

static void (*r_glTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void (*r_glTexStorage3D)(unsigned, int, unsigned, int, int, int);
static void w_glTexStorage3D(unsigned target, int levels, unsigned internalformat, int width, int height, int depth)
{
    if (!r_glTexStorage3D) r_glTexStorage3D = gl_proc("glTexStorage3D");
    if (r_glTexStorage3D) r_glTexStorage3D(target, levels, internalformat, width, height, depth);
    tl_log_line("gl: glTexStorage3D(target %#x, lvls %d, intfmt %#x, %dx%dx%d) -> err %#x",
                target, levels, internalformat, width, height, depth, gl_proc_get_error());

    /* GoreBox 2D Texture Array Auto-Populator:
     * - depth == 5: Characters & NPCs (_RoleMaps 0x8C43, _RoleBumps 0x8058).
     *   Populate 5 body part slices with human skin / clothing colors and flat tangent normal.
     * - depth == 2: Terrain (_TerrainAlbedo 0x8C43, _TerrainNormal 0x8058, _TerrainSpec 0x8058, _TerrainHeight 0x8229).
     *   Populate slice 0 (dirt road) with natural earthy dirt road brown, slice 1 with natural ground brown/green,
     *   normal map with flat tangent normals (128, 128, 255), spec with low specularity (30), and height with neutral 128!
     */
    if (target == 0x8C1A /* GL_TEXTURE_2D_ARRAY */ && depth >= 2 && width >= 4 && height >= 4) {
        if (!r_glTexSubImage3D) r_glTexSubImage3D = gl_proc("glTexSubImage3D");
        if (r_glTexSubImage3D) {
            bool is_r8 = (internalformat == 0x8229 /* GL_R8 */);
            unsigned upload_fmt = is_r8 ? 0x1903 /* GL_RED */ : 0x1908 /* GL_RGBA */;
            int bpp = is_r8 ? 1 : 4;
            size_t slice_bytes = (size_t)width * height * (size_t)bpp;
            uint8_t *slice_data = malloc(slice_bytes);
            if (slice_data) {
                if (depth >= 5) {
                    /* Character & NPC body parts (depth 5: Head, Torso, Arms, Legs, Shoes) */
                    bool is_bump = (internalformat == 0x8058 /* GL_RGBA8 */);
                    for (int s = 0; s < depth; s++) {
                        uint8_t r = 235, g = 195, b = 165, a = 255;
                        if (is_bump) {
                            /* Flat tangent-space normal (0, 0, 1):
                             * Set a = 128 (0.5) so that both standard RGB unpack (r,g,b) and Unity DXT5nm
                             * unpack (normal.x = a * 2 - 1, normal.y = g * 2 - 1) decode to (0, 0, 1).
                             * When a was 255, DXT5nm decoded normal.x = +1.0, tilting all normals 90 degrees!
                             */
                            r = 128; g = 128; b = 255; a = 128;
                        } else {
                            switch (s) {
                                case 0: r = 215; g = 185; b = 165; break; /* Head / Face skin */
                                case 1: r = 55;  g = 95;  b = 165; break; /* Torso / Navy Shirt */
                                case 2: r = 215; g = 185; b = 165; break; /* Arms / Hands skin */
                                case 3: r = 45;  g = 55;  b = 75;  break; /* Legs / Dark Jeans */
                                case 4: r = 35;  g = 35;  b = 40;  break; /* Shoes / Boots */
                                default: r = 200; g = 200; b = 200; break;
                            }
                        }
                        uint32_t pixel = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
                        uint32_t *p32 = (uint32_t *)slice_data;
                        size_t npix = (size_t)width * height;
                        for (size_t p = 0; p < npix; p++) p32[p] = pixel;
                        r_glTexSubImage3D(target, 0, 0, 0, s, width, height, 1, upload_fmt, 0x1401 /* GL_UNSIGNED_BYTE */, slice_data);
                    }
                } else if (depth == 2) {
                    /* Terrain 2D Array Textures (depth 2: slice 0 = dirt road / path, slice 1 = ground / terrain) */
                    for (int s = 0; s < depth; s++) {
                        if (is_r8) {
                            memset(slice_data, 128, slice_bytes); /* Neutral displacement height */
                        } else if (internalformat == 0x8058 /* GL_RGBA8 */) {
                            if (width >= 512) {
                                /* _TerrainNormal: Flat tangent-space normal (128, 128, 255, 128) */
                                uint32_t norm_pixel = 128 | (128 << 8) | (255 << 16) | (128u << 24);
                                uint32_t *p32 = (uint32_t *)slice_data;
                                size_t npix = (size_t)width * height;
                                for (size_t p = 0; p < npix; p++) p32[p] = norm_pixel;
                            } else {
                                /* _TerrainSpec: Low specularity matte dirt/ground */
                                uint32_t spec_pixel = 30 | (30 << 8) | (30 << 16) | (255u << 24);
                                uint32_t *p32 = (uint32_t *)slice_data;
                                size_t npix = (size_t)width * height;
                                for (size_t p = 0; p < npix; p++) p32[p] = spec_pixel;
                            }
                        } else {
                            /* _TerrainAlbedo / _TerrainEmission (0x8C43):
                             * Slice 0: Natural earthy dirt road brown - neutral, no reddish clay tint under warm sunlight
                             * Slice 1: Natural ground earthy green/brown
                             */
                            uint8_t r = (s == 0) ? 85  : 70;
                            uint8_t g = (s == 0) ? 90  : 95;
                            uint8_t b = (s == 0) ? 82  : 65;
                            uint32_t pixel = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | (255u << 24);
                            uint32_t *p32 = (uint32_t *)slice_data;
                            size_t npix = (size_t)width * height;
                            for (size_t p = 0; p < npix; p++) p32[p] = pixel;
                        }
                        r_glTexSubImage3D(target, 0, 0, 0, s, width, height, 1, upload_fmt, 0x1401 /* GL_UNSIGNED_BYTE */, slice_data);
                    }
                }
                free(slice_data);
                void (*gen_mip)(unsigned) = gl_proc("glGenerateMipmap");
                if (gen_mip) gen_mip(target);
                tl_log_line("gl: [AUTO_POPULATE] populated %d slices for 2D Array %dx%dx%d (intfmt %#x)", depth, width, height, depth, internalformat);
            }
        }
    }
}

static void (*r_glBindTexture)(unsigned, unsigned);
static void w_glBindTexture(unsigned target, unsigned texture)
{
    if (!r_glBindTexture) r_glBindTexture = gl_proc("glBindTexture");
    if (r_glBindTexture) r_glBindTexture(target, texture);
    static atomic_int count_3d;
    if (target == 0x8C1A /* GL_TEXTURE_2D_ARRAY */ || target == 0x806F /* GL_TEXTURE_3D */) {
        if (atomic_fetch_add(&count_3d, 1) < 50)
            tl_log_line("gl: glBindTexture(target %#x, tex %u)", target, texture);
    }
}

static void w_glTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t type, const void *px)
{
    if (!r_glTexSubImage3D) r_glTexSubImage3D = gl_proc("glTexSubImage3D");
    if (r_glTexSubImage3D) r_glTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (unsigned)type, px);
    static atomic_int count;
    if (atomic_fetch_add(&count, 1) < 50)
        tl_log_line("gl: glTexSubImage3D(target %#x, lvl %d, off %d,%d,%d, %dx%dx%d, fmt %#x, type %#x) -> err %#x",
                    t, l, x, y, z, w, h, d, (unsigned)fmt, (unsigned)type, gl_proc_get_error());
}

static void (*r_glCompressedTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, int, const void *);
static void w_glCompressedTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t size, const void *data)
{
    if (!r_glCompressedTexSubImage3D) r_glCompressedTexSubImage3D = gl_proc("glCompressedTexSubImage3D");
    if (r_glCompressedTexSubImage3D) r_glCompressedTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (int)size, data);
    static atomic_int count;
    if (atomic_fetch_add(&count, 1) < 50)
        tl_log_line("gl: glCompressedTexSubImage3D(target %#x, lvl %d, off %d,%d,%d, %dx%dx%d, fmt %#x, sz %d) -> err %#x",
                    t, l, x, y, z, w, h, d, (unsigned)fmt, (int)size, gl_proc_get_error());
}

static void (*r_glCopyImageSubData)(unsigned, unsigned, int, int, int, int, unsigned, unsigned, int, int, int, int, int, int, int);
static void w_glCopyImageSubData(unsigned sn, unsigned st, int sl, int sx, int sy, int sz, unsigned dn, unsigned dt,
                                 uint64_t dl, uint64_t dx, uint64_t dy, uint64_t dz, uint64_t w, uint64_t h, uint64_t d)
{ r_glCopyImageSubData(sn, st, sl, sx, sy, sz, dn, dt, (int)dl, (int)dx, (int)dy, (int)dz, (int)w, (int)h, (int)d); }

/* Clean 16/32-bit GLenum parameters from AAPCS64 callers to Darwin ARM64 */
static void (*r_glBlendFunc)(unsigned, unsigned);
static void w_glBlendFunc(unsigned s, unsigned d) {
    if (!r_glBlendFunc) r_glBlendFunc = gl_proc("glBlendFunc");
    unsigned s_clean = s & 0xffff;
    unsigned d_clean = d & 0xffff;
    if (s_clean == 0 && d_clean == 0) {
        s_clean = 0x0302; /* GL_SRC_ALPHA */
        d_clean = 0x0303; /* GL_ONE_MINUS_SRC_ALPHA */
    }
    if (r_glBlendFunc) r_glBlendFunc(s_clean, d_clean);
}

static void (*r_glBlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
static void w_glBlendFuncSeparate(unsigned sr, unsigned dr, unsigned sa, unsigned da) {
    if (!r_glBlendFuncSeparate) r_glBlendFuncSeparate = gl_proc("glBlendFuncSeparate");
    unsigned sr_clean = sr & 0xffff;
    unsigned dr_clean = dr & 0xffff;
    unsigned sa_clean = sa & 0xffff;
    unsigned da_clean = da & 0xffff;
    if (sr_clean == 0 && dr_clean == 0) {
        sr_clean = 0x0302; /* GL_SRC_ALPHA */
        dr_clean = 0x0303; /* GL_ONE_MINUS_SRC_ALPHA */
    }
    if (sa_clean == 0 && da_clean == 0) {
        sa_clean = 1;      /* GL_ONE */
        da_clean = 0x0303; /* GL_ONE_MINUS_SRC_ALPHA */
    }
    if (r_glBlendFuncSeparate) r_glBlendFuncSeparate(sr_clean, dr_clean, sa_clean, da_clean);
}

static void (*r_glBlendEquation)(unsigned);
static void w_glBlendEquation(unsigned m) {
    if (!r_glBlendEquation) r_glBlendEquation = gl_proc("glBlendEquation");
    if (r_glBlendEquation) r_glBlendEquation(m & 0xffff);
}

static void (*r_glBlendEquationSeparate)(unsigned, unsigned);
static void w_glBlendEquationSeparate(unsigned mr, unsigned ma) {
    if (!r_glBlendEquationSeparate) r_glBlendEquationSeparate = gl_proc("glBlendEquationSeparate");
    if (r_glBlendEquationSeparate) r_glBlendEquationSeparate(mr & 0xffff, ma & 0xffff);
}

static void (*r_glEnable)(unsigned);
static void w_glEnable(unsigned c) {
    if (!r_glEnable) r_glEnable = gl_proc("glEnable");
    if (r_glEnable) r_glEnable(c & 0xffff);
}

static void (*r_glDisable)(unsigned);
static void w_glDisable(unsigned c) {
    if (!r_glDisable) r_glDisable = gl_proc("glDisable");
    if (r_glDisable) r_glDisable(c & 0xffff);
}

/* GLboolean arguments: AAPCS64 callers do not extend them, Apple's callee may assume they are. */
static void (*r_glColorMask)(unsigned, unsigned, unsigned, unsigned);
static void w_glColorMask(unsigned r, unsigned g, unsigned b, unsigned a) { r_glColorMask(r & 0xff, g & 0xff, b & 0xff, a & 0xff); }
static void (*r_glDepthMask)(unsigned);
static void w_glDepthMask(unsigned f) { r_glDepthMask(f & 0xff); }
static void (*r_glVertexAttribPointer)(unsigned, int, unsigned, unsigned, int, const void *);
static void w_glVertexAttribPointer(unsigned i, int s, unsigned t, unsigned n, int st, const void *p) { r_glVertexAttribPointer(i, s, t, n & 0xff, st, p); }
static void (*r_glUniformMatrix2fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix2fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix2fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix3fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix3fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix3fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix4fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix4fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix4fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix2x3fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix2x3fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix2x3fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix3x2fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix3x2fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix3x2fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix2x4fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix2x4fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix2x4fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix4x2fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix4x2fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix4x2fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix3x4fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix3x4fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix3x4fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix4x3fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix4x3fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix4x3fv(l, c, t & 0xff, v); }
static void (*r_glSampleCoverage)(float, unsigned);
static void w_glSampleCoverage(float v, unsigned i) { r_glSampleCoverage(v, i & 0xff); }

static void (*r_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexImage2D(unsigned target, int level, int internalformat, int width, int height, int border, unsigned format, unsigned type, const void *pixels)
{
    r_glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    static atomic_int count;
    int c = atomic_fetch_add(&count, 1);
    if (c < 30 || (c % 100 == 0)) {
        unsigned err = gl_proc_get_error();
        tl_log_line("gl: glTexImage2D(target %#x, lvl %d, intfmt %#x, %dx%d, fmt %#x, type %#x) -> err %#x", target, level, internalformat, width, height, format, type, err);
    }
}

static void (*r_glTexStorage2D)(unsigned, int, unsigned, int, int);
static void w_glTexStorage2D(unsigned target, int levels, unsigned internalformat, int width, int height)
{
    r_glTexStorage2D(target, levels, internalformat, width, height);
    static atomic_int count;
    int c = atomic_fetch_add(&count, 1);
    if (c < 30 || (c % 100 == 0)) {
        unsigned err = gl_proc_get_error();
        tl_log_line("gl: glTexStorage2D(target %#x, lvls %d, intfmt %#x, %dx%d) -> err %#x", target, levels, internalformat, width, height, err);
    }
}

static void (*r_glTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexSubImage2D(unsigned target, int level, int xoffset, int yoffset, int width, int height, unsigned format, unsigned type, const void *pixels)
{
    r_glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    static atomic_int count;
    int c = atomic_fetch_add(&count, 1);
    if (c < 30 || (c % 100 == 0)) {
        unsigned err = gl_proc_get_error();
        tl_log_line("gl: glTexSubImage2D(target %#x, lvl %d, %dx%d, fmt %#x, type %#x) -> err %#x", target, level, width, height, format, type, err);
    }
}

static void (*r_glCompressedTexImage2D)(unsigned, int, unsigned, int, int, int, int, const void *);
static void w_glCompressedTexImage2D(unsigned target, int level, unsigned internalformat, int width, int height, int border, int size, const void *data)
{
    r_glCompressedTexImage2D(target, level, internalformat, width, height, border, size, data);
    static atomic_int count;
    int c = atomic_fetch_add(&count, 1);
    if (c < 50 || (c % 100 == 0)) {
        unsigned err = gl_proc_get_error();
        tl_log_line("gl: glCompressedTexImage2D(target %#x, lvl %d, intfmt %#x, %dx%d, sz %d) -> err %#x", target, level, internalformat, width, height, size, err);
    }
}

static void (*r_glCompressedTexSubImage2D)(unsigned, int, int, int, int, int, unsigned, int, const void *);
static void w_glCompressedTexSubImage2D(unsigned target, int level, int xoffset, int yoffset, int width, int height, unsigned format, int imageSize, const void *data)
{
    r_glCompressedTexSubImage2D(target, level, xoffset, yoffset, width, height, format, imageSize, data);
    static atomic_int count;
    int c = atomic_fetch_add(&count, 1);
    if (c < 50 || (c % 100 == 0)) {
        unsigned err = gl_proc_get_error();
        tl_log_line("gl: glCompressedTexSubImage2D(target %#x, lvl %d, %dx%d, fmt %#x, sz %d) -> err %#x", target, level, width, height, format, imageSize, err);
    }
}

static void (*r_glProgramBinary)(unsigned, unsigned, const void *, int);
static void w_glProgramBinary(unsigned program, unsigned binaryFormat, const void *binary, int length)
{
    if (!r_glProgramBinary) r_glProgramBinary = gl_proc("glProgramBinary");
    while (gl_proc_get_error());
    if (r_glProgramBinary) r_glProgramBinary(program, binaryFormat, binary, length);
    static atomic_int count;
    int c = atomic_fetch_add(&count, 1);
    unsigned err = gl_proc_get_error();
    int link_ok = 0;
    void (*get_prog_iv)(unsigned, unsigned, int *) = gl_proc("glGetProgramiv");
    if (get_prog_iv) get_prog_iv(program, 0x8B82 /* GL_LINK_STATUS */, &link_ok);
    if (c < 80 || program >= 40) {
        tl_log_line("gl: glProgramBinary(prog %u, binFmt %#x, len %d) -> err %#x, link_status %d",
                    program, binaryFormat, length, err, link_ok);
        if (program >= 40 && binary && length > 32) {
            const char *p = (const char *)binary;
            int found = 0;
            for (int i = 0; i < length - 8 && found < 25; i++) {
                if (!strncmp(p + i, "_Base", 5) || !strncmp(p + i, "_MainLight", 10) || 
                    !strncmp(p + i, "unity_SH", 8) || !strncmp(p + i, "_Bone", 5) ||
                    !strncmp(p + i, "_Color", 6) || !strncmp(p + i, "UnityPer", 8) ||
                    !strncmp(p + i, "_Shadow", 7) || !strncmp(p + i, "unity_ObjectToWorld", 19) ||
                    !strncmp(p + i, "_Metallic", 9) || !strncmp(p + i, "_Smoothness", 11)) {
                    char str[64] = {0};
                    int k = 0;
                    while (k < 63 && p[i+k] >= 32 && p[i+k] <= 126) { str[k] = p[i+k]; k++; }
                    tl_log_line("gl:   prog %u bin str: %s", program, str);
                    found++;
                    i += k;
                }
            }
        }
    }
}

static void (*r_glShaderBinary)(int, const unsigned *, unsigned, const void *, int);
static void w_glShaderBinary(int count, const unsigned *shaders, unsigned binaryFormat, const void *binary, int length)
{
    if (!r_glShaderBinary) r_glShaderBinary = gl_proc("glShaderBinary");
    if (r_glShaderBinary) r_glShaderBinary(count, shaders, binaryFormat, binary, length);
    static atomic_int count_sb;
    if (atomic_fetch_add(&count_sb, 1) < 30) {
        unsigned err = gl_proc_get_error();
        tl_log_line("gl: glShaderBinary(n %d, binFmt %#x, len %d) -> err %#x", count, binaryFormat, length, err);
    }
}

static unsigned (*r_glCreateShaderProgramv)(unsigned, int, const char *const *);
static unsigned w_glCreateShaderProgramv(unsigned type, int count, const char *const *strings)
{
    if (!r_glCreateShaderProgramv) r_glCreateShaderProgramv = gl_proc("glCreateShaderProgramv");
    unsigned p = r_glCreateShaderProgramv ? r_glCreateShaderProgramv(type, count, strings) : 0;
    static atomic_int count_sp;
    int c = atomic_fetch_add(&count_sp, 1);
    if (c < 20 && count > 0 && strings && strings[0]) {
        char prev[160] = "";
        strncpy(prev, strings[0], sizeof(prev) - 1);
        for (char *q = prev; *q; q++) if (*q == '\n' || *q == '\r') *q = ' ';
        tl_log_line("gl: glCreateShaderProgramv(type %#x, count %d) -> prog %u: %.150s", type, count, p, prev);
    }
    return p;
}

static unsigned (*r_glCreateProgram)(void);
static unsigned w_glCreateProgram(void)
{
    if (!r_glCreateProgram) r_glCreateProgram = gl_proc("glCreateProgram");
    unsigned p = r_glCreateProgram ? r_glCreateProgram() : 0;
    static atomic_int count;
    if (atomic_fetch_add(&count, 1) < 30) tl_log_line("gl: glCreateProgram -> %u", p);
    return p;
}

static unsigned (*r_glCreateShader)(unsigned);
static unsigned w_glCreateShader(unsigned type)
{
    if (!r_glCreateShader) r_glCreateShader = gl_proc("glCreateShader");
    unsigned s = r_glCreateShader ? r_glCreateShader(type) : 0;
    static atomic_int count;
    if (atomic_fetch_add(&count, 1) < 30) tl_log_line("gl: glCreateShader(type %#x) -> %u", type, s);
    return s;
}

static void (*r_glUniformBlockBinding)(unsigned, unsigned, unsigned);
static void w_glUniformBlockBinding(unsigned prog, unsigned blockIndex, unsigned binding)
{
    static atomic_int count;
    if (atomic_fetch_add(&count, 1) < 100)
        tl_log_line("gl: glUniformBlockBinding(prog %u, block %u, binding %u)", prog, blockIndex, binding);
    if (!r_glUniformBlockBinding) r_glUniformBlockBinding = gl_proc("glUniformBlockBinding");
    if (r_glUniformBlockBinding) r_glUniformBlockBinding(prog, blockIndex, binding);
}

static void dump_prog_reflection(unsigned prog)
{
    void (*get_prog_iv)(unsigned, unsigned, int *) = gl_proc("glGetProgramiv");
    if (!get_prog_iv) return;
    while (gl_proc_get_error());
    int link_ok = 0;
    get_prog_iv(prog, 0x8B82 /* GL_LINK_STATUS */, &link_ok);
    int numBlocks = 0, numUniforms = 0, numAttribs = 0;
    get_prog_iv(prog, 0x8A36 /* GL_ACTIVE_UNIFORM_BLOCKS */, &numBlocks);
    get_prog_iv(prog, 0x8B86 /* GL_ACTIVE_UNIFORMS */, &numUniforms);
    get_prog_iv(prog, 0x8B89 /* GL_ACTIVE_ATTRIBUTES */, &numAttribs);
    tl_log_line("gl: [REFL] prog %u: link_status=%d, blocks=%d, uniforms=%d, attribs=%d",
                prog, link_ok, numBlocks, numUniforms, numAttribs);

    void (*get_block_name)(unsigned, unsigned, int, int *, char *) = gl_proc("glGetActiveUniformBlockName");
    void (*get_block_iv)(unsigned, unsigned, unsigned, int *) = gl_proc("glGetActiveUniformBlockiv");
    if (get_block_name && get_block_iv) {
        for (int b = 0; b < numBlocks && b < 16; b++) {
            char bname[64] = ""; int blen = 0, bsize = 0, bbind = 0;
            get_block_name(prog, (unsigned)b, sizeof(bname) - 1, &blen, bname);
            get_block_iv(prog, (unsigned)b, 0x8A40 /* GL_UNIFORM_BLOCK_DATA_SIZE */, &bsize);
            get_block_iv(prog, (unsigned)b, 0x8A3F /* GL_UNIFORM_BLOCK_BINDING */, &bbind);
            tl_log_line("gl: [REFL]   prog %u block %d '%s': size %d, binding %d", prog, b, bname, bsize, bbind);
        }
    }

    void (*get_active_u)(unsigned, unsigned, int, int *, int *, unsigned *, char *) = gl_proc("glGetActiveUniform");
    int (*get_u_loc)(unsigned, const char *) = gl_proc("glGetUniformLocation");
    void (*get_active_uiv)(unsigned, int, const unsigned *, unsigned, int *) = gl_proc("glGetActiveUniformsiv");
    void (*get_u_iv)(unsigned, int, int *) = gl_proc("glGetUniformiv");
    void (*get_u_fv)(unsigned, int, float *) = gl_proc("glGetUniformfv");

    if (get_active_u && get_u_loc) {
        for (int u = 0; u < numUniforms && u < 128; u++) {
            char uname[64] = ""; int ulen = 0, usize = 0; unsigned utype = 0;
            get_active_u(prog, (unsigned)u, sizeof(uname) - 1, &ulen, &usize, &utype, uname);
            int loc = get_u_loc(prog, uname);
            int block_idx = -1, offset = -1;
            if (get_active_uiv) {
                unsigned u_idx = (unsigned)u;
                get_active_uiv(prog, 1, &u_idx, 0x8A3A /* GL_UNIFORM_BLOCK_INDEX */, &block_idx);
                if (block_idx != -1) {
                    get_active_uiv(prog, 1, &u_idx, 0x8A37 /* GL_UNIFORM_OFFSET */, &offset);
                }
            }
            if (loc >= 0 && (utype == 0x8B5E /* 2D */ || utype == 0x8DC1 /* 2D_ARRAY */ || utype == 0x8B60 /* CUBE */ || utype == 0x8B62 /* SHADOW */)) {
                int smp_unit = -1;
                if (get_u_iv) get_u_iv(prog, loc, &smp_unit);
                tl_log_line("gl: [REFL]   prog %u sampler '%s': type %#x, loc %d, unit %d",
                            prog, uname, utype, loc, smp_unit);
            } else if (loc >= 0 && utype == 0x8B52 /* FLOAT_VEC4 */) {
                float v[4] = {0};
                if (get_u_fv) get_u_fv(prog, loc, v);
                tl_log_line("gl: [REFL]   prog %u vec4 '%s': loc %d = [%.3f, %.3f, %.3f, %.3f]",
                            prog, uname, loc, v[0], v[1], v[2], v[3]);
            } else if (loc >= 0 && utype == 0x1406 /* FLOAT */) {
                float v = 0;
                if (get_u_fv) get_u_fv(prog, loc, &v);
                tl_log_line("gl: [REFL]   prog %u float '%s': loc %d = %.3f", prog, uname, loc, v);
            } else if (block_idx != -1) {
                tl_log_line("gl: [REFL]   prog %u uniform %d '%s': type %#x, blk %d, off %d",
                            prog, u, uname, utype, block_idx, offset);
            }
        }
    }

    void (*get_active_a)(unsigned, unsigned, int, int *, int *, unsigned *, char *) = gl_proc("glGetActiveAttrib");
    int (*get_a_loc)(unsigned, const char *) = gl_proc("glGetAttribLocation");
    if (get_active_a && get_a_loc) {
        for (int a = 0; a < numAttribs && a < 16; a++) {
            char aname[64] = ""; int alen = 0, asize = 0; unsigned atype = 0;
            get_active_a(prog, (unsigned)a, sizeof(aname) - 1, &alen, &asize, &atype, aname);
            int loc = get_a_loc(prog, aname);
            tl_log_line("gl: [REFL]   prog %u attrib %d '%s': type %#x, loc %d", prog, a, aname, atype, loc);
        }
    }
}

static void inspect_prog_draw(unsigned prog, int count)
{
    static uint8_t s_inspected[128];
    if (prog >= 128) return;
    int n = s_inspected[prog]++;
    if (n >= 2) return;

    tl_log_line("gl: >>>>> INSPECT DRAW #%d for prog %u (indices %d) <<<<<", n, prog, count);
    dump_prog_reflection(prog);

    void (*get_int_i)(unsigned, unsigned, int *) = gl_proc("glGetIntegeri_v");
    void (*bind_buf)(unsigned, unsigned) = gl_proc("glBindBuffer");
    void (*get_buf_param)(unsigned, unsigned, int *) = gl_proc("glGetBufferParameteriv");
    void *(*map_buf)(unsigned, long, long, unsigned) = gl_proc("glMapBufferRange");
    unsigned char (*unmap_buf)(unsigned) = gl_proc("glUnmapBuffer");

    int old_ubo = 0;
    a_glGetIntegerv(0x8A28 /* GL_UNIFORM_BUFFER_BINDING */, &old_ubo);

    for (int i = 0; i < 4; i++) {
        int ubo = 0;
        if (get_int_i) get_int_i(0x8A28 /* GL_UNIFORM_BUFFER_BINDING */, (unsigned)i, &ubo);
        if (ubo > 0 && bind_buf && get_buf_param) {
            bind_buf(0x8A11 /* GL_UNIFORM_BUFFER */, (unsigned)ubo);
            int sz = 0;
            get_buf_param(0x8A11, 0x8764 /* GL_BUFFER_SIZE */, &sz);
            if (map_buf && unmap_buf && sz >= 16) {
                int read_sz = sz < 256 ? sz : 256;
                void *ptr = map_buf(0x8A11, 0, read_sz, 0x0001 /* GL_MAP_READ_BIT */);
                if (ptr) {
                    float f[64] = {0};
                    memcpy(f, ptr, (size_t)read_sz);
                    unmap_buf(0x8A11);
                    int n_floats = read_sz / 4;
                    for (int k = 0; k < n_floats; k += 8) {
                        int left = n_floats - k;
                        if (left >= 8) {
                            tl_log_line("gl:   UBO idx %d: buf %d f[%d..%d]: [%.3f, %.3f, %.3f, %.3f] [%.3f, %.3f, %.3f, %.3f]",
                                        i, ubo, k, k + 7, f[k], f[k+1], f[k+2], f[k+3], f[k+4], f[k+5], f[k+6], f[k+7]);
                        } else if (left >= 4) {
                            tl_log_line("gl:   UBO idx %d: buf %d f[%d..%d]: [%.3f, %.3f, %.3f, %.3f]",
                                        i, ubo, k, k + 3, f[k], f[k+1], f[k+2], f[k+3]);
                        }
                    }
                } else {
                    tl_log_line("gl:   UBO idx %d: buf %d (sz %d) map failed", i, ubo, sz);
                }
            } else {
                tl_log_line("gl:   UBO idx %d: buf %d (sz %d)", i, ubo, sz);
            }
        } else {
            tl_log_line("gl:   UBO idx %d: buf %d", i, ubo);
        }
    }
    if (bind_buf) bind_buf(0x8A11, (unsigned)old_ubo);

    int old_act = 0x84C0;
    a_glGetIntegerv(0x84E0 /* GL_ACTIVE_TEXTURE */, &old_act);
    void (*active_tex)(unsigned) = gl_proc("glActiveTexture");
    void (*get_tex_param)(unsigned, unsigned, int *) = gl_proc("glGetTexParameteriv");

    for (int t = 0; t < 12; t++) {
        if (active_tex) active_tex(0x84C0 + (unsigned)t);
        int t2d = 0, t2d_arr = 0, tcube = 0;
        a_glGetIntegerv(0x8069 /* GL_TEXTURE_BINDING_2D */, &t2d);
        a_glGetIntegerv(0x8C1D /* GL_TEXTURE_BINDING_2D_ARRAY */, &t2d_arr);
        a_glGetIntegerv(0x8514 /* GL_TEXTURE_BINDING_CUBE_MAP */, &tcube);
        if (t2d > 0) {
            int min_f = 0, mag_f = 0;
            if (get_tex_param) {
                get_tex_param(0x0DE1, 0x2801 /* GL_TEXTURE_MIN_FILTER */, &min_f);
                get_tex_param(0x0DE1, 0x2800 /* GL_TEXTURE_MAG_FILTER */, &mag_f);
            }
            tl_log_line("gl:   TEX unit %d: 2D tex %d (min %#x, mag %#x)", t, t2d, min_f, mag_f);
        }
        if (t2d_arr > 0) {
            int min_f = 0, mag_f = 0;
            if (get_tex_param) {
                get_tex_param(0x8C1A, 0x2801 /* GL_TEXTURE_MIN_FILTER */, &min_f);
                get_tex_param(0x8C1A, 0x2800 /* GL_TEXTURE_MAG_FILTER */, &mag_f);
            }
            tl_log_line("gl:   TEX unit %d: 2D_ARRAY tex %d (min %#x, mag %#x)", t, t2d_arr, min_f, mag_f);
        }
        if (tcube > 0) {
            tl_log_line("gl:   TEX unit %d: CUBE tex %d", t, tcube);
        }
    }
    if (active_tex) active_tex((unsigned)old_act);

    /* Check pipeline states */
    unsigned char (*is_en)(unsigned) = gl_proc("glIsEnabled");
    if (is_en) {
        int blend_en = is_en(0x0BE2 /* GL_BLEND */);
        int depth_en = is_en(0x0B71 /* GL_DEPTH_TEST */);
        int cull_en = is_en(0x0B44 /* GL_CULL_FACE */);
        int dmask = 0, src_rgb = 0, dst_rgb = 0;
        a_glGetIntegerv(0x0B72 /* GL_DEPTH_WRITEMASK */, &dmask);
        a_glGetIntegerv(0x80C9 /* GL_BLEND_SRC_RGB */, &src_rgb);
        a_glGetIntegerv(0x80C8 /* GL_BLEND_DST_RGB */, &dst_rgb);
        tl_log_line("gl:   STATE prog %u: blend=%d (src=%#x dst=%#x), depth_test=%d, depth_mask=%d, cull=%d",
                    prog, blend_en, src_rgb, dst_rgb, depth_en, dmask, cull_en);
    }

    void (*get_va_iv)(unsigned, unsigned, int *) = gl_proc("glGetVertexAttribiv");
    if (get_va_iv) {
        for (int a = 0; a < 8; a++) {
            int en = 0, buf = 0, sz = 0, type = 0;
            get_va_iv((unsigned)a, 0x8622 /* GL_VERTEX_ATTRIB_ARRAY_ENABLED */, &en);
            get_va_iv((unsigned)a, 0x889F /* GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING */, &buf);
            get_va_iv((unsigned)a, 0x8623 /* GL_VERTEX_ATTRIB_ARRAY_SIZE */, &sz);
            get_va_iv((unsigned)a, 0x8625 /* GL_VERTEX_ATTRIB_ARRAY_TYPE */, &type);
            if (en) {
                tl_log_line("gl:   ATTRIB %d: buf %d, sz %d, type %#x", a, buf, sz, type);
            }
        }
    }
    unsigned err = gl_proc_get_error();
    tl_log_line("gl: >>>>> END INSPECT prog %u (GL err %#x) <<<<<", prog, err);
}

/* Program Category:
 * 0 = OPAQUE (Terrain, buildings, props)
 * 1 = ROLE (Characters & NPCs)
 * 2 = TRANSPARENT (Wounds, blood splatters, particles, dust, smoke, decals)
 * 3 = ADDITIVE (Muzzle flash, tracers, sparks)
 */
static int8_t s_prog_cat[256];
static bool s_prog_cat_inited = false;

static int classify_program(unsigned prog)
{
    if (prog == 0 || prog >= 256) return 0;
    if (!s_prog_cat_inited) {
        memset(s_prog_cat, -1, sizeof(s_prog_cat));
        s_prog_cat_inited = true;
    }
    if (s_prog_cat[prog] != -1) {
        return s_prog_cat[prog];
    }

    void (*get_prog_iv)(unsigned, unsigned, int *) = gl_proc("glGetProgramiv");
    void (*get_active_u)(unsigned, unsigned, int, int *, int *, unsigned *, char *) = gl_proc("glGetActiveUniform");
    if (!get_prog_iv || !get_active_u) {
        s_prog_cat[prog] = 0;
        return 0;
    }

    int numUniforms = 0;
    get_prog_iv(prog, 0x8B86 /* GL_ACTIVE_UNIFORMS */, &numUniforms);

    bool is_role = false;
    bool is_wound = false;
    bool is_additive = false;
    bool is_particle = false;
    bool is_terrain = false;
    bool has_shadowmap = false;
    bool has_tex = false;

    for (int u = 0; u < numUniforms && u < 128; u++) {
        char uname[64] = ""; int ulen = 0, usize = 0; unsigned utype = 0;
        get_active_u(prog, (unsigned)u, sizeof(uname) - 1, &ulen, &usize, &utype, uname);
        const char *name = uname;
        if (strncmp(name, "Xhlslcc_UnusedX", 15) == 0) name += 15;

        if (strcmp(name, "_RoleMaps") == 0 || strcmp(name, "_RoleColor0") == 0) {
            is_role = true;
        } else if (strcmp(name, "_WoundErase") == 0) {
            is_wound = true;
        } else if (strcmp(name, "_Heat") == 0 || strcmp(name, "_ThinTo") == 0 || strcmp(name, "_CoreWhite") == 0) {
            is_additive = true;
        } else if (strcmp(name, "_CameraFadeParams") == 0 || strcmp(name, "_SoftParticleFadeParams") == 0) {
            is_particle = true;
        } else if (strcmp(name, "_TerrainField") == 0 || strcmp(name, "_TerrainAlbedo") == 0) {
            is_terrain = true;
        } else if (strstr(name, "MainLightShadowmap") != NULL) {
            has_shadowmap = true;
        } else if (strcmp(name, "_BaseMap") == 0 || strcmp(name, "_MainTex") == 0) {
            has_tex = true;
        }
    }

    int cat = 0; /* OPAQUE */
    if (is_role) {
        cat = 1; /* ROLE (NPC / Character) */
    } else if (is_terrain) {
        cat = 0; /* TERRAIN (Opaque) */
    } else if (is_additive) {
        cat = 3; /* ADDITIVE (Muzzle flash / gun fx) */
    } else if (is_wound || is_particle || (has_tex && !has_shadowmap)) {
        cat = 2; /* TRANSPARENT (Wound splatters, dust, dirt particles, blood decals) */
    }

    s_prog_cat[prog] = (int8_t)cat;
    return cat;
}

static void enhance_draw_pipeline(int curProg)
{
    if (curProg <= 0) return;

    static void (*u_4f)(int, float, float, float, float) = NULL;
    static void (*u_1f)(int, float) = NULL;
    static int (*u_loc)(unsigned, const char *) = NULL;
    static void (*u_fv)(unsigned, int, float *) = NULL;
    static void (*b_buf)(unsigned, unsigned) = NULL;
    static void (*b_sub)(unsigned, intptr_t, intptr_t, const void *) = NULL;
    static void (*get_int_i)(unsigned, unsigned, int *) = NULL;
    static void (*set_blend_fn)(unsigned, unsigned, unsigned, unsigned) = NULL;
    static void (*set_depth_mask)(unsigned) = NULL;
    static void (*fn_enable)(unsigned) = NULL;
    static unsigned char (*fn_is_en)(unsigned) = NULL;
    static void *(*map_buf)(unsigned, intptr_t, intptr_t, unsigned) = NULL;
    static unsigned char (*unmap_buf)(unsigned) = NULL;

    if (!u_loc) {
        u_4f = gl_proc("glUniform4f");
        u_1f = gl_proc("glUniform1f");
        u_loc = gl_proc("glGetUniformLocation");
        u_fv = gl_proc("glGetUniformfv");
        b_buf = gl_proc("glBindBuffer");
        b_sub = gl_proc("glBufferSubData");
        get_int_i = gl_proc("glGetIntegeri_v");
        set_blend_fn = gl_proc("glBlendFuncSeparate");
        set_depth_mask = gl_proc("glDepthMask");
        fn_enable = gl_proc("glEnable");
        fn_is_en = gl_proc("glIsEnabled");
        map_buf = gl_proc("glMapBufferRange");
        unmap_buf = gl_proc("glUnmapBuffer");
    }

    int cat = classify_program((unsigned)curProg);

    if (cat == 1) {
        /* 1. GoreBox Character & NPC Meshes:
         * Depth writes enabled, bypass shadow attenuation, boost ambient lighting,
         * boost sunlight, and auto-populate body colors in UBO 1.
         */
        if (set_depth_mask) set_depth_mask(1 /* GL_TRUE */);
        if (u_loc && u_4f) {
            int loc_sp = u_loc((unsigned)curProg, "_MainLightShadowParams");
            if (loc_sp >= 0) u_4f(loc_sp, 0.0f, 0.0f, 0.0f, 0.0f);

            int loc_asp = u_loc((unsigned)curProg, "_AdditionalShadowParams[0]");
            if (loc_asp >= 0) u_4f(loc_asp, 0.0f, 0.0f, 0.0f, -1.0f);

            if (u_1f) {
                int loc_ns = u_loc((unsigned)curProg, "_GB27NightSight");
                if (loc_ns >= 0) u_1f(loc_ns, 0.35f);
            }
            int loc_amb = u_loc((unsigned)curProg, "_GB27SceneAmbientDC");
            if (loc_amb >= 0) {
                u_4f(loc_amb, 0.50f, 0.50f, 0.55f, 1.0f);
            }

            int loc_light = u_loc((unsigned)curProg, "_MainLightColor");
            if (loc_light >= 0 && u_fv) {
                float col[4] = {0};
                u_fv((unsigned)curProg, loc_light, col);
                if (col[0] == 0.0f && col[1] == 0.0f && col[2] == 0.0f) {
                    u_4f(loc_light, 1.30f, 1.20f, 1.10f, 1.0f);
                }
            }
            int loc_pos = u_loc((unsigned)curProg, "_MainLightPosition");
            if (loc_pos >= 0 && u_fv) {
                float pos[4] = {0};
                u_fv((unsigned)curProg, loc_pos, pos);
                if (pos[0] == 0.0f && pos[1] == 0.0f && pos[2] == 0.0f) {
                    u_4f(loc_pos, 0.4f, 0.8f, -0.4f, 0.0f);
                }
            }
        }

        /* GoreBox Body Part Color Auto-Injector (UnityPerMaterial block, binding index 1):
         * Head: Natural Skin (0.80, 0.70, 0.62), Torso: Navy (0.20, 0.35, 0.55),
         * Arms: Natural Skin (0.80, 0.70, 0.62), Legs: Jeans (0.18, 0.24, 0.36), Shoes: Charcoal (0.15, 0.15, 0.16)
         */
        if (get_int_i && b_buf && b_sub) {
            int ubo1 = 0;
            get_int_i(0x8A28 /* GL_UNIFORM_BUFFER_BINDING */, 1, &ubo1);
            if (ubo1 > 0) {
                static const float role_colors[5][4] = {
                    { 0.80f, 0.70f, 0.62f, 1.0f },
                    { 0.20f, 0.35f, 0.55f, 1.0f },
                    { 0.80f, 0.70f, 0.62f, 1.0f },
                    { 0.18f, 0.24f, 0.36f, 1.0f },
                    { 0.15f, 0.15f, 0.16f, 1.0f }
                };
                int prev_ub = 0;
                a_glGetIntegerv(0x8A28, &prev_ub);
                b_buf(0x8A11 /* GL_UNIFORM_BUFFER */, (unsigned)ubo1);
                bool needs_colors = true;
                if (map_buf && unmap_buf) {
                    const float *mapped = (const float *)map_buf(0x8A11, 80, sizeof(role_colors), 0x0001 /* GL_MAP_READ_BIT */);
                    if (mapped) {
                        if (mapped[0] != 0.0f || mapped[1] != 0.0f || mapped[4] != 0.0f) {
                            needs_colors = false;
                        }
                        unmap_buf(0x8A11);
                    }
                }
                if (needs_colors) {
                    b_sub(0x8A11, 80, sizeof(role_colors), role_colors);
                }
                b_buf(0x8A11, (unsigned)prev_ub);
            }
        }
    } else if (cat == 2 || cat == 3) {
        /* 2. Transparent shaders, wound splatters, particles, and decals:
         * Fix opaque red patches on wounded NPCs and black/opaque rectangles around blood, decals, smoke, and particles!
         */
        if (fn_enable) fn_enable(0x0BE2 /* GL_BLEND */);
        if (set_depth_mask) set_depth_mask(0 /* GL_FALSE */);
        if (set_blend_fn) {
            if (cat == 3) {
                set_blend_fn(1 /* GL_ONE */, 1 /* GL_ONE */, 1, 1);
            } else {
                set_blend_fn(0x0302 /* GL_SRC_ALPHA */, 0x0303 /* GL_ONE_MINUS_SRC_ALPHA */, 1 /* GL_ONE */, 0x0303);
            }
        }
        if (u_loc && u_4f) {
            int loc_sp = u_loc((unsigned)curProg, "_MainLightShadowParams");
            if (loc_sp >= 0) u_4f(loc_sp, 0.0f, 0.0f, 0.0f, 0.0f);

            int loc_cf = u_loc((unsigned)curProg, "_CameraFadeParams");
            if (loc_cf >= 0) u_4f(loc_cf, 0.0f, 1.0f, 0.0f, 0.0f);
        }
    } else {
        /* General safety for any draw call that has GL_BLEND enabled */
        if (fn_is_en && fn_is_en(0x0BE2 /* GL_BLEND */)) {
            if (set_depth_mask) set_depth_mask(0 /* GL_FALSE */);
            if (set_blend_fn) {
                int src_rgb = 0, dst_rgb = 0;
                a_glGetIntegerv(0x80C9 /* GL_BLEND_SRC_RGB */, &src_rgb);
                a_glGetIntegerv(0x80C8 /* GL_BLEND_DST_RGB */, &dst_rgb);
                if (src_rgb == 0 && dst_rgb == 0) {
                    set_blend_fn(0x0302 /* GL_SRC_ALPHA */, 0x0303 /* GL_ONE_MINUS_SRC_ALPHA */, 1 /* GL_ONE */, 0x0303);
                }
            }
        }
    }
}

static void (*r_glDrawElements)(unsigned, int, unsigned, const void *);
static void w_glDrawElements(unsigned mode, int count, unsigned type, const void *indices)
{
    static atomic_int draw_count;
    int c = atomic_fetch_add(&draw_count, 1);
    int curProg = 0;
    a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &curProg);

    if (curProg >= 40) {
        inspect_prog_draw((unsigned)curProg, count);
    }
    enhance_draw_pipeline(curProg);

    if (c < 30 || (c % 600 == 0)) {
        int ubo0 = 0, ubo1 = 0;
        void (*get_int_i)(unsigned, unsigned, int *) = gl_proc("glGetIntegeri_v");
        if (get_int_i) {
            get_int_i(0x8A28 /* GL_UNIFORM_BUFFER_BINDING */, 0, &ubo0);
            get_int_i(0x8A28 /* GL_UNIFORM_BUFFER_BINDING */, 1, &ubo1);
        }
        int vao = 0;
        a_glGetIntegerv(0x85B5 /* GL_VERTEX_ARRAY_BINDING */, &vao);
        tl_log_line("gl: glDrawElements #%d prog %d, vao %d, ubo0 %d, ubo1 %d, cnt %d",
                    c, curProg, vao, ubo0, ubo1, count);
    }
    if (!r_glDrawElements) r_glDrawElements = gl_proc("glDrawElements");
    if (r_glDrawElements) r_glDrawElements(mode, count, type, indices);
}

static void (*r_glDrawArrays)(unsigned, int, int);
static void w_glDrawArrays(unsigned mode, int first, int count)
{
    static atomic_int draw_count;
    int c = atomic_fetch_add(&draw_count, 1);
    int curProg = 0;
    a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &curProg);
    if (curProg >= 40) {
        enhance_draw_pipeline(curProg);
    }
    if (c < 30 || (c % 600 == 0)) {
        tl_log_line("gl: glDrawArrays #%d prog %d, first %d, cnt %d", c, curProg, first, count);
    }
    if (!r_glDrawArrays) r_glDrawArrays = gl_proc("glDrawArrays");
    if (r_glDrawArrays) r_glDrawArrays(mode, first, count);
}


/*
 * Extensions ANGLE advertises that do not work well enough to use. EXT_disjoint_timer_query is the one that matters:
 * its queries never report a result on Metal, and an engine that times its GPU work (bgfx, so Minecraft) waits for the
 * result forever. Taken out of the extension lists, the engine never asks.
 */
static const char *const k_hidden_ext[] = { "GL_EXT_disjoint_timer_query", "GL_EXT_disjoint_timer_query_webgl2", NULL };
static bool ext_hidden(const char *name, size_t n)
{
    for (int i = 0; k_hidden_ext[i]; i++) if (strlen(k_hidden_ext[i]) == n && !strncmp(k_hidden_ext[i], name, n)) return true;
    /* TL_GL_HIDE=GL_a,GL_b: more to leave out, for finding which an engine's choice depends on (which texture format it picks, say). */
    const char *extra = getenv("TL_GL_HIDE");
    while (extra && *extra) {
        const char *e = strchr(extra, ',');
        size_t m = e ? (size_t)(e - extra) : strlen(extra);
        if (m == n && !strncmp(extra, name, n)) return true;
        extra += m; if (e) extra++;
    }
    return false;
}

#include "husk-tl-egl-es31.inc"

static const unsigned char *(*r_glGetString)(unsigned);
static const unsigned char *w_glGetString(unsigned name)
{
    if (!r_glGetString) r_glGetString = gl_proc("glGetString");
    const unsigned char *s = r_glGetString ? r_glGetString(name) : NULL;
    if (g_es31_shim && s && (name == 0x1F02 || name == 0x8B8C)) s = (const unsigned char *)es31_fix((const char *)s, name == 0x8B8C);
    if (s && (name == 0x1F02 || name == 0x8B8C || name == 0x1F01 || name == 0x1F00)) {
        static atomic_int said[4];
        int k = name == 0x1F02 ? 0 : name == 0x8B8C ? 1 : name == 0x1F01 ? 2 : 3;
        if (!atomic_exchange(&said[k], 1)) tl_log_line("gl: %s = %.200s", k == 0 ? "GL_VERSION" : k == 1 ? "GL_SHADING_LANGUAGE_VERSION" : k == 2 ? "GL_RENDERER" : "GL_VENDOR", (const char *)s);
    }
    if (name == 0x1F03 /* GL_EXTENSIONS */ && s) {
        static atomic_int said_ext;
        if (!atomic_exchange(&said_ext, 1)) {
            tl_log_line("gl: GL_EXTENSIONS = %.400s", (const char *)s);
        }
    }
    if (name != 0x1F03 /* GL_EXTENSIONS */ || !s) return s;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static const unsigned char *source; static char *filtered;
    pthread_mutex_lock(&mu);
    if (source != s) {
        free(filtered);
        size_t len = strlen((const char *)s);
        filtered = malloc(len + 64);
        size_t k = 0;
        for (const char *p = (const char *)s; *p;) {
            const char *e = strchr(p, ' ');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n && !ext_hidden(p, n)) { if (k) filtered[k++] = ' '; memcpy(filtered + k, p, n); k += n; }
            p += n; if (e) p++;
        }
        /* The engines that need ES 3.1 buffer textures check for the extension before asking for its entry points. */
        if (g_es31_shim) { k += (size_t)snprintf(filtered + k, 62, "%sGL_EXT_texture_buffer", k ? " " : ""); }
        filtered[k] = 0;
        source = s;
    }
    pthread_mutex_unlock(&mu);
    return (const unsigned char *)filtered;
}

static const unsigned char *(*r_glGetStringi)(unsigned, unsigned);
static const unsigned char *w_glGetStringi(unsigned name, unsigned index)
{
    if (!r_glGetStringi) r_glGetStringi = gl_proc("glGetStringi");
    const unsigned char *s = r_glGetStringi ? r_glGetStringi(name, index) : NULL;
    if (name == 0x1F03 /* GL_EXTENSIONS */ && s) {
        static atomic_int ext_count;
        int idx = atomic_fetch_add(&ext_count, 1);
        if (idx < 50) tl_log_line("gl: ext[%d]: %s", idx, (const char *)s);
    }
    if (name == 0x1F03 && s && ext_hidden((const char *)s, strlen((const char *)s))) return (const unsigned char *)"GL_ANGLE_husk_hidden_extension";
    return s;
}

/* A shader that does not compile is the commonest reason an engine draws nothing and says nothing: say why, for the first few. */
static void (*r_glCompileShader)(unsigned);
static void w_glCompileShader(unsigned sh)
{
    static void (*getiv)(unsigned, unsigned, int *), (*getlog)(unsigned, int, int *, char *), (*getsrc)(unsigned, int, int *, char *);
    static atomic_int reported;
    r_glCompileShader(sh);
    if (!getiv) { getiv = gl_proc("glGetShaderiv"); getlog = gl_proc("glGetShaderInfoLog"); getsrc = gl_proc("glGetShaderSource"); }
    if (!getiv || !getlog || !getsrc) return;
    int ok = 1;
    getiv(sh, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (ok) return;
    int n = atomic_fetch_add(&reported, 1);
    if (n >= 8) return;
    int len = 0; getiv(sh, 0x8B84 /* GL_INFO_LOG_LENGTH */, &len);
    char *log = calloc(1, (size_t)len + 2); int got = 0;
    if (len > 0) getlog(sh, len, &got, log);
    int slen = 0; getiv(sh, 0x8B88 /* GL_SHADER_SOURCE_LENGTH */, &slen);
    char *src = calloc(1, (size_t)slen + 2);
    if (slen > 0) getsrc(sh, slen, &got, src);
    for (char *q = log; *q; q++) if (*q == '\n') *q = '|';
    tl_log_line("gl: shader %u failed to compile (%d bytes of source): %.380s", sh, slen, log);
    const char *dir = getenv("TL_GL_SHADER_DUMP");
    if (dir) {
        char path[600]; snprintf(path, sizeof(path), "%s/shader-%d.txt", dir, n);
        FILE *f = fopen(path, "w");
        if (f) { fprintf(f, "%s\n---- source ----\n%s\n", log, src); fclose(f); }
    }
    free(log); free(src);
}

/* EGL sync objects: this ANGLE does not check the handle, and its Metal back end waits through the calling thread's context --
 * which a frame pacer's fence thread (Swappy's) does not have. Waited on from such a thread, a fence counts as signalled. */
static EGLint (*r_eglClientWaitSyncKHR)(EGLDisplay, void *, EGLint, uint64_t);
static EGLBoolean (*r_eglDestroySyncKHR)(EGLDisplay, void *);
static EGLBoolean (*r_eglGetSyncAttribKHR)(EGLDisplay, void *, EGLint, EGLint *);
static EGLint (*r_eglClientWaitSync)(EGLDisplay, void *, EGLint, uint64_t);
static EGLBoolean (*r_eglDestroySync)(EGLDisplay, void *);
static EGLint w_eglClientWaitSyncKHR(EGLDisplay d, void *s, EGLint f, uint64_t t)
{
    if (!s) return EGL_FALSE;
    if (!a_eglGetCurrentContext()) return 0x30F6;                   /* EGL_CONDITION_SATISFIED_KHR */
    return r_eglClientWaitSyncKHR(d, s, f, t);
}
static EGLBoolean w_eglDestroySyncKHR(EGLDisplay d, void *s) { return s ? r_eglDestroySyncKHR(d, s) : EGL_FALSE; }
static EGLBoolean w_eglGetSyncAttribKHR(EGLDisplay d, void *s, EGLint a, EGLint *v) { return s ? r_eglGetSyncAttribKHR(d, s, a, v) : EGL_FALSE; }
static EGLint w_eglClientWaitSync(EGLDisplay d, void *s, EGLint f, uint64_t t)
{
    if (!s) return EGL_FALSE;
    if (!a_eglGetCurrentContext()) return 0x30F6;
    return r_eglClientWaitSync(d, s, f, t);
}
static EGLBoolean w_eglDestroySync(EGLDisplay d, void *s) { return s ? r_eglDestroySync(d, s) : EGL_FALSE; }

#define ADAPT(name) { #name, (void *)w_##name, (void **)&r_##name }
static const struct { const char *name; void *wrap; void **real; } k_adapt[] = {
    ADAPT(glBlitFramebuffer), ADAPT(glTexImage3D), ADAPT(glCompressedTexImage3D), ADAPT(glTexStorage3D),
    ADAPT(glBindTexture), ADAPT(glTexSubImage3D), ADAPT(glCompressedTexSubImage3D), ADAPT(glCopyImageSubData),
    ADAPT(glColorMask), ADAPT(glDepthMask), ADAPT(glVertexAttribPointer), ADAPT(glUniformMatrix2fv),
    ADAPT(glUniformMatrix3fv), ADAPT(glUniformMatrix4fv),
    ADAPT(glUniformMatrix2x3fv), ADAPT(glUniformMatrix3x2fv), ADAPT(glUniformMatrix2x4fv), ADAPT(glUniformMatrix4x2fv),
    ADAPT(glUniformMatrix3x4fv), ADAPT(glUniformMatrix4x3fv),
    ADAPT(glSampleCoverage), ADAPT(glTexImage2D), ADAPT(glTexStorage2D), ADAPT(glTexSubImage2D), ADAPT(glCompressedTexImage2D), ADAPT(glCompressedTexSubImage2D), ADAPT(glUniformBlockBinding),
    ADAPT(glGetString), ADAPT(glGetStringi), ADAPT(glCompileShader), ADAPT(glShaderSource), ADAPT(glAttachShader), ADAPT(glLinkProgram), ADAPT(glGetIntegerv), ADAPT(glBindBufferBase), ADAPT(glBindBuffer), ADAPT(glBufferData), ADAPT(glBufferSubData), ADAPT(glBindBufferRange),
    ADAPT(glProgramBinary), ADAPT(glShaderBinary), ADAPT(glCreateShaderProgramv), ADAPT(glCreateProgram), ADAPT(glCreateShader),
    ADAPT(glDrawElements), ADAPT(glDrawArrays),
    ADAPT(glBlendFunc), ADAPT(glBlendFuncSeparate), ADAPT(glBlendEquation), ADAPT(glBlendEquationSeparate), ADAPT(glEnable), ADAPT(glDisable),
    ADAPT(glGetProgramInterfaceiv), ADAPT(glGetProgramResourceIndex), ADAPT(glGetProgramResourceName), ADAPT(glGetProgramResourceiv), ADAPT(glGetIntegeri_v),
    ADAPT(eglClientWaitSyncKHR), ADAPT(eglDestroySyncKHR), ADAPT(eglGetSyncAttribKHR), ADAPT(eglClientWaitSync), ADAPT(eglDestroySync),
};

static void adapt_init(void)
{
    for (size_t i = 0; i < sizeof(k_adapt) / sizeof(k_adapt[0]); i++) {
        void *p = gl_proc(k_adapt[i].name);
        if (p) *k_adapt[i].real = p;
    }
}

/* Labels are never kept (setting one is a no-op above), so every label read back is empty. ANGLE's own getter raises an error for
 * an object type its context does not have (a 3.1 one, under the shim) and then writes nothing, not even the length, which a game
 * then uses to copy the label. */
static void w_glGetObjectLabel(unsigned type, unsigned obj, int bufsize, int *length, char *label)
{
    (void)type; (void)obj;
    if (length) *length = 0;
    if (label && bufsize > 0) label[0] = 0;
}
static void w_glGetObjectPtrLabel(const void *ptr, int bufsize, int *length, char *label) { w_glGetObjectLabel(0, 0, bufsize, length, label); (void)ptr; }

static const struct { const char *name; void *fn; } k_egl[] = {
    { "glGetObjectLabelEXT", w_glGetObjectLabel }, { "glGetObjectLabel", w_glGetObjectLabel }, { "glGetObjectLabelKHR", w_glGetObjectLabel },
    { "glGetObjectPtrLabel", w_glGetObjectPtrLabel }, { "glGetObjectPtrLabelKHR", w_glGetObjectPtrLabel },
    { "eglGetDisplay", w_eglGetDisplay }, { "eglInitialize", w_eglInitialize }, { "eglTerminate", w_eglTerminate },
    { "eglQueryString", w_eglQueryString }, { "eglGetConfigs", w_eglGetConfigs }, { "eglChooseConfig", w_eglChooseConfig },
    { "eglGetConfigAttrib", w_eglGetConfigAttrib }, { "eglCreateContext", w_eglCreateContext },
    { "eglDestroyContext", w_eglDestroyContext }, { "eglCreateWindowSurface", w_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", w_eglCreatePbufferSurface }, { "eglDestroySurface", w_eglDestroySurface },
    { "eglQuerySurface", w_eglQuerySurface }, { "eglSurfaceAttrib", w_eglSurfaceAttrib }, { "eglMakeCurrent", w_eglMakeCurrent },
    { "eglSwapBuffers", w_eglSwapBuffers }, { "eglSwapInterval", w_eglSwapInterval },
    { "eglSwapBuffersWithDamageKHR", w_eglSwapBuffersWithDamage }, { "eglSwapBuffersWithDamageEXT", w_eglSwapBuffersWithDamage },
    { "eglSetDamageRegionKHR", w_eglSetDamageRegion },
    { "eglGetCurrentContext", w_eglGetCurrentContext }, { "eglGetCurrentSurface", w_eglGetCurrentSurface },
    { "eglGetCurrentDisplay", w_eglGetCurrentDisplay }, { "eglBindAPI", w_eglBindAPI }, { "eglQueryAPI", w_eglQueryAPI },
    { "eglGetError", w_eglGetError }, { "eglWaitGL", w_eglWaitGL }, { "eglWaitNative", w_eglWaitNative },
    { "eglReleaseThread", w_eglReleaseThread },
    { "eglPresentationTimeANDROID", w_eglPresentationTimeANDROID }, { "eglGetNextFrameIdANDROID", w_eglGetNextFrameIdANDROID },
    { "eglGetFrameTimestampsANDROID", w_eglGetFrameTimestampsANDROID }, { "eglGetCompositorTimingANDROID", w_eglGetCompositorTimingANDROID },
};


/* ----------------------------------------------------------- GL call tracing */

/*
 * TL_GL_TRACE: every GL function the guest is given is wrapped in a trampoline that notes its name in a ring and then
 * tail-calls the real function with the registers (and stack) as they were, so any signature works. A crash report
 * can then say which calls came last (tl_egl_recent_calls). The trampolines live in the executable region.
 */
static const char *volatile g_glt_ring[GLT_RING];

/* how often each GL function was called, keyed by the (unique, interned) name pointer */
static struct { _Atomic(const char *) name; atomic_ulong n; } g_glt_hist[1024];
void tl_egl_gl_histogram(char *out, size_t cap)
{
    size_t k = 0; out[0] = 0;
    for (int i = 0; i < 1024 && k + 48 < cap; i++) {
        const char *nm = atomic_load(&g_glt_hist[i].name);
        if (nm) k += (size_t)snprintf(out + k, cap - k, "%s=%lu ", nm, (unsigned long)atomic_load(&g_glt_hist[i].n));
    }
}
void tl_glt_note(const char *name)
{
    g_glt_ring[atomic_fetch_add(&g_glt_n, 1) % GLT_RING] = name;
    for (unsigned h = (unsigned)(((uintptr_t)name >> 3) * 2654435761u) % 1024, probe = 0; probe < 1024; probe++, h = (h + 1) % 1024) {
        const char *cur = atomic_load(&g_glt_hist[h].name);
        if (!cur) { const char *z = NULL; if (atomic_compare_exchange_strong(&g_glt_hist[h].name, &z, name)) cur = name; else cur = z; }
        if (cur == name) { atomic_fetch_add(&g_glt_hist[h].n, 1); break; }
    }
    if (name[2] == 'D' && !strncmp(name, "glDraw", 6)) atomic_fetch_add(&g_glt_draws, 1);
    else if (!strncmp(name, "glBufferData", 12) || !strncmp(name, "glTexImage", 10) || !strncmp(name, "glTexSubImage", 13) || !strncmp(name, "glCompressedTex", 15)) atomic_fetch_add(&g_glt_uploads, 1);
}

void tl_egl_recent_calls(char *out, size_t n, int count)
{
    size_t k = 0;
    unsigned long total = atomic_load(&g_glt_n);
    out[0] = 0;
    for (int i = count; i >= 1 && k + 40 < n; i--) {
        if (total < (unsigned long)i) continue;
        const char *nm = g_glt_ring[(total - (unsigned long)i) % GLT_RING];
        if (nm) k += (size_t)snprintf(out + k, n - k, "%s%s", k ? " " : "", nm);
    }
}

__attribute__((naked, used)) static void glt_common(void)
{
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x29, x30, [sp, #-16]!\n"
        "sub sp, sp, #176\n"
        "stp x0, x1, [sp, #0]\n" "stp x2, x3, [sp, #16]\n" "stp x4, x5, [sp, #32]\n" "stp x6, x7, [sp, #48]\n"
        "stp x8, x17, [sp, #64]\n"
        "stp d0, d1, [sp, #80]\n" "stp d2, d3, [sp, #96]\n" "stp d4, d5, [sp, #112]\n" "stp d6, d7, [sp, #128]\n"
        "mov x0, x16\n"
        "bl _tl_glt_note\n"
        "ldp x0, x1, [sp, #0]\n" "ldp x2, x3, [sp, #16]\n" "ldp x4, x5, [sp, #32]\n" "ldp x6, x7, [sp, #48]\n"
        "ldp x8, x17, [sp, #64]\n"
        "ldp d0, d1, [sp, #80]\n" "ldp d2, d3, [sp, #96]\n" "ldp d4, d5, [sp, #112]\n" "ldp d6, d7, [sp, #128]\n"
        "add sp, sp, #176\n"
        "ldp x29, x30, [sp], #16\n"
        "br x17\n");
#endif
}

static void *glt_wrap(const char *name, void *real)
{
    static struct { const char *name; void *thunk; } cache[4096];
    static int n;
    static uint8_t *page_rx, *page_rw;
    static int used = 256;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) { void *t = cache[i].thunk; pthread_mutex_unlock(&mu); return t; }
    void *thunk = real;
    char err[120];
    if (n < 4096 && tl_xmem_open(768u << 20, err, sizeof(err))) {
        if (used == 256) { used = 0; if (!tl_xmem_alloc(TL_XMEM_PAGE, &page_rx, &page_rw)) page_rx = page_rw = NULL; }
        if (page_rx) {
            uint8_t *trx = page_rx + (size_t)used * 64, *trw = page_rw + (size_t)used * 64;
            used++;
            /* ldr x16, <name>; ldr x17, <real>; ldr x15, <common>; br x15 -- then the three 8-byte literals */
            uint32_t ins[4] = { 0x58000000u | ((16u / 4) << 5) | 16, 0x58000000u | ((20u / 4) << 5) | 17, 0x58000000u | ((24u / 4) << 5) | 15, 0xD61F01E0u };
            memcpy(trw, ins, 16);
            uint64_t lit[3] = { (uint64_t)(uintptr_t)strdup(name), (uint64_t)(uintptr_t)real, (uint64_t)(uintptr_t)glt_common };
            memcpy(trw + 16, lit, 24);
            tl_xmem_flush(trx, 64);
            thunk = trx;
        }
    }
    cache[n].name = strdup(name); cache[n].thunk = thunk; n++;
    pthread_mutex_unlock(&mu);
    return thunk;
}

/* Debug labels and groups name objects for a graphics debugger and change nothing else. ANGLE's versions of them are not
 * worth reaching (one dereferences a null while the guest names a buffer), so they do nothing. */
static void w_gl_noop(void) {}
static const char *const k_noop_names[] = {
    "glObjectLabel", "glObjectLabelKHR", "glObjectPtrLabel", "glObjectPtrLabelKHR", "glLabelObjectEXT", "glInsertEventMarkerEXT",
    "glPushGroupMarkerEXT", "glPopGroupMarkerEXT", "glPushDebugGroup", "glPushDebugGroupKHR", "glPopDebugGroup", "glPopDebugGroupKHR",
    "glDebugMessageInsert", "glDebugMessageInsertKHR", "glDebugMessageCallback", "glDebugMessageCallbackKHR", "glDebugMessageControl",
    "glDebugMessageControlKHR", NULL
};

/* ES 3.1 entry points an ES 3.0 context cannot honour. Under the 3.1 shim a game that asks for them is handed an empty function,
 * so that setup which merely creates the objects goes through; whatever really depends on them draws nothing. */
static const char *const k_es31_stub_names[] = {
    "glTexBufferEXT", "glTexBufferOES", "glTexBuffer", "glTexBufferRangeEXT", "glTexBufferRangeOES", "glTexBufferRange",
    "glBindImageTexture", "glDispatchCompute", "glDispatchComputeIndirect", "glMemoryBarrier", "glMemoryBarrierByRegion",
    "glFramebufferParameteri", "glGetFramebufferParameteriv", "glTexStorage2DMultisample", NULL
};

static void *egl_resolve(const char *name);
void *tl_egl_resolve(const char *name)
{
    void *r = egl_resolve(name);
    if (name) {
        static char seen[512][48];
        static int nseen;
        static pthread_mutex_t smu = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&smu);
        bool found = false;
        for (int i = 0; i < nseen; i++) {
            if (!strcmp(seen[i], name)) { found = true; break; }
        }
        if (!found && nseen < 512) {
            strncpy(seen[nseen++], name, 47);
            tl_log_line("gl: resolve %s -> %p", name, r);
        }
        pthread_mutex_unlock(&smu);
    }
    return r;
}
static void *egl_resolve(const char *name)
{
    for (int i = 0; k_noop_names[i]; i++) if (!strcmp(k_noop_names[i], name)) return (void *)w_gl_noop;
    if (g_es31_shim) for (int i = 0; k_es31_stub_names[i]; i++) if (!strcmp(k_es31_stub_names[i], name)) {
        void *real = gl_proc(name);
        if (real) break;
        static int said; if (!said++) tl_log_line("gl: stubbing ES 3.1 entry points ANGLE lacks (first: %s)", name);
        return (void *)w_gl_noop;
    }
    for (size_t i = 0; i < sizeof(k_egl) / sizeof(k_egl[0]); i++) if (!strcmp(k_egl[i].name, name)) return k_egl[i].fn;
    if (!E.ready) return NULL;
    void *real = gl_proc(name);
    if (!real) return NULL;
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_GL_TRACE") ? 1 : 0;
    for (size_t i = 0; i < sizeof(k_adapt) / sizeof(k_adapt[0]); i++) {
        if (!strcmp(k_adapt[i].name, name)) { *k_adapt[i].real = real; return trace ? glt_wrap(name, k_adapt[i].wrap) : k_adapt[i].wrap; }
    }
    return trace && name[0] == 'g' && name[1] == 'l' ? glt_wrap(name, real) : real;
}

/* eglGetProcAddress for the guest */
static void *w_eglGetProcAddress(const char *name) { return tl_egl_resolve(name); }
const tl_bionic_entry tl_tab_egl[] = {
    TL_WRAP("eglGetProcAddress", w_eglGetProcAddress), TL_WRAP("glGetString", w_glGetString),
    { "eglGetDisplay", w_eglGetDisplay }, { "eglInitialize", w_eglInitialize }, { "eglTerminate", w_eglTerminate },
    { "eglQueryString", w_eglQueryString }, { "eglGetConfigs", w_eglGetConfigs }, { "eglChooseConfig", w_eglChooseConfig },
    { "eglGetConfigAttrib", w_eglGetConfigAttrib }, { "eglCreateContext", w_eglCreateContext },
    { "eglDestroyContext", w_eglDestroyContext }, { "eglCreateWindowSurface", w_eglCreateWindowSurface },
    { "eglCreatePbufferSurface", w_eglCreatePbufferSurface }, { "eglDestroySurface", w_eglDestroySurface },
    { "eglQuerySurface", w_eglQuerySurface }, { "eglSurfaceAttrib", w_eglSurfaceAttrib }, { "eglMakeCurrent", w_eglMakeCurrent },
    { "eglSwapBuffers", w_eglSwapBuffers }, { "eglSwapInterval", w_eglSwapInterval },
    { "eglSwapBuffersWithDamageKHR", w_eglSwapBuffersWithDamage }, { "eglSwapBuffersWithDamageEXT", w_eglSwapBuffersWithDamage },
    { "eglSetDamageRegionKHR", w_eglSetDamageRegion },
    { "eglGetCurrentContext", w_eglGetCurrentContext }, { "eglGetCurrentSurface", w_eglGetCurrentSurface },
    { "eglGetCurrentDisplay", w_eglGetCurrentDisplay }, { "eglBindAPI", w_eglBindAPI }, { "eglQueryAPI", w_eglQueryAPI },
    { "eglGetError", w_eglGetError }, { "eglWaitGL", w_eglWaitGL }, { "eglWaitNative", w_eglWaitNative },
    { "eglReleaseThread", w_eglReleaseThread },
    TL_END
};
