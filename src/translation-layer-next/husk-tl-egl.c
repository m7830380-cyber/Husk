/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-egl.h"

#include <ctype.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#endif

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

/* The OpenGL ES underneath: ANGLE (over Metal), or Mesa's Zink (over Vulkan on MoltenVK, the game's "OpenGL ES (Zink)" renderer).
 * Zink is a whole OpenGL ES 3.2 with compute: none of ANGLE's stand-ins (the 3.0 shim, its 3.1 switch) are used on it. Mesa's EGL
 * runs surfaceless here -- iOS has no window system it knows -- so a game's window is an off-screen buffer whose frames are put on
 * the window's Metal layer at each swap (tl_present_rgba). */
static bool g_zink;
static bool g_es31_shim;           /* the 3.0 shim (husk-tl-egl-es31.inc) */
void tl_present_rgba(void *layer, const void *rgba, int w, int h);   /* husk-tl-present.m */

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

#define LOAD(lib, name) do { a_##name = dlsym(lib, #name); if (!a_##name) tl_log_line("egl: ANGLE does not export " #name); } while (0)

bool tl_egl_init(const char *egl_path, const char *gles_path, const char *frame_dir, int frame_every)
{
    pthread_mutex_lock(&E.lock);
    if (E.ready) { pthread_mutex_unlock(&E.lock); return true; }
    char fallback[600] = "";
    const bool shim_before = g_es31_shim;   /* what an engine claimed for ANGLE: given back if Zink hands over to it */
reload:
    snprintf(E.egl_path, sizeof(E.egl_path), "%s", egl_path);
    snprintf(E.gles_path, sizeof(E.gles_path), "%s", gles_path ? gles_path : egl_path);
    const char *base = strrchr(egl_path, '/');
    g_zink = !strcmp(base ? base + 1 : egl_path, "libEGL.1.dylib");
    if (g_zink) {
        /* Mesa's GL entry points are in libGLESv2, beside its libEGL */
        if (!gles_path) {
            snprintf(E.gles_path, sizeof(E.gles_path), "%.*slibGLESv2.2.dylib", base ? (int)(base + 1 - egl_path) : 0, egl_path);
            gles_path = E.gles_path;
        }
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        if (!getenv("EGL_LOG_LEVEL")) setenv("EGL_LOG_LEVEL", "info", 1);
        if (!getenv("MESA_SHADER_CACHE_DIR")) { const char *t = getenv("TMPDIR"); if (t) { char c[600]; snprintf(c, sizeof(c), "%s/mesa-shader-cache", t); setenv("MESA_SHADER_CACHE_DIR", c, 1); } }
        g_es31_shim = false;   /* whatever an engine claimed before this: Zink's versions are real */
        /* Mesa holds back OpenGL ES 3 for features MoltenVK lacks (transform feedback, and others Metal has no stage for); the rest of
         * 3.1 is there, and the games here need its contexts. A version set by hand is kept. */
        if (!getenv("MESA_GLES_VERSION_OVERRIDE")) setenv("MESA_GLES_VERSION_OVERRIDE", "3.1", 1);
    }
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
    if (g_zink && !fallback[0]) {
        /* Zink stands on the device's Vulkan: a device it refuses leaves no display at all, and the game no GL. Bring it up here, and
         * when it does not come, use ANGLE beside it instead */
        EGLDisplay d = a_eglGetPlatformDisplayEXT(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, NULL, NULL);
        EGLint mj = 0, mn = 0;
        EGLint es3 = 0;
        /* surfaceless offers pbuffer configs only, and eglChooseConfig's SURFACE_TYPE defaults to WINDOW_BIT: ask for any */
        const EGLint want_es3[] = { 0x3040 /* EGL_RENDERABLE_TYPE */, 0x40 /* EGL_OPENGL_ES3_BIT */, 0x3033 /* EGL_SURFACE_TYPE */, -1 /* EGL_DONT_CARE */, EGL_NONE };
        bool up = d && a_eglInitialize(d, &mj, &mn);
        if (up) {
            EGLint all = 0;
            a_eglGetConfigs(d, NULL, 0, &all);
            a_eglChooseConfig(d, want_es3, NULL, 0, &es3);
            tl_log_line("egl: Zink client APIs \"%s\", %d config(s), %d with ES 3", a_eglQueryString(d, 0x308D /* EGL_CLIENT_APIS */), all, es3);
        }
        /* a device Zink takes but cannot give OpenGL ES 3 on: games here want 3, so ANGLE serves them instead */
        if (up && !es3) tl_log_line("egl: Zink offers no OpenGL ES 3 config");
        if (up && es3) { E.display = d; tl_log_line("egl: Zink display up, EGL %d.%d, %d ES 3 config(s)", mj, mn, es3); }
        else {
            snprintf(fallback, sizeof(fallback), "%.*slibANGLE-shared.dylib", base ? (int)(base + 1 - egl_path) : 0, egl_path);
            tl_log_line("egl: Zink did not start (display %p, error 0x%x): using ANGLE (%s)", d, a_eglGetError ? a_eglGetError() : 0, fallback);
            egl_path = fallback; gles_path = NULL; E.gles = NULL; E.egl = NULL;
            g_es31_shim = shim_before;   /* ANGLE's 3.0 shim serves again */
            goto reload;
        }
    }
    /* GLES functions this layer itself calls (frame capture) */
    a_glReadPixels = a_eglGetProcAddress("glReadPixels");
    a_glGetIntegerv = a_eglGetProcAddress("glGetIntegerv");
    a_glBindFramebuffer = a_eglGetProcAddress("glBindFramebuffer");
    a_glPixelStorei = a_eglGetProcAddress("glPixelStorei");
    if (frame_dir) { snprintf(E.frame_dir, sizeof(E.frame_dir), "%s", frame_dir); E.latest_only = frame_every < 0; E.frame_every = frame_every != 0 ? (frame_every < 0 ? -frame_every : frame_every) : 60; }
    E.ready = true;
    pthread_mutex_unlock(&E.lock);
    tl_log_line("egl: %s loaded%s%s", g_zink ? "Zink (Mesa, over MoltenVK)" : "ANGLE", frame_dir ? ", off-screen, frames to " : "", frame_dir ? frame_dir : "");
    return true;
}

unsigned long tl_egl_frames_presented(void) { return atomic_load(&E.presented); }

/* ------------------------------------------------------------ EGL wrappers */

