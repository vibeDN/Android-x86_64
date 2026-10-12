// SPDX-License-Identifier: GPL-2.0-or-later
// armtr loader: aarch64 ELF64 shared objects/executables into identity-mapped host memory.
#include "armtr_loader_ext.h"
#include "armtr/engine.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

// ---- ELF64 definitions (own copies: no dependency on <elf.h> flavour) ----
struct Ehdr { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
  uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; };
struct Phdr { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; };
struct Dyn { int64_t tag; uint64_t val; };
struct Sym { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; };
struct Rela { uint64_t offset, info; int64_t addend; };

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_PHDR = 6, PT_TLS = 7, PT_GNU_RELRO = 0x6474e552 };
enum { ET_EXEC = 2, ET_DYN = 3, EM_AARCH64 = 183 };
enum { DT_NULL = 0, DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7,
  DT_RELASZ = 8, DT_RELAENT = 9, DT_STRSZ = 10, DT_INIT = 12, DT_FINI = 13, DT_SONAME = 14, DT_REL = 17,
  DT_RELSZ = 18, DT_PLTREL = 20, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26,
  DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28, DT_PREINIT_ARRAY = 32, DT_PREINIT_ARRAYSZ = 33,
  DT_RELRSZ = 35, DT_RELR = 36, DT_GNU_HASH = 0x6ffffef5, DT_VERSYM = 0x6ffffff0,
  DT_ANDROID_REL = 0x6000000f, DT_ANDROID_RELSZ = 0x60000010, DT_ANDROID_RELA = 0x60000011,
  DT_ANDROID_RELASZ = 0x60000012, DT_ANDROID_RELR = 0x6fffe000, DT_ANDROID_RELRSZ = 0x6fffe001 };
enum { R_NONE = 0, R_ABS64 = 257, R_COPY = 1024, R_GLOB_DAT = 1025, R_JUMP_SLOT = 1026, R_RELATIVE = 1027,
  R_TLS_DTPMOD64 = 1028, R_TLS_DTPREL64 = 1029, R_TLS_TPREL64 = 1030, R_TLSDESC = 1031, R_IRELATIVE = 1032 };
enum { STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2, STB_GNU_UNIQUE = 10 };
enum { STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2, STT_SECTION = 3, STT_FILE = 4, STT_TLS = 6, STT_GNU_IFUNC = 10 };

struct LoadError : std::runtime_error { using std::runtime_error::runtime_error; };
[[noreturn]] void fail(const char* fmt, ...) {
  char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
  throw LoadError(b);
}

uint64_t PG() { static uint64_t p = (uint64_t)sysconf(_SC_PAGESIZE); return p; }
uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }
uint64_t down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
int prot_of(uint32_t f) { return ((f & 4) ? 1 : 0) | ((f & 2) ? 2 : 0) | ((f & 1) ? 4 : 0); }
int host_prot(int p) { return (p & 1 ? PROT_READ : 0) | (p & 2 ? PROT_WRITE : 0) | (p & 4 ? PROT_EXEC : 0); }

struct Irel { uint64_t slot, resolver; };
struct Run { uint64_t addr, len; int prot; };

}  // namespace

struct armtr_image {
  std::string path, soname;
  uint16_t type = 0;
  uint64_t entry = 0, bias = 0, start = 0, len = 0;   // span = [start, start+len) in unbiased vaddr
  uint64_t phdr_addr = 0, phnum = 0, phentsize = 0;
  uint64_t relro_start = 0, relro_end = 0;
  struct Seg { uint64_t vaddr, memsz, filesz, off; uint32_t flags; };
  std::vector<Seg> segs;
  std::vector<uint8_t> pageprot;   // final prot per host page of the span
  std::vector<Run> runs;           // registered with the engine
  // dynamic
  const Sym* symtab = nullptr; const char* strtab = nullptr; uint64_t strsz = 0;
  const uint32_t* hash = nullptr; const uint32_t* gnuhash = nullptr; const uint16_t* versym = nullptr;
  std::vector<std::string> needed;
  uint64_t init = 0, fini = 0, init_arr = 0, init_arrsz = 0, fini_arr = 0, fini_arrsz = 0, preinit_arr = 0, preinit_arrsz = 0;
  uint64_t rela = 0, relasz = 0, jmprel = 0, jmprelsz = 0, arela = 0, arelasz = 0, relr = 0, relrsz = 0;
  uint64_t rel = 0, relsz = 0, arel = 0, arelsz = 0;
  // tls
  bool has_tls = false; uint64_t tls_vaddr = 0, tls_filesz = 0, tls_memsz = 0, tls_align = 1;
  uint32_t tls_module = 0; uint64_t tls_off = 0;
  // state
  std::vector<armtr_image*> deps;
  std::vector<std::string> missing, unresolved;
  std::vector<Irel> irel;
  size_t nreloc = 0;
  int refs = 1;
  bool init_done = false, relocated = false;
};

