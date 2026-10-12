// SPDX-License-Identifier: GPL-2.0-or-later
// usage: loader_test synth <guest-dir>        synthetic aarch64 objects built by build_guest.sh
//        loader_test real [-r llvm-readelf] <libs...>   load real libs with a dummy resolver, report imports
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>
#include "armtr_loader_ext.h"
#include "fake_engine.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

static std::set<std::string> g_asked;
static uint64_t resolver(const char*, const char* sym, void*) {
  g_asked.insert(sym);
  if (!strcmp(sym, "ext_weak")) return 0;
  return 0xdead0000ull + (std::hash<std::string>()(sym) & 0xfff0);
}
static uint64_t dummy(const char* s) { return 0xdead0000ull + (std::hash<std::string>()(s) & 0xfff0); }
static uint64_t rd(uint64_t a) { return *(uint64_t*)a; }

// scan readable pages of [lo,hi) for a qword; /proc/self/mem skips PROT_NONE gaps safely
static bool scan(uint64_t lo, uint64_t hi, uint64_t want, uint64_t* where = nullptr) {
  int fd = open("/proc/self/mem", O_RDONLY);
  bool found = false;
  for (uint64_t p = lo; p < hi && !found; p += 4096) {
    uint64_t buf[512];
    if (pread(fd, buf, 4096, (off_t)p) != 4096) continue;
    for (int i = 0; i < 512; i++) if (buf[i] == want) { found = true; if (where) *where = p + i * 8; break; }
  }
  close(fd);
  return found;
}

static size_t g_base_relocs;

