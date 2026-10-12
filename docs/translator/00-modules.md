# armtr: module split

Engine (Unicorn 2.1.4, GPL-2.0, see 01-engine-choice.md) behind `include/armtr/engine.h`.

| Module | Dir | Contract | Depends on |
|---|---|---|---|
| engine | `translator/engine/` | `engine.h` | Unicorn (fetched by script, not vendored) |
| loader | `translator/loader/` | `loader.h` | engine (mem_map) |
| syscalls | `translator/syscalls/` | `syscalls.h` | engine (types) |
| proxy | `translator/proxy/` | `proxy.h` | engine |
| runner | `translator/runner/` | `armtr-run` (docs 04) | all but bridge |
| bridge | `translator/bridge/` | ART NativeBridgeCallbacks | all of the above |
| tests / guest userland | `translator/tests/` | arm64 test programs | - |

Rules: every module builds and has unit tests on the host (glibc, cmake, `-j2`) first; Android.bp comes
after. Guest virtual address == host virtual address. GPL-2.0-or-later header in every file.
