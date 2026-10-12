# armtr: module split

Engine (Unicorn 2.1.4, GPL-2.0, see 01-engine-choice.md) behind `include/armtr/engine.h`.

| Module | Dir | Contract | Depends on |
|---|---|---|---|
| engine | `translator/engine/` | `engine.h` | Unicorn (fetched by script, not vendored) |
| loader | `translator/loader/` | `loader.h` | engine (mem_map) |
| syscalls | `translator/syscalls/` | `syscalls.h` | engine (types) |
| proxy | `translator/proxy/` | `proxy.h` | engine |
| runtime | `translator/runtime/` | `guest_rt.h`: guest process (exec setup, intercept, threads, `call`) | engine, loader, syscalls |
| runner | `translator/runner/` | `armtr-run` (docs 04) | runtime |
| hostlibs | `translator/hostlibs/` | `hostlibs.h`: platform libs (liblog, EGL, ...) for the guest | proxy |
| bridge | `translator/bridge/` | ART NativeBridgeCallbacks (docs 05) | all of the above |
| tests / guest userland | `translator/tests/` | arm64 test programs | - |

Rules: every module builds and has unit tests on the host (glibc, cmake, `-j2`) first; Android.bp comes
after. Guest virtual address == host virtual address. GPL-2.0-or-later header in every file.
