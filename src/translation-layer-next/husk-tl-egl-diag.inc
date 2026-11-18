/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Rendering diagnostics (included by husk-tl-egl.c): bounded, and off after the first couple of thousand frames.
 *
 * What a 3D scene that draws wrong needs looked at, without a graphics debugger on the phone:
 *  - the source of the shaders that draw the characters (Documents/husk-shaders.txt). A Unity player loads its programs from its
 *    own binary cache after the first run, so on a launch where that file does not exist yet, glProgramBinary is refused and the
 *    engine compiles from source again (as it does after a driver update);
 *  - for those programs, at their first draws: every sampler's unit and the texture bound there, the plain uniforms' values, and
 *    the blend/depth state;
 *  - what texture arrays are made and how they are filled (storage, sub-image uploads, copies, render-to-layer).
 * TL_GL_DIAG=0 turns all of it off.
 */

static int g_diag = -1;               /* -1 unread, 0 off, 1 on */
static atomic_bool g_diag_done;       /* set after enough frames: nothing more is looked at */
static bool diag_on(void)
{
    if (g_diag < 0) { const char *e = getenv("TL_GL_DIAG"); g_diag = e ? atoi(e) != 0 : 1; }
    return g_diag == 1 && !atomic_load(&g_diag_done);
}

/* ---- shader sources */
static int g_diag_dump = -1;          /* 1: this launch writes the shader file (and so refuses cached program binaries) */
static char g_diag_path[1024];
static pthread_mutex_t g_diag_mu = PTHREAD_MUTEX_INITIALIZER;
static bool diag_dumping(void)
{
    if (g_diag_dump >= 0) return g_diag_dump == 1;
    pthread_mutex_lock(&g_diag_mu);
    if (g_diag_dump < 0) {
        g_diag_dump = 0;
        const char *home = getenv("HOME");
        if (diag_on() && home) {
            snprintf(g_diag_path, sizeof(g_diag_path), "%s/Documents/husk-shaders.txt", home);
            FILE *f = fopen(g_diag_path, "r");
            if (f) fclose(f);
            else if ((f = fopen(g_diag_path, "w"))) {
                fprintf(f, "Husk: shaders of interest, written once (delete this file to have them written again)\n");
                fclose(f);
                g_diag_dump = 1;
                tl_log_line("gldiag: writing character shaders to %s; cached program binaries are refused on this launch", g_diag_path);
            }
        }
    }
    pthread_mutex_unlock(&g_diag_mu);
    return g_diag_dump == 1;
}

static bool diag_interesting_src(const char *s)
{
    static const char *const keys[] = { "_RoleMaps", "_RoleBumps", "_GB27", "_MainLightShadowParams", NULL };
    for (int i = 0; keys[i]; i++) if (strstr(s, keys[i])) return true;
    return false;
}

static void diag_shader_source(unsigned sh, const char *src)
{
    if (!src || !diag_dumping() || !diag_interesting_src(src)) return;
    static int n; static size_t written;
    size_t len = strlen(src);
    pthread_mutex_lock(&g_diag_mu);
    if (n < 24 && written + len < 3u * 1024 * 1024) {
        FILE *f = fopen(g_diag_path, "a");
        if (f) {
            fprintf(f, "\n\n======== shader %u (%zu bytes) ========\n%s\n", sh, len, src);
            fclose(f);
            n++; written += len;
        }
    }
    pthread_mutex_unlock(&g_diag_mu);
}

static void (*r_glProgramBinary)(unsigned, unsigned, const void *, int);
static void w_glProgramBinary(unsigned prog, unsigned fmt, const void *bin, int len)
{
    if (diag_dumping()) {
        static atomic_int said;
        if (!atomic_exchange(&said, 1)) tl_log_line("gldiag: program binaries refused on this launch, so shaders are compiled from source");
        return;   /* the program stays unlinked: the engine sees LINK_STATUS false and compiles it */
    }
    r_glProgramBinary(prog, fmt, bin, len);
}

