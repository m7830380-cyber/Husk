#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <stdbool.h>
#define tl_log_line(...) (fprintf(stderr, __VA_ARGS__), fputc('\n', stderr))
static bool es31_ident(unsigned char c) { return isalnum(c) || c == '_'; }
static char *subnormal_rewrite(const char *src, unsigned type)
{
    static int off = -1;
    if (off < 0) { const char *e = getenv("TL_GL_KEEP_SUBNORMAL"); off = e && *e == '1'; }
    if (off || (type != 0x8B30 && type != 0x8B31)) return NULL;
    const char *v = src; while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
    if (strncmp(v, "#version", 8) || atoi(v + 8) < 300) return NULL;
    const char *z = type == 0x8B30 ? "uint(gl_FragCoord.w < 0.0)" : "uint(gl_VertexID < 0)";
    size_t n = strlen(src), cap = n + 1, o = 0; char *out = NULL;
    int depth = 0, found = 0; size_t stmt = 0;                  /* stmt: where the current statement began in src */
    bool line_start = true;
    for (size_t i = 0; i < n;) {
        char c = src[i];
        if (c == '/' && src[i + 1] == '/') { const char *e = strchr(src + i, '\n'); size_t j = e ? (size_t)(e - src) : n; if (out) { memcpy(out + o, src + i, j - i); o += j - i; } i = j; continue; }
        if (c == '/' && src[i + 1] == '*') { const char *e = strstr(src + i + 2, "*/"); size_t j = e ? (size_t)(e - src) + 2 : n; if (out) { memcpy(out + o, src + i, j - i); o += j - i; } i = j; continue; }
        if (line_start && c == '#') { const char *e = strchr(src + i, '\n'); size_t j = e ? (size_t)(e - src) : n; if (out) { memcpy(out + o, src + i, j - i); o += j - i; } i = j; continue; }
        if (c == '\n') line_start = true; else if (c != ' ' && c != '\t' && c != '\r') line_start = false;
        if (c == '{') { depth++; stmt = i + 1; } else if (c == '}') { if (depth) depth--; stmt = i + 1; } else if (c == ';') stmt = i + 1;
        bool num = (c >= '0' && c <= '9') || (c == '.' && src[i + 1] >= '0' && src[i + 1] <= '9');
        if (num && (i == 0 || !es31_ident((unsigned char)src[i - 1]))) {
            size_t j = i; bool flt = false;
            if (c == '0' && (src[j + 1] == 'x' || src[j + 1] == 'X')) { j += 2; while (isxdigit((unsigned char)src[j])) j++; }
            else {
                while (src[j] >= '0' && src[j] <= '9') j++;
                if (src[j] == '.') { flt = true; j++; while (src[j] >= '0' && src[j] <= '9') j++; }
                if ((src[j] == 'e' || src[j] == 'E') && (isdigit((unsigned char)src[j + 1]) || ((src[j + 1] == '-' || src[j + 1] == '+') && isdigit((unsigned char)src[j + 2])))) {
                    flt = true; j += 2; while (src[j] >= '0' && src[j] <= '9') j++;
                }
            }
            size_t tok_end = j;
            if (flt && (src[j] == 'f' || src[j] == 'F')) j++;
            unsigned bits = 0;
            if (flt) {
                double d = strtod(src + i, NULL);
                if (d > 0 && d < 1.17549435082228751e-38) { double m = ldexp(d, 149); bits = m >= 0.5 ? (unsigned)llround(m) : 0; }
            }
            if (bits && bits < 0x800000u && !es31_ident((unsigned char)src[j])) {
                bool konst = depth == 0;
                for (size_t q = stmt; !konst && q + 5 <= i; q++)
                    if (!strncmp(src + q, "const", 5) && (q == 0 || !es31_ident((unsigned char)src[q - 1])) && !es31_ident((unsigned char)src[q + 5])) konst = true;
                char rep[96];
                int rl = konst ? snprintf(rep, sizeof rep, "uintBitsToFloat(%uu)", bits) : snprintf(rep, sizeof rep, "uintBitsToFloat(%uu | %s)", bits, z);
                if (!out) { cap = n + 4096; out = malloc(cap); memcpy(out, src, i); o = i; }
                if (o + (size_t)rl + (n - j) + 1 > cap) { cap = (o + (size_t)rl + (n - j) + 1) * 2; out = realloc(out, cap); }
                memcpy(out + o, rep, (size_t)rl); o += (size_t)rl; found++;
                i = j; continue;
            }
            (void)tok_end;
            if (out) { if (o + (j - i) + (n - j) + 1 > cap) { cap = (o + (j - i) + (n - j) + 1) * 2; out = realloc(out, cap); } memcpy(out + o, src + i, j - i); o += j - i; }
            i = j; continue;
        }
        if (out) { if (o + 2 + (n - i) > cap) { cap = (o + 2 + (n - i)) * 2; out = realloc(out, cap); } out[o++] = c; }
        i++;
    }
    if (!out) return NULL;
    out[o] = 0;
    static atomic_int said;
    if (!atomic_exchange(&said, 1)) tl_log_line("gl: a shader keeps integers in subnormal float constants (%d here), which Metal flushes to 0: they are written as uintBitsToFloat() (TL_GL_KEEP_SUBNORMAL=1: off)", found);
    return out;
}
int main(void) {
  const char *s = "#version 300 es\n#define X 1.4e-45\nconst float k = 1.40129846e-45;\nvec4 ImmCB_0[54];\n// 2.8e-45 comment\nvoid main()\n{\nImmCB_0[35] = vec4(1.40129846e-45,0.0,0.632352948,0.97058773);\n u_xlat9 = 2.94272678e-44; x = 1.17549435e-38; y=3.0e-5f; z = vec2(5.85882888e-42, 0x10);\n}\n";
  char *r = subnormal_rewrite(s, 0x8B30); puts(r ? r : "(null)");
  r = subnormal_rewrite(s, 0x8B31); puts(r ? r : "(null)");
  return 0; }
