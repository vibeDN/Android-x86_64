// SPDX-License-Identifier: GPL-2.0-or-later
// Guest test object A (aarch64). Never executed in the loader tests, only loaded and inspected.
extern int ext_fn(int);
extern int ext_weak(void) __attribute__((weak));
extern int ext_data;

int a_data = 42;
int a_arr[4] = {1, 2, 3, 4};
int* a_ptr = &a_arr[2];
int (*a_ext_ptr)(int) = ext_fn;
int* a_ext_data_ptr = &ext_data;
int (*a_weak_ptr)(void) = ext_weak;
__thread int a_tls = 7;
__thread int a_tls_bss;
__attribute__((tls_model("initial-exec"))) __thread int a_tls_ie = 9;
int a_inited;
void a_init(void) { a_inited = 2; }   // DT_INIT (-Wl,-init=a_init)
static void a_ctor(void) { a_inited = 1; }
__attribute__((section(".init_array"), used)) static void (*a_ctor_p)(void) = a_ctor;

int a_interposed(void) { return 1; }
int (*a_interp_ptr)(void) = a_interposed;
int a_func(int x) { return x + a_data + ext_fn(x); }
int* a_tls_addr(void) { return &a_tls; }
int* a_tls_bss_addr(void) { return &a_tls_bss; }
int* a_tls_ie_addr(void) { return &a_tls_ie; }

static int impl5(void) { return 5; }
static int (*resolve5(void))(void) { return impl5; }
int a_ifunc(void) __attribute__((ifunc("resolve5")));
int (*a_ifunc_ptr)(void) = a_ifunc;
int a_call_ifunc(void) { return a_ifunc(); }
