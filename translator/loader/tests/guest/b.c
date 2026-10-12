// SPDX-License-Identifier: GPL-2.0-or-later
// Guest test object B: depends on liba.so, interposes a_interposed, imports libc-ish symbols.
extern int a_func(int);
extern int a_data;
extern int a_interposed(void);
extern int missing_lib_fn(void);

int b_interposed(void) { return 2; }
int a_interposed(void) { return 2; }
int (*b_a_func_ptr)(int) = a_func;
int* b_a_data_ptr = &a_data;
int (*b_missing)(void) = missing_lib_fn;
int b_call(void) { return a_func(1) + a_interposed() + missing_lib_fn(); }
