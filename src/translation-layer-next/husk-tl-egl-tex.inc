/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Texture filtering that OpenGL ES 3.0 cannot do (included by husk-tl-egl.c, before husk-tl-egl-diag.inc).
 *
 * ES 3.0 calls a texture incomplete -- every sample of it reads (0,0,0,1) -- when it asks for LINEAR filtering on a format that
 * cannot be filtered: a depth format with no depth comparison, a 32-bit float format without GL_OES_texture_float_linear, an integer
 * format. ANGLE holds to that; Android's drivers mostly filter such a texture anyway, or point-sample it, so a game made on them reads
 * it fine and here gets black. For such a texture the filters ANGLE is given are the point-sampled ones (the game's own are kept, and
 * applied again when the texture becomes filterable: a depth comparison switched on). TL_GL_KEEP_FILTER=1 turns this off.
 */
#define TF_N 16384
enum { TF_OK = 0, TF_DEPTH, TF_FLOAT32, TF_INT };
static struct { unsigned name; unsigned char cls; unsigned short min, mag, cmp; } g_tf[TF_N];
static int g_tf_on = -1;
static int g_tf_float_linear = -1;
static atomic_int g_tf_said;
static void (*g_tf_param)(unsigned, unsigned, int);

static bool tf_on(void)
{
    if (g_tf_on < 0) { const char *e = getenv("TL_GL_KEEP_FILTER"); g_tf_on = !(e && atoi(e)); }
    if (!g_tf_on || !a_eglGetProcAddress || !a_glGetIntegerv) return false;
    if (!g_tf_param) g_tf_param = (void (*)(unsigned, unsigned, int))a_eglGetProcAddress("glTexParameteri");
    return g_tf_param != NULL;
}

static bool tf_float_linear(void)
{
    if (g_tf_float_linear >= 0) return g_tf_float_linear;
    const unsigned char *(*gsi)(unsigned, unsigned) = (const unsigned char *(*)(unsigned, unsigned))a_eglGetProcAddress("glGetStringi");
    int n = 0; a_glGetIntegerv(0x821D /* GL_NUM_EXTENSIONS */, &n);
    if (!gsi || n <= 0) return false;                         /* no context yet: ask again next time */
    int have = 0;
    for (int i = 0; i < n && !have; i++) { const char *s = (const char *)gsi(0x1F03, (unsigned)i); if (s && !strcmp(s, "GL_OES_texture_float_linear")) have = 1; }
    g_tf_float_linear = have;
    return have;
}

static unsigned char tf_class(unsigned ifmt, unsigned type)
{
    switch (ifmt) {
    case 0x81A5: case 0x81A6: case 0x81A7: case 0x8CAC: case 0x88F0: case 0x8CAD: case 0x1902: case 0x84F9: return TF_DEPTH;
    case 0x8814: case 0x8815: case 0x822E: case 0x8230: return TF_FLOAT32;            /* RGBA32F RGB32F R32F RG32F */
    case 0x8D70: case 0x8D71: case 0x8D76: case 0x8D77: case 0x8D7C: case 0x8D7D:       /* RGBA/RGB 32UI 16UI 8UI */
    case 0x8D82: case 0x8D83: case 0x8D88: case 0x8D89: case 0x8D8E: case 0x8D8F:       /* RGBA/RGB 32I 16I 8I */
    case 0x8231: case 0x8232: case 0x8233: case 0x8234: case 0x8235: case 0x8236:       /* R 8I 8UI 16I 16UI 32I 32UI */
    case 0x8237: case 0x8238: case 0x8239: case 0x823A: case 0x823B: case 0x823C:       /* RG of the same */
    case 0x906F: return TF_INT;                                                         /* RGB10_A2UI */
    default: break;
    }
    /* an unsized format (GL_RGBA and kin) with GL_FLOAT data is a 32-bit float texture */
    if (type == 0x1406 && (ifmt == 0x1908 || ifmt == 0x1907 || ifmt == 0x1909 || ifmt == 0x190A || ifmt == 0x1906)) return TF_FLOAT32;
    return TF_OK;
}

