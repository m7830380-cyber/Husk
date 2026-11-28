/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _DARWIN_C_SOURCE
#include "husk-tl-nativeactivity.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "husk-tl-bionic.h"
#include "husk-tl-dexindex.h"
#include "husk-tl-egl.h"
#include "husk-tl-gamepad.h"
#include "husk-tl-vulkan.h"
#include "husk-tl-jni.h"
#include "husk-tl-internal.h"
#include "husk-tl-ld.h"
#include "husk-tl-xmem.h"

void tl_jni_hle_install(void);
void tl_hle_configure(const char *pkg, const char *apk, const char *data, int w, int h);
void tl_hle_set_activity(jobj *a);
jobj *tl_hle_assets(void);
void tl_set_data_dir(const char *dir);
void tl_nwindow_configure(int w, int h, void *layer);
void *tl_nwindow_get(void);
void tl_ue4_hle_install(const char *pkg, const char *apk, const char *data, const char *ext_files);
void tl_ue4_set_activity(jobj *activity);
const char *tl_ue4_meta(const char *key);

#define CLS "com/epicgames/ue4/GameActivity"

/* android/native_activity.h */
typedef struct ANativeActivity ANativeActivity;
typedef struct ANativeActivityCallbacks {
    void (*onStart)(ANativeActivity *);
    void (*onResume)(ANativeActivity *);
    void *(*onSaveInstanceState)(ANativeActivity *, size_t *);
    void (*onPause)(ANativeActivity *);
    void (*onStop)(ANativeActivity *);
    void (*onDestroy)(ANativeActivity *);
    void (*onWindowFocusChanged)(ANativeActivity *, int);
    void (*onNativeWindowCreated)(ANativeActivity *, void *);
    void (*onNativeWindowResized)(ANativeActivity *, void *);
    void (*onNativeWindowRedrawNeeded)(ANativeActivity *, void *);
    void (*onNativeWindowDestroyed)(ANativeActivity *, void *);
    void (*onInputQueueCreated)(ANativeActivity *, void *);
    void (*onInputQueueDestroyed)(ANativeActivity *, void *);
    void (*onContentRectChanged)(ANativeActivity *, const void *);
    void (*onConfigurationChanged)(ANativeActivity *);
    void (*onLowMemory)(ANativeActivity *);
} ANativeActivityCallbacks;
struct ANativeActivity {
    ANativeActivityCallbacks *callbacks;
    void *vm, *env;
    void *clazz;
    const char *internalDataPath, *externalDataPath;
    int32_t sdkVersion;
    void *instance, *assetManager;
    const char *obbPath;
};

static struct {
    tl_ga_config cfg;
    char apk[1024], data[512], pkg[128], frame_dir[512], angle_egl[600], angle_gles[600];
    char internal_dir[600], ext_dir[700], obb_dir[800], obb_file[900];
    jobj *activity;
    ANativeActivity na;
    ANativeActivityCallbacks callbacks;
    bool vulkan;                           /* Unreal is told to use Vulkan (MoltenVK there, and the game packaged with Vulkan shaders) */
    bool generic;                          /* a NativeActivity game of its own (no libUE4): nothing of Unreal is set up for it */
    char main_lib[64];                     /* the library that exports ANativeActivity_onCreate */
    void *window;
    pthread_t ui;
    bool started;
} N;

static void load_library(const char *name)
{
    jvalue a; a.j = 0; a.l = tl_jni_new_string(name);
    tl_jni_call(tl_jni_class_object("java/lang/System"), "loadLibrary", "(Ljava/lang/String;)V", &a);
}

static void mkdirs(const char *path)
{
    char t[1024]; snprintf(t, sizeof(t), "%s", path);
    for (char *p = t + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(t, 0755); *p = '/'; }
    mkdir(t, 0755);
}


/* Where a stored (uncompressed) entry's bytes begin in the APK, and how many there are. */
static bool stored_range(const char *apk, const char *entry, unsigned long long *off, unsigned long long *size)
{
    tl_zip z; char err[160];
    if (!tl_zip_open(&z, apk, err, sizeof(err))) return false;
    const tl_zip_entry *e = tl_zip_find(&z, entry);
    bool ok = false;
    if (e && e->method == 0 && e->local_offset + 30 < z.size) {
        const uint8_t *h = z.map + e->local_offset;
        uint16_t nlen, elen; memcpy(&nlen, h + 26, 2); memcpy(&elen, h + 28, 2);
        *off = e->local_offset + 30 + nlen + elen; *size = e->usize; ok = true;
    }
    tl_zip_close(&z);
    return ok;
}

