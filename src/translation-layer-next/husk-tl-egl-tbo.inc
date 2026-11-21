/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Texture buffers (GL_EXT_texture_buffer, core in ES 3.2) where ANGLE has none: its Metal backend does not implement them, and an
 * engine that is told 3.1 (Unreal's ES 3.1 renderer) declares samplerBuffer in its vertex shaders and quits when they do not compile.
 *
 * A buffer texture here is a 2D texture TBO_W texels wide, as many rows as the buffer needs, of the same internal format:
 *  - shaders: samplerBuffer / isamplerBuffer / usamplerBuffer become sampler2D / isampler2D / usampler2D, and texelFetch(s, i) on one
 *    becomes tl_tbFetch(s, i), which fetches texel (i % TBO_W, i / TBO_W);
 *  - the GL_TEXTURE_BUFFER buffer target is the copy-write target for the length of each call that names it;
 *  - glTexBuffer(Range) records which buffer (and range) a texture shows; the texture is filled from that buffer on the GPU
 *    (a pixel-unpack copy) before a draw that samples it, whenever the buffer was written since;
 *  - binding a texture to GL_TEXTURE_BUFFER on a unit is remembered, and at a draw whose program samples a buffer texture on that
 *    unit it is put on the unit's 2D target (with no sampler object, whose filtering could make it incomplete); the game's own 2D
 *    texture and sampler go back on the unit at the next draw that does not need it.
 */
#define TBO_TARGET 0x8C2Au
#define TBO_W 4096
#define TBO_W_SHIFT 12
#define TBO_MAX_ROWS 16384
#define TBO_UNITS 64

static bool g_es31_shim;           /* defined with the rest of the shim (husk-tl-egl-es31.inc) */
static bool tbo_ident(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static void (*q_BindTexture)(unsigned, unsigned), (*q_ActiveTexture)(unsigned), (*q_BindBuffer)(unsigned, unsigned),
    (*q_TexImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *),
    (*q_TexSubImage2D)(unsigned, int, int, int, int, int, unsigned, unsigned, const void *),
    (*q_TexParameteri)(unsigned, unsigned, int), (*q_PixelStorei)(unsigned, int), (*q_GetBufferParameteriv)(unsigned, unsigned, int *),
    (*q_GetUniformiv)(unsigned, int, int *), (*q_BindSampler)(unsigned, unsigned);
static int (*q_GetUniformLocation)(unsigned, const char *);
static const unsigned char *(*q_GetString)(unsigned);

static bool tbo_init(void)
{
    static atomic_int ok = -1;
    int v = atomic_load(&ok);
    if (v >= 0) return v;
    if (!a_eglGetProcAddress || !a_glGetIntegerv) return false;
    q_BindTexture = a_eglGetProcAddress("glBindTexture"); q_ActiveTexture = a_eglGetProcAddress("glActiveTexture");
    q_BindBuffer = a_eglGetProcAddress("glBindBuffer"); q_TexImage2D = a_eglGetProcAddress("glTexImage2D");
    q_TexSubImage2D = a_eglGetProcAddress("glTexSubImage2D"); q_TexParameteri = a_eglGetProcAddress("glTexParameteri");
    q_PixelStorei = a_eglGetProcAddress("glPixelStorei"); q_GetBufferParameteriv = a_eglGetProcAddress("glGetBufferParameteriv");
    q_GetUniformiv = a_eglGetProcAddress("glGetUniformiv"); q_BindSampler = a_eglGetProcAddress("glBindSampler");
    q_GetUniformLocation = a_eglGetProcAddress("glGetUniformLocation"); q_GetString = a_eglGetProcAddress("glGetString");
    v = q_BindTexture && q_ActiveTexture && q_BindBuffer && q_TexImage2D && q_TexSubImage2D && q_TexParameteri && q_PixelStorei &&
        q_GetBufferParameteriv && q_GetUniformiv && q_BindSampler && q_GetUniformLocation && q_GetString;
    atomic_store(&ok, v);
    return v;
}

/* emulated when the game was told 3.1 (the shim or a real 3.1 context) and the context has no texture buffers of its own */
static bool tbo_on(void)
{
    static atomic_int on = -1;
    if (!g_es31_shim && !g_es31_native) return false;
    int v = atomic_load(&on);
    if (v >= 0) return v;
    if (!tbo_init()) return false;
    const char *ext = (const char *)q_GetString(0x1F03 /* GL_EXTENSIONS */);
    if (!ext) return false;                                    /* no context yet: ask again later */
    v = !strstr(ext, "GL_EXT_texture_buffer") && !strstr(ext, "GL_OES_texture_buffer");
    atomic_store(&on, v);
    if (v) tl_log_line("gl: ANGLE has no texture buffers: they are emulated with 2D textures %d texels wide", TBO_W);
    return v;
}

/* ---- formats a buffer texture may have (EXT_texture_buffer's table) */
typedef struct { unsigned ifmt, fmt, type, bpp; } tbo_fmt;
static const tbo_fmt k_tbo_fmt[] = {
    { 0x8229, 0x1903, 0x1401, 1 }, { 0x822D, 0x1903, 0x140B, 2 }, { 0x822E, 0x1903, 0x1406, 4 },                     /* R8 R16F R32F */
    { 0x8231, 0x8D94, 0x1400, 1 }, { 0x8232, 0x8D94, 0x1401, 1 }, { 0x8233, 0x8D94, 0x1402, 2 }, { 0x8234, 0x8D94, 0x1403, 2 },
    { 0x8235, 0x8D94, 0x1404, 4 }, { 0x8236, 0x8D94, 0x1405, 4 },                                                 /* R8I..R32UI */
    { 0x822B, 0x8227, 0x1401, 2 }, { 0x822F, 0x8227, 0x140B, 4 }, { 0x8230, 0x8227, 0x1406, 8 },                     /* RG8 RG16F RG32F */
    { 0x8237, 0x8228, 0x1400, 2 }, { 0x8238, 0x8228, 0x1401, 2 }, { 0x8239, 0x8228, 0x1402, 4 }, { 0x823A, 0x8228, 0x1403, 4 },
    { 0x823B, 0x8228, 0x1404, 8 }, { 0x823C, 0x8228, 0x1405, 8 },                                                 /* RG8I..RG32UI */
    { 0x8815, 0x1907, 0x1406, 12 }, { 0x8D83, 0x8D98, 0x1404, 12 }, { 0x8D71, 0x8D98, 0x1405, 12 },                 /* RGB32F/I/UI */
    { 0x8058, 0x1908, 0x1401, 4 }, { 0x881A, 0x1908, 0x140B, 8 }, { 0x8814, 0x1908, 0x1406, 16 },                    /* RGBA8 16F 32F */
    { 0x8D8E, 0x8D99, 0x1400, 4 }, { 0x8D7C, 0x8D99, 0x1401, 4 }, { 0x8D88, 0x8D99, 0x1402, 8 }, { 0x8D76, 0x8D99, 0x1403, 8 },
    { 0x8D82, 0x8D99, 0x1404, 16 }, { 0x8D70, 0x8D99, 0x1405, 16 },                                               /* RGBA8I..RGBA32UI */
};
static const tbo_fmt *tbo_fmt_of(unsigned ifmt)
{
    for (size_t i = 0; i < sizeof(k_tbo_fmt) / sizeof(k_tbo_fmt[0]); i++) if (k_tbo_fmt[i].ifmt == ifmt) return &k_tbo_fmt[i];
    return NULL;
}

/* ---- tables (shared by the contexts of a share group; names are global to it) */
#define TBO_TOMB 0xFFFFFFFFu
#define TBO_NTEX 2048
#define TBO_NBUF 2048
#define TBO_NSH 2048
#define TBO_NPROG 2048
#define TBO_NATT 4096
#define TBO_NAMES 8
typedef struct { unsigned tex, buf; const tbo_fmt *f; long off, size; int rows; unsigned gen; } tbo_tex;
static tbo_tex g_tbt[TBO_NTEX];
static struct { unsigned buf, gen; } g_tbb[TBO_NBUF];
static struct { unsigned sh; int n; char names[TBO_NAMES][40]; } g_tbs[TBO_NSH];
static struct { unsigned prog; int n; int locs[TBO_NAMES]; } g_tbp[TBO_NPROG];
static struct { unsigned prog, sh; } g_tba[TBO_NATT];
static int g_tba_n;
static atomic_uint g_tbo_gen_all = 1;   /* bumped when a write could not be pinned to one buffer */
static pthread_mutex_t g_tbo_mu = PTHREAD_MUTEX_INITIALIZER;

/* slot of key in a table of n entries whose key is the first unsigned of each stride-byte entry (open addressing, tombstones) */
static int tbo_slot(void *table, size_t stride, int n, unsigned key, bool create)
{
    if (!key || key == TBO_TOMB) return -1;
    int tomb = -1;
    for (int p = 0, h = (int)((key * 2654435761u) % (unsigned)n); p < n; p++, h = (h + 1) % n) {
        unsigned *k = (unsigned *)((char *)table + (size_t)h * stride);
        if (*k == key) return h;
        if (*k == TBO_TOMB) { if (tomb < 0) tomb = h; continue; }
        if (*k == 0) {
            if (!create) return -1;
            int s = tomb >= 0 ? tomb : h;
            memset((char *)table + (size_t)s * stride, 0, stride);
            *(unsigned *)((char *)table + (size_t)s * stride) = key;
            return s;
        }
    }
    if (create && tomb >= 0) { memset((char *)table + (size_t)tomb * stride, 0, stride); *(unsigned *)((char *)table + (size_t)tomb * stride) = key; }
    return create ? tomb : -1;
}
#define TBO_SLOT(tab, key, create) tbo_slot((tab), sizeof((tab)[0]), (int)(sizeof(tab) / sizeof((tab)[0])), (key), (create))

/* ---- per thread (a context is current on one thread): the game's view of the units */
static _Thread_local unsigned t_tbo_unit;                 /* active unit */
static _Thread_local unsigned t_tbo_tb[TBO_UNITS];        /* texture bound to GL_TEXTURE_BUFFER */
static _Thread_local unsigned t_tbo_2d[TBO_UNITS];        /* the game's 2D texture */
static _Thread_local unsigned t_tbo_samp[TBO_UNITS];      /* the game's sampler object */
static _Thread_local unsigned t_tbo_real[TBO_UNITS];      /* the 2D texture really there while the unit is taken */
static _Thread_local uint64_t t_tbo_taken_tex, t_tbo_taken_samp;
static _Thread_local unsigned t_tbo_buf;                  /* buffer bound to GL_TEXTURE_BUFFER */

static void tbo_dirty_buf(unsigned buf)
{
    if (!buf || !g_tbo_any) return;
    pthread_mutex_lock(&g_tbo_mu);
    int s = TBO_SLOT(g_tbb, buf, false);
    if (s >= 0) g_tbb[s].gen++;
    pthread_mutex_unlock(&g_tbo_mu);
}
/* the buffer a target has bound, for the write just made through it */
static void tbo_written(unsigned target)
{
    if (!g_tbo_any) return;
    if (target == TBO_TARGET) { tbo_dirty_buf(t_tbo_buf); return; }
    unsigned q = target == 0x8892 ? 0x8894 : target == 0x8893 ? 0x8895 : target == 0x8A11 ? 0x8A28 : target == 0x8F36 ? 0x8F36 :
                 target == 0x8F37 ? 0x8F37 : target == 0x88EB ? 0x88ED : target == 0x88EC ? 0x88EF : target == 0x8C8E ? 0x8C8F :
                 target == 0x90D2 ? 0x90D3 : target == 0x8F3F ? 0x8F43 : target == 0x90EE ? 0x90EF : 0;
    int b = 0;
    if (q) a_glGetIntegerv(q, &b);
    if (b) tbo_dirty_buf((unsigned)b); else if (!q) atomic_fetch_add(&g_tbo_gen_all, 1);
}

/* the calls that name GL_TEXTURE_BUFFER do their work on the copy-write target, with the game's buffer bound there for the call */
static unsigned tbo_alias_begin(unsigned alias, int *saved) { *saved = 0; a_glGetIntegerv(alias, saved); q_BindBuffer(alias, t_tbo_buf); return alias; }
static void tbo_alias_end(unsigned alias, int saved) { q_BindBuffer(alias, (unsigned)saved); }
#define TBO_ALIASED(t) ((t) == TBO_TARGET && tbo_on() && tbo_init())

/* ---- shaders */
static const char k_tbo_helpers[] =
    "highp vec4 tl_tbFetch(highp sampler2D s, highp int i) { return texelFetch(s, ivec2(i & 4095, i >> 12), 0); }\n"
    "highp ivec4 tl_tbFetch(highp isampler2D s, highp int i) { return texelFetch(s, ivec2(i & 4095, i >> 12), 0); }\n"
    "highp uvec4 tl_tbFetch(highp usampler2D s, highp int i) { return texelFetch(s, ivec2(i & 4095, i >> 12), 0); }\n"
    "highp int tl_tbSizeOf(highp sampler2D s) { return textureSize(s, 0).x * textureSize(s, 0).y; }\n"
    "highp int tl_tbSizeOf(highp isampler2D s) { return textureSize(s, 0).x * textureSize(s, 0).y; }\n"
    "highp int tl_tbSizeOf(highp usampler2D s) { return textureSize(s, 0).x * textureSize(s, 0).y; }\n";

static bool tbo_is_name(const char *p, char (*names)[40], int nn)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '(') return false;
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    const char *e = p; while (tbo_ident((unsigned char)*e)) e++;
    for (int i = 0; i < nn; i++) if ((size_t)(e - p) == strlen(names[i]) && !strncmp(p, names[i], (size_t)(e - p))) return true;
    return false;
}

