/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Rendering diagnostics (included by husk-tl-egl.c): bounded, and off after a few thousand frames.
 *
 * What a 3D scene that draws wrong needs looked at, without a graphics debugger on the phone. At a few frames (captures), every draw
 * into the scene's colour target is followed by a read-back of that target, so each pixel of the finished scene is known to come
 * from one draw. Per capture:
 *  - Documents/husk-cap<k>.bmp, the scene as drawn, and in the log a coarse map of which draw owns which part of it;
 *  - a line per draw: program, how many pixels it changed, their average colour;
 *  - for a draw that comes out (nearly) black over a good area: its program's state (blend/depth, blocks, samplers and the texture
 *    on each, plain uniforms), the GL error after it, and each sampled texture read back through its own sampler at lod 0 and 4
 *    (by a small draw of ours, so compressed textures are read too). The program's source goes to Documents/husk-shaders2.txt.
 * A Unity player loads its programs from its own binary cache after the first run, so on a launch where that file does not exist
 * yet glProgramBinary is refused and the engine compiles from source again (as it does after a driver update), which is what
 * makes the source available. TL_GL_DIAG=0 turns all of it off.
 */
#include <math.h>

static int g_diag = -1;               /* -1 unread, 0 off, 1 on */
static atomic_bool g_diag_done;       /* set after the last capture: nothing more is looked at */
static bool diag_on(void)
{
    if (g_diag < 0) { const char *e = getenv("TL_GL_DIAG"); g_diag = e ? atoi(e) != 0 : 1; }
    return g_diag == 1 && !atomic_load(&g_diag_done);
}

static char g_diag_docs[900];
static const char *diag_docs(void)
{
    if (!g_diag_docs[0]) { const char *home = getenv("HOME"); if (home) snprintf(g_diag_docs, sizeof(g_diag_docs), "%s/Documents", home); }
    return g_diag_docs[0] ? g_diag_docs : NULL;
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
        const char *docs = diag_docs();
        if (diag_on() && docs) {
            snprintf(g_diag_path, sizeof(g_diag_path), "%s/husk-shaders2.txt", docs);
            FILE *f = fopen(g_diag_path, "r");
            if (f) fclose(f);
            else if ((f = fopen(g_diag_path, "w"))) {
                fprintf(f, "Husk: programs that drew dark, with their source (delete this file to have it written again)\n");
                fclose(f);
                g_diag_dump = 1;
                tl_log_line("gldiag: writing shaders of dark draws to %s; cached program binaries are refused on this launch", g_diag_path);
            }
        }
    }
    pthread_mutex_unlock(&g_diag_mu);
    return g_diag_dump == 1;
}

#define DIAG_SH 65536
#define DIAG_PROGS 8192
static char *g_diag_src[DIAG_SH];               /* the latest source of each shader; an old one is kept, a program may hold it */
static size_t g_diag_src_bytes;
static char *g_diag_prog_src[DIAG_PROGS][2];    /* [0] vertex, [1] fragment */

static void diag_shader_source(unsigned sh, const char *src)
{
    if (!src || sh >= DIAG_SH || !diag_on() || atomic_load(&g_diag_done)) return;   /* kept for the cube-draw reports too */
    size_t len = strlen(src);
    pthread_mutex_lock(&g_diag_mu);
    if (g_diag_src_bytes + len < (24u << 20)) {
        char *c = malloc(len + 1);
        if (c) { memcpy(c, src, len + 1); g_diag_src[sh] = c; g_diag_src_bytes += len; }
    }
    pthread_mutex_unlock(&g_diag_mu);
}
static void diag_attach(unsigned prog, unsigned sh)
{
    if (prog >= DIAG_PROGS || sh >= DIAG_SH || !diag_on()) return;
    pthread_mutex_lock(&g_diag_mu);
    char *s = g_diag_src[sh];
    if (s) g_diag_prog_src[prog][strstr(s, "gl_Position") ? 0 : 1] = s;
    pthread_mutex_unlock(&g_diag_mu);
}
static void diag_write_prog_src(unsigned prog)
{
    if (prog >= DIAG_PROGS || !diag_dumping()) return;
    pthread_mutex_lock(&g_diag_mu);
    FILE *f = fopen(g_diag_path, "a");
    if (f) {
        for (int k = 0; k < 2; k++) {
            const char *s = g_diag_prog_src[prog][k];
            fprintf(f, "\n\n======== program %u %s shader (%zu bytes) ========\n%s\n", prog, k ? "fragment" : "vertex", s ? strlen(s) : 0,
                    s ? s : "(source not seen: the program came from a binary)");
        }
        fclose(f);
    }
    pthread_mutex_unlock(&g_diag_mu);
}

/*
 * Program binaries carry what Husk did to their shaders' source (the IEEE-maths rewrite, ieee_rewrite): one cached by an engine
 * before a change would bring the old shader back. So the format an engine is handed for a binary is tagged with HUSK_BIN_REV,
 * and a binary without the current tag is refused -- the program stays unlinked, and the engine, seeing LINK_STATUS false (as
 * after a driver update), compiles it from source again.
 */
#define HUSK_BIN_REV 3u   /* bumped whenever a shader rewrite changes */
#define HUSK_BIN_TAG (0x48000000u | (HUSK_BIN_REV << 16))
static void (*r_glGetProgramBinary)(unsigned, int, int *, unsigned *, void *);
static void w_glGetProgramBinary(unsigned prog, int size, int *len, unsigned *fmt, void *bin)
{
    r_glGetProgramBinary(prog, size, len, fmt, bin);
    if (fmt && *fmt && *fmt <= 0xFFFFu) *fmt |= HUSK_BIN_TAG;
}
static void (*r_glProgramBinary)(unsigned, unsigned, const void *, int);
static void w_glProgramBinary(unsigned prog, unsigned fmt, const void *bin, int len)
{
    if (diag_dumping()) {
        static atomic_int said;
        if (!atomic_exchange(&said, 1)) tl_log_line("gldiag: program binaries refused on this launch, so shaders are compiled from source");
        return;   /* the program stays unlinked: the engine sees LINK_STATUS false and compiles it */
    }
    if (tbo_on()) {
        /* a binary has no shader source, and the emulated buffer samplers are found by their names in it */
        static atomic_int said;
        if (!atomic_exchange(&said, 1)) tl_log_line("gl: program binaries refused while texture buffers are emulated, so shaders are compiled from source");
        return;
    }
    if ((fmt & 0xFFFF0000u) != HUSK_BIN_TAG) {
        static atomic_int said;
        if (!atomic_exchange(&said, 1)) tl_log_line("gl: program binaries cached before this Husk's shader changes are refused once; the app compiles them again");
        return;
    }
    r_glProgramBinary(prog, fmt & 0xFFFFu, bin, len);
}

/* ---- texture arrays */
static atomic_int g_diag_arr_storage, g_diag_arr_sub, g_diag_arr_csub, g_diag_arr_copy_in, g_diag_arr_layer, g_diag_copy_n, g_diag_copy_fail;

static void diag_cube_mips_done(unsigned tex);
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
    tf_storage(target, fmt, 0);
    if (!diag_on()) return;
    int n = atomic_fetch_add(&g_diag_arr_storage, 1);
    if (n < 40) tl_log_line("gldiag: glTexStorage3D(%s, tex %d, %d levels, fmt %#x, %dx%dx%d)",
                            target == 0x8C1A ? "2D_ARRAY" : target == 0x806F ? "3D" : "?", diag_bound(target), levels, fmt, w, h, d);
}
static void (*r_glTexImage3D)(unsigned, int, int, int, int, int, int, unsigned, unsigned, const void *);
static void w_glTexImage3D(unsigned target, int level, int ifmt, int w, int h, int d, int border, unsigned fmt, uint64_t type, const void *px)
{
    r_glTexImage3D(target, level, ifmt, w, h, d, border, fmt, (unsigned)type, px);
    if (!level) tf_storage(target, (unsigned)ifmt, (unsigned)type);
    if (!diag_on() || level) return;
    int n = atomic_fetch_add(&g_diag_arr_storage, 1);
    if (n < 40) tl_log_line("gldiag: glTexImage3D(%s, tex %d, fmt %#x, %dx%dx%d, data %s)",
                            target == 0x8C1A ? "2D_ARRAY" : target == 0x806F ? "3D" : "?", diag_bound(target), ifmt, w, h, d, px ? "yes" : "none");
}
/* draws by this thread since it last attached a texture layer to a framebuffer: whether anything rendered into an array before its mips */
static __thread long t_layer_draws = -1;
static void (*r_glFramebufferTextureLayer)(unsigned, unsigned, unsigned, int, int);
static void w_glFramebufferTextureLayer(unsigned fbt, unsigned att, unsigned tex, int level, int layer)
{
    r_glFramebufferTextureLayer(fbt, att, tex, level, layer);
    if (tex && t_layer_draws < 0) t_layer_draws = 0;
    if (diag_on() && tex) {
        int n = atomic_fetch_add(&g_diag_arr_layer, 1);
        if (n < 40) tl_log_line("gldiag: glFramebufferTextureLayer(%#x, att %#x, tex %u, level %d, layer %d)", fbt, att, tex, level, layer);
    }
}

static void diag_array_mips_done(unsigned tex);
static void err_check(const char *where);
static atomic_int g_diag_mips, g_diag_mips_arr, g_diag_mips_2d;
static void (*r_glGenerateMipmap)(unsigned);
static void w_glGenerateMipmap(unsigned target)
{
    r_glGenerateMipmap(target);
    if (!diag_on()) return;
    err_check("glGenerateMipmap");
    atomic_fetch_add(&g_diag_mips, 1);
    int n = target == 0x0DE1 ? atomic_fetch_add(&g_diag_mips_2d, 1) : target == 0x8C1A ? atomic_fetch_add(&g_diag_mips_arr, 1) : 0;
    if (n < 10) tl_log_line("gldiag: glGenerateMipmap(%s, tex %d)", target == 0x8C1A ? "2D_ARRAY" : target == 0x8513 ? "CUBE" :
                            target == 0x0DE1 ? "2D" : "3D", diag_bound(target));
    if (target == 0x8513) diag_cube_mips_done((unsigned)diag_bound(target));
    if (target == 0x8C1A) diag_array_mips_done((unsigned)diag_bound(target));
}