/* A native of GameActivity: registered by the library, or found by its JNI name. */
static void *native_of(const char *name, const char *sig, const char *mangled)
{
    void *fn = tl_jni_native(CLS, name, sig);
    if (!fn) {
        tl_lib *lib = tl_ld_find_lib("libUE4.so");
        if (lib) fn = tl_ld_sym(lib, mangled);
    }
    if (!fn) tl_log_line("ue4: native %s%s is not provided by libUE4", name, sig);
    return fn;
}
#define NATIVE(name, sig) native_of(name, sig, "Java_com_epicgames_ue4_GameActivity_" name)


/*
 * Make a function of the loaded engine return a fixed value: `mov w0, #value; ret` over its first two instructions, written through the writable view of
 * the image. For trying things -- here, to ask an engine that has decided to use a graphics API we do not have to use the other one -- not for shipping.
 */
bool tl_a32_is_fn(const void *p);
static bool patch_return(tl_lib *lib, const char *symbol, unsigned value)
{
    uint32_t *rx = (uint32_t *)tl_ld_sym(lib, symbol);
    if (!rx) { tl_log_line("ue4: no symbol %s to patch", symbol); return false; }
    if (tl_a32_is_fn(rx)) return false;          /* 32-bit ARM code: an arm64 patch would be written through a guest address */
    uint32_t *rw = (uint32_t *)((uint8_t *)rx + tl_xmem_delta());
    rw[0] = 0x52800000u | (value << 5);          /* mov w0, #value */
    rw[1] = 0xD65F03C0u;                           /* ret */
    tl_xmem_flush(rx, 8);
    tl_log_line("ue4: %s now returns %u", symbol, value);
    return true;
}

/* The OBB the engine looks for: <package>.obb in the OBB directory, named main.<version>.<package>.obb. */
#define OBB_VERSION 42

/* Controllers reach the engine as the NDK's own gamepad events: buttons as key events (source GAMEPAD), sticks and triggers as joystick motion events. */
void tl_inq_key(int device, int source, int action, int keycode);
void tl_inq_axes(int device, const float *axes48);
static void na_pad_key(jobj *ev, int device, int action, int keycode, int64_t down_ms, int64_t event_ms)
{
    (void)down_ms; (void)event_ms;
    tl_inq_key(device, 0x00000401, action, keycode);
    tl_jni_unref(ev);
}
static void na_pad_motion(jobj *ev, int device, int source, int64_t down_ms, int64_t event_ms)
{
    (void)source; (void)down_ms; (void)event_ms;
    float a[48];
    if (tl_input_event_axes(ev, a)) tl_inq_axes(device, a);
    tl_jni_unref(ev);
}

/* A NativeActivity game that is not Unreal: load its libraries (the manifest names one; here, whichever of the app's own libraries exports ANativeActivity_onCreate). */
struct na_libs { char name[6][64]; int n; };
static void na_lib_cb(const char *name, uint64_t size, void *user)
{
    (void)size;
    struct na_libs *l = user;
    static const char *const support[] = { "libc++_shared", "libSDL", "libopenal", "libsentry", "libFirebase", "libcrashlytics", "libandroidx" };
    for (size_t i = 0; i < sizeof(support) / sizeof(support[0]); i++) if (!strncmp(name, support[i], strlen(support[i]))) return;
    if (l->n < 6) snprintf(l->name[l->n++], 64, "%s", name);
}
static bool engine_is_64bit(const char *libname);