/* the shader with its buffer samplers made 2D ones (malloc'd), or NULL when it has none; their names go to names[0..*nn) */
static char *tbo_rewrite(const char *src, char (*names)[40], int *nn)
{
    *nn = 0;
    if (!strstr(src, "samplerBuffer")) return NULL;
    char *s = strdup(src);
    if (!s) return NULL;
    char *first = NULL;
    for (char *p = s; (p = strstr(p, "samplerBuffer")); ) {
        char *b = p;
        if (b > s && (b[-1] == 'i' || b[-1] == 'u')) b--;
        char *after = p + 13;
        if ((b > s && tbo_ident((unsigned char)b[-1])) || tbo_ident((unsigned char)*after)) { p = after; continue; }
        if (!first) first = b;
        const char *q = after; while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
        const char *e = q; while (tbo_ident((unsigned char)*e)) e++;
        if (e > q && e - q < 40 && *nn < TBO_NAMES) { memcpy(names[*nn], q, (size_t)(e - q)); names[*nn][e - q] = 0; (*nn)++; }
        memcpy(p, "sampler2D    ", 13);           /* same length: [iu]samplerBuffer -> [iu]sampler2D */
        p = after;
    }
    if (!first) { free(s); return NULL; }
    for (char *p = s; (p = strstr(p, "texelFetch")); p += 10)
        if ((p == s || !tbo_ident((unsigned char)p[-1])) && tbo_is_name(p + 10, names, *nn)) memcpy(p, "tl_tbFetch", 10);
    for (char *p = s; (p = strstr(p, "textureSize")); p += 11)
        if ((p == s || !tbo_ident((unsigned char)p[-1])) && tbo_is_name(p + 11, names, *nn)) memcpy(p, "tl_tbSizeOf", 11);
    /* ANGLE has neither extension: a directive that requires one would fail the shader */
    for (char *p = s; (p = strstr(p, "#extension")); p += 10) {
        const char *q = p + 10; while (*q == ' ' || *q == '\t') q++;
        if (!strncmp(q, "GL_EXT_texture_buffer", 21) || !strncmp(q, "GL_OES_texture_buffer", 21)) memcpy(p, "//", 2);
    }
    /* the helpers go at the start of the line that first declares one: global scope, past #version and the #extension lines */
    char *line = first;
    while (line > s && line[-1] != '\n') line--;
    size_t pre = (size_t)(line - s), rest = strlen(line), hl = sizeof(k_tbo_helpers) - 1;
    char *out = malloc(pre + hl + rest + 1);
    if (!out) { free(s); return NULL; }
    memcpy(out, s, pre); memcpy(out + pre, k_tbo_helpers, hl); memcpy(out + pre + hl, line, rest + 1);
    free(s);
    return out;
}