/* ---- texture arrays */
static atomic_int g_diag_arr_storage, g_diag_arr_sub, g_diag_arr_csub, g_diag_arr_copy_in, g_diag_arr_layer, g_diag_copy_n, g_diag_copy_fail;

static int diag_bound(unsigned target)
{
    unsigned q = target == 0x8C1A ? 0x8C1D : target == 0x806F ? 0x806A : target == 0x8513 ? 0x8514 : 0x8069;
    int t = 0;
    if (a_glGetIntegerv) a_glGetIntegerv(q, &t);
    return t;
}

static void (*r_glTexStorage3D)(unsigned, int, unsigned, int, int, int);
static void w_glTexStorage3D(unsigned target, int levels, unsigned fmt, int w, int h, int d)
{
    r_glTexStorage3D(target, levels, fmt, w, h, d);
    if (!diag_on()) return;
    int n = atomic_fetch_add(&g_diag_arr_storage, 1);
    if (n < 40) tl_log_line("gldiag: glTexStorage3D(%s, tex %d, %d levels, fmt %#x, %dx%dx%d)",
                            target == 0x8C1A ? "2D_ARRAY" : target == 0x806F ? "3D" : "?", diag_bound(target), levels, fmt, w, h, d);
}
static void (*r_glTexImage3D)(unsigned, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexImage3D(unsigned target, int level, int ifmt, int w, int h, int d, int border, unsigned fmt, uint64_t type, const void *px)
{
    r_glTexImage3D(target, level, ifmt, w, h, d, border, fmt, (unsigned)type, px);
    if (!diag_on() || level) return;
    int n = atomic_fetch_add(&g_diag_arr_storage, 1);
    if (n < 40) tl_log_line("gldiag: glTexImage3D(%s, tex %d, fmt %#x, %dx%dx%d, data %s)",
                            target == 0x8C1A ? "2D_ARRAY" : target == 0x806F ? "3D" : "?", diag_bound(target), ifmt, w, h, d, px ? "yes" : "none");
}
static void (*r_glFramebufferTextureLayer)(unsigned, unsigned, unsigned, int, int);
static void w_glFramebufferTextureLayer(unsigned fbt, unsigned att, unsigned tex, int level, int layer)
{
    r_glFramebufferTextureLayer(fbt, att, tex, level, layer);
    if (diag_on() && tex) {
        int n = atomic_fetch_add(&g_diag_arr_layer, 1);
        if (n < 12) tl_log_line("gldiag: render to layer: tex %u level %d layer %d (attachment %#x)", tex, level, layer, att);
    }
}

/* ---- programs at draw time */
#define DIAG_PROGS 8192
static unsigned char g_diag_prog[DIAG_PROGS];   /* 0 unseen, 1 not of interest, 2.. of interest (draws reported + 2) */
static atomic_int g_diag_reported;

static const char *diag_type(unsigned t)
{
    switch (t) {
    case 0x8B5E: return "sampler2D"; case 0x8B60: return "samplerCube"; case 0x8B5F: return "sampler3D";
    case 0x8DC1: return "sampler2DArray"; case 0x8B62: return "sampler2DShadow"; case 0x8DC4: return "sampler2DArrayShadow";
    case 0x8DC5: return "samplerCubeShadow"; case 0x8B65: return "mat2"; case 0x8B5B: return "mat3"; case 0x8B5C: return "mat4";
    case 0x1406: return "float"; case 0x8B50: return "vec2"; case 0x8B51: return "vec3"; case 0x8B52: return "vec4";
    case 0x1404: return "int"; case 0x8B53: return "ivec2"; case 0x8B54: return "ivec3"; case 0x8B55: return "ivec4";
    default: return "other";
    }
}
static bool diag_is_sampler(unsigned t, unsigned *target)
{
    switch (t) {
    case 0x8B5E: case 0x8B62: case 0x8DCA: case 0x8DD2: *target = 0x0DE1; return true;
    case 0x8B60: case 0x8DC5: case 0x8DCC: case 0x8DD4: *target = 0x8513; return true;
    case 0x8B5F: case 0x8DCB: case 0x8DD3: *target = 0x806F; return true;
    case 0x8DC1: case 0x8DC4: case 0x8DCF: case 0x8DD7: *target = 0x8C1A; return true;
    default: return false;
    }
}

static struct {
    bool ok;
    void (*get_prog_iv)(unsigned, unsigned, int *);
    void (*get_active_u)(unsigned, unsigned, int, int *, int *, unsigned *, char *);
    int (*get_u_loc)(unsigned, const char *);
    void (*get_u_iv)(unsigned, int, int *), (*get_u_fv)(unsigned, int, float *);
    void (*get_uiv)(unsigned, int, const unsigned *, unsigned, int *);
    void (*active_tex)(unsigned);
    unsigned char (*is_enabled)(unsigned);
    void (*get_blk_name)(unsigned, unsigned, int, int *, char *);
    void (*get_blk_iv)(unsigned, unsigned, unsigned, int *);
    void (*get_i_v)(unsigned, unsigned, int *);
    void (*get_samp_iv)(unsigned, unsigned, int *);
    void (*get_tex_iv)(unsigned, unsigned, int *);
} DG;
static bool diag_load(void)
{
    if (DG.ok) return true;
    if (!a_eglGetProcAddress || !a_glGetIntegerv) return false;
#define DGP(f, n) DG.f = a_eglGetProcAddress(n)
    DGP(get_prog_iv, "glGetProgramiv"); DGP(get_active_u, "glGetActiveUniform"); DGP(get_u_loc, "glGetUniformLocation");
    DGP(get_u_iv, "glGetUniformiv"); DGP(get_u_fv, "glGetUniformfv"); DGP(get_uiv, "glGetActiveUniformsiv");
    DGP(active_tex, "glActiveTexture"); DGP(is_enabled, "glIsEnabled"); DGP(get_blk_name, "glGetActiveUniformBlockName");
    DGP(get_blk_iv, "glGetActiveUniformBlockiv"); DGP(get_i_v, "glGetIntegeri_v"); DGP(get_samp_iv, "glGetSamplerParameteriv");
    DGP(get_tex_iv, "glGetTexParameteriv");
#undef DGP
    DG.ok = DG.get_prog_iv && DG.get_active_u && DG.get_u_loc && DG.get_u_iv && DG.get_u_fv && DG.get_uiv && DG.active_tex &&
            DG.is_enabled && DG.get_blk_name && DG.get_blk_iv && DG.get_i_v && DG.get_samp_iv && DG.get_tex_iv;
    return DG.ok;
}

/* a program draws characters if it samples a texture whose name says so, or reads the main light's shadow */
static bool diag_prog_interesting(unsigned prog)
{
    int n = 0;
    DG.get_prog_iv(prog, 0x8B86 /* GL_ACTIVE_UNIFORMS */, &n);
    for (int i = 0; i < n && i < 512; i++) {
        char name[128] = ""; int l = 0, sz = 0; unsigned ty = 0;
        DG.get_active_u(prog, (unsigned)i, sizeof(name), &l, &sz, &ty, name);
        if (strstr(name, "_RoleMaps") || strstr(name, "_RoleBumps") || strstr(name, "_GB27")) return true;
    }
    return false;
}

static void diag_report_prog(unsigned prog, unsigned mode, int count, int draw_no)
{
    char line[1400]; size_t k;
    int n = 0, nblk = 0;
    DG.get_prog_iv(prog, 0x8B86, &n);
    DG.get_prog_iv(prog, 0x8A36 /* GL_ACTIVE_UNIFORM_BLOCKS */, &nblk);
    int fb = 0; a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    int vp[4] = {0}; a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    int bs = 0, bd = 0, bsa = 0, bda = 0, df = 0, dm = 0, cm[4] = {0}, cullf = 0;
    a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */, &bs); a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */, &bd);
    a_glGetIntegerv(0x80CB /* BLEND_SRC_ALPHA */, &bsa); a_glGetIntegerv(0x80CA /* BLEND_DST_ALPHA */, &bda);
    a_glGetIntegerv(0x0B74 /* DEPTH_FUNC */, &df); a_glGetIntegerv(0x0B72 /* DEPTH_WRITEMASK */, &dm);
    a_glGetIntegerv(0x0C23 /* COLOR_WRITEMASK */, cm); a_glGetIntegerv(0x0B45 /* CULL_FACE_MODE */, &cullf);
    tl_log_line("gldiag: prog %u draw #%d: mode %#x count %d, fbo %d viewport %dx%d, %d uniforms, %d blocks", prog, draw_no, mode, count,
                fb, vp[2], vp[3], n, nblk);
    tl_log_line("gldiag:   state: blend %d (%#x,%#x / %#x,%#x) depth test %d func %#x write %d, cull %d (%#x), colormask %d%d%d%d",
                DG.is_enabled(0x0BE2), bs, bd, bsa, bda, DG.is_enabled(0x0B71), df, dm, DG.is_enabled(0x0B44), cullf, cm[0], cm[1], cm[2], cm[3]);
    for (int b = 0; b < nblk && b < 16; b++) {
        char name[96] = ""; int l = 0, bind = -1, size = 0, ubo = 0;
        DG.get_blk_name(prog, (unsigned)b, sizeof(name), &l, name);
        DG.get_blk_iv(prog, (unsigned)b, 0x8A3F /* UNIFORM_BLOCK_BINDING */, &bind);
        DG.get_blk_iv(prog, (unsigned)b, 0x8A40 /* UNIFORM_BLOCK_DATA_SIZE */, &size);
        if (bind >= 0) DG.get_i_v(0x8A28 /* UNIFORM_BUFFER_BINDING */, (unsigned)bind, &ubo);
        tl_log_line("gldiag:   block '%s': %d bytes, binding %d -> buffer %d", name, size, bind, ubo);
    }
    int active = 0; a_glGetIntegerv(0x84E0 /* ACTIVE_TEXTURE */, &active);
    k = 0; line[0] = 0;
    for (int i = 0; i < n && i < 512; i++) {
        char name[128] = ""; int l = 0, sz = 0; unsigned ty = 0, target = 0;
        DG.get_active_u(prog, (unsigned)i, sizeof(name), &l, &sz, &ty, name);
        unsigned idx = (unsigned)i; int blk = -1;
        DG.get_uiv(prog, 1, &idx, 0x8A3A /* UNIFORM_BLOCK_INDEX */, &blk);
        if (blk >= 0) continue;                       /* in a block: its buffer is named above */
        int loc = DG.get_u_loc(prog, name);
        if (diag_is_sampler(ty, &target)) {
            int unit = -1, tex = 0, samp = 0, cmp = 0, minf = 0;
            if (loc >= 0) DG.get_u_iv(prog, loc, &unit);
            if (unit >= 0 && unit < 32) {
                DG.active_tex(0x84C0 /* TEXTURE0 */ + (unsigned)unit);
                tex = diag_bound(target);
                a_glGetIntegerv(0x8919 /* SAMPLER_BINDING */, &samp);
                if (tex) { DG.get_tex_iv(target, 0x884C /* TEXTURE_COMPARE_MODE */, &cmp); DG.get_tex_iv(target, 0x2801 /* MIN_FILTER */, &minf); }
                if (samp) { DG.get_samp_iv((unsigned)samp, 0x884C, &cmp); DG.get_samp_iv((unsigned)samp, 0x2801, &minf); }
            }
            tl_log_line("gldiag:   %s %s: unit %d -> tex %d (sampler obj %d, compare %#x, min %#x)", diag_type(ty), name, unit, tex, samp, cmp, minf);
        } else if (loc >= 0 && (ty == 0x1406 || ty == 0x8B50 || ty == 0x8B51 || ty == 0x8B52)) {
            float v[4] = {0};
            DG.get_u_fv(prog, loc, v);
            int c = ty == 0x1406 ? 1 : ty == 0x8B50 ? 2 : ty == 0x8B51 ? 3 : 4;
            char one[200];
            int m = snprintf(one, sizeof(one), "%s=(%g", name, v[0]);
            for (int j = 1; j < c; j++) m += snprintf(one + m, sizeof(one) - (size_t)m, ",%g", v[j]);
            snprintf(one + m, sizeof(one) - (size_t)m, ")%s ", sz > 1 ? "[0]" : "");
            size_t ol = strlen(one);
            if (k + ol >= 1200) { tl_log_line("gldiag:   uniforms: %s", line); k = 0; line[0] = 0; }
            memcpy(line + k, one, ol + 1); k += ol;
        } else {
            char one[200];
            snprintf(one, sizeof(one), "%s:%s%s ", name, diag_type(ty), loc < 0 ? "(no loc)" : "");
            size_t ol = strlen(one);
            if (k + ol >= 1200) { tl_log_line("gldiag:   uniforms: %s", line); k = 0; line[0] = 0; }
            memcpy(line + k, one, ol + 1); k += ol;
        }
    }
    if (k) tl_log_line("gldiag:   uniforms: %s", line);
    DG.active_tex((unsigned)active);
}