static bool start_generic(void)
{
    struct na_libs libs = { .n = 0 };
    tl_ld_apk_libs(na_lib_cb, &libs);
    if (tl_ld_has_lib("libc++_shared.so")) load_library("c++_shared");
    for (int i = 0; i < libs.n; i++) {
        char base[64]; snprintf(base, sizeof(base), "%s", libs.name[i] + 3); base[strlen(base) - 3] = 0;
        load_library(base);
        if (tl_jni_pending()) { tl_log_line("na: loading %s failed", libs.name[i]); tl_jni_clear(); continue; }
        tl_lib *l = tl_ld_find_lib(libs.name[i]);
        if (l && tl_ld_sym(l, "ANativeActivity_onCreate")) { snprintf(N.main_lib, sizeof(N.main_lib), "%s", libs.name[i]); break; }
    }
    if (!N.main_lib[0]) { tl_log_line("na: no library exports ANativeActivity_onCreate"); return false; }
    if (!engine_is_64bit(N.main_lib)) return false;
    tl_log_line("na: native activity in %s", N.main_lib);
    N.started = true;
    return true;
}

/*
 * An Unreal save that is empty: what a game ended while rewriting it leaves behind (before Husk made rewrites crash-safe). A real
 * one never is -- it starts with a GVAS header -- and Minecraft Dungeons waits forever on its title screen for an empty
 * GlobalSave.sav it cannot read. Taken away, the game makes a new one, and the other saves (its characters) stay.
 */
static void drop_empty_saves(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        struct stat st;
        if (lstat(p, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 10) drop_empty_saves(p, depth + 1); continue; }
        size_t n = strlen(e->d_name);
        if (S_ISREG(st.st_mode) && st.st_size == 0 && n > 4 && !strcmp(e->d_name + n - 4, ".sav") && strstr(dir, "/SaveGames")) {
            if (unlink(p) == 0) tl_log_line("ue4: removed an empty save, %s (left by a save cut off part-way)", p);
        }
    }
    closedir(d);
}

/* The NativeActivity path calls the engine directly, as arm64 code. A 32-bit ARM engine (armeabi-v7a only) runs on Husk's ARM32 interpreter, which covers small
 * entry points (Minecraft PE's) but not booting a whole engine; calling its functions as host code jumps to a guest address and kills Husk. Say so and stop. */
bool tl_a32_is_fn(const void *p);
static bool engine_is_64bit(const char *libname)
{
    tl_lib *lib = tl_ld_find_lib(libname);
    void *p = lib ? tl_ld_sym(lib, "ANativeActivity_onCreate") : NULL;
    if (!tl_a32_is_fn(p)) return true;
    tl_log_line("na: %s is 32-bit ARM only (armeabi-v7a); this engine needs an arm64 build to run on Husk", libname);
    return false;
}

/*
 * Which shader formats the game was packaged with, from the names of its shader libraries (ShaderArchive-*-<format>.ushaderbytecode) in the index of
 * each .pak inside its OBB. A project built without Vulkan has no SF_VULKAN_* library, and an engine told to use Vulkan anyway stops in
 * FShaderCodeLibrary::InitForRuntime (ForceQuit, a black screen). 1: Vulkan shaders are there; 0: only other formats (OpenGL ES); -1: cannot tell
 * (no shader library, or an encrypted index), and the old choice stands.
 */