/* glShaderSource: the source to give ANGLE (malloc'd), or NULL to keep it; the shader's buffer samplers are noted */
static char *tbo_shader_source(unsigned sh, const char *src)
{
    if (!tbo_on()) return NULL;
    char names[TBO_NAMES][40]; int nn = 0;
    char *out = tbo_rewrite(src, names, &nn);
    pthread_mutex_lock(&g_tbo_mu);
    int s = TBO_SLOT(g_tbs, sh, nn > 0);
    if (s >= 0) {
        if (nn > 0) { g_tbs[s].n = nn; memcpy(g_tbs[s].names, names, sizeof(names)); }
        else g_tbs[s].sh = TBO_TOMB;
    }
    pthread_mutex_unlock(&g_tbo_mu);
    static atomic_int said;
    if (out && atomic_fetch_add(&said, 1) < 3) tl_log_line("gl: shader %u samples %d buffer texture(s) (first %s): made 2D textures", sh, nn, names[0]);
    return out;
}
static void tbo_attach(unsigned prog, unsigned sh)
{
    if (!tbo_on()) return;
    pthread_mutex_lock(&g_tbo_mu);
    if (TBO_SLOT(g_tbs, sh, false) >= 0) { g_tba[g_tba_n % TBO_NATT].prog = prog; g_tba[g_tba_n % TBO_NATT].sh = sh; g_tba_n++; }
    pthread_mutex_unlock(&g_tbo_mu);
}
/* after a link: where the program's buffer samplers are */
static void tbo_linked(unsigned prog)
{
    if (!tbo_on() || !tbo_init()) return;
    int locs[TBO_NAMES]; int n = 0;
    pthread_mutex_lock(&g_tbo_mu);
    int total = g_tba_n < TBO_NATT ? g_tba_n : TBO_NATT;
    for (int a = 0; a < total; a++) {
        if (g_tba[a].prog != prog) continue;
        int s = TBO_SLOT(g_tbs, g_tba[a].sh, false);
        if (s < 0) continue;
        for (int i = 0; i < g_tbs[s].n && n < TBO_NAMES; i++) {
            int loc = q_GetUniformLocation(prog, g_tbs[s].names[i]);
            bool dup = false;
            for (int j = 0; j < n; j++) dup |= locs[j] == loc;
            if (loc >= 0 && !dup) locs[n++] = loc;
        }
    }
    int p = TBO_SLOT(g_tbp, prog, n > 0);
    if (p >= 0) {
        if (n > 0) { g_tbp[p].n = n; memcpy(g_tbp[p].locs, locs, sizeof(int) * (size_t)n); }
        else g_tbp[p].prog = TBO_TOMB;
    }
    pthread_mutex_unlock(&g_tbo_mu);
}