/* ---- GL entry points of our own (ANGLE's, called directly) */
static const char *diag_type(unsigned t)
{
    switch (t) {
    case 0x8B5E: return "sampler2D"; case 0x8B60: return "samplerCube"; case 0x8B5F: return "sampler3D";
    case 0x8DC1: return "sampler2DArray"; case 0x8B62: return "sampler2DShadow"; case 0x8DC4: return "sampler2DArrayShadow";
    case 0x8DC5: return "samplerCubeShadow"; case 0x8B65: return "mat2"; case 0x8B5B: return "mat3"; case 0x8B5C: return "mat4";
    case 0x1406: return "float"; case 0x8B50: return "vec2"; case 0x8B51: return "vec3"; case 0x8B52: return "vec4";
    case 0x1404: return "int"; case 0x8B53: return "ivec2"; case 0x8B54: return "ivec3"; case 0x8B55: return "ivec4";
    case 0x1405: return "uint"; case 0x8B56: return "bool";
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
    bool ok, tried;
    void (*get_prog_iv)(unsigned, unsigned, int *);
    void (*get_active_u)(unsigned, unsigned, int, int *, int *, unsigned *, char *);
    int (*get_u_loc)(unsigned, const char *);
    void (*get_u_iv)(unsigned, int, int *), (*get_u_fv)(unsigned, int, float *), (*get_u_uiv)(unsigned, int, unsigned *);
    void (*get_uiv)(unsigned, int, const unsigned *, unsigned, int *);
    void (*active_tex)(unsigned);
    unsigned char (*is_enabled)(unsigned);
    void (*get_blk_name)(unsigned, unsigned, int, int *, char *);
    void (*get_blk_iv)(unsigned, unsigned, unsigned, int *);
    void (*get_i_v)(unsigned, unsigned, int *);
    void (*get_samp_iv)(unsigned, unsigned, int *);
    void (*get_tex_iv)(unsigned, unsigned, int *);
    void (*fb_att_iv)(unsigned, unsigned, unsigned, int *);
    void (*rb_iv)(unsigned, unsigned, int *);
    void (*gen_rb)(int, unsigned *), (*bind_rb)(unsigned, unsigned), (*rb_storage)(unsigned, unsigned, int, int);
    void (*gen_fb)(int, unsigned *), (*fb_rb)(unsigned, unsigned, unsigned, unsigned), (*fb_tex2d)(unsigned, unsigned, unsigned, unsigned, int);
    unsigned (*check_fb)(unsigned);
    void (*blit)(int, int, int, int, int, int, int, int, unsigned, unsigned);
    unsigned (*create_sh)(unsigned);
    void (*sh_src)(unsigned, int, const char *const *, const int *), (*compile)(unsigned), (*get_sh_iv)(unsigned, unsigned, int *);
    void (*sh_log)(unsigned, int, int *, char *), (*prog_log)(unsigned, int, int *, char *);
    unsigned (*create_prog)(void);
    void (*attach)(unsigned, unsigned), (*link)(unsigned), (*use)(unsigned);
    void (*u1i)(int, int), (*u1f)(int, float), (*u4fv)(int, int, const float *);
    void (*gen_va)(int, unsigned *), (*bind_va)(unsigned);
    void (*draw_arrays)(unsigned, int, int);
    void (*enable)(unsigned), (*disable)(unsigned), (*viewport)(int, int, int, int);
    void (*bind_tex)(unsigned, unsigned), (*bind_samp)(unsigned, unsigned), (*gen_tex)(int, unsigned *);
    void (*tex_storage2d)(unsigned, int, unsigned, int, int), (*tex_param_i)(unsigned, unsigned, int);
    unsigned (*get_error)(void);
    void (*color_mask)(unsigned char, unsigned char, unsigned char, unsigned char);
    void (*read_buffer)(unsigned), (*bind_buffer)(unsigned, unsigned);
} DG;
static bool diag_load(void)
{
    if (DG.ok) return true;
    if (DG.tried || !a_eglGetProcAddress || !a_glGetIntegerv || !a_glReadPixels || !a_glBindFramebuffer || !a_glPixelStorei) return false;
    DG.tried = true;
    int miss = 0;
#define DGP(f, n) do { *(void **)&DG.f = a_eglGetProcAddress(n); if (!DG.f) miss++; } while (0)
    DGP(get_prog_iv, "glGetProgramiv"); DGP(get_active_u, "glGetActiveUniform"); DGP(get_u_loc, "glGetUniformLocation");
    DGP(get_u_iv, "glGetUniformiv"); DGP(get_u_fv, "glGetUniformfv"); DGP(get_u_uiv, "glGetUniformuiv"); DGP(get_uiv, "glGetActiveUniformsiv");
    DGP(active_tex, "glActiveTexture"); DGP(is_enabled, "glIsEnabled"); DGP(get_blk_name, "glGetActiveUniformBlockName");
    DGP(get_blk_iv, "glGetActiveUniformBlockiv"); DGP(get_i_v, "glGetIntegeri_v"); DGP(get_samp_iv, "glGetSamplerParameteriv");
    DGP(get_tex_iv, "glGetTexParameteriv"); DGP(fb_att_iv, "glGetFramebufferAttachmentParameteriv");
    DGP(rb_iv, "glGetRenderbufferParameteriv"); DGP(gen_rb, "glGenRenderbuffers"); DGP(bind_rb, "glBindRenderbuffer");
    DGP(rb_storage, "glRenderbufferStorage"); DGP(gen_fb, "glGenFramebuffers"); DGP(fb_rb, "glFramebufferRenderbuffer");
    DGP(fb_tex2d, "glFramebufferTexture2D"); DGP(check_fb, "glCheckFramebufferStatus"); DGP(blit, "glBlitFramebuffer");
    DGP(create_sh, "glCreateShader"); DGP(sh_src, "glShaderSource"); DGP(compile, "glCompileShader"); DGP(get_sh_iv, "glGetShaderiv");
    DGP(sh_log, "glGetShaderInfoLog"); DGP(prog_log, "glGetProgramInfoLog"); DGP(create_prog, "glCreateProgram");
    DGP(attach, "glAttachShader"); DGP(link, "glLinkProgram"); DGP(use, "glUseProgram"); DGP(u1i, "glUniform1i"); DGP(u1f, "glUniform1f");
    DGP(u4fv, "glUniform4fv"); DGP(gen_va, "glGenVertexArrays"); DGP(bind_va, "glBindVertexArray"); DGP(draw_arrays, "glDrawArrays");
    DGP(enable, "glEnable"); DGP(disable, "glDisable"); DGP(viewport, "glViewport"); DGP(bind_tex, "glBindTexture");
    DGP(bind_samp, "glBindSampler"); DGP(gen_tex, "glGenTextures"); DGP(tex_storage2d, "glTexStorage2D"); DGP(tex_param_i, "glTexParameteri");
    DGP(get_error, "glGetError"); DGP(color_mask, "glColorMask"); DGP(read_buffer, "glReadBuffer"); DGP(bind_buffer, "glBindBuffer");
#undef DGP
    DG.ok = miss == 0;
    if (!DG.ok) tl_log_line("gldiag: %d GL entry points missing, diagnostics off", miss);
    return DG.ok;
}

/* ---- a texture read back through a sampler: a 4x2 draw of ours, row 0 at lod 0, row 1 at lod 4 */
static const char k_probe_vs[] =
    "#version 300 es\nvoid main() { gl_Position = vec4(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0, 0.0, 1.0); }\n";
static const char *const k_probe_fs[3] = {
    "#version 300 es\nprecision highp float;\nuniform highp sampler2D s;\nuniform vec4 uv[4];\nuniform float L;\nout vec4 o;\n"
    "void main() { int i = int(gl_FragCoord.x); o = textureLod(s, uv[i].xy, gl_FragCoord.y < 1.0 ? 0.0 : L); }\n",
    "#version 300 es\nprecision highp float;\nuniform highp samplerCube s;\nuniform vec4 uv[4];\nuniform float L;\nout vec4 o;\n"
    "void main() { int i = int(gl_FragCoord.x); o = textureLod(s, uv[i].xyz, gl_FragCoord.y < 1.0 ? 0.0 : L); }\n",
    "#version 300 es\nprecision highp float;\nuniform highp sampler2DArray s;\nuniform vec4 uv[4];\nuniform float L;\nout vec4 o;\n"
    "void main() { int i = int(gl_FragCoord.x); o = textureLod(s, vec3(uv[i].xy, 0.0), gl_FragCoord.y < 1.0 ? 0.0 : L); }\n",
};
static struct { bool tried, ok, flt; unsigned prog[3], va, fb, tex; int loc_s[3], loc_uv[3], loc_l[3]; int unit; } P;

static unsigned probe_shader(unsigned type, const char *src)
{
    unsigned sh = DG.create_sh(type);
    DG.sh_src(sh, 1, &src, NULL); DG.compile(sh);
    int ok = 0; DG.get_sh_iv(sh, 0x8B81 /* COMPILE_STATUS */, &ok);
    if (!ok) { char log[512] = ""; DG.sh_log(sh, sizeof(log), NULL, log); tl_log_line("gldiag: probe shader: %s", log); return 0; }
    return sh;
}
static bool probe_init(void)
{
    if (P.tried) return P.ok;
    P.tried = true;
    int old_fb = 0, old_tex = 0, old_at = 0, old_va = 0;
    a_glGetIntegerv(0x8CA6, &old_fb); a_glGetIntegerv(0x84E0, &old_at); a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &old_va);
    unsigned vs = probe_shader(0x8B31, k_probe_vs);
    if (!vs) return false;
    for (int t = 0; t < 3; t++) {
        unsigned fs = probe_shader(0x8B30, k_probe_fs[t]);
        if (!fs) return false;
        P.prog[t] = DG.create_prog(); DG.attach(P.prog[t], vs); DG.attach(P.prog[t], fs); DG.link(P.prog[t]);
        int ok = 0; DG.get_prog_iv(P.prog[t], 0x8B82, &ok);
        if (!ok) { char log[512] = ""; DG.prog_log(P.prog[t], sizeof(log), NULL, log); tl_log_line("gldiag: probe link: %s", log); return false; }
        P.loc_s[t] = DG.get_u_loc(P.prog[t], "s"); P.loc_uv[t] = DG.get_u_loc(P.prog[t], "uv"); P.loc_l[t] = DG.get_u_loc(P.prog[t], "L");
    }
    int units = 16; a_glGetIntegerv(0x8B4D /* MAX_COMBINED_TEXTURE_IMAGE_UNITS */, &units);
    P.unit = units > 1 ? units - 1 : 0;
    DG.gen_va(1, &P.va);
    DG.active_tex(0x84C0 + (unsigned)P.unit);
    a_glGetIntegerv(0x8069, &old_tex);
    DG.gen_fb(1, &P.fb); DG.gen_tex(1, &P.tex);
    DG.bind_tex(0x0DE1, P.tex);
    DG.tex_storage2d(0x0DE1, 1, 0x8814 /* RGBA32F */, 4, 2);
    a_glBindFramebuffer(0x8CA9 /* DRAW */, P.fb);
    DG.fb_tex2d(0x8CA9, 0x8CE0, 0x0DE1, P.tex, 0);
    P.flt = DG.check_fb(0x8CA9) == 0x8CD5;
    if (!P.flt) {   /* float is not renderable here: 8-bit, so values above 1 read as 1 */
        unsigned t2 = 0; DG.gen_tex(1, &t2); DG.bind_tex(0x0DE1, t2); DG.tex_storage2d(0x0DE1, 1, 0x8058 /* RGBA8 */, 4, 2);
        DG.fb_tex2d(0x8CA9, 0x8CE0, 0x0DE1, t2, 0); P.tex = t2;
    }
    P.ok = DG.check_fb(0x8CA9) == 0x8CD5;
    DG.bind_tex(0x0DE1, (unsigned)old_tex);
    DG.active_tex((unsigned)old_at);
    a_glBindFramebuffer(0x8CA9, (unsigned)old_fb);
    tl_log_line("gldiag: texture probe %s (%s)", P.ok ? "ready" : "unavailable", P.flt ? "float" : "8-bit");
    return P.ok;
}