static int pak_index_vulkan(int fd, unsigned long long pak, unsigned long long size)
{
    if (size < 256) return -1;
    uint8_t tail[512]; size_t tn = size < sizeof(tail) ? (size_t)size : sizeof(tail);
    if (pread(fd, tail, tn, (off_t)(pak + size - tn)) != (ssize_t)tn) return -1;
    static const uint8_t magic[4] = { 0xE1, 0x12, 0x6F, 0x5A };
    int at = -1;
    for (int i = (int)tn - 24; i >= 17; i--) if (!memcmp(tail + i, magic, 4)) { at = i; break; }
    if (at < 0 || tail[at - 1] == 1) return -1;                       /* no footer, or bEncryptedIndex */
    uint64_t index_off; memcpy(&index_off, tail + at + 8, 8);
    if (index_off >= size) return -1;
    unsigned long long n = size - index_off;
    if (n > (64ull << 20)) return -1;
    uint8_t *ix = malloc((size_t)n);
    if (!ix) return -1;
    int r = -1;
    if (pread(fd, ix, (size_t)n, (off_t)(pak + index_off)) == (ssize_t)n) {
        bool vk = false, other = false;
        static const char key[] = "ShaderArchive-";
        for (uint8_t *q = ix; (q = memmem(q, (size_t)(ix + n - q), key, sizeof(key) - 1)); q += sizeof(key) - 1) {
            const char *name = (const char *)q; size_t len = 0;
            while (q + len < ix + n && len < 160 && q[len] >= 0x20 && q[len] < 0x7F) len++;
            if (!memmem(name, len, ".ushaderbytecode", 16)) continue;
            if (memmem(name, len, "SF_VULKAN", 9)) vk = true; else other = true;
        }
        r = vk ? 1 : other ? 0 : -1;
    }
    free(ix);
    return r;
}
static int obb_has_vulkan_shaders(const char *apk, unsigned long long obb, unsigned long long obb_size)
{
    int fd = open(apk, O_RDONLY);
    if (fd < 0) return -1;
    int result = -1;
    unsigned long long p = obb;
    for (int k = 0; k < 4096 && p + 30 <= obb + obb_size; k++) {    /* the OBB is a zip of stored .pak files: walk its local headers */
        uint8_t h[30];
        if (pread(fd, h, 30, (off_t)p) != 30 || memcmp(h, "PK\3\4", 4)) break;
        uint16_t method, nlen, elen; uint32_t csize;
        memcpy(&method, h + 8, 2); memcpy(&csize, h + 18, 4); memcpy(&nlen, h + 26, 2); memcpy(&elen, h + 28, 2);
        char name[256] = { 0 };
        if (pread(fd, name, nlen < 255 ? nlen : 255, (off_t)(p + 30)) < 0) break;
        unsigned long long data = p + 30 + nlen + elen;
        size_t L = strlen(name);
        if (method == 0 && L > 4 && !strcmp(name + L - 4, ".pak")) {
            int r = pak_index_vulkan(fd, data, csize);
            if (r == 1) { result = 1; break; }
            if (r == 0) result = 0;
        }
        if (!csize) break;
        p = data + csize;
    }
    close(fd);
    return result;
}