/* ---- texture side */
static void (*r_glBindTexture)(unsigned, unsigned);
static void w_glBindTexture(unsigned target, unsigned tex)
{
    unsigned u = t_tbo_unit;
    if (target == TBO_TARGET && tbo_on()) { if (u < TBO_UNITS) t_tbo_tb[u] = tex; return; }
    if (target == 0x0DE1 /* GL_TEXTURE_2D */ && u < TBO_UNITS) { t_tbo_2d[u] = tex; t_tbo_taken_tex &= ~(1ull << u); }
    r_glBindTexture(target, tex);
}
static void (*r_glActiveTexture)(unsigned);
static void w_glActiveTexture(unsigned unit) { t_tbo_unit = unit - 0x84C0u; r_glActiveTexture(unit); }
static void (*r_glBindSampler)(unsigned, unsigned);
static void w_glBindSampler(unsigned unit, unsigned s)
{
    if (unit < TBO_UNITS) { t_tbo_samp[unit] = s; t_tbo_taken_samp &= ~(1ull << unit); }
    r_glBindSampler(unit, s);
}
static void tbo_forget_tex(unsigned tex)
{
    if (!tex) return;
    for (int u = 0; u < TBO_UNITS; u++) {
        if (t_tbo_tb[u] == tex) t_tbo_tb[u] = 0;
        if (t_tbo_2d[u] == tex) t_tbo_2d[u] = 0;
        if (t_tbo_real[u] == tex) { t_tbo_real[u] = 0; }
    }
    if (!g_tbo_any) return;
    pthread_mutex_lock(&g_tbo_mu);
    int s = TBO_SLOT(g_tbt, tex, false);
    if (s >= 0) g_tbt[s].tex = TBO_TOMB;
    pthread_mutex_unlock(&g_tbo_mu);
}

