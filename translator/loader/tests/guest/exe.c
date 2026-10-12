// SPDX-License-Identifier: GPL-2.0-or-later
extern int a_func(int);
int exe_data = 3;
int* exe_ptr = &exe_data;
int (*exe_fp)(int) = a_func;
void _start(void) { for (;;) {} }