bool tl_na_start(const tl_ga_config *cfg)
{
    N.cfg = *cfg;
    snprintf(N.apk, sizeof(N.apk), "%s", cfg->apk_path);
    /* the data folder as its canonical path, as the Java driver has it: a game that changes into its files folder and checks getcwd()
       against it (Inner Breeze) sees /private/var/... for /var/... (iOS) or /private/tmp/... (the Mac) and gives up. Unless that path has a
       space, which an Android data path never has. */
    mkdir(cfg->data_dir, 0755);
    { char canon[PATH_MAX]; snprintf(N.data, sizeof(N.data), "%s", realpath(cfg->data_dir, canon) && !strchr(canon, ' ') ? canon : cfg->data_dir); }
    snprintf(N.pkg, sizeof(N.pkg), "%s", cfg->package_name);
    N.cfg.apk_path = N.apk; N.cfg.data_dir = N.data; N.cfg.package_name = N.pkg;
    if (cfg->frame_dir) { snprintf(N.frame_dir, sizeof(N.frame_dir), "%s", cfg->frame_dir); N.cfg.frame_dir = N.frame_dir; }
    if (cfg->angle_egl) { snprintf(N.angle_egl, sizeof(N.angle_egl), "%s", cfg->angle_egl); N.cfg.angle_egl = N.angle_egl; }
    if (cfg->angle_gles) { snprintf(N.angle_gles, sizeof(N.angle_gles), "%s", cfg->angle_gles); N.cfg.angle_gles = N.angle_gles; }

    snprintf(N.internal_dir, sizeof(N.internal_dir), "%s/files", N.data);
    snprintf(N.ext_dir, sizeof(N.ext_dir), "%s/sdcard/Android/data/%s/files", N.data, N.pkg);
    snprintf(N.obb_dir, sizeof(N.obb_dir), "%s/sdcard/Android/obb/%s", N.data, N.pkg);
    snprintf(N.obb_file, sizeof(N.obb_file), "%s/main.%d.%s.obb", N.obb_dir, OBB_VERSION, N.pkg);
    mkdirs(N.internal_dir); mkdirs(N.ext_dir); mkdirs(N.obb_dir);

    tl_set_data_dir(cfg->data_dir);
    tl_nwindow_configure(cfg->width, cfg->height, cfg->metal_layer);
    int n = tl_dexidx_open(cfg->apk_path);
    tl_log_line("ue4: %d classes in the APK's DEX", n);
    if (!tl_ld_add_apk(cfg->apk_path)) return false;
    N.generic = !tl_ld_has_lib("libUE4.so");
    if (!N.generic) {
        N.vulkan = !getenv("TL_UE4_NO_VULKAN") && (getenv("TL_UE4_FORCE_VULKAN") || tl_vk_available());
        if (N.vulkan && !getenv("TL_UE4_FORCE_VULKAN")) {
            unsigned long long off = 0, size = 0;
            const char *entry = getenv("TL_UE4_OBB_ENTRY") ? getenv("TL_UE4_OBB_ENTRY") : "assets/main.obb.png";
            int vk = stored_range(N.apk, entry, &off, &size) ? obb_has_vulkan_shaders(N.apk, off, size) : -1;
            if (vk == 0) { N.vulkan = false; tl_log_line("ue4: the game was packaged without Vulkan shaders; it runs on OpenGL ES, as it would on a phone"); }
            else tl_log_line("ue4: Vulkan shaders in the package: %s", vk == 1 ? "yes" : "could not tell");
        }
        tl_egl_es31_shim(true);
        tl_egl_offscreen_windows(N.vulkan);
    }
    if (cfg->angle_egl && !tl_egl_init(cfg->angle_egl, cfg->angle_gles, cfg->frame_dir, cfg->frame_every)) return false;
    tl_jni_init();
    tl_hle_configure(cfg->package_name, cfg->apk_path, cfg->data_dir, cfg->width, cfg->height);
    tl_jni_hle_install();
    if (N.generic) {
        tl_jni_declare("android/app/NativeActivity", "android/app/Activity");
        N.activity = tl_jni_new_object(tl_jni_class("android/app/NativeActivity"));
        tl_hle_set_activity(N.activity);
        return start_generic();
    }
    tl_ue4_hle_install(cfg->package_name, cfg->apk_path, cfg->data_dir, N.ext_dir);
    drop_empty_saves(N.data, 0);

    N.activity = tl_jni_new_object(tl_jni_class(CLS));
    tl_hle_set_activity(N.activity);
    tl_ue4_set_activity(N.activity);

    /* GameActivity's static initialiser, and NativeActivity's loading of the library its manifest names. */
    static const char *const libs[] = { "c++_shared", "EOSSDK", "UE4", NULL };
    for (int i = 0; libs[i]; i++) {
        if (!strcmp(libs[i], "EOSSDK") && !tl_ld_has_lib("libEOSSDK.so")) continue;      /* Epic Online Services: only the games that use it ship it */
        load_library(libs[i]);
        if (tl_jni_pending()) { tl_log_line("ue4: loading lib%s.so failed", libs[i]); return false; }
    }
    if (!engine_is_64bit("libUE4.so")) return false;
    if (!N.vulkan) {
        tl_lib *ue = tl_ld_find_lib("libUE4.so");
        if (ue) { patch_return(ue, "_ZN12FAndroidMisc15ShouldUseVulkanEv", 0); patch_return(ue, "_ZN12FAndroidMisc22ShouldUseDesktopVulkanEv", 0); }
    }
    /* ANGLE over Metal speaks OpenGL ES 3.0 and the engine's ES 3.1 shaders will not compile on it, so when MoltenVK is there the engine is asked for Vulkan -- also for a project
     * that defaults to OpenGL ES (bDetectVulkanByDefault off), which Minecraft Dungeons is. TL_UE4_NO_VULKAN opts out; TL_UE4_FORCE_VULKAN forces it even with no MoltenVK. */
    if (N.vulkan) {
        tl_lib *ue = tl_ld_find_lib("libUE4.so");
        if (ue) { patch_return(ue, "_ZN12FAndroidMisc15ShouldUseVulkanEv", 1); patch_return(ue, "_ZN12FAndroidMisc17IsVulkanAvailableEv", 1); }
    }
    tl_log_line("ue4: libraries loaded");
    setenv("TL_PAD_DPAD", "keys", 0);          /* the D-pad as DPAD_* keys, which the engine maps like a stick's arrows */
    static const tl_pad_sink sink = { na_pad_key, na_pad_motion };
    tl_pad_set_sink(&sink);
    N.started = true;
    return true;
}