static void tbo_texbuffer(unsigned target, unsigned ifmt, unsigned buf, long off, long size)
{
    (void)target;
    unsigned u = t_tbo_unit, tex = u < TBO_UNITS ? t_tbo_tb[u] : 0;
    if (!tex || !tbo_init()) return;
    const tbo_fmt *f = tbo_fmt_of(ifmt);
    if (!f) { static atomic_int said; if (atomic_fetch_add(&said, 1) < 4) tl_log_line("gl: buffer texture %u has format %#x, which is not emulated", tex, ifmt); return; }
    pthread_mutex_lock(&g_tbo_mu);
    int s = TBO_SLOT(g_tbt, tex, true);
    if (s >= 0) {
        g_tbt[s].buf = buf; g_tbt[s].f = f; g_tbt[s].off = off; g_tbt[s].size = size; g_tbt[s].gen = 0;
        if (g_tbt[s].rows == 0) g_tbt[s].rows = -1;      /* the texture is not made yet */
        int b = TBO_SLOT(g_tbb, buf, true);
        if (b >= 0 && g_tbb[b].gen == 0) g_tbb[b].gen = 1;
    }
    pthread_mutex_unlock(&g_tbo_mu);
    g_tbo_any = true;
    static atomic_int said;
    if (atomic_fetch_add(&said, 1) < 3) tl_log_line("gl: buffer texture %u shows buffer %u (format %#x, offset %ld, size %ld)", tex, buf, ifmt, off, size);
}
static void (*r_glTexBuffer)(unsigned, unsigned, unsigned);
static void (*r_glTexBufferRange)(unsigned, unsigned, unsigned, long, long);
static void w_glTexBuffer(unsigned t, unsigned ifmt, unsigned buf)
{
    if (tbo_on()) { tbo_texbuffer(t, ifmt, buf, 0, -1); return; }
    if (!r_glTexBuffer && a_eglGetProcAddress) r_glTexBuffer = a_eglGetProcAddress("glTexBufferEXT");
    if (r_glTexBuffer) r_glTexBuffer(t, ifmt, buf);
}
static void w_glTexBufferRange(unsigned t, unsigned ifmt, unsigned buf, long off, long size)
{
    if (tbo_on()) { tbo_texbuffer(t, ifmt, buf, off, size); return; }
    if (!r_glTexBufferRange && a_eglGetProcAddress) r_glTexBufferRange = a_eglGetProcAddress("glTexBufferRangeEXT");
    if (r_glTexBufferRange) r_glTexBufferRange(t, ifmt, buf, off, size);
}