/* the texture bound to target (a cube face counts as the cube) on the active unit */
static unsigned tf_target(unsigned target) { return target >= 0x8515 && target <= 0x851A ? 0x8513 : target; }
static unsigned tf_bound(unsigned target)
{
    unsigned q = target == 0x8C1A ? 0x8C1D : target == 0x806F ? 0x806A : target == 0x8513 ? 0x8514 : target == 0x0DE1 ? 0x8069 : 0;
    int t = 0;
    if (q) a_glGetIntegerv(q, &t);
    return (unsigned)t;
}
static int tf_slot(unsigned name, bool make)
{
    if (!name) return -1;
    int i = (int)(name % TF_N);
    if (g_tf[i].name == name) return i;
    if (!make) return -1;
    g_tf[i].name = name; g_tf[i].cls = TF_OK;
    g_tf[i].min = 0x2702; g_tf[i].mag = 0x2601; g_tf[i].cmp = 0;   /* GL's defaults: NEAREST_MIPMAP_LINEAR, LINEAR, no comparison */
    return i;
}

static bool tf_unfilterable(int i)
{
    switch (g_tf[i].cls) {
    case TF_DEPTH: return g_tf[i].cmp == 0;
    case TF_FLOAT32: return !tf_float_linear();
    case TF_INT: return true;
    default: return false;
    }
}
static unsigned tf_point(unsigned f)
{
    if (f == 0x2601) return 0x2600;                                         /* LINEAR -> NEAREST */
    if (f == 0x2701 || f == 0x2702 || f == 0x2703) return 0x2700;           /* any mip filter with LINEAR in it -> NEAREST_MIPMAP_NEAREST */
    return f;
}

/* give ANGLE the filters this texture can have: the game's own, or their point-sampled form */
static void tf_apply(unsigned target, int i)
{
    bool point = tf_unfilterable(i);
    unsigned mn = point ? tf_point(g_tf[i].min) : g_tf[i].min, mg = point ? tf_point(g_tf[i].mag) : g_tf[i].mag;
    g_tf_param(target, 0x2801 /* MIN_FILTER */, (int)mn);
    g_tf_param(target, 0x2800 /* MAG_FILTER */, (int)mg);
    if (point && (mn != g_tf[i].min || mg != g_tf[i].mag) && atomic_fetch_add(&g_tf_said, 1) < 6) {
        static const char *const what[] = { "", "a depth format with no depth comparison", "a 32-bit float format (no GL_OES_texture_float_linear)", "an integer format" };
        tl_log_line("gl: texture %u asks for linear filtering (min %#x mag %#x) on %s, which OpenGL ES cannot filter and samples as black: "
                    "point-sampled instead (TL_GL_KEEP_FILTER=1: off)", g_tf[i].name, g_tf[i].min, g_tf[i].mag, what[g_tf[i].cls]);
    }
}

/* after the bound texture got its format */
static void tf_storage(unsigned target, unsigned ifmt, unsigned type)
{
    if (!tf_on()) return;
    target = tf_target(target);
    int i = tf_slot(tf_bound(target), true);
    if (i < 0) return;
    unsigned char cls = tf_class(ifmt, type);
    bool was = g_tf[i].cls != TF_OK;
    g_tf[i].cls = cls;
    if (cls != TF_OK || was) tf_apply(target, i);
}

/* a filter or comparison setting from the game; true when it was given to ANGLE here */
static bool tf_param(unsigned target, unsigned pname, int v)
{
    if (pname != 0x2801 && pname != 0x2800 && pname != 0x884C /* COMPARE_MODE */) return false;
    if (!tf_on()) return false;
    target = tf_target(target);
    int i = tf_slot(tf_bound(target), true);
    if (i < 0) return false;
    if (pname == 0x2801) g_tf[i].min = (unsigned short)v;
    else if (pname == 0x2800) g_tf[i].mag = (unsigned short)v;
    else g_tf[i].cmp = (unsigned short)v;
    if (g_tf[i].cls == TF_OK) return false;
    if (pname == 0x884C) g_tf_param(target, pname, v);
    tf_apply(target, i);
    return true;
}

