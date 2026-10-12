// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host libGLESv2 (+ a pretend extension): records args.
#include <stdint.h>
extern "C" {
double fake_gl_d[8]; long fake_gl_i[16]; const void* fake_gl_p;
void glUniform4f(int loc, float a, float b, float c, float d) { fake_gl_i[0] = loc; fake_gl_d[0] = a; fake_gl_d[1] = b; fake_gl_d[2] = c; fake_gl_d[3] = d; }
void glMatrixFrustumEXT(unsigned m, double a, double b, double c, double d, double e, double f) { fake_gl_i[8] = m; double v[] = {a, b, c, d, e, f}; for (int i = 0; i < 6; i++) fake_gl_d[i] = v[i]; }
void glTexImage2D(unsigned t, int l, int ifmt, int w, int h, int b, unsigned f, unsigned ty, const void* px) {
  long v[] = {t, l, ifmt, w, h, b, f, ty}; for (int i = 0; i < 8; i++) fake_gl_i[i] = v[i]; fake_gl_p = px;
}
void glUniformMatrix4fv(int loc, int cnt, unsigned char tr, const float* m) { fake_gl_i[0] = loc; fake_gl_i[1] = cnt; fake_gl_i[2] = tr; fake_gl_d[0] = m[15]; }
const unsigned char* glGetString(unsigned n) { return n == 0x1F02 ? (const unsigned char*)"FakeGL 3.2" : nullptr; }
void glGetInteger64v(unsigned, long* p) { *p = 0x123456789abcl; }
void glVertexAttribPointer(unsigned i, int s, unsigned t, unsigned char n, int st, const void* p) { fake_gl_i[0] = i; fake_gl_i[1] = s; fake_gl_i[2] = t; fake_gl_i[3] = n; fake_gl_i[4] = st; fake_gl_p = p; }
typedef void (*Dbg)(unsigned, unsigned, unsigned, unsigned, int, const char*, const void*);
static Dbg dcb; static const void* dud;
void glDebugMessageCallback(Dbg cb, const void* u) { dcb = cb; dud = u; }
void fake_gl_fire_debug(const char* msg) { dcb(1, 2, 3, 4, 5, msg, dud); }
}