/* row 0 at lod 0, row 1 at lod `lod`; `why` (when set) heads the line instead of the plain texture line */
static void diag_probe_at(unsigned target, unsigned tex, unsigned samp, int lod, const char *why)
{
    int t = target == 0x8513 ? 1 : target == 0x8C1A ? 2 : target == 0x0DE1 ? 0 : -1;
    if (t < 0 || !tex || !probe_init()) return;
    unsigned unit = 0x84C0 + (unsigned)P.unit;
    int prog = 0, dfb = 0, rfb = 0, vp[4] = {0}, va = 0, at = 0, otex = 0, osamp = 0, cm[4] = {1, 1, 1, 1}, pack = 4, pbo = 0, rb = 0;
    a_glGetIntegerv(0x8B8D, &prog); a_glGetIntegerv(0x8CA6, &dfb); a_glGetIntegerv(0x8CAA, &rfb); a_glGetIntegerv(0x0BA2, vp);
    a_glGetIntegerv(0x85B5, &va); a_glGetIntegerv(0x84E0, &at); a_glGetIntegerv(0x0C23, cm); a_glGetIntegerv(0x0D05, &pack);
    a_glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pbo);
    static const unsigned caps[] = { 0x0BE2, 0x0B71, 0x0B44, 0x0C11, 0x0B90, 0x8C89, 0x809E, 0x8037 };
    unsigned char was[8];
    for (int i = 0; i < 8; i++) { was[i] = DG.is_enabled(caps[i]); if (was[i]) DG.disable(caps[i]); }
    DG.active_tex(unit);
    otex = diag_bound(target); a_glGetIntegerv(0x8919, &osamp);
    a_glGetIntegerv(0x0C02 /* READ_BUFFER */, &rb);
    DG.bind_tex(target, tex); DG.bind_samp((unsigned)P.unit, samp);
    int base = 0, maxl = 0, minf = 0, magf = 0, imm = 0, imml = 0, cmp = 0;
    DG.get_tex_iv(target, 0x813C, &base); DG.get_tex_iv(target, 0x813D, &maxl); DG.get_tex_iv(target, 0x2801, &minf);
    DG.get_tex_iv(target, 0x2800, &magf); DG.get_tex_iv(target, 0x912F, &imm); DG.get_tex_iv(target, 0x82DF, &imml);
    DG.get_tex_iv(target, 0x884C, &cmp);
    if (samp) { DG.get_samp_iv(samp, 0x2801, &minf); DG.get_samp_iv(samp, 0x2800, &magf); DG.get_samp_iv(samp, 0x884C, &cmp); }
    float out[32] = {0}; bool read = false;
    if (!cmp) {
        static const float uv2d[16] = { 0.25f, 0.25f, 0, 0, 0.5f, 0.5f, 0, 0, 0.75f, 0.75f, 0, 0, 0.3f, 0.7f, 0, 0 };
        static const float dirs[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, -1, 0, 0, 0, 0, 1, 0 };   /* +X, +Y (up), -Y (down), +Z */
        DG.use(P.prog[t]); DG.u1i(P.loc_s[t], P.unit); DG.u4fv(P.loc_uv[t], 4, t == 1 ? dirs : uv2d); DG.u1f(P.loc_l[t], (float)lod);
        DG.bind_va(P.va);
        a_glBindFramebuffer(0x8CA9, P.fb); DG.viewport(0, 0, 4, 2); DG.color_mask(1, 1, 1, 1);
        DG.draw_arrays(0x0004, 0, 3);
        a_glBindFramebuffer(0x8CA8, P.fb); DG.read_buffer(0x8CE0);
        if (pbo) DG.bind_buffer(0x88EB, 0);
        a_glPixelStorei(0x0D05, 4);
        if (P.flt) a_glReadPixels(0, 0, 4, 2, 0x1908, 0x1406, out);
        else { uint8_t b[32] = {0}; a_glReadPixels(0, 0, 4, 2, 0x1908, 0x1401, b); for (int i = 0; i < 32; i++) out[i] = b[i] / 255.0f; }
        read = true;
        a_glPixelStorei(0x0D05, pack);
        if (pbo) DG.bind_buffer(0x88EB, (unsigned)pbo);
        a_glBindFramebuffer(0x8CA8, (unsigned)rfb); DG.read_buffer((unsigned)rb);
        a_glBindFramebuffer(0x8CA9, (unsigned)dfb);
        DG.viewport(vp[0], vp[1], vp[2], vp[3]); DG.color_mask((unsigned char)cm[0], (unsigned char)cm[1], (unsigned char)cm[2], (unsigned char)cm[3]);
        DG.bind_va((unsigned)va); DG.use((unsigned)prog);
    }
    DG.bind_tex(target, (unsigned)otex); DG.bind_samp((unsigned)P.unit, (unsigned)osamp);
    DG.active_tex((unsigned)at);
    for (int i = 0; i < 8; i++) if (was[i]) DG.enable(caps[i]);
    char px[600]; size_t k = 0; px[0] = 0;
    if (read)
        for (int i = 0; i < 8; i++)
            k += (size_t)snprintf(px + k, sizeof(px) - k, "%s(%.3g %.3g %.3g %.3g)", i == 0 ? "lod0 " : i == 4 ? " | lodN " : " ",
                                  out[i * 4], out[i * 4 + 1], out[i * 4 + 2], out[i * 4 + 3]);
    if (read) { char *q = strstr(px, "lodN"); if (q) q[3] = lod < 10 ? (char)('0' + lod) : '+'; }
    bool nan = false;
    if (read) for (int i = 0; i < 32; i++) if (out[i] != out[i] || out[i] > 65000.0f || out[i] < -65000.0f) nan = true;
    if (why) { tl_log_line("gldiag:     %s: tex %u%s; %s", why, tex, nan ? " HAS NaN/inf" : "", read ? px : "(not read)"); return; }
    tl_log_line("gldiag:     tex %u: base %d max %d min %#x mag %#x immutable %d (%d levels) compare %#x; %s", tex, base, maxl, minf, magf,
                imm, imml, cmp, read ? px : "(depth compare: not read)");
}
static void diag_probe(unsigned target, unsigned tex, unsigned samp) { diag_probe_at(target, tex, samp, 4, NULL); }

/* the reflection cube's lower mips read back NaN: whether glGenerateMipmap or the copies that follow put them there */
static void diag_cube_mips_done(unsigned tex)
{
    static atomic_int n;
    if (!diag_load() || atomic_fetch_add(&n, 1) >= 8) return;
    unsigned err = DG.get_error();
    char why[96]; snprintf(why, sizeof(why), "after glGenerateMipmap(CUBE)%s", err ? " (GL error)" : "");
    diag_probe_at(0x8513, tex, 0, 4, why);
}
/* a texture array the game rendered into layer by layer (terrain layers), then mipmapped: what its layer 0 holds at lod 0 and 4,
 * and how many draws went into it -- an array that reads as zeros or garbage here is what the terrain samples */
static void diag_array_mips_done(unsigned tex)
{
    static atomic_int n;
    long draws = t_layer_draws; t_layer_draws = -1;
    if (!diag_load() || atomic_fetch_add(&n, 1) >= 16) return;
    unsigned err = DG.get_error();
    char why[120]; snprintf(why, sizeof(why), "after glGenerateMipmap(2D_ARRAY), %ld draws since a layer was attached%s", draws, err ? " (GL error)" : "");
    diag_probe_at(0x8C1A, tex, 0, 4, why);
}
/* after a copy into a cube's level: that level read back, and the GL error the copy raised (if any) */
static void diag_cube_copy_done(unsigned sn, unsigned st, int sl, unsigned dn, int dl, int face, int w, int h, unsigned err, int rfmt, int rtype)
{
    static atomic_int n;
    if (!diag_load()) return;
    int k = atomic_fetch_add(&n, 1);
    if (k >= 48) return;
    char why[200];
    snprintf(why, sizeof(why), "copy %#x %u lvl %d -> cube %u lvl %d face %d, %dx%d, source reads as fmt %#x type %#x, GL error %#x",
             st, sn, sl, dn, dl, face, w, h, rfmt, rtype, err);
    if (face == 5 || err) {
        diag_probe_at(0x8513, dn, 0, dl, why);
        snprintf(why, sizeof(why), "  ... and its source, %#x %u at lvl %d (before the copy it is what was rendered)", st, sn, sl);
        if (st == 0x8513 || st == 0x0DE1) diag_probe_at(st, sn, 0, sl, why);
    } else tl_log_line("gldiag:     %s", why);
}