/* fill the texture bound to the active unit's 2D target from its buffer, on the GPU */
static void tbo_upload(tbo_tex *r)
{
    int pbo = 0, al = 4, rl = 0, sp = 0, sr = 0;
    a_glGetIntegerv(0x88EF /* GL_PIXEL_UNPACK_BUFFER_BINDING */, &pbo);
    a_glGetIntegerv(0x0CF5 /* GL_UNPACK_ALIGNMENT */, &al); a_glGetIntegerv(0x0CF2 /* GL_UNPACK_ROW_LENGTH */, &rl);
    a_glGetIntegerv(0x0CF4 /* GL_UNPACK_SKIP_PIXELS */, &sp); a_glGetIntegerv(0x0CF3 /* GL_UNPACK_SKIP_ROWS */, &sr);
    q_BindBuffer(0x88EC /* GL_PIXEL_UNPACK_BUFFER */, r->buf);
    long size = r->size;
    if (size < 0) { int bs = 0; q_GetBufferParameteriv(0x88EC, 0x8764 /* GL_BUFFER_SIZE */, &bs); size = (long)bs - r->off; }
    long n = size > 0 ? size / (long)r->f->bpp : 0;
    if (n > (long)TBO_W * TBO_MAX_ROWS) n = (long)TBO_W * TBO_MAX_ROWS;
    if (n > 0) {
        int rows = (int)((n + TBO_W - 1) / TBO_W);
        q_PixelStorei(0x0CF5, 1); q_PixelStorei(0x0CF2, 0); q_PixelStorei(0x0CF4, 0); q_PixelStorei(0x0CF3, 0);
        if (r->rows < rows) {
            q_BindBuffer(0x88EC, 0);
            q_TexImage2D(0x0DE1, 0, (int)r->f->ifmt, TBO_W, rows, 0, r->f->fmt, r->f->type, NULL);
            q_TexParameteri(0x0DE1, 0x2801 /* MIN_FILTER */, 0x2600 /* NEAREST */); q_TexParameteri(0x0DE1, 0x2800 /* MAG_FILTER */, 0x2600);
            q_TexParameteri(0x0DE1, 0x2802 /* WRAP_S */, 0x812F /* CLAMP_TO_EDGE */); q_TexParameteri(0x0DE1, 0x2803 /* WRAP_T */, 0x812F);
            q_TexParameteri(0x0DE1, 0x813D /* MAX_LEVEL */, 0);
            q_BindBuffer(0x88EC, r->buf);
            r->rows = rows;
        }
        long full = n / TBO_W, rem = n % TBO_W;
        if (full) q_TexSubImage2D(0x0DE1, 0, 0, 0, TBO_W, (int)full, r->f->fmt, r->f->type, (const void *)(uintptr_t)r->off);
        if (rem) q_TexSubImage2D(0x0DE1, 0, 0, (int)full, (int)rem, 1, r->f->fmt, r->f->type, (const void *)(uintptr_t)(r->off + full * TBO_W * (long)r->f->bpp));
        q_PixelStorei(0x0CF5, al); q_PixelStorei(0x0CF2, rl); q_PixelStorei(0x0CF4, sp); q_PixelStorei(0x0CF3, sr);
    }
    q_BindBuffer(0x88EC, (unsigned)pbo);
}