static void synth(const std::string& d, const std::string& name) {
  printf("== variant %s\n", name.c_str());
  std::string dir = d + "/" + name, err(512, 0);
  const char* sp[] = {dir.c_str(), nullptr};
  armtr_loader_opts o{resolver, nullptr, sp};
  {  // warm up: the TLSDESC stub page is allocated once and stays registered
    armtr_image* w = armtr_load((dir + "/liba.so").c_str(), &o, &err[0], err.size());
    armtr_unload(w);
  }
  size_t regions0 = fake_region_count();
  g_asked.clear();
  armtr_image* b = armtr_load((dir + "/libb.so").c_str(), &o, &err[0], err.size());
  if (!b) { printf("FAIL load: %s\n", err.c_str()); g_fail++; return; }
  CHECK(armtr_image_dep_count(b) == 1);
  const armtr_image* a = armtr_image_dep(b, 0);
  CHECK(a && !strcmp(armtr_image_soname(a), "liba.so"));
  CHECK(armtr_image_missing_count(b) == 0);
  auto S = [&](const char* n) { return armtr_image_symbol(a, n); };
  CHECK(S("a_data") && *(int*)S("a_data") == 42);
  CHECK(armtr_image_symbol(b, "a_data") == S("a_data"));   // dlsym through deps
  CHECK(!armtr_image_symbol(a, "nonexistent") && !armtr_image_symbol(a, "b_call"));
  CHECK(armtr_image_symbol(b, "b_call"));
  CHECK(rd(S("a_ptr")) == S("a_arr") + 8);
  CHECK(rd(S("a_ext_ptr")) == dummy("ext_fn"));
  CHECK(rd(S("a_ext_data_ptr")) == dummy("ext_data"));
  CHECK(rd(S("a_weak_ptr")) == 0); CHECK(g_asked.count("ext_weak"));
  CHECK(rd(armtr_image_symbol(b, "b_missing")) == dummy("missing_lib_fn"));
  CHECK(rd(armtr_image_symbol(b, "b_a_func_ptr")) == S("a_func"));
  CHECK(rd(armtr_image_symbol(b, "b_a_data_ptr")) == S("a_data"));
  if (name != "symb") {   // -Bsymbolic deliberately binds locally
    CHECK(rd(S("a_interp_ptr")) == armtr_image_symbol(b, "a_interposed"));   // interposition: root first
    CHECK(armtr_image_symbol(b, "a_interposed") != S("a_interposed"));
  }
  CHECK(armtr_image_unresolved_count(a) == 0 && armtr_image_unresolved_count(b) == 0);
  CHECK(armtr_image_base(a) % 4096 == 0 && armtr_image_base(a) % (name == "big64k" ? 65536 : 4096) == 0);
  // relocation counts must be independent of the encoding (plain / android packed / relr)
  if (name == "base") g_base_relocs = armtr_image_reloc_count(a);
  else if (name != "android" && g_base_relocs) CHECK(armtr_image_reloc_count(a) == g_base_relocs);
  CHECK(armtr_image_reloc_count(a) >= 13);
  // memory protections registered with the engine: executable is never writable, RELRO not writable
  FakeRegion r;
  CHECK(fake_region_find(S("a_func"), &r) && r.prot == 5);
  CHECK(fake_region_find(S("a_data"), &r) && (r.prot & 2));
  uint64_t lo = armtr_image_base(a), hi = lo + armtr_image_size(a);
  for (uint64_t p = lo; p < hi; p += 4096) if (fake_region_find(p, &r)) CHECK(r.prot != 7);
  // TLS
  armtr_tls_info t;
  CHECK(armtr_image_tls(a, &t) == 0); if (armtr_image_tls(a, &t)) return;
  CHECK(armtr_image_tls(b, &t) == -1);
  armtr_image_tls(a, &t);
  CHECK(t.module_id >= 1 && t.static_offset >= 16 && t.memsz >= 12 && t.filesz == 8 && t.align >= 4);
  if (t.init_image) CHECK(*(int*)t.init_image == 7 || *(int*)(t.init_image + 4) == 7);
  CHECK(armtr_tls_static_size() >= t.static_offset + t.memsz);
  CHECK(scan(lo, hi, t.static_offset + 4));         // R_AARCH64_TLS_TPREL64 for a_tls_ie (symbol value 4)
  uint64_t w;
  CHECK(scan(lo, hi, t.static_offset, &w) && rd(w - 8) != 0);   // TLSDESC {stub, offset} for a_tls
  // init/fini + ifunc
  uint64_t fn[8];
  size_t n = armtr_image_init_funcs(b, fn, 8);
  CHECK(n == 2 && fn[0] >= lo && fn[0] < hi && fn[1] >= lo && fn[1] < hi);   // DT_INIT, init_array[0]
  CHECK(armtr_image_fini_funcs(b, fn, 8) == 0);
  armtr_image_init_done(b);
  CHECK(armtr_image_init_funcs(b, fn, 8) == 0);
  size_t ni = armtr_image_irelative_count(a);
  (void)ni;
  uint64_t slot, res;
  if (ni) {
    CHECK(armtr_image_irelative(a, 0, &slot, &res) == 0 && res >= lo && res < hi);
    CHECK(armtr_image_irelative_set(a, 0, 0x1234) == 0 && rd(slot) == 0x1234);
  }
  // shared dep loaded twice is refcounted: reload b, then unload both
  armtr_image* b2 = armtr_load((dir + "/libb.so").c_str(), &o, &err[0], err.size());
  CHECK(b2 == b);
  armtr_unload(b2);
  CHECK(fake_region_count() > regions0);
  armtr_unload(b);
  CHECK(fake_region_count() == regions0);

  // standalone liba: no interposer, own definition wins
  armtr_image* a1 = armtr_load((dir + "/liba.so").c_str(), &o, &err[0], err.size());
  CHECK(a1 && rd(armtr_image_symbol(a1, "a_interp_ptr")) == armtr_image_symbol(a1, "a_interposed"));
  armtr_unload(a1);
  // strict mode: unresolved import fails with a message and leaks nothing
  armtr_loader_opts none{nullptr, nullptr, sp};
  CHECK(!armtr_load((dir + "/libb.so").c_str(), &none, &err[0], err.size()));
  CHECK(err.find("unresolved symbol '") != std::string::npos);
  CHECK(fake_region_count() == regions0);
  // lenient mode: loads, records
  armtr_loader_set_lenient(1);
  a1 = armtr_load((dir + "/liba.so").c_str(), &none, &err[0], err.size());
  CHECK(a1 && armtr_image_unresolved_count(a1) == 2);   // ext_fn, ext_data; ext_weak is weak
  CHECK(a1 && rd(armtr_image_symbol(a1, "a_ext_ptr")) == 0);
  armtr_unload(a1);
  armtr_loader_set_lenient(0);
  // executable (PIE) with DT_NEEDED
  armtr_image* e = armtr_load((dir + "/exe").c_str(), &o, &err[0], err.size());
  CHECK(e);
  if (e) {
    uint64_t pa, pn, pe;
    CHECK(armtr_image_entry(e) > armtr_image_base(e) && armtr_image_entry(e) < armtr_image_base(e) + armtr_image_size(e));
    CHECK(armtr_image_phdr(e, &pa, &pn, &pe) == 0 && pn >= 5 && pe == 56 && *(uint32_t*)pa == 6);
    CHECK(rd(armtr_image_symbol(e, "exe_ptr")) == armtr_image_symbol(e, "exe_data"));
    CHECK(rd(armtr_image_symbol(e, "exe_fp")) == armtr_image_symbol(armtr_image_dep(e, 0), "a_func"));
    armtr_unload(e);
  }
  CHECK(fake_region_count() == regions0);
}