static void (*r_glTexParameteri)(unsigned, unsigned, int);
static void w_glTexParameteri(unsigned t, unsigned p, int v) { if (!tf_param(t, p, v)) r_glTexParameteri(t, p, v); }
static void (*r_glTexParameterf)(unsigned, unsigned, float);
static void w_glTexParameterf(unsigned t, unsigned p, float v) { if (!tf_param(t, p, (int)v)) r_glTexParameterf(t, p, v); }
static void (*r_glTexParameteriv)(unsigned, unsigned, const int *);
static void w_glTexParameteriv(unsigned t, unsigned p, const int *v) { if (!v || !tf_param(t, p, v[0])) r_glTexParameteriv(t, p, v); }
static void (*r_glTexParameterfv)(unsigned, unsigned, const float *);
static void w_glTexParameterfv(unsigned t, unsigned p, const float *v) { if (!v || !tf_param(t, p, (int)v[0])) r_glTexParameterfv(t, p, v); }
static void (*r_glTexStorage2D)(unsigned, int, unsigned, int, int);
static void diag_tex_storage2d(unsigned target, int levels, unsigned fmt, int w, int h);
/*
 * Zink over MoltenVK: an RGBA32F colour target cannot be blended on an Apple GPU, and MoltenVK refuses the pipeline of any draw that blends
 * into one ("Blending is enabled for attachment with format VK_FORMAT_R32G32B32A32_SFLOAT"), so the draw is dropped -- an Unreal scene stays
 * blank (white) behind its UI. Android GPUs blend it, and games count on that. 2D textures and renderbuffers asked for as RGBA32F are made
 * RGBA16F, which blends; uploads of GL_FLOAT data into them stay valid. TL_GL_KEEP_RGBA32F=1: off.
 */
static unsigned f32_blendable(unsigned fmt)
{
    if (fmt != 0x8814 /* GL_RGBA32F */ || !g_zink) return fmt;
    static int keep = -1;
    if (keep < 0) { const char *k = getenv("TL_GL_KEEP_RGBA32F"); keep = k && *k == '1'; }
    if (keep) return fmt;
    static atomic_int said;
    if (!atomic_exchange(&said, 1)) tl_log_line("gl: Zink: RGBA32F 2D textures and renderbuffers made RGBA16F, as Apple GPUs cannot blend 32-bit float targets (TL_GL_KEEP_RGBA32F=1: off)");
    return 0x881A; /* GL_RGBA16F */
}
static void w_glTexStorage2D(unsigned t, int levels, unsigned fmt, int w, int h) { fmt = f32_blendable(fmt); r_glTexStorage2D(t, levels, fmt, w, h); tf_storage(t, fmt, 0); diag_tex_storage2d(t, levels, fmt, w, h); }
static void (*r_glTexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexImage2D(unsigned t, int level, int ifmt, int w, int h, int border, unsigned fmt, unsigned type, const void *px)
{ ifmt = (int)f32_blendable((unsigned)ifmt); r_glTexImage2D(t, level, ifmt, w, h, border, fmt, type, px); if (level == 0) tf_storage(t, (unsigned)ifmt, type); }
static void (*r_glRenderbufferStorage)(unsigned, unsigned, int, int);
static void w_glRenderbufferStorage(unsigned t, unsigned fmt, int w, int h) { r_glRenderbufferStorage(t, f32_blendable(fmt), w, h); }
static void (*r_glRenderbufferStorageMultisample)(unsigned, int, unsigned, int, int);
static void w_glRenderbufferStorageMultisample(unsigned t, int n, unsigned fmt, int w, int h) { r_glRenderbufferStorageMultisample(t, n, f32_blendable(fmt), w, h); }
/* a deleted texture's name is handed out again, for a texture with GL's default filters */
static void (*r_glDeleteTextures)(int, const unsigned *);
static void w_glDeleteTextures(int n, const unsigned *names)
{
    if (names) for (int k = 0; k < n; k++) { int i = tf_slot(names[k], false); if (i >= 0) g_tf[i].name = 0; tbo_forget_tex(names[k]); }
    r_glDeleteTextures(n, names);
}