/* before a draw: the buffer textures the program samples go on their units, filled; units taken earlier are given back */
static void tbo_draw(void)
{
    if (!tbo_init()) return;
    int prog = 0;
    a_glGetIntegerv(0x8B8D /* GL_CURRENT_PROGRAM */, &prog);
    int locs[TBO_NAMES], n = 0;
    pthread_mutex_lock(&g_tbo_mu);
    int p = prog ? TBO_SLOT(g_tbp, (unsigned)prog, false) : -1;
    if (p >= 0) { n = g_tbp[p].n; memcpy(locs, g_tbp[p].locs, sizeof(int) * (size_t)n); }
    pthread_mutex_unlock(&g_tbo_mu);
    uint64_t need = 0;
    bool moved = false;
    for (int i = 0; i < n; i++) {
        int unit = -1;
        q_GetUniformiv((unsigned)prog, locs[i], &unit);
        if (unit < 0 || unit >= TBO_UNITS || !t_tbo_tb[unit]) continue;
        unsigned tex = t_tbo_tb[unit];
        uint64_t bit = 1ull << unit;
        need |= bit;
        pthread_mutex_lock(&g_tbo_mu);
        int s = TBO_SLOT(g_tbt, tex, false);
        tbo_tex *r = s >= 0 ? &g_tbt[s] : NULL;
        unsigned gen = 0;
        if (r && r->buf) { int b = TBO_SLOT(g_tbb, r->buf, false); gen = (b >= 0 ? g_tbb[b].gen : 0) + atomic_load(&g_tbo_gen_all) * 0x10000u; }
        bool stale = r && r->buf && r->gen != gen;
        bool bind = !(t_tbo_taken_tex & bit) || t_tbo_real[unit] != tex;
        if (bind || stale) {
            q_ActiveTexture(0x84C0u + (unsigned)unit); moved = true;
            if (bind) { q_BindTexture(0x0DE1, tex); t_tbo_real[unit] = tex; t_tbo_taken_tex |= bit; }
            if (stale) { tbo_upload(r); r->gen = gen; }
        }
        pthread_mutex_unlock(&g_tbo_mu);
        if (t_tbo_samp[unit] && !(t_tbo_taken_samp & bit)) { q_BindSampler((unsigned)unit, 0); t_tbo_taken_samp |= bit; }
    }
    uint64_t back = (t_tbo_taken_tex | t_tbo_taken_samp) & ~need;
    for (int u = 0; back && u < TBO_UNITS; u++) {
        uint64_t bit = 1ull << u;
        if (!(back & bit)) continue;
        back &= ~bit;
        if (t_tbo_taken_tex & bit) { q_ActiveTexture(0x84C0u + (unsigned)u); moved = true; q_BindTexture(0x0DE1, t_tbo_2d[u]); t_tbo_taken_tex &= ~bit; }
        if (t_tbo_taken_samp & bit) { q_BindSampler((unsigned)u, t_tbo_samp[u]); t_tbo_taken_samp &= ~bit; }
    }
    if (moved) q_ActiveTexture(0x84C0u + t_tbo_unit);
}

