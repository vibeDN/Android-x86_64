// SPDX-License-Identifier: GPL-2.0-or-later
static int impl5(void) { return 5; }
static int (*resolve5(void))(void) { return impl5; }
int fn(void) __attribute__((ifunc("resolve5")));
int ifunc_user(void) { return fn(); }
void _start(void) { ifunc_user(); for (;;) {} }