/* the values in a uniform block, as the buffer holds them now: material properties (UnityPerMaterial) the engine filled in */
static void diag_block_values(unsigned prog, unsigned blk, unsigned ubo, int off, int size)
{
    static void *(*map)(unsigned, intptr_t, intptr_t, unsigned);
    static unsigned char (*unmap)(unsigned);
    static void (*buf_iv)(unsigned, unsigned, int *);
    if (!map) { *(void **)&map = a_eglGetProcAddress("glMapBufferRange"); *(void **)&unmap = a_eglGetProcAddress("glUnmapBuffer");
                *(void **)&buf_iv = a_eglGetProcAddress("glGetBufferParameteriv"); }
    if (!map || !unmap || !buf_iv) return;
    while (DG.get_error()) {}
    int prev = 0; a_glGetIntegerv(0x8F36 /* COPY_READ_BUFFER_BINDING */, &prev);
    DG.bind_buffer(0x8F36, ubo);
    int total = 0, mapped = 0; buf_iv(0x8F36, 0x8764 /* BUFFER_SIZE */, &total); buf_iv(0x8F36, 0x88BC /* BUFFER_MAPPED */, &mapped);
    int len = total - off < size ? total - off : size;
    const uint8_t *m = NULL; uint8_t copy[4096];
    if (!mapped && len > 0 && len <= (int)sizeof(copy)) {
        m = map(0x8F36, off, len, 0x0001 /* MAP_READ_BIT */);
        if (m) { memcpy(copy, m, (size_t)len); unmap(0x8F36); m = copy; }
    }
    DG.bind_buffer(0x8F36, (unsigned)prev);
    while (DG.get_error()) {}
    if (!m) { tl_log_line("gldiag:     (buffer %u not readable: %s, size %d)", ubo, mapped ? "mapped by the app" : "map failed", total); return; }
    int n = 0; DG.get_prog_iv(prog, 0x8B86, &n);
    char line[1400]; size_t k = 0; line[0] = 0;
    for (int i = 0; i < n && i < 1024; i++) {
        unsigned idx = (unsigned)i; int ub = -1, uo = -1;
        DG.get_uiv(prog, 1, &idx, 0x8A3A /* UNIFORM_BLOCK_INDEX */, &ub);
        if (ub != (int)blk) continue;
        DG.get_uiv(prog, 1, &idx, 0x8A3B /* UNIFORM_OFFSET */, &uo);
        char name[96] = ""; int l = 0, sz = 0; unsigned ty = 0;
        DG.get_active_u(prog, idx, sizeof(name), &l, &sz, &ty, name);
        int c = ty == 0x1406 || ty == 0x1404 || ty == 0x1405 ? 1 : ty == 0x8B50 ? 2 : ty == 0x8B51 ? 3 : ty == 0x8B52 ? 4 : ty == 0x8B5C ? 4 : 0;
        if (uo < 0 || uo + 4 * c > len) continue;
        char one[200]; int w = snprintf(one, sizeof(one), "%s=(", name);
        if (!c) w += snprintf(one + w, sizeof(one) - (size_t)w, "%s", diag_type(ty));
        for (int j = 0; j < c; j++) {
            if (ty == 0x1404 || ty == 0x1405) { int32_t v; memcpy(&v, m + uo + 4 * j, 4); w += snprintf(one + w, sizeof(one) - (size_t)w, "%s%d", j ? "," : "", v); }
            else { float v; memcpy(&v, m + uo + 4 * j, 4); w += snprintf(one + w, sizeof(one) - (size_t)w, "%s%g", j ? "," : "", v); }
        }
        snprintf(one + w, sizeof(one) - (size_t)w, ") ");
        size_t ol = strlen(one);
        if (k + ol >= 1200) { tl_log_line("gldiag:     %s", line); k = 0; line[0] = 0; }
        memcpy(line + k, one, ol + 1); k += ol;
    }
    if (k) tl_log_line("gldiag:     %s", line);
}

