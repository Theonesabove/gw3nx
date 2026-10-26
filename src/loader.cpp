// Minimal ELF32/ARM shared-object loader into guest memory.
#include "core.hpp"

#include <elf.h>
#include <unordered_map>
#include <vector>
#ifdef __SWITCH__
#define IS_SWITCH true
#else
#define IS_SWITCH false
#endif

static std::vector<GLib*> g_libs;
static std::unordered_map<GLib*, std::unordered_map<std::string, gaddr>> g_exports;

static void missing_import(Ctx& c) {
    logf("UNIMPLEMENTED import called: %s (r0=%08x r1=%08x lr=%s)", g_cur_hle, c.u(0), c.u(1), addr_name(reg(14)).c_str());
    c.ret(0);
}

static gaddr resolve(GLib* self, const char* name, bool weak, bool* is_data) {
    for (GLib* l : g_libs) if (l != self) { gaddr a = lib_sym(l, name); if (a) return a; }
    if (gaddr a = hle_lookup(name)) return a;
    if (gaddr a = hle_data(name)) return a;
    if (weak) return 0;
    if (*is_data) { logf("loader: %s: unresolved DATA import %s", self->name.c_str(), name); return 0; }
    return hle_stub(name, missing_import);
}

GLib* load_lib(const char* path, gaddr base) {
    FILE* f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    std::vector<uint8_t> file;
    fseek(f, 0, SEEK_END); file.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    if (fread(file.data(), 1, file.size(), f) != file.size()) fatal("read %s", path);
    fclose(f);

    auto* eh = (Elf32_Ehdr*)file.data();
    if (memcmp(eh->e_ident, ELFMAG, 4) || eh->e_machine != EM_ARM) fatal("%s: not ARM ELF", path);
    auto* ph = (Elf32_Phdr*)(file.data() + eh->e_phoff);
    auto* lib = new GLib;
    lib->name = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    lib->base = base;
    Elf32_Dyn* dyn = nullptr;
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            memcpy(g2h(base + ph[i].p_vaddr), file.data() + ph[i].p_offset, ph[i].p_filesz);
            memset(g2h(base + ph[i].p_vaddr + ph[i].p_filesz), 0, ph[i].p_memsz - ph[i].p_filesz);
            lib->end = std::max(lib->end, base + ph[i].p_vaddr + ph[i].p_memsz);
        } else if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (Elf32_Dyn*)g2h(base + ph[i].p_vaddr);
        } else if (ph[i].p_type == PT_ARM_EXIDX) {
            lib->exidx = base + ph[i].p_vaddr;
            lib->exidx_count = ph[i].p_memsz / 8;
        }
    }
    if (!dyn) fatal("%s: no PT_DYNAMIC", path);
    gaddr rel = 0, jmprel = 0; uint32_t relsz = 0, jmprelsz = 0;
    for (Elf32_Dyn* d = dyn; d->d_tag != DT_NULL; d++) {
        uint32_t v = d->d_un.d_val;
        switch (d->d_tag) {
        case DT_SYMTAB: lib->dynsym = base + v; break;
        case DT_STRTAB: lib->dynstr = base + v; break;
        case DT_HASH: lib->hash = base + v; break;
        case DT_REL: rel = base + v; break;
        case DT_RELSZ: relsz = v; break;
        case DT_JMPREL: jmprel = base + v; break;
        case DT_PLTRELSZ: jmprelsz = v; break;
        case DT_INIT: lib->init_fn = base + v; break;
        case DT_INIT_ARRAY: lib->init_array = base + v; break;
        case DT_INIT_ARRAYSZ: lib->init_count = v / 4; break;
        }
    }
    lib->nsyms = gref(lib->hash + 4);  // nchain
    g_libs.push_back(lib);

    auto* syms = (Elf32_Sym*)g2h(lib->dynsym);
    auto& ex = g_exports[lib];
    for (uint32_t i = 1; i < lib->nsyms; i++)
        if (syms[i].st_shndx != SHN_UNDEF && syms[i].st_value)
            ex.emplace(gstr(lib->dynstr + syms[i].st_name), base + syms[i].st_value);

    int unresolved = 0;
    auto apply = [&](gaddr r, uint32_t sz) {
        for (auto* e = (Elf32_Rel*)g2h(r); (uint8_t*)e < (uint8_t*)g2h(r + sz); e++) {
            uint32_t type = ELF32_R_TYPE(e->r_info), si = ELF32_R_SYM(e->r_info);
            uint32_t& where = gref(base + e->r_offset);
            gaddr S = 0;
            if (si) {
                Elf32_Sym& s = syms[si];
                const char* nm = gstr(lib->dynstr + s.st_name);
                if (s.st_shndx != SHN_UNDEF) S = base + s.st_value;
                else {
                    bool is_data = type == R_ARM_GLOB_DAT || type == R_ARM_ABS32;
                    is_data = is_data && ELF32_ST_TYPE(s.st_info) == STT_OBJECT;
                    S = resolve(lib, nm, ELF32_ST_BIND(s.st_info) == STB_WEAK, &is_data);
                    if (!S && ELF32_ST_BIND(s.st_info) != STB_WEAK) unresolved++;
                }
            }
            switch (type) {
            case R_ARM_ABS32: where += S; break;
            case R_ARM_GLOB_DAT: case R_ARM_JUMP_SLOT: where = S; break;
            case R_ARM_RELATIVE: where += base; break;
            case R_ARM_REL32: where += S - (base + e->r_offset); break;
            case R_ARM_NONE: break;
            default: fatal("%s: reloc type %u", path, type);
            }
        }
    };
    apply(rel, relsz);
    apply(jmprel, jmprelsz);
    logf("loader: %s at %08x-%08x, %zu exports, %d unresolved data", lib->name.c_str(), base, lib->end, ex.size(), unresolved);
    return lib;
}