static EGLDisplay w_eglGetDisplay(void *native)
{
    (void)native;
    pthread_mutex_lock(&E.lock);
    if (!E.display && E.ready && g_zink) {
        E.display = a_eglGetPlatformDisplayEXT(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, NULL, NULL);
        tl_log_line("egl: display %p (Zink, surfaceless)", E.display);
    }
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
        /* surfaceless Zink has no window configs: a window is a buffer here (see w_eglCreateWindowSurface) */
        if (g_zink && at[i] == 0x3033 /* EGL_SURFACE_TYPE */ && at[i + 1] != -1 && (at[i + 1] & 0x4 /* EGL_WINDOW_BIT */))
            filtered[nf - 1] = (at[i + 1] & ~0x4) | 0x1 /* EGL_PBUFFER_BIT */;
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
    if (ok && g_zink && v && at == 0x3033 /* EGL_SURFACE_TYPE */ && (*v & 0x1)) *v |= 0x4;   /* its buffers serve as windows */
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
static bool g_es31_native;         /* the game runs on a real 3.1 context ANGLE made (the OpenGL ES 3.1 game setting) */

/*
 * OpenGL ES 3.1 on ANGLE's Metal backend, experimental (the game's "OpenGL ES 3.1" setting, TL_GL_ES31=1). The backend caps the version
 * it offers in one virtual function, rx::DisplayMtl::getMaxSupportedESVersion (3.0 on GPU family 4+), while its shader compiler and most
 * of the state behind it handle 3.1: what is not there is compute (dispatchCompute is unimplemented), so a game that needs compute
 * shaders gets errors where it would otherwise be told 3.0. The function is found by its symbol in the loaded ANGLE and its slot in
 * DisplayMtl's vtable pointed at one that answers 3.1 -- the same Version encoding (major << 8 | minor) -- or back at the original.
 */
static uint32_t angle_es31_version(const void *self) { (void)self; return 0x0301; }

/*
 * The backend leaves the limits only 3.1 has at 0 in the caps it reports (rx::ContextMtl::getNativeCaps returns gl::Caps by value), and
 * ANGLE's linker refuses any 3.1 program that has a uniform: "Exceeded maximum uniform location size ... max uniform locations = 0".
 * The slot of getNativeCaps in ContextMtl's vtable goes to a trampoline that calls it and then sets maxUniformLocations to 1024 (the
 * 3.1 minimum). The field is found by the limits Metal puts just before it -- maxUniformBufferBindings 32, maxUniformBlockSize 16384
 * (64-bit), uniformBufferOffsetAlignment 16 or 256, maxCombinedUniformBlocks 32 -- so another layout of gl::Caps is left alone.
 */
void *g_husk_caps_orig;
__attribute__((used)) void husk_caps_fix(uint32_t *c)
{
    static int at = -1, told;
    if (at < 0) {
        /* maxUniformBlockSize (int64 16384), uniformBufferOffsetAlignment (16/256), maxCombinedUniformBlocks,
           maxVaryingComponents, maxVaryingVectors, maxCombinedTextureImageUnits, maxCombinedShaderOutputResources (0),
           maxUniformLocations (0). Block and sampler counts differ between ANGLE versions, so only their shape is checked. */
        for (int j = 2; j < 700 && at < 0; j += 2) {
            if (c[j] != 16384 || c[j + 1] != 0 || (c[j + 2] != 16 && c[j + 2] != 256)) continue;
            if (c[j + 3] < 4 || c[j + 3] > 128) continue;
            for (int k = j + 4; k <= j + 8; k++)
                if (c[k] >= 8 && c[k] <= 128 && c[k + 1] == 0 && c[k + 2] == 0) { at = k + 2; break; }
        }
        if (at < 0) {
            at = 0; tl_log_line("egl: es31: ANGLE's caps are laid out differently: uniform locations not raised");
            char b[1100];
            for (int r = 0; r < 700; r += 64) {
                int n = snprintf(b, sizeof b, "egl: es31: caps[%d]:", r);
                for (int i = r; i < r + 64 && i < 700 && n < (int)sizeof b - 12; i++) n += snprintf(b + n, sizeof b - n, " %x", c[i]);
                tl_log_line("%s", b);
            }
        } else {
            tl_log_line("egl: es31: max uniform locations found at caps word %d", at);
        }
    }
    if (at > 0) {
        /* maxCombinedShaderOutputResources sits right before maxUniformLocations; ES 3.1 links count every
           fragment output against it, so 0 refuses any program that draws. 16 covers 8 draw buffers plus room. */
        bool raised = false;
        if ((int32_t)c[at] < 1024) { c[at] = 1024; raised = true; }
        if ((int32_t)c[at - 1] < 16) { c[at - 1] = 16; raised = true; }
        if (raised && !told++) tl_log_line("egl: es31: ANGLE's max uniform locations raised to 1024 and max combined shader output resources to 16 (Metal reports 0 for both)");
    }
}
#if defined(__aarch64__) && defined(__APPLE__)
__attribute__((naked)) static void husk_caps_tramp(void)
{
    __asm__ volatile(
        "stp x29, x30, [sp, #-32]!\n" "mov x29, sp\n" "str x8, [sp, #16]\n"
        "adrp x16, _g_husk_caps_orig@PAGE\n" "ldr x16, [x16, _g_husk_caps_orig@PAGEOFF]\n" "blr x16\n"
        "ldr x0, [sp, #16]\n" "bl _husk_caps_fix\n"
        "ldp x29, x30, [sp], #32\n" "ret\n");
}
#endif

static bool angle_es31(bool on)
{
#ifdef __APPLE__
    static void **slot, **caps_slot; static void *orig; static int found = -1;
    if (found < 0) {
        found = 0;
        Dl_info info;
        if (!a_eglGetProcAddress || !dladdr((void *)a_eglGetProcAddress, &info) || !info.dli_fbase) { tl_log_line("egl: es31: ANGLE's image not found"); return false; }
        const struct mach_header_64 *mh = info.dli_fbase;
        const struct load_command *lc = (const void *)(mh + 1);
        const struct segment_command_64 *text = NULL, *linkedit = NULL; const struct symtab_command *st = NULL;
        const struct section_64 *dconst = NULL;
        for (uint32_t i = 0; i < mh->ncmds; i++, lc = (const void *)((const char *)lc + lc->cmdsize)) {
            if (lc->cmd == LC_SEGMENT_64) {
                const struct segment_command_64 *sg = (const void *)lc;
                if (!strcmp(sg->segname, "__TEXT")) text = sg;
                if (!strcmp(sg->segname, "__LINKEDIT")) linkedit = sg;
                const struct section_64 *sc = (const void *)(sg + 1);
                for (uint32_t k = 0; k < sg->nsects; k++) if (!strcmp(sc[k].segname, "__DATA_CONST") && !strcmp(sc[k].sectname, "__const")) dconst = &sc[k];
            } else if (lc->cmd == LC_SYMTAB) st = (const void *)lc;
        }
        if (!text || !linkedit || !st || !dconst) { tl_log_line("egl: es31: ANGLE's symbol table not found"); return false; }
        intptr_t slide = (intptr_t)mh - (intptr_t)text->vmaddr;
        const char *le = (const char *)(slide + linkedit->vmaddr - linkedit->fileoff);
        const struct nlist_64 *sym = (const void *)(le + st->symoff); const char *str = le + st->stroff;
        uintptr_t fn = 0;
        for (uint32_t i = 0; i < st->nsyms; i++)
            if (sym[i].n_un.n_strx && !strcmp(str + sym[i].n_un.n_strx, "__ZNK2rx10DisplayMtl24getMaxSupportedESVersionEv") && sym[i].n_value) { fn = (uintptr_t)(slide + sym[i].n_value); break; }
        if (!fn) { tl_log_line("egl: es31: ANGLE has no rx::DisplayMtl::getMaxSupportedESVersion"); return false; }
        void **p = (void **)(slide + dconst->addr), **end = (void **)(slide + dconst->addr + dconst->size);
        int hits = 0;
        for (; p < end; p++) if ((uintptr_t)*p == fn) { if (!hits) slot = p; hits++; }
        if (hits != 1) { tl_log_line("egl: es31: %d vtable slots hold getMaxSupportedESVersion; not patching", hits); slot = NULL; return false; }
        orig = *slot; found = 1;
        uintptr_t cfn = 0;
        for (uint32_t i = 0; i < st->nsyms; i++)
            if (sym[i].n_un.n_strx && !strcmp(str + sym[i].n_un.n_strx, "__ZNK2rx10ContextMtl13getNativeCapsEv") && sym[i].n_value) { cfn = (uintptr_t)(slide + sym[i].n_value); break; }
        hits = 0;
        if (cfn) for (p = (void **)(slide + dconst->addr); p < end; p++) if ((uintptr_t)*p == cfn) { if (!hits) caps_slot = p; hits++; }
        if (hits != 1) { tl_log_line("egl: es31: rx::ContextMtl::getNativeCaps %s; uniform locations not raised", cfn ? "is in more than one vtable slot or none" : "not found"); caps_slot = NULL; }
        else g_husk_caps_orig = *caps_slot;
    }
#if defined(__aarch64__)
    if (caps_slot) {
        void *cwant = on ? (void *)husk_caps_tramp : g_husk_caps_orig;
        if (*caps_slot != cwant) {
            long cpg = sysconf(_SC_PAGESIZE);
            void *cpage = (void *)((uintptr_t)caps_slot & ~(uintptr_t)(cpg - 1));
            if (mprotect(cpage, (size_t)cpg, PROT_READ | PROT_WRITE) == 0) { *caps_slot = cwant; mprotect(cpage, (size_t)cpg, PROT_READ); }
            else tl_log_line("egl: es31: cannot write ContextMtl's vtable");
        }
    }
#endif
    if (found != 1) return false;
    void *want = on ? (void *)angle_es31_version : orig;
    if (*slot == want) return true;
    long pg = sysconf(_SC_PAGESIZE);
    void *page = (void *)((uintptr_t)slot & ~(uintptr_t)(pg - 1));
    if (mprotect(page, (size_t)pg, PROT_READ | PROT_WRITE) != 0) { tl_log_line("egl: es31: cannot write ANGLE's vtable"); return false; }
    *slot = want;
    mprotect(page, (size_t)pg, PROT_READ);
    tl_log_line("egl: %s", on ? "OpenGL ES 3.1 offered (experimental, game setting): ANGLE's Metal backend now allows 3.1 contexts" : "OpenGL ES 3.1 setting off: ANGLE back to 3.0");
    return true;
#else
    (void)on; return false;
#endif
}
static EGLContext w_eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share, const EGLint *at)
{
    /* A game that insists on ES 3.1 or 3.2 (Unreal Engine tries 3.2, then 3.1) is given the 3.0 context ANGLE can make: the version it is then told is the shim's. */
    EGLint patched[40];
    /* A newer ANGLE can make a real 3.1 context on Metal: when it does, the game gets what it asked for and the 3.0 shim steps aside
       (its shaders compile as written, with the varyings' explicit locations Unreal's 3.1 shaders use). Only with the game's OpenGL ES 3.1 setting (TL_GL_ES31=1). */
    static int native;                                         /* 1: a real 3.x context was made; bit 1<<minor: ANGLE refused 3.minor */
    static int native_minor;                                   /* the minor version of that real context */
    if (g_zink) {
        EGLContext ctx = a_eglCreateContext(d, cfg_real(c), share, at);
        int maj = 0, min = 0;
        for (int n = 0; at && at[n] != EGL_NONE && n < 36; n += 2) { if (at[n] == 0x3098) maj = at[n + 1]; if (at[n] == 0x30fb) min = at[n + 1]; }
        tl_log_line("egl: Zink OpenGL ES %d.%d context (share %p) -> %p%s", maj, min, share, ctx, ctx ? "" : " (failed)");
        if (!ctx) tl_log_line("egl: eglGetError %#x", a_eglGetError());
        return ctx;
    }
    int want_minor = 0;
    for (int n = 0; at && at[n] != EGL_NONE && n < 36; n += 2) if (at[n] == 0x30fb) want_minor = at[n + 1];
    /* from the version asked for down to 3.1 (a game asking for 3.2 takes 3.1 as well: Unreal asks for 3.2, then 3.1) */
    bool es31 = getenv("TL_GL_ES31") && *getenv("TL_GL_ES31") == '1';
    static int applied = -1;
    if (applied != es31) { angle_es31(es31); applied = es31; }
    for (int m = want_minor < 8 ? want_minor : 0; es31 && g_es31_shim && at && m > 0; m--) {
        if (native & (1 << m)) continue;
        EGLint try_at[40]; int n = 0;
        for (; at[n] != EGL_NONE && n < 36; n += 2) { try_at[n] = at[n]; try_at[n + 1] = at[n] == 0x30fb ? m : at[n + 1]; }
        try_at[n] = EGL_NONE;
        EGLContext real = a_eglCreateContext(d, cfg_real(c), share, try_at);
        if (real) {
            if (!(native & 1)) tl_log_line("egl: ANGLE made a real OpenGL ES 3.%d context; the 3.0 shim is off", m);
            native |= 1; native_minor = m; g_es31_shim = false; g_es31_native = true;
            return real;
        }
        tl_log_line("egl: ANGLE has no OpenGL ES 3.%d context (eglGetError %#x)", m, a_eglGetError ? a_eglGetError() : 0);
        native |= 1 << m;
    }
    /* Once a real 3.x context exists, later ones (shared contexts, the rendering thread's) asking for more are given that version too:
       the game is already running on it, and ANGLE refuses the higher minor every time. */
    if (es31 && !g_es31_shim && (native & 1) && at && want_minor > native_minor) {
        int n = 0;
        for (; at[n] != EGL_NONE && n < 36; n += 2) { patched[n] = at[n]; patched[n + 1] = at[n] == 0x30fb ? native_minor : at[n + 1]; }
        patched[n] = EGL_NONE;
        at = patched;
    }
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
/* Zink's windows: buffers, each put on its window's layer at a swap */
#define MAX_ZINK_SURFACES 8
static struct { EGLSurface s; void *layer; } g_zink_surf[MAX_ZINK_SURFACES];
static void *zink_layer_of(EGLSurface s)
{
    void *l = NULL;
    pthread_mutex_lock(&g_tex_lock);
    for (int i = 0; i < MAX_ZINK_SURFACES && s; i++) if (g_zink_surf[i].s == s) { l = g_zink_surf[i].layer; break; }
    pthread_mutex_unlock(&g_tex_lock);
    return l;
}
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
    if (g_zink) {
        EGLint pb[] = { EGL_WIDTH, tl_nwindow_width(win), EGL_HEIGHT, tl_nwindow_height(win), EGL_NONE };
        EGLSurface s = a_eglCreatePbufferSurface(d, cfg, pb);
        if (s) {
            pthread_mutex_lock(&g_tex_lock);
            for (int i = 0; i < MAX_ZINK_SURFACES; i++) if (!g_zink_surf[i].s) { g_zink_surf[i].s = s; g_zink_surf[i].layer = layer; break; }
            pthread_mutex_unlock(&g_tex_lock);
        }
        tl_log_line("egl: window %dx%d on layer %p -> Zink buffer %p%s", pb[1], pb[3], layer, s, s ? "" : " (failed)");
        if (!s) tl_log_line("egl: eglGetError %#x", a_eglGetError());
        return s;
    }
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
    for (int i = 0; i < MAX_ZINK_SURFACES && s; i++) if (g_zink_surf[i].s == s) g_zink_surf[i].s = NULL;
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
static void diag_swap(unsigned long n);   /* husk-tl-egl-diag.inc */
static EGLBoolean w_eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    void *texwin = tex_window_of(s);
    if (texwin) { tex_swap(d, s, texwin); return EGL_TRUE; }
    unsigned long n = atomic_fetch_add(&E.presented, 1) + 1;
    diag_swap(n);
    if (comp_surface(s)) {
        EGLint w = 0, h = 0;
        const uint8_t *px = read_back(d, s, &w, &h);
        if (px && tl_javaapp_app_pixels) tl_javaapp_app_pixels(px, w, h);
        if (n <= 3 || n % 600 == 0) tl_log_line("egl: swap #%lu (composited)", n);
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
    void *zl = g_zink && !E.frame_dir[0] ? zink_layer_of(s) : NULL;
    if (zl) {
        EGLint w = 0, h = 0;
        const uint8_t *px = read_back(d, s, &w, &h);
        if (px) tl_present_rgba(zl, px, w, h);
        return EGL_TRUE;
    }
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

/* texture buffers where ANGLE has none (husk-tl-egl-tbo.inc) */
static bool g_tbo_any;
static bool tbo_on(void);
static void tbo_draw(void);
static void tbo_forget_tex(unsigned tex);

#include "husk-tl-egl-tex.inc"
/* compute fallback (husk-tl-egl-cs.inc), reached from earlier wrappers */
static bool cs_fake(unsigned prog);
static void cs_linked(unsigned prog);
static unsigned g_cs_cur;
#include "husk-tl-egl-diag.inc"

/* ------------------------------------------------- GLES: stack-argument fixes */

/*
 * AAPCS64 gives each stack argument its own 8-byte slot; Apple's ABI packs small ones. A call
 * with two 4-byte stack arguments therefore lays them out differently, and the callee reads
 * the second from the wrong place. Each adapter takes its stack arguments as 8-byte values,
 * which is how the guest wrote them, and passes them on properly typed.
 */
static void (*r_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
static void w_glBlitFramebuffer(int a0, int a1, int a2, int a3, int a4, int a5, int a6, int a7, uint64_t mask, uint64_t filter)
{ r_glBlitFramebuffer(a0, a1, a2, a3, a4, a5, a6, a7, (unsigned)mask, (unsigned)filter); cap_dirty(); if (!cap_here()) err_check("glBlitFramebuffer"); }

static void (*r_glTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t type, const void *px)
{ r_glTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (unsigned)type, px); if (diag_on()) atomic_fetch_add(&g_diag_arr_sub, 1); }

static void (*r_glCompressedTexSubImage3D)(unsigned, int, int, int, int, int, int, int, unsigned, int, const void *);
static void w_glCompressedTexSubImage3D(unsigned t, int l, int x, int y, int z, int w, int h, int d, uint64_t fmt, uint64_t size, const void *data)
{ int pbo; void *m; const void *src = cpbo_begin((int)size, data, &pbo, &m);
  r_glCompressedTexSubImage3D(t, l, x, y, z, w, h, d, (unsigned)fmt, (int)size, src); if (diag_on()) atomic_fetch_add(&g_diag_arr_csub, 1); cpbo_end(pbo, m); }

/*
 * EXT_copy_image on ANGLE's Metal back end: the entry point is there, but TextureMtl has no copyTextureSubData, so the extension is
 * not offered and a call does nothing. Unity's Graphics.CopyTexture is that call -- it is how a game builds a texture array out of
 * single textures at run time (GoreBox's character and terrain maps) and how reflection probes fill their mips -- and without it those
 * textures stay empty. Here it is done with what ES 3.0 has: the source image is the read framebuffer's attachment, and the
 * destination takes it by glCopyTexSubImage (colour), or by glBlitFramebuffer (depth/stencil, or a renderbuffer destination).
 * Compressed images cannot be read back in ES 3.0 and are not copied.
 */
static int g_copyimg_native = -1;   /* -1: not looked yet; 1: ANGLE offers EXT/OES_copy_image itself */
static bool copyimg_native(void)
{
    if (g_copyimg_native < 0 && a_eglGetProcAddress) {
        const unsigned char *(*gs)(unsigned) = a_eglGetProcAddress("glGetString");
        const char *s = gs ? (const char *)gs(0x1F03 /* GL_EXTENSIONS */) : NULL;
        if (s) g_copyimg_native = (strstr(s, "GL_EXT_copy_image") || strstr(s, "GL_OES_copy_image")) ? 1 : 0;
    }
    return g_copyimg_native == 1;
}
static const char *const k_copyimg_ext[] = { "GL_EXT_copy_image", "GL_OES_copy_image" };
#define COPYIMG_EXT_COUNT ((int)(sizeof(k_copyimg_ext) / sizeof(k_copyimg_ext[0])))
static int copyimg_extra(void) { return copyimg_native() ? 0 : COPYIMG_EXT_COUNT; }

static struct {
    bool ok;
    void (*gen_fb)(int, unsigned *), (*del_fb)(int, const unsigned *), (*bind_fb)(unsigned, unsigned);
    void (*fb_tex2d)(unsigned, unsigned, unsigned, unsigned, int), (*fb_layer)(unsigned, unsigned, unsigned, int, int);
    void (*fb_rb)(unsigned, unsigned, unsigned, unsigned);
    unsigned (*status)(unsigned);
    void (*bind_tex)(unsigned, unsigned), (*get_iv)(unsigned, int *);
    void (*copy2d)(unsigned, int, int, int, int, int, int, int), (*copy3d)(unsigned, int, int, int, int, int, int, int, int);
    void (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    unsigned char (*is_enabled)(unsigned);
    void (*enable)(unsigned), (*disable)(unsigned);
} CI;
static bool copyimg_load(void)
{
    if (CI.ok) return true;
    if (!a_eglGetProcAddress) return false;
#define CIP(f, n) CI.f = a_eglGetProcAddress(n)
    CIP(gen_fb, "glGenFramebuffers"); CIP(del_fb, "glDeleteFramebuffers"); CIP(bind_fb, "glBindFramebuffer");
    CIP(fb_tex2d, "glFramebufferTexture2D"); CIP(fb_layer, "glFramebufferTextureLayer"); CIP(fb_rb, "glFramebufferRenderbuffer");
    CIP(status, "glCheckFramebufferStatus"); CIP(bind_tex, "glBindTexture"); CIP(get_iv, "glGetIntegerv");
    CIP(copy2d, "glCopyTexSubImage2D"); CIP(copy3d, "glCopyTexSubImage3D"); CIP(blit, "glBlitFramebuffer");
    CIP(is_enabled, "glIsEnabled"); CIP(enable, "glEnable"); CIP(disable, "glDisable");
#undef CIP
    CI.ok = CI.gen_fb && CI.del_fb && CI.bind_fb && CI.fb_tex2d && CI.fb_layer && CI.fb_rb && CI.status && CI.bind_tex && CI.get_iv &&
            CI.copy2d && CI.copy3d && CI.blit && CI.is_enabled && CI.enable && CI.disable;
    return CI.ok;
}

#define CI_RENDERBUFFER 0x8D41u
#define CI_TEX_2D 0x0DE1u
#define CI_TEX_CUBE 0x8513u
#define CI_TEX_3D 0x806Fu
#define CI_TEX_2D_ARRAY 0x8C1Au
#define CI_TEX_CUBE_ARRAY 0x9009u
#define CI_TEX_2D_MS 0x9100u
/* one image (level, layer/face) of a texture or a renderbuffer as attachment `att` of framebuffer target `fbt`; name 0 detaches */
static void copyimg_attach(unsigned fbt, unsigned att, unsigned name, unsigned target, int level, int layer)
{
    if (target == CI_RENDERBUFFER) CI.fb_rb(fbt, att, CI_RENDERBUFFER, name);
    else if (target == CI_TEX_CUBE) CI.fb_tex2d(fbt, att, 0x8515u /* GL_TEXTURE_CUBE_MAP_POSITIVE_X */ + (unsigned)layer, name, level);
    else if (target == CI_TEX_3D || target == CI_TEX_2D_ARRAY || target == CI_TEX_CUBE_ARRAY) CI.fb_layer(fbt, att, name, level, layer);
    else CI.fb_tex2d(fbt, att, target, name, level);
}
static unsigned copyimg_binding_query(unsigned target)
{
    switch (target) {
    case CI_TEX_2D: return 0x8069;        /* GL_TEXTURE_BINDING_2D */
    case CI_TEX_CUBE: return 0x8514;      /* GL_TEXTURE_BINDING_CUBE_MAP */
    case CI_TEX_3D: return 0x806A;        /* GL_TEXTURE_BINDING_3D */
    case CI_TEX_2D_ARRAY: return 0x8C1D;  /* GL_TEXTURE_BINDING_2D_ARRAY */
    default: return 0;
    }
}

static void copyimg_emulate(unsigned sn, unsigned st, int sl, int sx, int sy, int sz, unsigned dn, unsigned dt,
                            int dl, int dx, int dy, int dz, int w, int h, int d)
{
    static atomic_int said_fail, said_first;
    if (w <= 0 || h <= 0 || d <= 0 || !sn || !dn) return;
    if (!copyimg_load()) { if (!atomic_exchange(&said_fail, 1)) tl_log_line("gl: copy_image: ANGLE lacks what the emulation needs"); return; }
    atomic_fetch_add(&g_diag_copy_n, 1);
    if (dt == CI_TEX_2D_ARRAY || dt == CI_TEX_3D) {
        int c = atomic_fetch_add(&g_diag_arr_copy_in, 1);
        if (c < 8 && diag_on()) tl_log_line("gldiag: copy %#x %u lvl %d (%d,%d,%d) -> %#x %u lvl %d layer %d, %dx%dx%d", st, sn, sl, sx, sy, sz, dt, dn, dl, dz, w, h, d);
    }
    if (!atomic_exchange(&said_first, 1))
        tl_log_line("gl: glCopyImageSubData emulated (ANGLE/Metal has no copy_image): first %#x %u lvl %d -> %#x %u lvl %d, %dx%dx%d",
                    st, sn, sl, dt, dn, dl, w, h, d);

    int old_read = 0, old_draw = 0, old_tex = 0;
    CI.get_iv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &old_read);
    CI.get_iv(0x8CA6 /* GL_DRAW_FRAMEBUFFER_BINDING */, &old_draw);
    unsigned bq = dt != CI_RENDERBUFFER ? copyimg_binding_query(dt) : 0;
    if (bq) CI.get_iv(bq, &old_tex);
    bool scissor = CI.is_enabled(0x0C11 /* GL_SCISSOR_TEST */) != 0;

    unsigned fb[2] = { 0, 0 };
    CI.gen_fb(2, fb);
    const unsigned READ = 0x8CA8, DRAW = 0x8CA9, COLOR0 = 0x8CE0, COMPLETE = 0x8CD5;
    CI.bind_fb(READ, fb[0]);
    bool texbound = false, failed = false;

    for (int i = 0; i < d && !failed; i++) {
        int sly = sz + i, dly = dz + i;
        /* colour: the source as the read framebuffer's colour attachment */
        copyimg_attach(READ, COLOR0, sn, st, sl, sly);
        bool color = CI.status(READ) == COMPLETE;
        if (color && dt != CI_RENDERBUFFER && dt != CI_TEX_2D_MS) {
            if (!texbound) { CI.bind_tex(dt == CI_TEX_CUBE ? CI_TEX_CUBE : dt, dn); texbound = true; }
            bool watch = dt == CI_TEX_CUBE && diag_on() && diag_load();
            int rfmt = 0, rtype = 0;
            if (watch) { (void)DG.get_error(); CI.get_iv(0x8B9B /* IMPLEMENTATION_COLOR_READ_FORMAT */, &rfmt); CI.get_iv(0x8B9A /* ..._TYPE */, &rtype); }
            if (dt == CI_TEX_3D || dt == CI_TEX_2D_ARRAY || dt == CI_TEX_CUBE_ARRAY) CI.copy3d(dt, dl, dx, dy, dly, sx, sy, w, h);
            else CI.copy2d(dt == CI_TEX_CUBE ? 0x8515u + (unsigned)dly : dt, dl, dx, dy, sx, sy, w, h);
            if (watch) diag_cube_copy_done(sn, st, sl, dn, dl, dly, w, h, DG.get_error(), rfmt, rtype);
            continue;
        }
        /* depth/stencil, a renderbuffer or multisampled destination: a blit between two framebuffers */
        unsigned att = COLOR0, mask = 0x4000 /* GL_COLOR_BUFFER_BIT */;
        if (!color) {
            copyimg_attach(READ, COLOR0, 0, st, sl, sly);
            static const unsigned atts[3] = { 0x821A /* DEPTH_STENCIL */, 0x8D00 /* DEPTH */, 0x8D20 /* STENCIL */ };
            static const unsigned masks[3] = { 0x0100 | 0x0400, 0x0100, 0x0400 };
            att = 0;
            for (int k = 0; k < 3; k++) {
                copyimg_attach(READ, atts[k], sn, st, sl, sly);
                if (CI.status(READ) == COMPLETE) { att = atts[k]; mask = masks[k]; break; }
                copyimg_attach(READ, atts[k], 0, st, sl, sly);
            }
            if (!att) {
                static atomic_int said;
                if (atomic_fetch_add(&said, 1) < 4)
                    tl_log_line("gl: copy_image: %#x %u lvl %d cannot be read (compressed or not renderable): not copied", st, sn, sl);
                failed = true;
                atomic_fetch_add(&g_diag_copy_fail, 1);
                break;
            }
        }
        if (!fb[1]) break;
        CI.bind_fb(DRAW, fb[1]);
        copyimg_attach(DRAW, att, dn, dt, dl, dly);
        if (CI.status(DRAW) == COMPLETE) {
            if (scissor) CI.disable(0x0C11);
            CI.blit(sx, sy, sx + w, sy + h, dx, dy, dx + w, dy + h, mask, 0x2600 /* GL_NEAREST */);
            if (scissor) CI.enable(0x0C11);
        } else {
            static atomic_int said;
            atomic_fetch_add(&g_diag_copy_fail, 1);
            if (atomic_fetch_add(&said, 1) < 4) tl_log_line("gl: copy_image: destination %#x %u lvl %d cannot be drawn to: not copied", dt, dn, dl);
            failed = true;
        }
        copyimg_attach(DRAW, att, 0, dt, dl, dly);
        if (att != COLOR0) copyimg_attach(READ, att, 0, st, sl, sly);
    }

    CI.bind_fb(READ, (unsigned)old_read);
    CI.bind_fb(DRAW, (unsigned)old_draw);
    if (texbound) CI.bind_tex(dt == CI_TEX_CUBE ? CI_TEX_CUBE : dt, (unsigned)old_tex);
    CI.del_fb(2, fb);
}

static void (*r_glCopyImageSubData)(unsigned, unsigned, int, int, int, int, unsigned, unsigned, int, int, int, int, int, int, int);
static void w_glCopyImageSubData(unsigned sn, unsigned st, int sl, int sx, int sy, int sz, unsigned dn, unsigned dt,
                                 uint64_t dl, uint64_t dx, uint64_t dy, uint64_t dz, uint64_t w, uint64_t h, uint64_t d)
{
    if (r_glCopyImageSubData && copyimg_native())
        r_glCopyImageSubData(sn, st, sl, sx, sy, sz, dn, dt, (int)dl, (int)dx, (int)dy, (int)dz, (int)w, (int)h, (int)d);
    else
        copyimg_emulate(sn, st, sl, sx, sy, sz, dn, dt, (int)dl, (int)dx, (int)dy, (int)dz, (int)w, (int)h, (int)d);
}

/* GLboolean arguments: AAPCS64 callers do not extend them, Apple's callee may assume they are. */
static void (*r_glColorMask)(unsigned, unsigned, unsigned, unsigned);
static void w_glColorMask(unsigned r, unsigned g, unsigned b, unsigned a) { r_glColorMask(r & 0xff, g & 0xff, b & 0xff, a & 0xff); }
static void w_glDepthMask(unsigned f) { t_game_depth_mask = (unsigned char)(f & 0xff); r_glDepthMask(f & 0xff); }
static void (*r_glVertexAttribPointer)(unsigned, int, unsigned, unsigned, int, const void *);
static void w_glVertexAttribPointer(unsigned i, int s, unsigned t, unsigned n, int st, const void *p) { r_glVertexAttribPointer(i, s, t, n & 0xff, st, p); }
static void (*r_glUniformMatrix2fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix2fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix2fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix3fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix3fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix3fv(l, c, t & 0xff, v); }
static void (*r_glUniformMatrix4fv)(int, int, unsigned, const float *);
static void w_glUniformMatrix4fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix4fv(l, c, t & 0xff, v); }
static void (*r_glSampleCoverage)(float, unsigned);
static void w_glSampleCoverage(float v, unsigned i) { r_glSampleCoverage(v, i & 0xff); }
/* the ES 3.0 non-square matrix uploads carry the same GLboolean transpose */
#define UMAT_NS(n) static void (*r_glUniformMatrix##n##fv)(int, int, unsigned, const float *); \
    static void w_glUniformMatrix##n##fv(int l, int c, unsigned t, const float *v) { r_glUniformMatrix##n##fv(l, c, t & 0xff, v); }
UMAT_NS(2x3) UMAT_NS(3x2) UMAT_NS(2x4) UMAT_NS(4x2) UMAT_NS(3x4) UMAT_NS(4x3)
#undef UMAT_NS


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

#include "husk-tl-egl-tbo.inc"
#include "husk-tl-egl-es31.inc"
#include "husk-tl-egl-cs.inc"

static const unsigned char *(*r_glGetString)(unsigned);
static const unsigned char *w_glGetString(unsigned name)
{
    if (!r_glGetString && a_eglGetProcAddress) r_glGetString = a_eglGetProcAddress("glGetString");
    const unsigned char *s = r_glGetString ? r_glGetString(name) : NULL;
    if (g_es31_shim && s && (name == 0x1F02 || name == 0x8B8C)) s = (const unsigned char *)es31_fix((const char *)s, name == 0x8B8C);
    if (s && (name == 0x1F02 || name == 0x8B8C || name == 0x1F01 || name == 0x1F00)) {
        static atomic_int said[4];
        int k = name == 0x1F02 ? 0 : name == 0x8B8C ? 1 : name == 0x1F01 ? 2 : 3;
        if (!atomic_exchange(&said[k], 1)) tl_log_line("gl: %s = %.200s", k == 0 ? "GL_VERSION" : k == 1 ? "GL_SHADING_LANGUAGE_VERSION" : k == 2 ? "GL_RENDERER" : "GL_VENDOR", (const char *)s);
    }
    if (name != 0x1F03 /* GL_EXTENSIONS */ || !s) return s;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static const unsigned char *source; static char *filtered;
    pthread_mutex_lock(&mu);
    if (source != s) {
        free(filtered);
        size_t len = strlen((const char *)s);
        filtered = malloc(len + 192);
        size_t k = 0;
        for (const char *p = (const char *)s; *p;) {
            const char *e = strchr(p, ' ');
            size_t n = e ? (size_t)(e - p) : strlen(p);
            if (n && !ext_hidden(p, n)) { if (k) filtered[k++] = ' '; memcpy(filtered + k, p, n); k += n; }
            p += n; if (e) p++;
        }
        /* The engines that need ES 3.1 buffer textures check for the extension before asking for its entry points. */
        /* On a real 3.1 context whose ANGLE lacks it, too: Unreal's ES 3.1 renderer calls glTexBufferEXT whether or not it found the
           extension, and only resolves it when the extension is listed (a jump to address 0 otherwise). */
        if ((g_es31_shim || g_es31_native) && !strstr((const char *)s, "GL_EXT_texture_buffer")) { k += (size_t)snprintf(filtered + k, 62, "%sGL_EXT_texture_buffer", k ? " " : ""); }
        /* copy_image, done by copyimg_emulate where ANGLE does not */
        if (copyimg_extra()) for (int i = 0; i < COPYIMG_EXT_COUNT; i++) k += (size_t)snprintf(filtered + k, 62, "%s%s", k ? " " : "", k_copyimg_ext[i]);
        filtered[k] = 0;
        source = s;
        if (diag_on()) for (size_t o = 0; o < k; o += 360) tl_log_line("gldiag: GL_EXTENSIONS[%zu]: %.360s", o, filtered + o);
    }
    pthread_mutex_unlock(&mu);
    return (const unsigned char *)filtered;
}

static const unsigned char *(*r_glGetStringi)(unsigned, unsigned);
static const unsigned char *w_glGetStringi(unsigned name, unsigned index)
{
    if (name == 0x1F03 && copyimg_extra() && a_glGetIntegerv) {
        int n = 0;
        a_glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
        if (n > 0 && index >= (unsigned)n && index < (unsigned)n + COPYIMG_EXT_COUNT) return (const unsigned char *)k_copyimg_ext[index - (unsigned)n];
    }
    const unsigned char *s = r_glGetStringi ? r_glGetStringi(name, index) : NULL;
    if (name == 0x1F03 && index == 0 && r_glGetStringi && diag_on()) {
        /* what the game is told it has, once: which of its features it turns on depends on this list */
        static atomic_int said;
        int n = 0;
        if (!atomic_exchange(&said, 1) && a_glGetIntegerv) {
            a_glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
            char line[400]; size_t k = 0;
            for (int i = 0; i < n; i++) {
                const char *x = (const char *)r_glGetStringi(0x1F03, (unsigned)i);
                if (!x) continue;
                size_t m = strlen(x);
                if (k + m + 2 > sizeof(line)) { line[k] = 0; tl_log_line("gldiag: extensions: %s", line); k = 0; }
                if (m + 2 > sizeof(line)) continue;
                if (k) line[k++] = ' ';
                memcpy(line + k, x, m); k += m;
            }
            if (k) { line[k] = 0; tl_log_line("gldiag: extensions: %s", line); }
        }
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
    if (!getiv && a_eglGetProcAddress) { getiv = a_eglGetProcAddress("glGetShaderiv"); getlog = a_eglGetProcAddress("glGetShaderInfoLog"); getsrc = a_eglGetProcAddress("glGetShaderSource"); }
    char *orig = ieee_take(sh);
    if (!getiv || !getlog || !getsrc) { free(orig); return; }
    int ok = 1;
    getiv(sh, 0x8B81 /* GL_COMPILE_STATUS */, &ok);
    if (!ok && orig) {
        /* the IEEE rewrite (ieee_rewrite) did not compile: the shader as the app wrote it, with fast math */
        int u = atomic_fetch_add(&g_ieee_undone, 1);
        if (u < 4) {
            int len = 0; getiv(sh, 0x8B84, &len);
            char *log = calloc(1, (size_t)len + 2); int got = 0;
            if (len > 0) getlog(sh, len, &got, log);
            for (char *q = log; *q; q++) if (*q == '\n') *q = '|';
            tl_log_line("gl: shader %u: the rewrite (IEEE maths, subnormal constants) did not compile, so it is compiled as written: %.300s", sh, log);
            free(log);
        }
        const char *one = orig; int one_len = (int)strlen(orig);
        t_ieee_off = true; w_glShaderSource(sh, 1, &one, &one_len); t_ieee_off = false;
        r_glCompileShader(sh);
        getiv(sh, 0x8B81, &ok);
    } else if (ok && orig) {
        static atomic_int said;
        if (!atomic_exchange(&said, 1)) tl_log_line("gl: fragment shaders get an isnan() call, so ANGLE compiles them for Metal with IEEE maths, not fast math, and a NaN they write is written as 0 (TL_GL_FAST_MATH=1, TL_GL_KEEP_NAN=1: off)");
    }
    free(orig);
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
    /* the source lines the first errors point at ("ERROR: 0:<line>:"), and the version line, so a log shows what did not compile */
    if (n < 4) {
        int lines[3], nl = 0;
        for (const char *e = log; nl < 3 && (e = strstr(e, "ERROR: 0:")); e += 9) {
            int ln = atoi(e + 9);
            bool dup = false; for (int i = 0; i < nl; i++) dup |= lines[i] == ln;
            if (ln > 0 && !dup) lines[nl++] = ln;
        }
        int ln = 1; const char *l = src;
        while (l && *l) {
            const char *eol = strchr(l, '\n'); int len = eol ? (int)(eol - l) : (int)strlen(l);
            bool show = !strncmp(l, "#version", 8);
            for (int i = 0; i < nl; i++) show |= ln >= lines[i] - 2 && ln <= lines[i] + 1;
            if (show) tl_log_line("gl:   %4d| %.*s", ln, len > 200 ? 200 : len, l);
            l = eol ? eol + 1 : NULL; ln++;
        }
    }
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
    ADAPT(glBlitFramebuffer), ADAPT(glTexSubImage3D), ADAPT(glCompressedTexSubImage3D), ADAPT(glCopyImageSubData),
    ADAPT(glColorMask), ADAPT(glDepthMask), ADAPT(glVertexAttribPointer), ADAPT(glUniformMatrix2fv),
    ADAPT(glUniformMatrix3fv), ADAPT(glUniformMatrix4fv), ADAPT(glSampleCoverage),
    ADAPT(glUniformMatrix2x3fv), ADAPT(glUniformMatrix3x2fv), ADAPT(glUniformMatrix2x4fv), ADAPT(glUniformMatrix4x2fv),
    ADAPT(glUniformMatrix3x4fv), ADAPT(glUniformMatrix4x3fv),
    ADAPT(glProgramBinary), { "glProgramBinaryOES", (void *)w_glProgramBinary, (void **)&r_glProgramBinary }, ADAPT(glGetProgramBinary), { "glGetProgramBinaryOES", (void *)w_glGetProgramBinary, (void **)&r_glGetProgramBinary },
    ADAPT(glTexStorage3D), ADAPT(glTexImage3D), ADAPT(glTexStorage2D), ADAPT(glTexSubImage2D), ADAPT(glCompressedTexSubImage2D), ADAPT(glCompressedTexImage2D), ADAPT(glTexImage2D), ADAPT(glDeleteTextures),
    ADAPT(glTexParameteri), ADAPT(glTexParameterf), ADAPT(glTexParameteriv), ADAPT(glTexParameterfv), ADAPT(glGetError), ADAPT(glFramebufferTextureLayer),
    ADAPT(glBlendFunc), ADAPT(glEnable), ADAPT(glDisable), ADAPT(glUseProgram), ADAPT(glBlendEquation), ADAPT(glBlendEquationSeparate), ADAPT(glBlendFuncSeparate), ADAPT(glBlendFunci), ADAPT(glBlendFuncSeparatei),
    { "glBlendFunciEXT", (void *)w_glBlendFunci, (void **)&r_glBlendFunci }, { "glBlendFunciOES", (void *)w_glBlendFunci, (void **)&r_glBlendFunci },
    { "glBlendFuncSeparateiEXT", (void *)w_glBlendFuncSeparatei, (void **)&r_glBlendFuncSeparatei },
    { "glBlendFuncSeparateiOES", (void *)w_glBlendFuncSeparatei, (void **)&r_glBlendFuncSeparatei },
    ADAPT(glDrawElements), ADAPT(glDrawElementsInstanced), ADAPT(glDrawRangeElements), ADAPT(glDrawArrays), ADAPT(glDrawArraysInstanced), ADAPT(glClear), ADAPT(glGenerateMipmap),
    { "glCopyImageSubDataEXT", (void *)w_glCopyImageSubData, (void **)&r_glCopyImageSubData },
    { "glCopyImageSubDataOES", (void *)w_glCopyImageSubData, (void **)&r_glCopyImageSubData }, ADAPT(glGetString), ADAPT(glGetStringi), ADAPT(glCompileShader), ADAPT(glShaderSource), ADAPT(glAttachShader), ADAPT(glLinkProgram), ADAPT(glGetIntegerv), ADAPT(glBindBufferBase), ADAPT(glBindBuffer), ADAPT(glBufferData), ADAPT(glBufferSubData), ADAPT(glBindBufferRange),
    ADAPT(glBindTexture), ADAPT(glActiveTexture), ADAPT(glBindSampler), ADAPT(glMapBufferRange), { "glMapBufferRangeEXT", (void *)w_glMapBufferRange, (void **)&r_glMapBufferRange },
    ADAPT(glUnmapBuffer), { "glUnmapBufferOES", (void *)w_glUnmapBuffer, (void **)&r_glUnmapBuffer }, ADAPT(glFlushMappedBufferRange),
    { "glFlushMappedBufferRangeEXT", (void *)w_glFlushMappedBufferRange, (void **)&r_glFlushMappedBufferRange }, ADAPT(glCopyBufferSubData),
    ADAPT(glGetBufferParameteriv), ADAPT(glGetBufferParameteri64v), ADAPT(glGetBufferPointerv), { "glGetBufferPointervOES", (void *)w_glGetBufferPointerv, (void **)&r_glGetBufferPointerv },
    ADAPT(glGetProgramInterfaceiv), ADAPT(glGetProgramResourceIndex), ADAPT(glGetProgramResourceName), ADAPT(glGetProgramResourceiv), ADAPT(glGetIntegeri_v), ADAPT(glGetShaderiv), ADAPT(glGetProgramiv), ADAPT(glGetProgramInfoLog), ADAPT(glDispatchCompute), ADAPT(glDispatchComputeIndirect), ADAPT(glGetProgramResourceLocation),
    ADAPT(eglClientWaitSyncKHR), ADAPT(eglDestroySyncKHR), ADAPT(eglGetSyncAttribKHR), ADAPT(eglClientWaitSync), ADAPT(eglDestroySync),
};

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
    "glBindImageTexture", "glDispatchCompute", "glDispatchComputeIndirect", "glMemoryBarrier", "glMemoryBarrierByRegion",
    "glFramebufferParameteri", "glGetFramebufferParameteriv", "glTexStorage2DMultisample", NULL
};

static void *egl_resolve(const char *name);
void *tl_egl_resolve(const char *name)
{
    void *r = egl_resolve(name);
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_EGL_TRACE") ? 1 : 0;
    if (trace && name[0] == 'e') tl_log_line("egl: resolve %s -> %p", name, r);
    return r;
}
static void *egl_resolve(const char *name)
{
    for (int i = 0; k_noop_names[i]; i++) if (!strcmp(k_noop_names[i], name)) return (void *)w_gl_noop;
    /* texture buffers: emulated where the context has none (husk-tl-egl-tbo.inc), ANGLE's own where it has them */
    if (g_es31_shim || g_es31_native) {
        if (!strcmp(name, "glTexBufferEXT") || !strcmp(name, "glTexBufferOES") || !strcmp(name, "glTexBuffer")) return (void *)w_glTexBuffer;
        if (!strcmp(name, "glTexBufferRangeEXT") || !strcmp(name, "glTexBufferRangeOES") || !strcmp(name, "glTexBufferRange")) return (void *)w_glTexBufferRange;
    }
    /* under the 3.0 shim, and on a real 3.1 context whose ANGLE has no such entry point */
    if (g_es31_shim || g_es31_native) for (int i = 0; k_es31_stub_names[i]; i++) if (!strcmp(k_es31_stub_names[i], name)) {
        void *real = E.ready && a_eglGetProcAddress ? a_eglGetProcAddress(name) : NULL;
        if (real) break;
        static int said; if (!said++) tl_log_line("gl: stubbing ES 3.1 entry points ANGLE lacks (first: %s)", name);
        return (void *)w_gl_noop;
    }
    for (size_t i = 0; i < sizeof(k_egl) / sizeof(k_egl[0]); i++) if (!strcmp(k_egl[i].name, name)) return k_egl[i].fn;
    if (!E.ready || !a_eglGetProcAddress) return NULL;
    void *real = a_eglGetProcAddress(name);
    if (!real && E.gles) {
        /* dlsym on the handle also searches what ANGLE links against, and on a Mac that is the system's desktop OpenGL:
         * glPolygonMode and its kin would resolve there, and a GLES guest that calls one must not reach them. */
        real = dlsym(E.gles, name);
        Dl_info di;
        if (real && dladdr(real, &di) && di.dli_fname && strcmp(di.dli_fname, E.gles_path) != 0 && strcmp(di.dli_fname, E.egl_path) != 0) real = NULL;
    }
    if (!real) {
        static atomic_int missed;
        if (diag_on() && atomic_fetch_add(&missed, 1) < 80) tl_log_line("gldiag: the game asked for %s: ANGLE has none (NULL returned)", name);
        return NULL;
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("TL_GL_TRACE") ? 1 : 0;
    if (diag_on() && (strstr(name, "Blend") || strstr(name, "Enablei") || strstr(name, "ColorMaski"))) tl_log_line("gldiag: the game resolved %s", name);
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