namespace {

std::recursive_mutex g_mu;
std::vector<armtr_image*> g_images;
bool g_lenient = false;
uint64_t g_tcb = 16, g_tls_end = 16;
uint32_t g_tls_mod = 0;
uint64_t g_tlsdesc_stub = 0;
bool g_raw = false;          // armtr_load_raw in progress (under g_mu): map only
uint64_t g_raw_base = 0;

// ---- symbol lookup ----
uint32_t gnu_hash(const char* s) { uint32_t h = 5381; for (; *s; s++) h = h * 33 + (uint8_t)*s; return h; }
uint32_t sysv_hash(const char* s) {
  uint32_t h = 0, g;
  for (; *s; s++) { h = (h << 4) + (uint8_t)*s; g = h & 0xf0000000; if (g) h ^= g >> 24; h &= ~g; }
  return h;
}
bool is_def(const armtr_image* im, uint32_t idx, const char* name) {
  const Sym& s = im->symtab[idx];
  if (s.shndx == 0) return false;
  unsigned b = s.info >> 4, t = s.info & 15;
  if (b != STB_GLOBAL && b != STB_WEAK && b != STB_GNU_UNIQUE) return false;
  if (t == STT_SECTION || t == STT_FILE) return false;
  if (im->versym && (im->versym[idx] & 0x8000)) return false;   // hidden (non-default) version
  return !strcmp(im->strtab + s.name, name);
}
const Sym* lookup_in(const armtr_image* im, const char* name) {
  if (!im->symtab || !im->strtab) return nullptr;
  if (im->gnuhash) {
    const uint32_t* h = im->gnuhash;
    uint32_t nb = h[0], symoff = h[1], bloomsz = h[2], shift = h[3];
    const uint64_t* bloom = (const uint64_t*)(h + 4);
    const uint32_t* bucket = (const uint32_t*)(bloom + bloomsz);
    const uint32_t* chain = bucket + nb;
    uint32_t hv = gnu_hash(name);
    uint64_t w = bloom[(hv / 64) % bloomsz], mask = (1ull << (hv % 64)) | (1ull << ((hv >> shift) % 64));
    if ((w & mask) != mask) return nullptr;
    uint32_t n = bucket[hv % nb];
    if (n < symoff) return nullptr;
    for (;; n++) {
      uint32_t c = chain[n - symoff];
      if (((c ^ hv) >> 1) == 0 && is_def(im, n, name)) return &im->symtab[n];
      if (c & 1) break;
    }
    return nullptr;
  }
  if (im->hash) {
    uint32_t nb = im->hash[0], nc = im->hash[1];
    const uint32_t* bucket = im->hash + 2; const uint32_t* chain = bucket + nb;
    if (!nb) return nullptr;
    for (uint32_t n = bucket[sysv_hash(name) % nb]; n && n < nc; n = chain[n])
      if (is_def(im, n, name)) return &im->symtab[n];
  }
  return nullptr;
}

// BFS: img, its deps, deps of deps (dlsym / local group order).
std::vector<armtr_image*> bfs(armtr_image* root) {
  std::vector<armtr_image*> g{root};
  for (size_t i = 0; i < g.size(); i++)
    for (auto* d : g[i]->deps) { bool seen = false; for (auto* x : g) seen |= x == d; if (!seen) g.push_back(d); }
  return g;
}

// ---- file mapping ----
struct File {
  int fd = -1; const uint8_t* p = nullptr; size_t n = 0;
  ~File() { if (p) munmap((void*)p, n); if (fd >= 0) close(fd); }
};

template <class T> const T* at(const File& f, uint64_t off, uint64_t cnt, const char* what) {
  if (off > f.n || cnt > (f.n - off) / sizeof(T)) fail("truncated ELF (%s)", what);
  return (const T*)(f.p + off);
}

void protect_all(armtr_image* im);

void map_image(armtr_image* im, const std::string& path) {
  File f;
  f.fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (f.fd < 0) fail("%s: cannot open: %s", path.c_str(), strerror(errno));
  struct stat st; if (fstat(f.fd, &st) || st.st_size < (off_t)sizeof(Ehdr)) fail("%s: not an ELF file", path.c_str());
  f.n = (size_t)st.st_size;
  f.p = (const uint8_t*)mmap(nullptr, f.n, PROT_READ, MAP_PRIVATE, f.fd, 0);
  if (f.p == MAP_FAILED) { f.p = nullptr; fail("%s: mmap failed", path.c_str()); }
  const Ehdr* eh = (const Ehdr*)f.p;
  if (memcmp(eh->ident, "\177ELF", 4) || eh->ident[4] != 2 || eh->ident[5] != 1) fail("%s: not a little-endian ELF64", path.c_str());
  if (eh->machine != EM_AARCH64) fail("%s: not an aarch64 ELF (e_machine=%u)", path.c_str(), eh->machine);
  if (eh->type != ET_DYN && eh->type != ET_EXEC) fail("%s: unsupported e_type %u", path.c_str(), eh->type);
  if (eh->phentsize != sizeof(Phdr)) fail("%s: bad e_phentsize", path.c_str());
  const Phdr* ph = at<Phdr>(f, eh->phoff, eh->phnum, "program headers");
  im->type = eh->type; im->entry = eh->entry;
  im->phnum = eh->phnum; im->phentsize = sizeof(Phdr);

  uint64_t lo = UINT64_MAX, hi = 0, maxalign = PG();
  for (unsigned i = 0; i < eh->phnum; i++) {
    const Phdr& p = ph[i];
    if (p.type == PT_LOAD) {
      if (p.filesz > p.memsz || p.offset > f.n || p.filesz > f.n - p.offset) fail("%s: bad PT_LOAD", path.c_str());
      if (p.vaddr + p.memsz < p.vaddr) fail("%s: PT_LOAD overflow", path.c_str());
      if ((p.align & (p.align - 1)) || (p.align > 1 && (p.vaddr - p.offset) % p.align)) fail("%s: PT_LOAD misaligned", path.c_str());
      lo = std::min(lo, down(p.vaddr, PG())); hi = std::max(hi, up(p.vaddr + p.memsz, PG()));
      maxalign = std::max(maxalign, p.align);
      im->segs.push_back({p.vaddr, p.memsz, p.filesz, p.offset, p.flags});
    } else if (p.type == PT_TLS) {
      im->has_tls = true; im->tls_vaddr = p.vaddr; im->tls_filesz = p.filesz; im->tls_memsz = p.memsz;
      im->tls_align = p.align ? p.align : 1;
    } else if (p.type == PT_GNU_RELRO) { im->relro_start = p.vaddr; im->relro_end = p.vaddr + p.memsz; }
    else if (p.type == PT_PHDR) im->phdr_addr = p.vaddr;
  }
  if (im->segs.empty()) fail("%s: no PT_LOAD", path.c_str());
  if (maxalign > (1ull << 30)) fail("%s: absurd alignment", path.c_str());
  uint64_t len = hi - lo;
  if (!im->phdr_addr)   // phdrs inside the first loaded segment
    for (auto& s : im->segs) if (eh->phoff >= s.off && eh->phoff + eh->phnum * sizeof(Phdr) <= s.off + s.filesz) { im->phdr_addr = s.vaddr + (eh->phoff - s.off); break; }

  // reserve address space
  uint8_t* m;
  if (eh->type == ET_EXEC) {
    m = (uint8_t*)mmap((void*)lo, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED || (uint64_t)m != lo) { if (m != MAP_FAILED) munmap(m, len); fail("%s: cannot map ET_EXEC at 0x%llx", path.c_str(), (unsigned long long)lo); }
    im->bias = 0;
  } else if (g_raw && g_raw_base) {
    if (g_raw_base & (maxalign - 1)) fail("%s: base 0x%llx not aligned to 0x%llx", path.c_str(), (unsigned long long)g_raw_base, (unsigned long long)maxalign);
    m = (uint8_t*)mmap((void*)(g_raw_base + lo), len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (m == MAP_FAILED || (uint64_t)m != g_raw_base + lo) { if (m != MAP_FAILED) munmap(m, len); fail("%s: cannot map at 0x%llx", path.c_str(), (unsigned long long)g_raw_base); }
    im->bias = g_raw_base;
  } else {
    uint64_t rl = len + maxalign;
    m = (uint8_t*)mmap(nullptr, rl, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) fail("%s: cannot reserve 0x%llx bytes", path.c_str(), (unsigned long long)rl);
    int64_t bias = ((int64_t)(uint64_t)m - (int64_t)lo + (int64_t)maxalign - 1) & ~((int64_t)maxalign - 1);
    uint64_t b = lo + (uint64_t)bias;   // span start
    if (b > (uint64_t)m) munmap(m, b - (uint64_t)m);
    uint64_t e = b + len, me = (uint64_t)m + rl;
    if (me > e) munmap((void*)e, me - e);
    im->bias = (uint64_t)bias;
  }
  im->start = lo; im->len = len;   // from here the span exists
  im->pageprot.assign(im->len / PG(), 7);
  for (auto& s : im->segs)
    if (s.filesz) memcpy((void*)(im->bias + s.vaddr), f.p + s.off, s.filesz);   // bss is already zero
  if (im->phdr_addr) im->phdr_addr += im->bias;
  if (g_raw) { im->relro_start = im->relro_end = 0; return; }   // the guest linker relocates and protects it

  // dynamic
  const Phdr* dp = nullptr;
  for (unsigned i = 0; i < eh->phnum; i++) if (ph[i].type == PT_DYNAMIC) dp = &ph[i];
  if (!dp) fail("%s: no PT_DYNAMIC (static binaries unsupported)", path.c_str());
  if (dp->vaddr < im->start || dp->vaddr + dp->memsz > im->start + im->len) fail("%s: PT_DYNAMIC outside image", path.c_str());
  const Dyn* d = (const Dyn*)(im->bias + dp->vaddr);
  uint64_t soname = UINT64_MAX; std::vector<uint64_t> need;
  auto P = [&](uint64_t v) { return im->bias + v; };
  for (size_t i = 0; i < dp->memsz / sizeof(Dyn) && d[i].tag != DT_NULL; i++) {
    uint64_t v = d[i].val;
    switch (d[i].tag) {
      case DT_NEEDED: need.push_back(v); break;
      case DT_SONAME: soname = v; break;
      case DT_HASH: im->hash = (const uint32_t*)P(v); break;
      case DT_GNU_HASH: im->gnuhash = (const uint32_t*)P(v); break;
      case DT_STRTAB: im->strtab = (const char*)P(v); break;
      case DT_STRSZ: im->strsz = v; break;
      case DT_SYMTAB: im->symtab = (const Sym*)P(v); break;
      case DT_VERSYM: im->versym = (const uint16_t*)P(v); break;
      case DT_RELA: im->rela = P(v); break; case DT_RELASZ: im->relasz = v; break;
      case DT_JMPREL: im->jmprel = P(v); break; case DT_PLTRELSZ: im->jmprelsz = v; break;
      case DT_PLTREL: if (v != 7) fail("%s: DT_PLTREL is not RELA", path.c_str()); break;
      case DT_REL: im->rel = v; break; case DT_RELSZ: im->relsz = v; break;
      case DT_ANDROID_REL: im->arel = P(v); break; case DT_ANDROID_RELSZ: im->arelsz = v; break;
      case DT_ANDROID_RELA: im->arela = P(v); break; case DT_ANDROID_RELASZ: im->arelasz = v; break;
      case DT_RELR: case DT_ANDROID_RELR: im->relr = P(v); break;
      case DT_RELRSZ: case DT_ANDROID_RELRSZ: im->relrsz = v; break;
      case DT_INIT: im->init = v ? P(v) : 0; break; case DT_FINI: im->fini = v ? P(v) : 0; break;
      case DT_INIT_ARRAY: im->init_arr = P(v); break; case DT_INIT_ARRAYSZ: im->init_arrsz = v; break;
      case DT_FINI_ARRAY: im->fini_arr = P(v); break; case DT_FINI_ARRAYSZ: im->fini_arrsz = v; break;
      case DT_PREINIT_ARRAY: im->preinit_arr = P(v); break; case DT_PREINIT_ARRAYSZ: im->preinit_arrsz = v; break;
    }
  }
  if (!im->strtab) fail("%s: no DT_STRTAB", path.c_str());
  if (!im->symtab && (im->gnuhash || im->hash)) fail("%s: hash without DT_SYMTAB", path.c_str());
  if (im->rel && im->relsz) fail("%s: DT_REL (non-RELA) relocations unsupported on aarch64", path.c_str());
  if (im->arel && im->arelsz) fail("%s: DT_ANDROID_REL unsupported (aarch64 uses RELA)", path.c_str());
  for (auto o : need) { if (o >= im->strsz) fail("%s: bad DT_NEEDED", path.c_str()); im->needed.push_back(im->strtab + o); }
  if (soname != UINT64_MAX && soname < im->strsz) im->soname = im->strtab + soname;
  if (im->has_tls) {
    im->tls_module = ++g_tls_mod;
    im->tls_off = up(g_tls_end, im->tls_align);
    g_tls_end = im->tls_off + im->tls_memsz;
  }
}

// ---- relocation ----
struct RelCtx { armtr_image* im; const std::vector<armtr_image*>* group; const armtr_loader_opts* opts; };

uint64_t tlsdesc_stub() {
  if (g_tlsdesc_stub) return g_tlsdesc_stub;
  void* p = mmap(nullptr, PG(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) fail("cannot allocate TLSDESC stub");
  ((uint32_t*)p)[0] = 0xF9400400;   // ldr x0, [x0, #8]
  ((uint32_t*)p)[1] = 0xD65F03C0;   // ret
  mprotect(p, PG(), PROT_READ | PROT_EXEC);
  if (armtr_mem_map((uint64_t)p, PG(), 5) < 0) fail("armtr_mem_map failed for TLSDESC stub");
  return g_tlsdesc_stub = (uint64_t)p;
}

void note_unresolved(armtr_image* im, const char* name) {
  for (auto& s : im->unresolved) if (s == name) return;
  im->unresolved.push_back(name);
}

void do_reloc(RelCtx& c, uint64_t off, uint64_t info, int64_t addend) {
  armtr_image* im = c.im;
  uint32_t type = (uint32_t)info, symi = (uint32_t)(info >> 32);
  if (type == R_NONE) return;
  if (off < im->start || off > im->start + im->len - 8) fail("%s: relocation offset 0x%llx outside image", im->path.c_str(), (unsigned long long)off);
  uint64_t* where = (uint64_t*)(im->bias + off);
  im->nreloc++;
  if (type == R_RELATIVE) { *where = im->bias + addend; return; }
  if (type == R_IRELATIVE) { im->irel.push_back({(uint64_t)where, im->bias + addend}); return; }

  const Sym* sym = nullptr; const char* name = "";
  armtr_image* defim = nullptr; const Sym* def = nullptr; uint64_t S = 0;
  if (symi) {
    if (!im->symtab) fail("%s: symbol reloc without DT_SYMTAB", im->path.c_str());
    sym = &im->symtab[symi]; name = im->strtab + sym->name;
    bool own = sym->shndx != 0 && ((sym->other & 3) != 0 || (sym->info >> 4) == STB_LOCAL);
    if (own) { def = sym; defim = im; }
    else for (auto* g : *c.group) {
      if (type == R_COPY && g == im) continue;
      if ((def = lookup_in(g, name))) { defim = g; break; }
    }
    if (def) S = defim->bias + def->value;
    else if (type != R_TLS_DTPMOD64 && type != R_TLS_DTPREL64 && type != R_TLS_TPREL64 && type != R_TLSDESC) {
      if (c.opts && c.opts->resolver) S = c.opts->resolver(nullptr, name, c.opts->resolver_user);
      if (!S && (sym->info >> 4) != STB_WEAK) {
        note_unresolved(im, name);
        if (!g_lenient) fail("%s: unresolved symbol '%s'", im->path.c_str(), name);
      }
    } else if ((sym->info >> 4) != STB_WEAK && !sym->shndx) {
      note_unresolved(im, name);
      if (!g_lenient) fail("%s: unresolved TLS symbol '%s'", im->path.c_str(), name);
    }
  }
  bool ifunc = def && (def->info & 15) == STT_GNU_IFUNC;
  switch (type) {
    case R_ABS64: case R_GLOB_DAT: case R_JUMP_SLOT:
      if (ifunc) { im->irel.push_back({(uint64_t)where, S}); *where = 0; }
      else if (sym && !S) *where = 0;   // weak undefined (or unresolved in lenient mode)
      else *where = S + addend;
      break;
    case R_COPY:
      if (!def) { *where = 0; break; }   // unresolved already reported
      memcpy(where, (void*)S, def->size);
      break;
    case R_TLS_DTPMOD64: case R_TLS_DTPREL64: case R_TLS_TPREL64: case R_TLSDESC: {
      armtr_image* t = symi ? defim : im;
      if (symi && !def) { *where = 0; break; }   // weak undefined TLS (or lenient)
      if (!t->has_tls) fail("%s: TLS relocation against object without PT_TLS ('%s')", im->path.c_str(), name);
      uint64_t symoff = symi ? def->value : 0;
      if (type == R_TLS_DTPMOD64) *where = t->tls_module;
      else if (type == R_TLS_DTPREL64) *where = symoff + addend;
      else if (type == R_TLS_TPREL64) *where = t->tls_off + symoff + addend;
      else { where[0] = tlsdesc_stub(); where[1] = t->tls_off + symoff + addend; }
      break;
    }
    default: fail("%s: unsupported relocation type %u", im->path.c_str(), type);
  }
}

// Android packed relocations (APS2).
int64_t sleb(const uint8_t*& p, const uint8_t* e) {
  int64_t r = 0; unsigned sh = 0; uint8_t b;
  do { if (p >= e) fail("truncated packed relocations"); b = *p++; if (sh < 64) r |= (int64_t)(b & 0x7f) << sh; sh += 7; } while (b & 0x80);
  if (sh < 64 && (b & 0x40)) r |= -((int64_t)1 << sh);
  return r;
}
void do_packed(RelCtx& c, const uint8_t* p, uint64_t sz) {
  const uint8_t* e = p + sz;
  if (sz < 4 || memcmp(p, "APS2", 4)) fail("%s: bad Android packed relocation magic", c.im->path.c_str());
  p += 4;
  uint64_t count = (uint64_t)sleb(p, e), off = (uint64_t)sleb(p, e), info = 0; int64_t addend = 0;
  for (uint64_t done = 0; done < count;) {
    uint64_t gsize = (uint64_t)sleb(p, e), flags = (uint64_t)sleb(p, e);
    bool byinfo = flags & 1, bydelta = flags & 2, byadd = flags & 4, hasadd = flags & 8;
    if (gsize > count - done) fail("%s: bad packed relocation group", c.im->path.c_str());
    uint64_t delta = bydelta ? (uint64_t)sleb(p, e) : 0;
    if (byinfo) info = (uint64_t)sleb(p, e);
    if (hasadd && byadd) addend += sleb(p, e);
    if (!hasadd) addend = 0;
    for (uint64_t i = 0; i < gsize; i++) {
      off += bydelta ? delta : (uint64_t)sleb(p, e);
      if (!byinfo) info = (uint64_t)sleb(p, e);
      if (hasadd && !byadd) addend += sleb(p, e);
      do_reloc(c, off, info, addend);
    }
    done += gsize;
  }
}
void do_relr(RelCtx& c, const uint64_t* r, uint64_t sz) {
  uint64_t n = sz / 8, where = 0; armtr_image* im = c.im;
  auto rel1 = [&](uint64_t a) {
    if (a < im->bias + im->start || a > im->bias + im->start + im->len - 8) fail("%s: RELR offset outside image", im->path.c_str());
    *(uint64_t*)a += im->bias; im->nreloc++;
  };
  for (uint64_t i = 0; i < n; i++) {
    uint64_t e = r[i];
    if (!(e & 1)) { where = im->bias + e; rel1(where); where += 8; }
    else { for (unsigned b = 0; b < 63; b++) if ((e >> (b + 1)) & 1) rel1(where + b * 8); where += 63 * 8; }
  }
}

void relocate(armtr_image* im, const std::vector<armtr_image*>& group, const armtr_loader_opts* opts) {
  RelCtx c{im, &group, opts};
  if (im->relr) do_relr(c, (const uint64_t*)im->relr, im->relrsz);
  if (im->arela && im->arelasz) do_packed(c, (const uint8_t*)im->arela, im->arelasz);
  for (auto [b, n] : {std::pair<uint64_t, uint64_t>{im->rela, im->relasz}, {im->jmprel, im->jmprelsz}})
    if (b) for (uint64_t i = 0; i < n / sizeof(Rela); i++) { const Rela& r = ((const Rela*)b)[i]; do_reloc(c, r.offset, r.info, r.addend); }
  im->relocated = true;
}

// Compute final protections (RELRO read-only), mprotect and register with the engine.
void protect_all(armtr_image* im) {
  uint64_t npg = im->len / PG();
  std::vector<uint8_t> pp(npg, 0);
  for (auto& s : im->segs) {
    uint64_t a = down(s.vaddr, PG()), b = up(s.vaddr + s.memsz, PG());
    for (uint64_t x = a; x < b; x += PG()) pp[(x - im->start) / PG()] |= prot_of(s.flags);
  }
  if (im->relro_end > im->relro_start)
    for (uint64_t x = down(im->relro_start, PG()); x < up(im->relro_end, PG()) && x < im->start + im->len; x += PG()) pp[(x - im->start) / PG()] &= ~2;
  for (uint64_t i = 0; i < npg;) {
    uint64_t j = i; while (j < npg && pp[j] == pp[i]) j++;
    uint64_t a = im->bias + im->start + i * PG(), l = (j - i) * PG();
    if (mprotect((void*)a, l, host_prot(pp[i]))) fail("%s: mprotect failed", im->path.c_str());
    if (pp[i]) {
      if (armtr_mem_map(a, l, pp[i]) < 0) fail("%s: armtr_mem_map failed", im->path.c_str());
      im->runs.push_back({a, l, pp[i]});
    }
    i = j;
  }
  im->pageprot = pp;
}

void destroy(armtr_image* im) {
  for (auto& r : im->runs) armtr_mem_unmap(r.addr, r.len);
  if (im->len) munmap((void*)(im->bias + im->start), im->len);
  delete im;
}

void release(armtr_image* im) {
  if (--im->refs > 0) return;
  for (size_t i = 0; i < g_images.size(); i++) if (g_images[i] == im) { g_images.erase(g_images.begin() + i); break; }
  auto deps = im->deps;
  destroy(im);
  for (auto* d : deps) release(d);
}

// ---- loading ----
std::string base_of(const std::string& p) { size_t s = p.rfind('/'); return s == std::string::npos ? p : p.substr(s + 1); }

armtr_image* find_loaded(const std::string& real, const std::string& name) {
  for (auto* i : g_images)
    if ((!real.empty() && i->path == real) || (!name.empty() && (i->soname == name || base_of(i->path) == name))) return i;
  return nullptr;
}

armtr_image* load_rec(const std::string& path, const armtr_loader_opts* opts, std::vector<armtr_image*>& fresh) {
  char rp[PATH_MAX];
  std::string real = realpath(path.c_str(), rp) ? rp : path;
  if (auto* e = find_loaded(real, "")) { e->refs++; return e; }
  auto* im = new armtr_image;
  im->path = real;
  g_images.push_back(im);
  fresh.push_back(im);
  map_image(im, real);
  if (g_raw) return im;
  std::string dir = real.substr(0, real.rfind('/') == std::string::npos ? 0 : real.rfind('/'));
  for (auto& n : im->needed) {
    if (auto* e = find_loaded("", n)) { e->refs++; im->deps.push_back(e); continue; }
    std::string found;
    auto try_dir = [&](const std::string& d) {
      if (!found.empty()) return;
      std::string f = d + "/" + n;
      if (access(f.c_str(), R_OK) == 0) found = f;
    };
    if (n.find('/') != std::string::npos) { if (access(n.c_str(), R_OK) == 0) found = n; }
    else {
      if (opts && opts->search_paths) for (const char* const* sp = opts->search_paths; *sp; sp++) try_dir(*sp);
      try_dir(dir);
    }
    if (found.empty()) { im->missing.push_back(n); continue; }
    im->deps.push_back(load_rec(found, opts, fresh));
  }
  return im;
}

}  // namespace

extern "C" {

armtr_image* armtr_load(const char* path, const armtr_loader_opts* opts, char* err, size_t errlen) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  std::vector<armtr_image*> fresh;
  armtr_image* root = nullptr;
  try {
    if (!path) fail("null path");
    root = load_rec(path, opts, fresh);
    auto group = bfs(root);
    for (auto* im : fresh) relocate(im, group, opts);
    for (auto* im : fresh) protect_all(im);
    return root;
  } catch (const std::exception& e) {
    if (err && errlen) snprintf(err, errlen, "%s", e.what());
    for (auto* im : fresh)
      for (size_t i = 0; i < g_images.size(); i++) if (g_images[i] == im) { g_images.erase(g_images.begin() + i); break; }
    for (auto* im : fresh)
      for (auto* d : im->deps) if (std::find(fresh.begin(), fresh.end(), d) == fresh.end()) release(d);
    for (auto* im : fresh) destroy(im);
    return nullptr;
  }
}

uint64_t armtr_image_base(const armtr_image* i) { return i->bias + i->start; }
uint64_t armtr_image_entry(const armtr_image* i) { return i->type == ET_EXEC ? i->entry : i->bias + i->entry; }

uint64_t armtr_image_symbol(const armtr_image* i, const char* name) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  for (auto* g : bfs(const_cast<armtr_image*>(i)))
    if (const Sym* s = lookup_in(g, name)) return (s->info & 15) == STT_TLS ? 0 : g->bias + s->value;
  return 0;
}

size_t armtr_image_init_funcs(const armtr_image* root, uint64_t* out, size_t max) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  std::vector<const armtr_image*> seen; size_t n = 0;
  auto put = [&](uint64_t v) { if (v && v != UINT64_MAX) { if (out && n < max) out[n] = v; n++; } };
  struct V { static void go(const armtr_image* im, std::vector<const armtr_image*>& seen, decltype(put)& put) {
    for (auto* s : seen) if (s == im) return;
    seen.push_back(im);
    for (auto* d : im->deps) go(d, seen, put);
    if (im->init_done) return;
    for (uint64_t i = 0; i < im->preinit_arrsz / 8; i++) put(((const uint64_t*)im->preinit_arr)[i]);
    put(im->init);
    for (uint64_t i = 0; i < im->init_arrsz / 8; i++) put(((const uint64_t*)im->init_arr)[i]);
  } };
  V::go(root, seen, put);
  return n;
}