static void diag_report_prog(unsigned prog, unsigned mode, int count, bool probe)
{
    char line[1400]; size_t k;
    int n = 0, nblk = 0;
    DG.get_prog_iv(prog, 0x8B86, &n);
    DG.get_prog_iv(prog, 0x8A36 /* GL_ACTIVE_UNIFORM_BLOCKS */, &nblk);
    int fb = 0; a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    int vp[4] = {0}; a_glGetIntegerv(0x0BA2 /* VIEWPORT */, vp);
    int bs = 0, bd = 0, bsa = 0, bda = 0, df = 0, dm = 0, cm[4] = {0}, cullf = 0, ff = 0;
    a_glGetIntegerv(0x80C9 /* BLEND_SRC_RGB */, &bs); a_glGetIntegerv(0x80C8 /* BLEND_DST_RGB */, &bd);
    a_glGetIntegerv(0x80CB /* BLEND_SRC_ALPHA */, &bsa); a_glGetIntegerv(0x80CA /* BLEND_DST_ALPHA */, &bda);
    a_glGetIntegerv(0x0B74 /* DEPTH_FUNC */, &df); a_glGetIntegerv(0x0B72 /* DEPTH_WRITEMASK */, &dm);
    a_glGetIntegerv(0x0C23 /* COLOR_WRITEMASK */, cm); a_glGetIntegerv(0x0B45 /* CULL_FACE_MODE */, &cullf);
    a_glGetIntegerv(0x0B46 /* FRONT_FACE */, &ff);
    tl_log_line("gldiag: prog %u: mode %#x count %d, fbo %d viewport %d,%d %dx%d, %d uniforms, %d blocks", prog, mode, count,
                fb, vp[0], vp[1], vp[2], vp[3], n, nblk);
    tl_log_line("gldiag:   state: blend %d (%#x,%#x / %#x,%#x) depth test %d func %#x write %d, cull %d (%#x, front %#x), colormask %d%d%d%d, a2c %d",
                DG.is_enabled(0x0BE2), bs, bd, bsa, bda, DG.is_enabled(0x0B71), df, dm, DG.is_enabled(0x0B44), cullf, ff,
                cm[0], cm[1], cm[2], cm[3], DG.is_enabled(0x809E));
    for (int b = 0; b < nblk && b < 16; b++) {
        char name[96] = ""; int l = 0, bind = -1, size = 0, ubo = 0, off = 0, bsz = 0;
        DG.get_blk_name(prog, (unsigned)b, sizeof(name), &l, name);
        DG.get_blk_iv(prog, (unsigned)b, 0x8A3F /* UNIFORM_BLOCK_BINDING */, &bind);
        DG.get_blk_iv(prog, (unsigned)b, 0x8A40 /* UNIFORM_BLOCK_DATA_SIZE */, &size);
        if (bind >= 0) {
            DG.get_i_v(0x8A28 /* UNIFORM_BUFFER_BINDING */, (unsigned)bind, &ubo);
            DG.get_i_v(0x8A29 /* UNIFORM_BUFFER_START */, (unsigned)bind, &off);
            DG.get_i_v(0x8A2A /* UNIFORM_BUFFER_SIZE */, (unsigned)bind, &bsz);
        }
        tl_log_line("gldiag:   block '%s': %d bytes, binding %d -> buffer %d (offset %d, size %d)", name, size, bind, ubo, off, bsz);
        if (ubo > 0 && size > 0 && strcmp(name, "UnityPerDraw")) diag_block_values(prog, (unsigned)b, (unsigned)ubo, off, size);
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
            int unit = -1, tex = 0, samp = 0;
            if (loc >= 0) DG.get_u_iv(prog, loc, &unit);
            if (unit >= 0 && unit < 96) {
                DG.active_tex(0x84C0 /* TEXTURE0 */ + (unsigned)unit);
                tex = diag_bound(target);
                a_glGetIntegerv(0x8919 /* SAMPLER_BINDING */, &samp);
            }
            DG.active_tex((unsigned)active);
            tl_log_line("gldiag:   %s %s: unit %d -> tex %d (sampler obj %d)", diag_type(ty), name, unit, tex, samp);
            if (probe) diag_probe(target, (unsigned)tex, (unsigned)samp);
        } else if (loc >= 0 && (ty == 0x1406 || ty == 0x8B50 || ty == 0x8B51 || ty == 0x8B52 || ty == 0x1404 || ty == 0x1405)) {
            float v[4] = {0}; int iv[4] = {0}; unsigned uv[4] = {0};
            char one[200];
            int m;
            if (ty == 0x1404) { DG.get_u_iv(prog, loc, iv); m = snprintf(one, sizeof(one), "%s=(%d", name, iv[0]); }
            else if (ty == 0x1405) { DG.get_u_uiv(prog, loc, uv); m = snprintf(one, sizeof(one), "%s=(%#x", name, uv[0]); }
            else {
                DG.get_u_fv(prog, loc, v);
                int c = ty == 0x1406 ? 1 : ty == 0x8B50 ? 2 : ty == 0x8B51 ? 3 : 4;
                m = snprintf(one, sizeof(one), "%s=(%g", name, v[0]);
                for (int j = 1; j < c; j++) m += snprintf(one + m, sizeof(one) - (size_t)m, ",%g", v[j]);
            }
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

/* ---- frame captures */
#define CAP_DRAWS 4096
#define CAP_CLEARED 0xFFFF
static const unsigned long k_cap_frames[] = { 150, 300, 900, 1800, 3600 };
#define CAP_N (sizeof(k_cap_frames) / sizeof(k_cap_frames[0]))
static struct {
    int k;                                   /* capture number (1-based) of the frame being captured, 0 none */
    unsigned next;                           /* how many captures have been started */
    pthread_t thr;                           /* the thread whose frame is captured: the one that draws the scene */
    bool active, chosen, dirty, tracked, failed;
    unsigned fbo; int w, h; bool flt, srgb, ms;
    unsigned tmp_fb;
    uint8_t *prev, *cur; size_t bpp;
    uint16_t *owner;
    int ndraw, lines, reports;
    struct { unsigned prog, mode; int count, changed; } d[CAP_DRAWS];
} C;
static unsigned char g_diag_dark_done[DIAG_PROGS];
static __thread unsigned long t_diag_draws;   /* draws by this thread since its last swap */
static atomic_ulong g_diag_draws_all;          /* draws by every thread since the last swap */
/* a capture follows one thread: other threads present their own surfaces (the app's UI) and draw nothing of the scene */
static inline bool cap_here(void) { if (!C.active) return false; return pthread_equal(pthread_self(), C.thr); }

static void cap_px(const uint8_t *buf, size_t i, float rgb[3])
{
    if (C.flt) { const float *f = (const float *)(buf + i * 16); rgb[0] = f[0]; rgb[1] = f[1]; rgb[2] = f[2]; }
    else { const uint8_t *b = buf + i * 4; rgb[0] = b[0] / 255.0f; rgb[1] = b[1] / 255.0f; rgb[2] = b[2] / 255.0f; }
}
static bool cap_dark(const float rgb[3])
{
    float m = rgb[0] > rgb[1] ? rgb[0] : rgb[1]; m = m > rgb[2] ? m : rgb[2];
    return C.flt ? m < 0.006f : m < 0.07f;
}

static bool cap_choose(int fb)
{
    int vp[4] = {0}; a_glGetIntegerv(0x0BA2, vp);
    if (vp[2] < 300 || vp[3] < 120 || vp[2] < vp[3] * 13 / 10) return false;   /* the scene: landscape, not a square shadow map or atlas */
    int old_read = 0; a_glGetIntegerv(0x8CAA, &old_read);
    a_glBindFramebuffer(0x8CA8, (unsigned)fb);
    unsigned att = fb ? 0x8CE0 : 0x0405 /* BACK */;
    int type = 0, ct = 0, enc = 0, name = 0, sb = 0;
    DG.fb_att_iv(0x8CA8, att, 0x8CD0 /* OBJECT_TYPE */, &type);
    if (type) {
        DG.fb_att_iv(0x8CA8, att, 0x8211 /* COMPONENT_TYPE */, &ct);
        DG.fb_att_iv(0x8CA8, att, 0x8210 /* COLOR_ENCODING */, &enc);
        if (fb) DG.fb_att_iv(0x8CA8, att, 0x8CD1 /* OBJECT_NAME */, &name);
        a_glGetIntegerv(0x80A8 /* SAMPLE_BUFFERS */, &sb);
    }
    a_glBindFramebuffer(0x8CA8, (unsigned)old_read);
    if (!type) return false;
    C.fbo = (unsigned)fb; C.w = vp[0] + vp[2]; C.h = vp[1] + vp[3];
    C.flt = ct == 0x1406; C.srgb = enc == 0x8C40; C.ms = sb > 0; C.bpp = C.flt ? 16 : 4;
    if (C.ms) {
        int rbf = 0, old_rb = 0;
        if (type == 0x8D41 /* RENDERBUFFER */) {
            a_glGetIntegerv(0x8CA7 /* RENDERBUFFER_BINDING */, &old_rb);
            DG.bind_rb(0x8D41, (unsigned)name); DG.rb_iv(0x8D41, 0x8D44 /* INTERNAL_FORMAT */, &rbf);
            unsigned rbo = 0; DG.gen_rb(1, &rbo); DG.bind_rb(0x8D41, rbo); DG.rb_storage(0x8D41, (unsigned)rbf, C.w, C.h);
            DG.bind_rb(0x8D41, (unsigned)old_rb);
            int old_draw = 0; a_glGetIntegerv(0x8CA6, &old_draw);
            DG.gen_fb(1, &C.tmp_fb); a_glBindFramebuffer(0x8CA9, C.tmp_fb); DG.fb_rb(0x8CA9, 0x8CE0, 0x8D41, rbo);
            bool ok = DG.check_fb(0x8CA9) == 0x8CD5;
            a_glBindFramebuffer(0x8CA9, (unsigned)old_draw);
            if (!ok) C.tmp_fb = 0;
        }
        if (!C.tmp_fb) { tl_log_line("gldiag: cap%d: scene target fbo %d is multisampled and cannot be resolved here", C.k, fb); C.failed = true; return true; }
    }
    size_t n = (size_t)C.w * C.h;
    C.prev = malloc(n * C.bpp); C.cur = malloc(n * C.bpp); C.owner = calloc(n, sizeof(uint16_t));
    if (!C.prev || !C.cur || !C.owner) { C.failed = true; return true; }
    tl_log_line("gldiag: cap%d: scene target fbo %d, %dx%d, %s%s%s (attachment %#x name %d)", C.k, fb, C.w, C.h, C.flt ? "float" : "8-bit",
                C.srgb ? " sRGB" : "", C.ms ? ", multisampled" : "", type, name);
    return true;
}

static bool cap_read(uint8_t *buf)
{
    int old_read = 0, old_draw = 0, pack = 4, pbo = 0, rb = 0;
    a_glGetIntegerv(0x8CAA, &old_read); a_glGetIntegerv(0x8CA6, &old_draw); a_glGetIntegerv(0x0D05, &pack);
    a_glGetIntegerv(0x88ED, &pbo);
    if (pbo) DG.bind_buffer(0x88EB, 0);
    a_glBindFramebuffer(0x8CA8, C.fbo);
    a_glGetIntegerv(0x0C02, &rb);
    DG.read_buffer(C.fbo ? 0x8CE0 : 0x0405);
    if (C.ms) {
        bool sc = DG.is_enabled(0x0C11), rd = DG.is_enabled(0x8C89);
        if (sc) DG.disable(0x0C11);
        if (rd) DG.disable(0x8C89);
        a_glBindFramebuffer(0x8CA9, C.tmp_fb);
        DG.blit(0, 0, C.w, C.h, 0, 0, C.w, C.h, 0x4000, 0x2600);
        if (sc) DG.enable(0x0C11);
        if (rd) DG.enable(0x8C89);
        a_glBindFramebuffer(0x8CA9, (unsigned)old_draw);
        DG.read_buffer((unsigned)rb);
        a_glBindFramebuffer(0x8CA8, C.tmp_fb);
    }
    a_glPixelStorei(0x0D05, 4);
    while (DG.get_error()) {}
    a_glReadPixels(0, 0, C.w, C.h, 0x1908, C.flt ? 0x1406 : 0x1401, buf);
    if (!C.ms) DG.read_buffer((unsigned)rb);
    unsigned err = DG.get_error();
    a_glPixelStorei(0x0D05, pack);
    if (pbo) DG.bind_buffer(0x88EB, (unsigned)pbo);
    a_glBindFramebuffer(0x8CA8, (unsigned)old_read);
    if (err) { tl_log_line("gldiag: cap%d: reading the scene failed (GL error %#x)", C.k, err); C.failed = true; return false; }
    return true;
}

/* every pixel that differs between prev and cur gets this owner; returns how many, their colour sum and how many are dark */
static int cap_diff(uint16_t owner, double sum[3], int *dark)
{
    size_t n = (size_t)C.w * C.h; int changed = 0, dk = 0;
    for (size_t i = 0; i < n; i++) {
        bool diff;
        if (C.flt) { const uint64_t *a = (const uint64_t *)(C.prev + i * 16), *b = (const uint64_t *)(C.cur + i * 16); diff = a[0] != b[0] || a[1] != b[1]; }
        else diff = ((const uint32_t *)C.prev)[i] != ((const uint32_t *)C.cur)[i];
        if (!diff) continue;
        C.owner[i] = owner; changed++;
        if (sum) { float c[3]; cap_px(C.cur, i, c); sum[0] += c[0]; sum[1] += c[1]; sum[2] += c[2]; if (cap_dark(c)) dk++; }
    }
    if (dark) *dark = dk;
    uint8_t *t = C.prev; C.prev = C.cur; C.cur = t;
    return changed;
}

static void cap_dirty(void) { if (cap_here()) C.dirty = true; }

static void cap_pre(void)
{
    C.tracked = false;
    if (!C.active || C.failed || !diag_load()) return;
    int fb = 0; a_glGetIntegerv(0x8CA6, &fb);
    if (!C.chosen) {
        if (!cap_choose(fb)) return;
        C.chosen = true;
        if (C.failed || !cap_read(C.prev)) return;
        C.dirty = false;
    }
    if ((unsigned)fb != C.fbo) { C.dirty = true; return; }
    if (C.dirty) {                            /* cleared, blitted to or drawn by something unseen since: re-read, without an owner */
        if (!cap_read(C.cur)) return;
        cap_diff(CAP_CLEARED, NULL, NULL);
        C.dirty = false;
    }
    C.tracked = C.ndraw < CAP_DRAWS;
}

static void cap_post(unsigned mode, int count)
{
    if (!C.tracked) return;
    C.tracked = false;
    unsigned err = DG.get_error();
    int prog = 0; a_glGetIntegerv(0x8B8D, &prog);
    if (!cap_read(C.cur)) return;
    int idx = C.ndraw++;
    double s[3] = {0}; int dark = 0;
    int changed = cap_diff((uint16_t)(idx + 1), s, &dark);
    C.d[idx].prog = (unsigned)prog; C.d[idx].mode = mode; C.d[idx].count = count; C.d[idx].changed = changed;
    if (changed && C.lines < 400) {
        C.lines++;
        tl_log_line("gldiag: cap%d #%d prog %d count %d: %d px, avg (%.3f %.3f %.3f), dark %d%%%s", C.k, idx, prog, count, changed,
                    s[0] / changed, s[1] / changed, s[2] / changed, dark * 100 / changed, err ? " GL error before" : "");
    }
    bool arr = false;
    if (changed >= 120 && prog > 0 && prog < DIAG_PROGS && !g_diag_dark_done[prog] && C.reports < 24) {
        int nu = 0; DG.get_prog_iv((unsigned)prog, 0x8B86, &nu);
        for (int i = 0; i < nu && i < 512 && !arr; i++) {
            char nm[96]; int l = 0, sz = 0; unsigned ty = 0;
            DG.get_active_u((unsigned)prog, (unsigned)i, sizeof(nm), &l, &sz, &ty, nm);
            arr = ty == 0x8DC1;
        }
    }
    if (arr) {
        g_diag_dark_done[prog] = 1; C.reports++;
        tl_log_line("gldiag: cap%d #%d: program %d samples a texture array and drew %d px (%d%% dark); its state:", C.k, idx, prog, changed, dark * 100 / changed);
        diag_report_prog((unsigned)prog, mode, count, true);
        diag_write_prog_src((unsigned)prog);
    }
    if (changed >= 120 && dark * 10 >= changed * 6 && prog > 0 && prog < DIAG_PROGS && !g_diag_dark_done[prog] && C.reports < 24) {
        g_diag_dark_done[prog] = 1; C.reports++;
        tl_log_line("gldiag: cap%d #%d: program %d drew %d px, %d%% of them dark; its state:", C.k, idx, prog, changed, dark * 100 / changed);
        diag_report_prog((unsigned)prog, mode, count, true);
        diag_write_prog_src((unsigned)prog);
    }
}

static void cap_bmp(const char *path)
{
    int w = C.w, h = C.h, row = (w * 3 + 3) & ~3;
    size_t sz = 54 + (size_t)row * h;
    uint8_t *b = calloc(1, sz);
    if (!b) return;
    uint32_t v;
    b[0] = 'B'; b[1] = 'M'; v = (uint32_t)sz; memcpy(b + 2, &v, 4); v = 54; memcpy(b + 10, &v, 4);
    v = 40; memcpy(b + 14, &v, 4); int32_t iw = w, ih = h; memcpy(b + 18, &iw, 4); memcpy(b + 22, &ih, 4);
    b[26] = 1; b[28] = 24; v = (uint32_t)(row * h); memcpy(b + 34, &v, 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float c[3]; cap_px(C.prev, (size_t)y * w + x, c);
            uint8_t *o = b + 54 + (size_t)y * row + (size_t)x * 3;
            for (int j = 0; j < 3; j++) {
                float f = c[j];
                if (!(f == f)) f = 1.0f;                                  /* NaN shows as full magenta-ish */
                if (C.flt) { f = f < 0 ? 0 : f / (1.0f + f); f = sqrtf(f); }   /* linear HDR, roughly tone-mapped for a look */
                f = f < 0 ? 0 : f > 1 ? 1 : f;
                o[2 - j] = (uint8_t)(f * 255.0f + 0.5f);
            }
        }
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(b, 1, sz, f); fclose(f); }
    free(b);
}

static void cap_finish(void)
{
    C.active = false;
    if (!C.chosen || C.failed || !C.owner) { tl_log_line("gldiag: cap%d: no scene captured%s", C.k, C.failed ? " (failed)" : ""); goto out; }
    size_t n = (size_t)C.w * C.h;
    static int own[CAP_DRAWS + 1]; static int own_dark[CAP_DRAWS + 1]; static double own_sum[CAP_DRAWS + 1][3];
    memset(own, 0, sizeof(own)); memset(own_dark, 0, sizeof(own_dark)); memset(own_sum, 0, sizeof(own_sum));
    int nan = 0;
    for (size_t i = 0; i < n; i++) {
        int o = C.owner[i] == CAP_CLEARED ? 0 : C.owner[i];
        float c[3]; cap_px(C.prev, i, c);
        if (!(c[0] == c[0]) || !(c[1] == c[1]) || !(c[2] == c[2])) nan++;
        own[o]++; own_sum[o][0] += c[0]; own_sum[o][1] += c[1]; own_sum[o][2] += c[2]; if (cap_dark(c)) own_dark[o]++;
    }
    tl_log_line("gldiag: cap%d: %d draws into the scene; %d px not drawn this frame, %d NaN px", C.k, C.ndraw, own[0], nan);
    /* the map: one character per cell, a code per draw that owns a cell's centre */
    enum { MW = 72, MH = 26 };
    static const char codes[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    int code_of[CAP_DRAWS + 1]; for (int i = 0; i <= CAP_DRAWS; i++) code_of[i] = -1;
    int legend[62], nleg = 0;
    char map[MH][MW + 1];
    for (int r = 0; r < MH; r++) {
        for (int c = 0; c < MW; c++) {
            int x = (int)((c + 0.5) * C.w / MW), y = (int)((MH - 1 - r + 0.5) * C.h / MH);   /* top row first */
            uint16_t o = C.owner[(size_t)y * C.w + x];
            char ch = '.';
            if (o == CAP_CLEARED) ch = '~';
            else if (o) {
                if (code_of[o] < 0 && nleg < 62) { code_of[o] = nleg; legend[nleg++] = o; }
                ch = code_of[o] >= 0 ? codes[code_of[o]] : '?';
            }
            map[r][c] = ch;
        }
        map[r][MW] = 0;
    }
    tl_log_line("gldiag: cap%d map ('.' not drawn, '~' cleared/copied, else the draw below):", C.k);
    for (int r = 0; r < MH; r++) tl_log_line("gldiag: cap%d | %s |", C.k, map[r]);
    for (int i = 0; i < nleg; i++) {
        int o = legend[i], d = o - 1, cnt = own[o] ? own[o] : 1;
        tl_log_line("gldiag: cap%d %c = #%d prog %u count %d: owns %d px, avg (%.3f %.3f %.3f), dark %d%%", C.k, codes[i], d, C.d[d].prog,
                    C.d[d].count, own[o], own_sum[o][0] / cnt, own_sum[o][1] / cnt, own_sum[o][2] / cnt, own_dark[o] * 100 / cnt);
    }
    const char *docs = diag_docs();
    if (docs) {
        char path[1024]; snprintf(path, sizeof(path), "%s/husk-cap%d.bmp", docs, C.k);
        cap_bmp(path);
        tl_log_line("gldiag: cap%d: scene written to %s", C.k, path);
    }
out:
    free(C.prev); free(C.cur); free(C.owner);
    C.prev = C.cur = NULL; C.owner = NULL;
}

/* ---- blend state as the game sets it: the characters draw with blending on and both factors ZERO, which is black.
 * Which call sets that (and whether it arrives through an indexed, per-draw-buffer call) is what these say. */
static atomic_int g_diag_blend_calls[4], g_diag_blend_zero;
/* After the first ZERO,ZERO blend (past the first frames), the next events that decide what a draw blends with, in order: which
 * call leaves a draw blending with ZERO,ZERO -- the engine asking for it, or a glDisable(GL_BLEND) that it thinks it made. */
static atomic_int g_bt_left = -1;       /* -1: not started; >0: events still to log */
static bool bt_on(void) { return atomic_load(&g_bt_left) > 0; }
static void bt_log(const char *what)
{
    if (atomic_fetch_sub(&g_bt_left, 1) <= 0) return;
    int prog = 0, fb = 0; if (a_glGetIntegerv) { a_glGetIntegerv(0x8B8D, &prog); a_glGetIntegerv(0x8CA6, &fb); }
    int s = 0, d = 0, sa = 0, da = 0, cm[4] = {0};
    if (a_glGetIntegerv) { a_glGetIntegerv(0x80C9, &s); a_glGetIntegerv(0x80C8, &d); a_glGetIntegerv(0x80CB, &sa); a_glGetIntegerv(0x80CA, &da); a_glGetIntegerv(0x0C23, cm); }
    unsigned char on = DG.is_enabled ? DG.is_enabled(0x0BE2) : 2;
    tl_log_line("gldiag: trace %-34s | now: program %d fbo %d blend %d (%#x,%#x / %#x,%#x) colormask %d%d%d%d", what, prog, fb, on, s, d, sa, da, cm[0], cm[1], cm[2], cm[3]);
}
static void diag_blend(int kind, int buf, unsigned s, unsigned d, unsigned sa, unsigned da)
{
    if (!diag_on()) return;
    static const char *const names[4] = { "glBlendFunc", "glBlendFuncSeparate", "glBlendFunci", "glBlendFuncSeparatei" };
    int n = atomic_fetch_add(&g_diag_blend_calls[kind], 1);
    bool zero = s == 0 && d == 0;
    int z = zero ? atomic_fetch_add(&g_diag_blend_zero, 1) : 0;
    if (zero) { int want = -1; if (atomic_compare_exchange_strong(&g_bt_left, &want, 200)) diag_load(); }
    if (bt_on()) { char w[80]; snprintf(w, sizeof(w), "%s(%#x,%#x,%#x,%#x)", names[kind], s, d, sa, da); bt_log(w); }
    if (n < 6 || (zero && z < 16)) {
        int prog = 0; if (a_glGetIntegerv) a_glGetIntegerv(0x8B8D, &prog);
        int fb = 0; if (a_glGetIntegerv) a_glGetIntegerv(0x8CA6, &fb);
        tl_log_line("gldiag: %s(%s%d%s%#x, %#x, %#x, %#x) #%d, program %d, fbo %d%s", names[kind], kind >= 2 ? "buf " : "", kind >= 2 ? buf : 0,
                    kind >= 2 ? ", " : "", s, d, sa, da, n, prog, fb, zero ? "  <- ZERO,ZERO" : "");
    }
}
/* Blending with both factors ZERO writes 0 whatever the shader computes and whatever is under it: the draw is a black silhouette.
 * No renderer asks for that on purpose (black is a shader output, or a clear); an engine gets there from a material whose blend
 * properties are unset (Unity's `Blend [_SrcBlend] [_DstBlend]` reads 0 for each). It is drawn as unblended, ONE,ZERO -- what the
 * draw would be with those properties at their defaults. TL_GL_KEEP_ZERO_BLEND=1: off. */
static int g_zero_blend_keep = -1;
/* The material that leaves ZERO,ZERO also leaves depth writes off (and culling off): every property it has reads 0. Drawn so, a
 * closed model shows its back faces over its front ones (it looks inside out) and the sky drawn later covers it. While blending
 * is on with ZERO,ZERO asked for, a draw writes depth whatever glDepthMask the game set; its own mask is put back after. */
static __thread bool t_zb_zero, t_zb_blend_on;
static __thread unsigned char t_game_depth_mask = 1;
static void (*r_glDepthMask)(unsigned);
static inline bool zb_pre(void)
{
    if (!t_zb_zero || !t_zb_blend_on || t_game_depth_mask) return false;
    static atomic_int said;
    if (!atomic_exchange(&said, 1))
        tl_log_line("gl: draws with that unset material also have depth writes off (they look inside out, and the sky covers them): they write depth (TL_GL_KEEP_ZERO_BLEND=1: off)");
    r_glDepthMask(1);
    return true;
}
static inline void zb_post(bool f) { if (f) r_glDepthMask(0); }
static inline void zero_blend_fix(unsigned *s, unsigned *d, unsigned *sa, unsigned *da)
{
    t_zb_zero = false;
    if (*s || *d) return;
    if (g_zero_blend_keep < 0) { const char *e = getenv("TL_GL_KEEP_ZERO_BLEND"); g_zero_blend_keep = e && *e == '1'; }
    if (g_zero_blend_keep) return;
    t_zb_zero = true;
    static atomic_int said;
    if (!atomic_exchange(&said, 1))
        tl_log_line("gl: the game blends with both factors ZERO (a black silhouette, from unset material blend properties): drawn as ONE,ZERO (TL_GL_KEEP_ZERO_BLEND=1: off)");
    *s = 1;
    if (!*sa && !*da) *sa = 1;
}
static void (*r_glBlendFunc)(unsigned, unsigned);
static void w_glBlendFunc(unsigned s, unsigned d) { unsigned fs = s, fd = d, x = s, y = d; zero_blend_fix(&fs, &fd, &x, &y); r_glBlendFunc(fs, fd); diag_blend(0, 0, s, d, s, d); }
static void (*r_glBlendFuncSeparate)(unsigned, unsigned, unsigned, unsigned);
static void w_glBlendFuncSeparate(unsigned s, unsigned d, unsigned sa, unsigned da)
{ unsigned fs = s, fd = d, fsa = sa, fda = da; zero_blend_fix(&fs, &fd, &fsa, &fda); r_glBlendFuncSeparate(fs, fd, fsa, fda); diag_blend(1, 0, s, d, sa, da); }
static void (*r_glBlendFunci)(unsigned, unsigned, unsigned);
static void w_glBlendFunci(unsigned b, unsigned s, unsigned d) { unsigned fs = s, fd = d, x = s, y = d; zero_blend_fix(&fs, &fd, &x, &y); r_glBlendFunci(b, fs, fd); diag_blend(2, (int)b, s, d, s, d); }
static void (*r_glBlendFuncSeparatei)(unsigned, unsigned, unsigned, unsigned, unsigned);
static void w_glBlendFuncSeparatei(unsigned b, unsigned s, unsigned d, unsigned sa, unsigned da)
{ unsigned fs = s, fd = d, fsa = sa, fda = da; zero_blend_fix(&fs, &fd, &fsa, &fda); r_glBlendFuncSeparatei(b, fs, fd, fsa, fda); diag_blend(3, (int)b, s, d, sa, da); }
static void (*r_glEnable)(unsigned);
static void w_glEnable(unsigned cap) { r_glEnable(cap); if (cap == 0x0BE2) t_zb_blend_on = true; if (cap == 0x0BE2 && bt_on()) bt_log("glEnable(GL_BLEND)"); }
static void (*r_glDisable)(unsigned);
static void w_glDisable(unsigned cap) { r_glDisable(cap); if (cap == 0x0BE2) t_zb_blend_on = false; if (cap == 0x0BE2 && bt_on()) bt_log("glDisable(GL_BLEND)"); }
static void (*r_glUseProgram)(unsigned);
static void w_glUseProgram(unsigned p) { g_cs_cur = cs_fake(p) ? p : 0; r_glUseProgram(g_cs_cur ? 0 : p); if (bt_on()) { char w[40]; snprintf(w, sizeof(w), "glUseProgram(%u)", p); bt_log(w); } }
static void (*r_glBlendEquationSeparate)(unsigned, unsigned);
static void w_glBlendEquationSeparate(unsigned c, unsigned a) { r_glBlendEquationSeparate(c, a); if (bt_on()) { char w[60]; snprintf(w, sizeof(w), "glBlendEquationSeparate(%#x,%#x)", c, a); bt_log(w); } }
static void (*r_glBlendEquation)(unsigned);
static void w_glBlendEquation(unsigned m) { r_glBlendEquation(m); if (bt_on()) { char w[60]; snprintf(w, sizeof(w), "glBlendEquation(%#x)", m); bt_log(w); } }
static void bt_draw(unsigned mode, int count) { if (bt_on()) { char w[60]; snprintf(w, sizeof(w), "  draw mode %#x count %d", mode, count); bt_log(w); } }

/* ---- draws into a cube map's face: reflection probes and their convolution. The convolution writes NaN (alpha 1) into every
 * level, and the characters and terrain that sample those levels come out black. Each program that draws into a cube face is
 * reported once: its state and uniforms (with the textures it samples read back), its vertex attributes, its source, and the
 * pixel at the viewport's centre before and after the draw. */
static unsigned char g_cube_done[DIAG_PROGS];
static atomic_int g_cube_n;
static __thread bool t_cube_post;
static struct {
    bool tried, ok;
    void (*get_attr)(unsigned, unsigned, int, int *, int *, unsigned *, char *);
    int (*attr_loc)(unsigned, const char *);
    void (*va_iv)(unsigned, unsigned, int *), (*va_fv)(unsigned, unsigned, float *), (*va_ptr)(unsigned, unsigned, void **);
} DA;
static float cube_h2f(uint16_t h)
{
    unsigned e = (h >> 10) & 31, m = h & 1023; float f;
    if (e == 31) f = m ? NAN : INFINITY;
    else if (!e) f = ldexpf((float)m, -24);
    else f = ldexpf((float)(m | 1024), (int)e - 25);
    return (h & 0x8000) ? -f : f;
}
static void cube_px(const char *when)
{
    int pbo = 0; a_glGetIntegerv(0x88ED /* PIXEL_PACK_BUFFER_BINDING */, &pbo);
    if (pbo) { tl_log_line("gldiag:   %s: not read (a pack buffer is bound)", when); return; }
    int fb = 0, rfb = 0, vp[4] = {0}, fmt = 0, type = 0;
    a_glGetIntegerv(0x8CA6, &fb); a_glGetIntegerv(0x8CAA /* READ_FRAMEBUFFER_BINDING */, &rfb); a_glGetIntegerv(0x0BA2, vp);
    if (rfb != fb) a_glBindFramebuffer(0x8CA8 /* READ_FRAMEBUFFER */, (unsigned)fb);
    a_glGetIntegerv(0x8B9B /* IMPLEMENTATION_COLOR_READ_FORMAT */, &fmt); a_glGetIntegerv(0x8B9A /* ..._TYPE */, &type);
    float c[4] = {0}; uint8_t b[32] = {0};
    int x = vp[0] + vp[2] / 2, y = vp[1] + vp[3] / 2;
    if (fmt == 0x1908 && (type == 0x140B || type == 0x8D61 || type == 0x1406)) {
        a_glReadPixels(x, y, 1, 1, 0x1908, (unsigned)type, b);
        for (int j = 0; j < 4; j++) {
            if (type == 0x1406) memcpy(&c[j], b + 4 * j, 4);
            else { uint16_t h; memcpy(&h, b + 2 * j, 2); c[j] = cube_h2f(h); }
        }
    } else {
        a_glReadPixels(x, y, 1, 1, 0x1908, 0x1401, b); type = 0x1401;
        for (int j = 0; j < 4; j++) c[j] = b[j] / 255.0f;
    }
    if (rfb != fb) a_glBindFramebuffer(0x8CA8, (unsigned)rfb);
    tl_log_line("gldiag:   %s: centre pixel (%g %g %g %g) [read as type %#x]", when, c[0], c[1], c[2], c[3], type);
}
static void cube_attrs(unsigned prog)
{
    if (!DA.tried) {
        DA.tried = true;
        *(void **)&DA.get_attr = a_eglGetProcAddress("glGetActiveAttrib"); *(void **)&DA.attr_loc = a_eglGetProcAddress("glGetAttribLocation");
        *(void **)&DA.va_iv = a_eglGetProcAddress("glGetVertexAttribiv"); *(void **)&DA.va_fv = a_eglGetProcAddress("glGetVertexAttribfv");
        *(void **)&DA.va_ptr = a_eglGetProcAddress("glGetVertexAttribPointerv");
        DA.ok = DA.get_attr && DA.attr_loc && DA.va_iv && DA.va_fv && DA.va_ptr;
    }
    if (!DA.ok) return;
    int n = 0, vao = 0, ebo = 0; DG.get_prog_iv(prog, 0x8B89 /* ACTIVE_ATTRIBUTES */, &n);
    a_glGetIntegerv(0x85B5 /* VERTEX_ARRAY_BINDING */, &vao); a_glGetIntegerv(0x8895 /* ELEMENT_ARRAY_BUFFER_BINDING */, &ebo);
    tl_log_line("gldiag:   %d attributes, vertex array %d, index buffer %d", n, vao, ebo);
    for (int i = 0; i < n && i < 16; i++) {
        char name[96] = ""; int l = 0, sz = 0; unsigned ty = 0;
        DA.get_attr(prog, (unsigned)i, sizeof(name), &l, &sz, &ty, name);
        int loc = DA.attr_loc(prog, name);
        if (loc < 0) { tl_log_line("gldiag:   attrib %s (%s): no location", name, diag_type(ty)); continue; }
        int en = 0, size = 0, stride = 0, type = 0, norm = 0, buf = 0, integer = 0, div = 0; void *off = NULL; float cur[4] = {0};
        DA.va_iv((unsigned)loc, 0x8622, &en); DA.va_iv((unsigned)loc, 0x8623, &size); DA.va_iv((unsigned)loc, 0x8624, &stride);
        DA.va_iv((unsigned)loc, 0x8625, &type); DA.va_iv((unsigned)loc, 0x886A, &norm); DA.va_iv((unsigned)loc, 0x889F, &buf);
        DA.va_iv((unsigned)loc, 0x88FD, &integer); DA.va_iv((unsigned)loc, 0x88FE, &div);
        DA.va_ptr((unsigned)loc, 0x8645, &off); DA.va_fv((unsigned)loc, 0x8626, cur);
        tl_log_line("gldiag:   attrib %s (%s) loc %d: %s size %d type %#x norm %d int %d stride %d buffer %d offset %p divisor %d; current (%g %g %g %g)",
                    name, diag_type(ty), loc, en ? "array" : "OFF", size, type, norm, integer, stride, buf, off, div, cur[0], cur[1], cur[2], cur[3]);
    }
}
static void cube_src(unsigned prog)
{
    if (prog >= DIAG_PROGS) return;
    for (int k = 0; k < 2; k++) {
        pthread_mutex_lock(&g_diag_mu);
        const char *s = g_diag_prog_src[prog][k];
        pthread_mutex_unlock(&g_diag_mu);
        if (!s) { tl_log_line("gldiag:   %s source: not seen (the program came from a binary)", k ? "fragment" : "vertex"); continue; }
        tl_log_line("gldiag:   %s source (%zu bytes):", k ? "fragment" : "vertex", strlen(s));
        int lines = 0;
        for (const char *p = s; *p && lines < 600; lines++) {
            const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l > 1900) l = 1900;
            tl_log_line("gldiag:src| %.*s", (int)l, p);
            if (!e) break;
            p = e + 1;
        }
    }
}
static void cube_pre(unsigned mode, int count)
{
    if (!diag_on() || atomic_load(&g_diag_done) || atomic_load(&g_cube_n) >= 40 || !a_glGetIntegerv) return;
    int fb = 0; a_glGetIntegerv(0x8CA6, &fb);
    if (!fb || !diag_load()) return;
    int ot = 0, face = 0, name = 0, lvl = 0;
    DG.fb_att_iv(0x8CA9 /* DRAW_FRAMEBUFFER */, 0x8CE0, 0x8CD0 /* OBJECT_TYPE */, &ot);
    if (ot != 0x1702) return;
    DG.fb_att_iv(0x8CA9, 0x8CE0, 0x8CD3 /* TEXTURE_CUBE_MAP_FACE */, &face);
    if (!face) return;
    int prog = 0; a_glGetIntegerv(0x8B8D, &prog);
    if (prog <= 0 || prog >= DIAG_PROGS || g_cube_done[prog]) return;
    g_cube_done[prog] = 1;
    if (atomic_fetch_add(&g_cube_n, 1) >= 40) return;
    DG.fb_att_iv(0x8CA9, 0x8CE0, 0x8CD1 /* OBJECT_NAME */, &name); DG.fb_att_iv(0x8CA9, 0x8CE0, 0x8CD2 /* TEXTURE_LEVEL */, &lvl);
    tl_log_line("gldiag: cube draw: program %d into cube %d face %#x level %d", prog, name, face, lvl);
    diag_report_prog((unsigned)prog, mode, count, true);
    cube_attrs((unsigned)prog);
    cube_src((unsigned)prog);
    cube_px("before the draw");
    t_cube_post = true;
}
static void cube_post(void) { if (t_cube_post) { t_cube_post = false; cube_px("after the draw"); } }
#define CUBE_PRE(m, c) cube_pre(m, c)
#define CUBE_POST() cube_post()

/* ---- GL errors: a call ANGLE refuses does nothing and says nothing, so a draw that is refused is a hole in the frame. While the
 * diagnostics run, the error is read after each draw (and clear, blit, mipmap generation); the first of each kind is logged, and
 * the error is kept for the game's own next glGetError, so it still sees it. */
static unsigned (*r_glGetError)(void);
static unsigned (*g_err_get)(void);
static __thread unsigned t_err_held;
static pthread_mutex_t g_err_mu = PTHREAD_MUTEX_INITIALIZER;
static struct { const char *where; unsigned err; int prog; } g_err_seen[40];
static int g_err_nseen;
static void err_check(const char *where)
{
    if (!diag_on() || !a_eglGetProcAddress || !a_glGetIntegerv) return;
    if (!g_err_get) g_err_get = (unsigned (*)(void))a_eglGetProcAddress("glGetError");
    if (!g_err_get) return;
    unsigned e = g_err_get();
    if (!e) return;
    if (!t_err_held) t_err_held = e;
    int prog = 0, fb = 0; a_glGetIntegerv(0x8B8D /* CURRENT_PROGRAM */, &prog); a_glGetIntegerv(0x8CA6 /* DRAW_FRAMEBUFFER_BINDING */, &fb);
    pthread_mutex_lock(&g_err_mu);
    bool seen = g_err_nseen >= 40;
    for (int i = 0; i < g_err_nseen && !seen; i++) if (g_err_seen[i].where == where && g_err_seen[i].err == e && g_err_seen[i].prog == prog) seen = true;
    if (!seen) { g_err_seen[g_err_nseen].where = where; g_err_seen[g_err_nseen].err = e; g_err_seen[g_err_nseen].prog = prog; g_err_nseen++; }
    pthread_mutex_unlock(&g_err_mu);
    if (seen) return;
    const char *name = e == 0x500 ? "INVALID_ENUM" : e == 0x501 ? "INVALID_VALUE" : e == 0x502 ? "INVALID_OPERATION" : e == 0x505 ? "OUT_OF_MEMORY" :
                       e == 0x506 ? "INVALID_FRAMEBUFFER_OPERATION" : "?";
    unsigned st = 0;
    if (e == 0x506) { unsigned (*cfs)(unsigned) = (unsigned (*)(unsigned))a_eglGetProcAddress("glCheckFramebufferStatus"); if (cfs) st = cfs(0x8CA9 /* DRAW_FRAMEBUFFER */); }
    tl_log_line("gldiag: GL error %#x %s at %s (or a call before it since the last check): program %d, fbo %d%s%s", e, name, where, prog, fb,
                st ? ", framebuffer status " : "", st == 0x8CD6 ? "INCOMPLETE_ATTACHMENT" : st == 0x8CD7 ? "MISSING_ATTACHMENT" : st == 0x8CD9 ? "INCOMPLETE_DIMENSIONS" :
                st == 0x8CDD ? "UNSUPPORTED" : st == 0x8D56 ? "INCOMPLETE_MULTISAMPLE" : st == 0x8CD5 ? "complete" : st ? "other" : "");
}
static unsigned w_glGetError(void)
{
    unsigned e = t_err_held;
    if (e) { t_err_held = 0; return e; }
    return r_glGetError ? r_glGetError() : 0;
}

static void (*r_glDrawElements)(unsigned, int, unsigned, const void *);
static void w_glDrawElements(unsigned mode, int count, unsigned type, const void *idx)
{ if (g_tbo_any) tbo_draw(); t_diag_draws++; atomic_fetch_add_explicit(&g_diag_draws_all, 1, memory_order_relaxed); if (t_layer_draws >= 0) t_layer_draws++; bt_draw(mode, count); CUBE_PRE(mode, count); bool c = cap_here(); if (c) cap_pre(); { bool zf = zb_pre(); r_glDrawElements(mode, count, type, idx); zb_post(zf); } CUBE_POST(); if (c) cap_post(mode, count); else err_check("a draw"); }
static void (*r_glDrawElementsInstanced)(unsigned, int, unsigned, const void *, int);
static void w_glDrawElementsInstanced(unsigned mode, int count, unsigned type, const void *idx, int inst)
{ if (g_tbo_any) tbo_draw(); t_diag_draws++; atomic_fetch_add_explicit(&g_diag_draws_all, 1, memory_order_relaxed); if (t_layer_draws >= 0) t_layer_draws++; bt_draw(mode, count); CUBE_PRE(mode, count); bool c = cap_here(); if (c) cap_pre(); { bool zf = zb_pre(); r_glDrawElementsInstanced(mode, count, type, idx, inst); zb_post(zf); } CUBE_POST(); if (c) cap_post(mode, count); else err_check("a draw"); }
static void (*r_glDrawRangeElements)(unsigned, unsigned, unsigned, int, unsigned, const void *);
static void w_glDrawRangeElements(unsigned mode, unsigned s, unsigned e, int count, unsigned type, const void *idx)
{ if (g_tbo_any) tbo_draw(); t_diag_draws++; atomic_fetch_add_explicit(&g_diag_draws_all, 1, memory_order_relaxed); if (t_layer_draws >= 0) t_layer_draws++; bt_draw(mode, count); CUBE_PRE(mode, count); bool c = cap_here(); if (c) cap_pre(); { bool zf = zb_pre(); r_glDrawRangeElements(mode, s, e, count, type, idx); zb_post(zf); } CUBE_POST(); if (c) cap_post(mode, count); else err_check("a draw"); }
static void (*r_glDrawArrays)(unsigned, int, int);
static void w_glDrawArrays(unsigned mode, int first, int count)
{ if (g_tbo_any) tbo_draw(); t_diag_draws++; atomic_fetch_add_explicit(&g_diag_draws_all, 1, memory_order_relaxed); if (t_layer_draws >= 0) t_layer_draws++; bt_draw(mode, count); CUBE_PRE(mode, count); bool c = cap_here(); if (c) cap_pre(); { bool zf = zb_pre(); r_glDrawArrays(mode, first, count); zb_post(zf); } CUBE_POST(); if (c) cap_post(mode, count); else err_check("a draw"); }
static void (*r_glDrawArraysInstanced)(unsigned, int, int, int);
static void w_glDrawArraysInstanced(unsigned mode, int first, int count, int inst)
{ if (g_tbo_any) tbo_draw(); t_diag_draws++; atomic_fetch_add_explicit(&g_diag_draws_all, 1, memory_order_relaxed); if (t_layer_draws >= 0) t_layer_draws++; bt_draw(mode, count); CUBE_PRE(mode, count); bool c = cap_here(); if (c) cap_pre(); { bool zf = zb_pre(); r_glDrawArraysInstanced(mode, first, count, inst); zb_post(zf); } CUBE_POST(); if (c) cap_post(mode, count); else err_check("a draw"); }
static void (*r_glClear)(unsigned);
static void w_glClear(unsigned mask) { r_glClear(mask); cap_dirty(); if (!cap_here()) err_check("glClear"); }

/* at each swap: a capture starts with the frame after a listed one's predecessor, and ends at that frame's swap */
static void diag_summary(unsigned long n)
{
    tl_log_line("gldiag: by swap #%lu: mipmap generations %d (arrays %d), texture arrays/3D made %d, sub-image uploads %d (compressed %d), copies %d (into arrays %d, failed %d), render-to-layer %d",
                n, atomic_load(&g_diag_mips), atomic_load(&g_diag_mips_arr), atomic_load(&g_diag_arr_storage), atomic_load(&g_diag_arr_sub), atomic_load(&g_diag_arr_csub),
                atomic_load(&g_diag_copy_n), atomic_load(&g_diag_arr_copy_in), atomic_load(&g_diag_copy_fail), atomic_load(&g_diag_arr_layer));
    tl_log_line("gldiag: by swap #%lu: blend calls: BlendFunc %d, BlendFuncSeparate %d, indexed %d + %d; with both factors ZERO %d", n,
                atomic_load(&g_diag_blend_calls[0]), atomic_load(&g_diag_blend_calls[1]), atomic_load(&g_diag_blend_calls[2]),
                atomic_load(&g_diag_blend_calls[3]), atomic_load(&g_diag_blend_zero));
}
static void diag_swap(unsigned long n)
{
    if (!diag_on()) return;
    if (!cap_here()) err_check("the end of a frame");
    unsigned long drew = t_diag_draws; t_diag_draws = 0;
    unsigned long drew_all = atomic_exchange(&g_diag_draws_all, 0);
    if (n == 100 || n == 600 || n == 1500) tl_log_line("gldiag: swap #%lu: %lu draws by the swapping thread, %lu by all threads, in the last frame", n, drew, drew_all);
#if defined(__aarch64__)
    if (n == 300) {   /* FPCR.FZ (bit 24) flushes subnormals on the CPU; Android starts threads with it off */
        unsigned long fpcr; __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        tl_log_line("gldiag: GL thread fpcr %#lx (FZ %lu, FZ16 %lu, DN %lu)", fpcr, (fpcr >> 24) & 1, (fpcr >> 19) & 1, (fpcr >> 25) & 1);
    }
#endif
    if (cap_here()) {
        cap_finish();
        if (C.next >= CAP_N) { diag_summary(n); atomic_store(&g_diag_done, true); tl_log_line("gldiag: done"); return; }
    }
    /* the next capture: the first frame, from the listed swap on, of a thread that drew a scene's worth in its last one */
    if (!C.active && C.next < CAP_N && n + 1 >= k_cap_frames[C.next] && drew >= 8) {
        C.k = (int)++C.next; C.thr = pthread_self(); C.chosen = C.dirty = C.tracked = C.failed = C.ms = false;
        C.tmp_fb = 0; C.ndraw = C.lines = 0; C.active = true;
        tl_log_line("gldiag: cap%d: capturing the next frame of a thread that drew %lu times in its last one", C.k, drew);
    }
    if (n == 600) diag_summary(n);
    if (n >= 20000) { diag_summary(n); atomic_store(&g_diag_done, true); tl_log_line("gldiag: done (not every capture found a scene)"); }
}
