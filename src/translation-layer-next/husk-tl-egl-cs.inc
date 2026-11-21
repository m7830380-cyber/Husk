/* Compute shaders on ANGLE's Metal back end. ANGLE takes a 3.1 context's compute shaders but cannot compile them for Metal (its
 * max work group size is 0), so the program does not link, and an engine that checks quits there (Unreal does, at its first
 * buffer clear). A compute shader that fails to compile reads back as compiled, a program made of one that fails to link reads
 * back as linked, and dispatching it does nothing: whatever it would have written stays as it was. Programs that really link
 * are left alone. TL_GL_STRICT_COMPUTE=1: off. */

#define CS_FAKE_MAX 512
static unsigned g_cs_fake[CS_FAKE_MAX];
static atomic_int g_cs_nfake;
static unsigned g_cs_cur;

static bool cs_off(void)
{
    static int off = -1;
    if (off < 0) off = getenv("TL_GL_STRICT_COMPUTE") ? 1 : 0;
    return off;
}
static bool cs_fake(unsigned prog)
{
    if (!prog) return false;
    int n = atomic_load(&g_cs_nfake); if (n > CS_FAKE_MAX) n = CS_FAKE_MAX;
    for (int i = 0; i < n; i++) if (g_cs_fake[i] == prog) return true;
    return false;
}
static void (*cs_real_getsh)(unsigned, unsigned, int *);
static bool cs_is_compute_shader(unsigned sh)
{
    if (!cs_real_getsh && a_eglGetProcAddress) cs_real_getsh = a_eglGetProcAddress("glGetShaderiv");
    if (!cs_real_getsh) return false;
    int type = 0; cs_real_getsh(sh, 0x8B4F /* GL_SHADER_TYPE */, &type);
    return type == 0x91B9 /* GL_COMPUTE_SHADER */;
}

/* called by glLinkProgram after the real link */
static void cs_linked(unsigned prog)
{
    if (cs_off() || cs_fake(prog) || !a_eglGetProcAddress) return;
    static void (*piv)(unsigned, unsigned, int *), (*getatt)(unsigned, int, int *, unsigned *);
    if (!piv) { piv = a_eglGetProcAddress("glGetProgramiv"); getatt = a_eglGetProcAddress("glGetAttachedShaders"); }
    if (!piv || !getatt) return;
    int ok = 1; piv(prog, 0x8B82 /* GL_LINK_STATUS */, &ok);
    if (ok) return;
    unsigned sh[8]; int n = 0; getatt(prog, 8, &n, sh);
    bool compute = false;
    for (int i = 0; i < n; i++) if (cs_is_compute_shader(sh[i])) compute = true;
    if (!compute) return;
    int slot = atomic_fetch_add(&g_cs_nfake, 1);
    if (slot >= CS_FAKE_MAX) { atomic_fetch_sub(&g_cs_nfake, 1); return; }
    g_cs_fake[slot] = prog;
    static atomic_int told;
    if (!atomic_exchange(&told, 1))
        tl_log_line("gl: ANGLE cannot run compute shaders on Metal: program %u (and any like it) reads back as linked and its dispatches do nothing (TL_GL_STRICT_COMPUTE=1: off)", prog);
}

static void (*r_glGetShaderiv)(unsigned, unsigned, int *);
static void w_glGetShaderiv(unsigned sh, unsigned pname, int *v)
{
    r_glGetShaderiv(sh, pname, v);
    if (cs_off() || !v) return;
    if (pname == 0x8B81 /* GL_COMPILE_STATUS */ && !*v && cs_is_compute_shader(sh)) *v = 1;
    else if (pname == 0x8B84 /* GL_INFO_LOG_LENGTH */ && *v && cs_is_compute_shader(sh)) {
        int ok = 1; r_glGetShaderiv(sh, 0x8B81, &ok);
        if (!ok) *v = 0;
    }
}

static void (*r_glGetProgramiv)(unsigned, unsigned, int *);
static void w_glGetProgramiv(unsigned prog, unsigned pname, int *v)
{
    if (!v || !cs_fake(prog)) { r_glGetProgramiv(prog, pname, v); return; }
    switch (pname) {
    case 0x8B82: /* GL_LINK_STATUS */ case 0x8B83: /* GL_VALIDATE_STATUS */ *v = 1; return;
    case 0x8B80: /* GL_DELETE_STATUS */ *v = 0; return;
    case 0x8B84: /* GL_INFO_LOG_LENGTH */ case 0x8B86: /* GL_ACTIVE_UNIFORMS */ case 0x8B87: /* GL_ACTIVE_UNIFORM_MAX_LENGTH */
    case 0x8B89: /* GL_ACTIVE_ATTRIBUTES */ case 0x8B8A: case 0x8A36: /* GL_ACTIVE_UNIFORM_BLOCKS */ case 0x8A35:
    case 0x8741: /* GL_PROGRAM_BINARY_LENGTH */ case 0x92D9: /* GL_ACTIVE_ATOMIC_COUNTER_BUFFERS */ *v = 0; return;
    case 0x8267: /* GL_COMPUTE_WORK_GROUP_SIZE */ v[0] = v[1] = v[2] = 1; return;
    default: *v = 0; r_glGetProgramiv(prog, pname, v); return;     /* an error leaves the 0, never what the caller had there */
    }
}

/* glGetProgramResourceLocation on such a program: nothing active (the other interface queries check in husk-tl-egl-es31.inc) */
static int (*r_glGetProgramResourceLocation)(unsigned, unsigned, const char *);
static int w_glGetProgramResourceLocation(unsigned prog, unsigned iface, const char *name)
{
    return cs_fake(prog) ? -1 : r_glGetProgramResourceLocation(prog, iface, name);
}

static void (*r_glGetProgramInfoLog)(unsigned, int, int *, char *);
static void w_glGetProgramInfoLog(unsigned prog, int size, int *len, char *log)
{
    if (!cs_fake(prog)) { r_glGetProgramInfoLog(prog, size, len, log); return; }
    if (len) *len = 0;
    if (log && size > 0) log[0] = 0;
}

static void (*r_glDispatchCompute)(unsigned, unsigned, unsigned);
static void w_glDispatchCompute(unsigned x, unsigned y, unsigned z) { if (!g_cs_cur) r_glDispatchCompute(x, y, z); }
static void (*r_glDispatchComputeIndirect)(intptr_t);
static void w_glDispatchComputeIndirect(intptr_t off) { if (!g_cs_cur) r_glDispatchComputeIndirect(off); }
