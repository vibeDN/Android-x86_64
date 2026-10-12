// SPDX-License-Identifier: GPL-2.0-or-later
// AAPCS64 variadic readers and guest va_list -> host (x86_64 SysV) va_list conversion.
#include <string.h>
#include "proxy_ext.h"

extern "C" {
void armtr_va_from_regs(armtr_va* v, const armtr_regs* r, unsigned named_int, unsigned named_fp) {
  memset(v, 0, sizeof *v);
  v->regs = r; v->gr = named_int; v->vr = named_fp; v->stack = (const uint64_t*)r->sp;
}
void armtr_va_from_list(armtr_va* v, uint64_t p) {
  struct L { void *stack, *gr_top, *vr_top; int32_t gr_offs, vr_offs; };
  const L* l = (const L*)p;
  memset(v, 0, sizeof *v);
  v->stack = (const uint64_t*)l->stack; v->gr_top = (const char*)l->gr_top; v->vr_top = (const char*)l->vr_top;
  v->gr_offs = l->gr_offs; v->vr_offs = l->vr_offs;
}
uint64_t armtr_va_int(armtr_va* v) {
  if (v->regs) return v->gr < 8 ? v->regs->x[v->gr++] : *v->stack++;
  if (v->gr_offs < 0) { int o = v->gr_offs; v->gr_offs += 8; return *(const uint64_t*)(v->gr_top + o); }
  return *v->stack++;
}
double armtr_va_double(armtr_va* v) {
  double d;
  if (v->regs) { if (v->vr < 8) memcpy(&d, v->regs->v[v->vr++], 8); else memcpy(&d, v->stack++, 8); return d; }
  if (v->vr_offs < 0) { int o = v->vr_offs; v->vr_offs += 16; memcpy(&d, v->vr_top + o, 8); return d; }
  memcpy(&d, v->stack++, 8);
  return d;
}

int armtr_va_to_host_types(armtr_va* g, const char* t, armtr_host_va* out) {
#if defined(__x86_64__)
  size_t n = strlen(t);
  if (n > 32) return -1;
  for (size_t i = 0; i < n; i++) {
    if (t[i] == 'd' || t[i] == 'f') { double d = armtr_va_double(g); memcpy(&out->slot[i], &d, 8); }
    else if (strchr("iulp", t[i])) out->slot[i] = armtr_va_int(g);
    else return -1;
  }
  // Everything lives in the overflow area: gp/fp offsets say the register save area is exhausted.
  struct Tag { unsigned gp, fp; void *overflow, *regsave; };   // SysV x86_64 __va_list_tag
  Tag* tg = (Tag*)(void*)out->ap;
  tg->gp = 48; tg->fp = 176; tg->overflow = out->slot; tg->regsave = nullptr;
  return 0;
#else
  (void)g; (void)t; (void)out; return -1;
#endif
}

int armtr_va_to_host_printf(armtr_va* g, const char* f, armtr_host_va* out) {
  char t[33]; size_t n = 0;
  for (; *f; f++) {
    if (*f != '%') continue;
    f++;
    if (*f == '%') continue;
    while (*f && strchr("-+ #0'", *f)) f++;
    if (*f == '*') { if (n >= 32) return -1; t[n++] = 'i'; f++; } else while (*f >= '0' && *f <= '9') f++;
    if (*f == '.') { f++; if (*f == '*') { if (n >= 32) return -1; t[n++] = 'i'; f++; } else while (*f >= '0' && *f <= '9') f++; }
    bool L = false, ll = false;
    for (; *f && strchr("hlLjzt", *f); f++) { if (*f == 'L') L = true; if (*f == 'l' || *f == 'j' || *f == 'z' || *f == 't') ll = true; }
    if (!*f || n >= 32) return -1;
    switch (*f) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': t[n++] = ll ? 'l' : 'i'; break;
      case 's': case 'p': case 'n': t[n++] = 'p'; break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        if (L) return -1;
        t[n++] = 'd'; break;
      default: return -1;
    }
  }
  t[n] = 0;
  return armtr_va_to_host_types(g, t, out);
}
}