static void diag_draw(unsigned mode, int count)
{
    if (!diag_on() || !diag_load()) return;
    if (atomic_load(&g_diag_reported) >= 24) return;
    int prog = 0;
    a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog);
    if (prog <= 0 || prog >= DIAG_PROGS) return;
    unsigned char st = g_diag_prog[prog];
    if (st == 1) return;
    if (st == 0) { st = diag_prog_interesting((unsigned)prog) ? 2 : 1; g_diag_prog[prog] = st; if (st == 1) return; }
    if (st >= 2 + 2) return;                          /* two draws of each program are enough */
    g_diag_prog[prog] = (unsigned char)(st + 1);
    atomic_fetch_add(&g_diag_reported, 1);
    diag_report_prog((unsigned)prog, mode, count, st - 1);
}

static void (*r_glDrawElements)(unsigned, int, unsigned, const void *);
static void w_glDrawElements(unsigned mode, int count, unsigned type, const void *idx) { diag_draw(mode, count); r_glDrawElements(mode, count, type, idx); }
static void (*r_glDrawElementsInstanced)(unsigned, int, unsigned, const void *, int);
static void w_glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int inst) { diag_draw(mode, count); r_glDrawElementsInstanced(mode, count, type, idx, inst); }
static void (*r_glDrawRangeElements)(unsigned, unsigned, unsigned, int, unsigned, const void *);
static void w_glDrawRangeElements(unsigned mode, unsigned s, unsigned e, int count, unsigned type, const void *idx) { diag_draw(mode, count); r_glDrawRangeElements(mode, s, e, count, type, idx); }

/* at the swaps that are logged anyway */
static void diag_swap(unsigned long n)
{
    if (!diag_on()) return;
    if (n == 600 || n == 1800) {
        tl_log_line("gldiag: by swap #%lu: texture arrays/3D made %d, sub-image uploads %d (compressed %d), copies %d (into arrays %d, failed %d), render-to-layer %d",
                    n, atomic_load(&g_diag_arr_storage), atomic_load(&g_diag_arr_sub), atomic_load(&g_diag_arr_csub), atomic_load(&g_diag_copy_n),
                    atomic_load(&g_diag_arr_copy_in), atomic_load(&g_diag_copy_fail), atomic_load(&g_diag_arr_layer));
        if (n == 1800) { atomic_store(&g_diag_done, true); tl_log_line("gldiag: done"); }
    }
}