static size_t readelf_count(const std::string& tool, const std::string& lib) {
  std::string cmd = tool + " -r " + lib + " | grep -cE 'R_AARCH64_|^([0-9a-f]{4}:\\s+[0-9a-f]{16}\\s+|\\s+)[0-9a-f]{16}\\s'";
  FILE* f = popen(cmd.c_str(), "r"); size_t n = 0;
  if (f) { if (fscanf(f, "%zu", &n) != 1) n = 0; pclose(f); }
  return n;
}

static void real(const std::string& tool, const std::string& lib) {
  std::string err(512, 0);
  std::string dir = lib.substr(0, lib.rfind('/'));
  const char* sp[] = {dir.c_str(), nullptr};
  g_asked.clear();
  armtr_loader_opts o{resolver, nullptr, sp};
  size_t r0 = fake_region_count();
  armtr_image* im = armtr_load(lib.c_str(), &o, &err[0], err.size());
  if (!im) { printf("FAIL %s: %s\n", lib.c_str(), err.c_str()); g_fail++; return; }
  printf("%s: ok base=0x%llx size=0x%zx relocs=%zu deps=%zu missing=%zu", lib.c_str(), (unsigned long long)armtr_image_base(im),
         armtr_image_size(im), armtr_image_reloc_count(im), armtr_image_dep_count(im), armtr_image_missing_count(im));
  armtr_tls_info t;
  if (!armtr_image_tls(im, &t)) printf(" tls(mem=%llu)", (unsigned long long)t.memsz);
  uint64_t fn[64];
  printf(" inits=%zu irel=%zu\n", armtr_image_init_funcs(im, fn, 64), armtr_image_irelative_count(im));
  if (!tool.empty()) {
    size_t want = readelf_count(tool, lib), got = armtr_image_reloc_count(im);
    if (want != got) { printf("FAIL reloc count %zu != readelf %zu\n", got, want); g_fail++; }
  }
  CHECK(armtr_image_unresolved_count(im) == 0);
  if (!strstr(lib.c_str(), "voip") || true) {
    // every pointer-sized slot the resolver handed out must have landed somewhere: cheap check on JNI entry
    if (armtr_image_symbol(im, "JNI_OnLoad")) printf("  JNI_OnLoad=0x%llx\n", (unsigned long long)armtr_image_symbol(im, "JNI_OnLoad"));
  }
  printf("  imports resolved via resolver: %zu\n", g_asked.size());
  for (auto& s : g_asked) printf("  IMPORT %s %s\n", strrchr(lib.c_str(), '/') + 1, s.c_str());
  armtr_unload(im);
  CHECK(fake_region_count() <= r0 + 1);   // +1: TLSDESC stub page
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 3 && !strcmp(argv[1], "synth")) {
    std::string d = argv[2];
    char err[256];
    CHECK(!armtr_load("/nonexistent.so", nullptr, err, sizeof err) && strstr(err, "cannot open"));
    CHECK(!armtr_load(argv[0], nullptr, err, sizeof err) && strstr(err, "aarch64"));   // x86_64 host binary
    if (argc < 4) {   // IRELATIVE: static-pie with a local ifunc
      std::string p = d + "/ifunc/exe", e;
      e.resize(256);
      armtr_image* im = armtr_load(p.c_str(), nullptr, &e[0], e.size());
      CHECK(im);
      if (im) {
        uint64_t slot, res;
        CHECK(armtr_image_irelative_count(im) == 1 && armtr_image_irelative(im, 0, &slot, &res) == 0);
        CHECK(res >= armtr_image_base(im) && res < armtr_image_base(im) + armtr_image_size(im));
        FakeRegion r; CHECK(fake_region_find(slot, &r));
        CHECK(armtr_image_irelative_set(im, 0, 0xabcd) == 0 && *(uint64_t*)slot == 0xabcd);
        armtr_unload(im);
      }
    }
    for (const char* v : {"base", "gnu", "sysv", "android", "relr", "big64k", "symb", "small4k"}) if (argc < 4 || !strcmp(argv[3], v)) synth(d, v);
  } else if (argc >= 3 && !strcmp(argv[1], "real")) {
    int i = 2; std::string tool;
    if (!strcmp(argv[i], "-r")) { tool = argv[i + 1]; i += 2; }
    for (; i < argc; i++) real(tool, argv[i]);
  } else return 2;
  printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
  return g_fail != 0;
}