gaddr lib_sym(GLib* lib, const char* name) {
    auto& ex = g_exports[lib];
    auto it = ex.find(name);
    return it == ex.end() ? 0 : it->second;
}
gaddr any_sym(const char* name) {
    for (GLib* l : g_libs) if (gaddr a = lib_sym(l, name)) return a;
    return 0;
}
GLib* lib_for_addr(gaddr a) {
    for (GLib* l : g_libs) if (a >= l->base && a < l->end) return l;
    return nullptr;
}
std::string addr_name(gaddr a) {
    GLib* l = lib_for_addr(a);
    if (!l) return a >= 0x0F000000 && a < 0x10000000 ? "[hle stub]" : "?";
    auto* syms = (Elf32_Sym*)g2h(l->dynsym);
    const char* best = nullptr; gaddr bv = 0;
    for (uint32_t i = 1; i < l->nsyms; i++) {
        gaddr v = l->base + syms[i].st_value;
        if (syms[i].st_shndx != SHN_UNDEF && ELF32_ST_TYPE(syms[i].st_info) == STT_FUNC && v <= a && v > bv) { bv = v; best = gstr(l->dynstr + syms[i].st_name); }
    }
    char buf[512];
    snprintf(buf, sizeof buf, "%s+0x%x (%s+0x%x)", l->name.c_str(), a - l->base, best ? best : "?", best ? a - bv : 0);
    return buf;
}

void lib_run_init(GLib* lib) {
    if (lib->init_fn) call_guest(lib->init_fn);
    for (uint32_t i = 0; i < lib->init_count; i++) {
        gaddr f = gref(lib->init_array + 4 * i);
        if (f && f != 0xFFFFFFFF) {
            if (getenv("GW3_TRACE_INIT") || IS_SWITCH) logf("loader: %s ctor %u/%u %s", lib->name.c_str(), i + 1, lib->init_count, addr_name(f).c_str());
            call_guest(f);
        }
    }
    logf("loader: %s: ran %u constructors", lib->name.c_str(), lib->init_count);
}

// libgcc's unwinder asks the C library where a PC's exception index table is.
static void hle_find_exidx(Ctx& c) {
    GLib* l = lib_for_addr(c.u(0));
    if (!l) { c.ret(0); return; }
    if (c.u(1)) gref(c.u(1)) = l->exidx_count;
    c.ret(l->exidx);
}
static struct LoaderReg { LoaderReg() {
    hle_register("__gnu_Unwind_Find_exidx", hle_find_exidx);
    hle_register("dl_unwind_find_exidx", hle_find_exidx);
} } s_loader_reg;