size_t armtr_image_fini_funcs(const armtr_image* root, uint64_t* out, size_t max) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  std::vector<const armtr_image*> seen; size_t n = 0;
  auto put = [&](uint64_t v) { if (v && v != UINT64_MAX) { if (out && n < max) out[n] = v; n++; } };
  struct V { static void go(const armtr_image* im, std::vector<const armtr_image*>& seen, decltype(put)& put) {
    for (auto* s : seen) if (s == im) return;
    seen.push_back(im);
    for (uint64_t i = im->fini_arrsz / 8; i-- > 0;) put(((const uint64_t*)im->fini_arr)[i]);
    put(im->fini);
    for (auto* d : im->deps) go(d, seen, put);
  } };
  V::go(root, seen, put);
  return n;
}

void armtr_image_init_done(armtr_image* im) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  for (auto* g : bfs(im)) g->init_done = true;
}

void armtr_unload(armtr_image* im) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (im) release(im);
}

armtr_image* armtr_load_raw(const char* path, uint64_t base, char* err, size_t errlen) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  std::vector<armtr_image*> fresh;
  g_raw = true; g_raw_base = base;
  try {
    if (!path) fail("null path");
    armtr_image* im = load_rec(path, nullptr, fresh);
    g_raw = false;
    if (im->refs == 1 && !fresh.empty()) protect_all(im);
    return im;
  } catch (const std::exception& e) {
    g_raw = false;
    if (err && errlen) snprintf(err, errlen, "%s", e.what());
    for (auto* im : fresh) {
      for (size_t i = 0; i < g_images.size(); i++) if (g_images[i] == im) { g_images.erase(g_images.begin() + i); break; }
      destroy(im);
    }
    return nullptr;
  }
}