static void *ui_main(void *arg)
{
    (void)arg;
    pthread_setname_np("UiThread");
    ((void *(*)(int))tl_bionic_find("ALooper_prepare"))(0);
    void *env = tl_jni_env();
    jobj *activity = N.activity;

    /* GameActivity.onCreate, before it hands over to NativeActivity: the facts the engine wants to know. */
    if (!N.generic) {
        typedef void (*ver_fn)(void *env, void *self, void *release, int sdk, void *a, void *b, void *c, void *d);
        ver_fn set_version = (ver_fn)NATIVE("nativeSetAndroidVersionInformation", "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        if (set_version) set_version(env, activity, tl_jni_new_string("14"), 34, tl_jni_new_string("Google"), tl_jni_new_string("Pixel 8"),
                                     tl_jni_new_string("shiba"), tl_jni_new_string("google"));
        typedef void (*glob_fn)(void *env, void *self, uint8_t ext, uint8_t pub, void *internal_path, void *external_path, uint8_t obb_in_apk, void *apk);
        glob_fn set_global = (glob_fn)NATIVE("nativeSetGlobalActivity", "(ZZLjava/lang/String;Ljava/lang/String;ZLjava/lang/String;)V");
        if (set_global) set_global(env, activity, 1, 1, tl_jni_new_string(N.internal_dir), tl_jni_new_string(N.ext_dir), 0, tl_jni_new_string(N.apk));
        typedef void (*obb_fn)(void *env, void *self, void *project, void *package, int version, int patch, void *apptype);
        obb_fn set_obb = (obb_fn)NATIVE("nativeSetObbInfo", "(Ljava/lang/String;Ljava/lang/String;IILjava/lang/String;)V");
        if (set_obb) set_obb(env, activity, tl_jni_new_string(tl_ue4_meta("com.epicgames.ue4.GameActivity.ProjectName") ? tl_ue4_meta("com.epicgames.ue4.GameActivity.ProjectName") : "ShooterGame"), tl_jni_new_string(N.pkg), OBB_VERSION, 0, tl_jni_new_string(""));
        typedef void (*obbp_fn)(void *env, void *self, void *a, void *b, void *c, void *d);
        obbp_fn set_obb_paths = (obbp_fn)NATIVE("nativeSetObbFilePaths", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V");
        if (set_obb_paths) set_obb_paths(env, activity, tl_jni_new_string(N.obb_file), tl_jni_new_string(""), tl_jni_new_string(""), tl_jni_new_string(""));
        typedef void (*rules_fn)(void *env, void *self, void *vars);
        rules_fn set_rules = (rules_fn)NATIVE("nativeSetConfigRulesVariables", "([Ljava/lang/String;)V");
        if (set_rules) set_rules(env, activity, tl_jni_new_obj_array(tl_jni_class("java/lang/String"), 0));

    }

    /* NativeActivity: the system calls the library's entry point with the activity it describes. */
    tl_lib *lib = tl_ld_find_lib(N.generic ? N.main_lib : "libUE4.so");
    void (*on_create)(ANativeActivity *, void *, size_t) = lib ? tl_ld_sym(lib, "ANativeActivity_onCreate") : NULL;
    if (!on_create) { tl_log_line("na: the library has no ANativeActivity_onCreate"); return NULL; }
    jobj *assets = tl_hle_assets();
    void *(*from_java)(void *, void *) = tl_bionic_find("AAssetManager_fromJava");
    N.na.callbacks = &N.callbacks;
    N.na.vm = tl_jni_vm(); N.na.env = env; N.na.clazz = activity;
    N.na.internalDataPath = N.internal_dir; N.na.externalDataPath = N.ext_dir;
    N.na.sdkVersion = 34;
    N.na.assetManager = from_java ? from_java(env, assets) : NULL;
    N.na.obbPath = N.obb_dir;
    tl_log_line("ue4: ANativeActivity_onCreate");
    on_create(&N.na, NULL, 0);
    if (tl_jni_pending()) { tl_log_line("ue4: a Java exception is pending after onCreate"); tl_jni_clear(); }
    tl_log_line("ue4: onCreate returned; callbacks start=%p resume=%p window=%p", (void *)N.callbacks.onStart, (void *)N.callbacks.onResume, (void *)N.callbacks.onNativeWindowCreated);

    N.window = tl_nwindow_get();
    if (N.callbacks.onStart) N.callbacks.onStart(&N.na);
    if (N.callbacks.onResume) N.callbacks.onResume(&N.na);
    if (N.callbacks.onInputQueueCreated) N.callbacks.onInputQueueCreated(&N.na, (void *)&N);          /* a queue nothing arrives through */
    if (!N.generic) {
        typedef void (*win_fn)(void *env, void *self, uint8_t portrait, int depth);
        win_fn set_window_info = (win_fn)NATIVE("nativeSetWindowInfo", "(ZI)V");
        if (set_window_info) set_window_info(env, activity, N.cfg.width < N.cfg.height, 24);
        typedef void (*sv_fn)(void *env, void *self, int w, int h);
        sv_fn set_surface = (sv_fn)NATIVE("nativeSetSurfaceViewInfo", "(II)V");
        if (set_surface) set_surface(env, activity, N.cfg.width, N.cfg.height);
    }
    if (N.callbacks.onNativeWindowCreated) N.callbacks.onNativeWindowCreated(&N.na, N.window);
    if (N.callbacks.onNativeWindowResized) N.callbacks.onNativeWindowResized(&N.na, N.window);
    if (N.callbacks.onWindowFocusChanged) N.callbacks.onWindowFocusChanged(&N.na, 1);
    if (!N.generic) {
        typedef void (*void_fn)(void *env, void *self);
        void_fn resume_init = (void_fn)NATIVE("nativeResumeMainInit", "()V");
        if (resume_init) resume_init(env, activity);
    }
    tl_log_line("ue4: lifecycle delivered");

    /* The activity's message loop: nothing to serve beyond keeping the thread (and its looper) alive. */
    int (*poll_once)(int, int *, int *, void **) = tl_bionic_find("ALooper_pollOnce");
    for (;;) { poll_once(200, NULL, NULL, NULL); if (tl_jni_pending()) tl_jni_clear(); }
    return NULL;
}

bool tl_na_run(void)
{
    if (!N.started) return false;
    /* The OBB is inside the APK: say where, so the engine finds it where it expects. */
    if (!N.generic) {
        unsigned long long off = 0, size = 0;
        const char *entry = getenv("TL_UE4_OBB_ENTRY") ? getenv("TL_UE4_OBB_ENTRY") : "assets/main.obb.png";
        char name[200]; snprintf(name, sizeof(name), "main.%d.%s.obb", OBB_VERSION, N.pkg);
        if (stored_range(N.apk, entry, &off, &size)) { tl_vfile_add(name, N.apk, off, size); tl_log_line("ue4: OBB %s is %llu bytes at %llu of the APK", name, size, off); }
        else tl_log_line("ue4: the APK has no stored %s; the engine will find no OBB", entry);
    }
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setstacksize(&a, 8u << 20);
    if (pthread_create(&N.ui, &a, ui_main, NULL) != 0) return false;
    return true;
}

unsigned long tl_vk_frames_presented(void);
unsigned long tl_na_frames(void) { return tl_egl_frames_presented() + tl_vk_frames_presented(); }

void tl_inq_touch(int phase, int id, float x, float y);
void tl_na_touch(int phase, int id, float x, float y) { tl_inq_touch(phase, id, x, y); }
/* The app left the screen or came back: the engine is told as an activity is -- onPause and the window losing focus, onResume and gaining it -- so it stops drawing and
 * silences its audio instead of feeding a GPU iOS will no longer let it have. Only real changes are passed on. */
void tl_na_set_paused(bool paused)
{
    static atomic_bool is_paused;
    if (!N.started || !N.callbacks.onResume) return;
    if (atomic_exchange(&is_paused, paused) == paused) return;
    tl_log_line("ue4: %s", paused ? "pausing" : "resuming");
    if (paused) {
        if (N.callbacks.onWindowFocusChanged) N.callbacks.onWindowFocusChanged(&N.na, 0);
        if (N.callbacks.onPause) N.callbacks.onPause(&N.na);
    } else {
        if (N.callbacks.onResume) N.callbacks.onResume(&N.na);
        if (N.callbacks.onWindowFocusChanged) N.callbacks.onWindowFocusChanged(&N.na, 1);
    }
}
