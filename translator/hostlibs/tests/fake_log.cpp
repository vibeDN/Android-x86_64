// SPDX-License-Identifier: GPL-2.0-or-later
// Fake host liblog: records the formatted message.
#include <stdarg.h>
#include <stdio.h>
extern "C" {
char fake_log_msg[512]; char fake_log_tag[64]; int fake_log_prio; int fake_log_calls;
static void rec(int p, const char* t, const char* f, va_list ap) {
  fake_log_prio = p; snprintf(fake_log_tag, sizeof fake_log_tag, "%s", t); vsnprintf(fake_log_msg, sizeof fake_log_msg, f, ap); fake_log_calls++;
}
int __android_log_print(int p, const char* t, const char* f, ...) { va_list ap; va_start(ap, f); rec(p, t, f, ap); va_end(ap); return 7; }
int __android_log_vprint(int p, const char* t, const char* f, va_list ap) { rec(p, t, f, ap); return 8; }
int __android_log_write(int p, const char* t, const char* m) { fake_log_prio = p; snprintf(fake_log_tag, sizeof fake_log_tag, "%s", t); snprintf(fake_log_msg, sizeof fake_log_msg, "%s", m); return 9; }
void __android_log_assert(const char*, const char* t, const char* f, ...) { va_list ap; va_start(ap, f); rec(0, t, f, ap); va_end(ap); }
}