void armtr_loader_set_lenient(int on) { g_lenient = on != 0; }
void armtr_loader_set_tcb_size(size_t b) { g_tcb = b; if (g_tls_end < b) g_tls_end = b; }

int armtr_image_tls(const armtr_image* i, armtr_tls_info* o) {
  if (!i->has_tls) return -1;
  o->init_image = i->bias + i->tls_vaddr; o->filesz = i->tls_filesz; o->memsz = i->tls_memsz;
  o->align = i->tls_align; o->module_id = i->tls_module; o->static_offset = i->tls_off;
  return 0;
}
size_t armtr_tls_static_size(void) { return g_tls_end; }

size_t armtr_image_irelative_count(const armtr_image* i) { return i->irel.size(); }
int armtr_image_irelative(const armtr_image* i, size_t k, uint64_t* slot, uint64_t* res) {
  if (k >= i->irel.size()) return -1;
  *slot = i->irel[k].slot; *res = i->irel[k].resolver; return 0;
}
int armtr_image_irelative_set(const armtr_image* i, size_t k, uint64_t v) {
  if (k >= i->irel.size()) return -1;
  uint64_t slot = i->irel[k].slot, pg = down(slot, PG());
  size_t idx = (pg - i->bias - i->start) / PG();
  int p = idx < i->pageprot.size() ? i->pageprot[idx] : 3;
  if (!(p & 2)) mprotect((void*)pg, PG(), PROT_READ | PROT_WRITE);
  *(uint64_t*)slot = v;
  if (!(p & 2)) mprotect((void*)pg, PG(), host_prot(p));
  return 0;
}