/* ---- buffer side: the calls that take a target */
static void *(*r_glMapBufferRange)(unsigned, long, long, unsigned);
static void *w_glMapBufferRange(unsigned t, long off, long len, unsigned access)
{
    if (TBO_ALIASED(t)) { int sv; unsigned a = tbo_alias_begin(0x8F37, &sv); void *p = r_glMapBufferRange(a, off, len, access); tbo_alias_end(a, sv); return p; }
    return r_glMapBufferRange(t, off, len, access);
}
static unsigned (*r_glUnmapBuffer)(unsigned);
static unsigned w_glUnmapBuffer(unsigned t)
{
    if (TBO_ALIASED(t)) { int sv; unsigned a = tbo_alias_begin(0x8F37, &sv); unsigned ok = r_glUnmapBuffer(a); tbo_alias_end(a, sv); tbo_dirty_buf(t_tbo_buf); return ok; }
    unsigned ok = r_glUnmapBuffer(t);
    tbo_written(t);
    return ok;
}
static void (*r_glFlushMappedBufferRange)(unsigned, long, long);
static void w_glFlushMappedBufferRange(unsigned t, long off, long len)
{
    if (TBO_ALIASED(t)) { int sv; unsigned a = tbo_alias_begin(0x8F37, &sv); r_glFlushMappedBufferRange(a, off, len); tbo_alias_end(a, sv); tbo_dirty_buf(t_tbo_buf); return; }
    r_glFlushMappedBufferRange(t, off, len);
    tbo_written(t);
}
static void (*r_glCopyBufferSubData)(unsigned, unsigned, long, long, long);
static void w_glCopyBufferSubData(unsigned rt, unsigned wt, long ro, long wo, long n)
{
    if ((rt == TBO_TARGET || wt == TBO_TARGET) && tbo_on() && tbo_init()) {
        unsigned other = rt == TBO_TARGET ? wt : rt, alias = other == 0x8F37 ? 0x8F36u : 0x8F37u;
        int sv; tbo_alias_begin(alias, &sv);
        r_glCopyBufferSubData(rt == TBO_TARGET ? alias : rt, wt == TBO_TARGET ? alias : wt, ro, wo, n);
        tbo_alias_end(alias, sv);
        if (wt == TBO_TARGET) tbo_dirty_buf(t_tbo_buf); else tbo_written(wt);
        return;
    }
    r_glCopyBufferSubData(rt, wt, ro, wo, n);
    tbo_written(wt);
}
static void (*r_glGetBufferParameteriv)(unsigned, unsigned, int *);
static void w_glGetBufferParameteriv(unsigned t, unsigned pname, int *v)
{
    if (TBO_ALIASED(t)) { int sv; unsigned a = tbo_alias_begin(0x8F37, &sv); r_glGetBufferParameteriv(a, pname, v); tbo_alias_end(a, sv); return; }
    r_glGetBufferParameteriv(t, pname, v);
}
static void (*r_glGetBufferParameteri64v)(unsigned, unsigned, int64_t *);
static void w_glGetBufferParameteri64v(unsigned t, unsigned pname, int64_t *v)
{
    if (TBO_ALIASED(t)) { int sv; unsigned a = tbo_alias_begin(0x8F37, &sv); r_glGetBufferParameteri64v(a, pname, v); tbo_alias_end(a, sv); return; }
    r_glGetBufferParameteri64v(t, pname, v);
}
static void (*r_glGetBufferPointerv)(unsigned, unsigned, void **);
static void w_glGetBufferPointerv(unsigned t, unsigned pname, void **v)
{
    if (TBO_ALIASED(t)) { int sv; unsigned a = tbo_alias_begin(0x8F37, &sv); r_glGetBufferPointerv(a, pname, v); tbo_alias_end(a, sv); return; }
    r_glGetBufferPointerv(t, pname, v);
}

/* glGetIntegerv: the texture-buffer state and limits, which ANGLE does not know; true when answered here */
static bool tbo_get_integer(unsigned pname, int *data)
{
    if (!data || !tbo_on()) return false;
    switch (pname) {
    case 0x8C2B: *data = TBO_W * TBO_MAX_ROWS; return true;                 /* GL_MAX_TEXTURE_BUFFER_SIZE */
    case 0x919F: *data = 16; return true;                                    /* GL_TEXTURE_BUFFER_OFFSET_ALIGNMENT */
    case 0x8C2A: *data = (int)t_tbo_buf; return true;                        /* GL_TEXTURE_BUFFER_BINDING */
    case 0x8C2C: *data = t_tbo_unit < TBO_UNITS ? (int)t_tbo_tb[t_tbo_unit] : 0; return true;   /* GL_TEXTURE_BINDING_BUFFER */
    default: return false;
    }
}