const char* armtr_image_path(const armtr_image* i) { return i->path.c_str(); }
const char* armtr_image_soname(const armtr_image* i) { return i->soname.c_str(); }
size_t armtr_image_size(const armtr_image* i) { return i->len; }
size_t armtr_image_dep_count(const armtr_image* i) { return i->deps.size(); }
const armtr_image* armtr_image_dep(const armtr_image* i, size_t k) { return k < i->deps.size() ? i->deps[k] : nullptr; }
size_t armtr_image_missing_count(const armtr_image* i) { return i->missing.size(); }
const char* armtr_image_missing(const armtr_image* i, size_t k) { return k < i->missing.size() ? i->missing[k].c_str() : nullptr; }
size_t armtr_image_unresolved_count(const armtr_image* i) { return i->unresolved.size(); }
const char* armtr_image_unresolved(const armtr_image* i, size_t k) { return k < i->unresolved.size() ? i->unresolved[k].c_str() : nullptr; }
size_t armtr_image_reloc_count(const armtr_image* i) { return i->nreloc; }
int armtr_image_phdr(const armtr_image* i, uint64_t* a, uint64_t* n, uint64_t* e) {
  if (!i->phdr_addr) return -1;
  *a = i->phdr_addr; *n = i->phnum; *e = i->phentsize; return 0;
}

}  // extern "C"
