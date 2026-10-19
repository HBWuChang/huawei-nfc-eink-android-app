// Q-Poll compatibility hook for the NXP NFC HAL.
//
// The SN220 firmware supports an NFC-A Q-Poll discovery mode (technology
// byte 0x71). Android's NFA layer only knows NFC-A/B/F/V (0x00..0x03), so it
// drops the activation. While /data/vendor/nfc/qphook/enable exists this hook:
//   * rewrites outgoing RF_DISCOVER NFC-A entries 0x00 -> 0x71;
//   * maps incoming 61 03 / 61 05 technology 0x71 back to 0x00.
// The enable file is created only for an app Q-Poll session and removed when
// that session ends, so normal NFC-A is untouched. Nothing is written to the
// NFC controller, firmware, or DB/DA configuration.
#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <link.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define LOG_TAG "QPHook"
#define HAL_NAME "nfc_nci.nqx.default.hw.so"
#define PROCESS_SYMBOL "_Z27phNxpNciHal_process_ext_rspPhPt"
#define WRITE_SYMBOL "_Z26phNxpNciHal_write_internaltPKh"
#define ENABLE_PATH "/data/vendor/nfc/qphook/enable"
#ifndef AT_FDCWD
#define AT_FDCWD -100
#endif

typedef int (*process_ext_rsp_t)(uint8_t *ntf, uint16_t *len);
typedef int (*write_internal_t)(unsigned short len, const uint8_t *data);
static process_ext_rsp_t real_process;
static write_internal_t real_write_internal;

struct module_info {
    uintptr_t base;
    const ElfW(Phdr) *phdr;
    size_t phnum;
    uintptr_t end;
    int found;
};

static int callback(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    struct module_info *module = data;
    if (info->dlpi_name == NULL || strstr(info->dlpi_name, HAL_NAME) == NULL)
        return 0;
    module->base = (uintptr_t)info->dlpi_addr;
    module->phdr = info->dlpi_phdr;
    module->phnum = info->dlpi_phnum;
    for (size_t i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type == PT_LOAD) {
            uintptr_t end = module->base + ph->p_vaddr + ph->p_memsz;
            if (end > module->end) module->end = end;
        }
    }
    module->found = 1;
    return 1;
}

static uintptr_t dyn_ptr(uintptr_t value, uintptr_t base, uintptr_t end) {
    if (value >= base && value < end) return value;
    return base + value;
}

static int page_protection(uintptr_t address) {
    FILE *fp = fopen("/proc/self/maps", "re");
    if (!fp) return PROT_READ;
    char line[1024];
    int prot = PROT_READ;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0, offset = 0;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s %lx", &start, &end, perms, &offset) != 4) continue;
        if (address < start || address >= end) continue;
        prot = 0;
        if (perms[0] == 'r') prot |= PROT_READ;
        if (perms[1] == 'w') prot |= PROT_WRITE;
        if (perms[2] == 'x') prot |= PROT_EXEC;
        break;
    }
    fclose(fp);
    return prot;
}

static int patch_got(uintptr_t got, uintptr_t target, uintptr_t *old_out) {
    void **slot = (void **)got;
    uintptr_t old = (uintptr_t)*slot;
    *old_out = old;
    if (old == target) return 0;

    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) page_size = 4096;
    uintptr_t page = got & ~((uintptr_t)page_size - 1);
    int original = page_protection(page);
    if (original < 0) original = PROT_READ;
    if (mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_WRITE) != 0) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "mprotect RW failed: %s", strerror(errno));
        return -1;
    }
    *slot = (void *)target;
    __builtin___clear_cache((char *)slot, (char *)slot + sizeof(void *));
    if (mprotect((void *)page, (size_t)page_size, original) != 0) {
        __android_log_print(ANDROID_LOG_WARN, LOG_TAG, "mprotect restore failed: %s", strerror(errno));
    }
    return 1;
}

// Raw syscalls only: the hook .so is loaded into a vendor HAL whose linker
// namespace may leave this library's libc PLT lazy. Resolving stat/time from
// the HAL write path has crashed on this ROM, so avoid the PLT entirely.
static long raw_syscall2(long number, long a0, long a1) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory", "cc");
    return x0;
}

static long raw_syscall4(long number, long a0, long a1, long a2, long a3) {
    register long x8 asm("x8") = number;
    register long x0 asm("x0") = a0;
    register long x1 asm("x1") = a1;
    register long x2 asm("x2") = a2;
    register long x3 asm("x3") = a3;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
                 : "memory", "cc");
    return x0;
}

static int qphook_enabled(void) {
    struct stat st;
    long rc = raw_syscall4(SYS_newfstatat, AT_FDCWD, (long)ENABLE_PATH, (long)&st, 0);
    if (rc != 0) return 0;
    struct timespec ts;
    if (raw_syscall2(SYS_clock_gettime, CLOCK_REALTIME, (long)&ts) != 0) return 1;
    if (ts.tv_sec > st.st_mtime && ts.tv_sec - st.st_mtime > 600) return 0;
    return 1;
}

static int hook_process_ext_rsp(uint8_t *ntf, uint16_t *len) {
    if (ntf != NULL && len != NULL && *len >= 7 && ntf[0] == 0x61) {
        int offset = -1;
        if (ntf[1] == 0x03) offset = 5;
        else if (ntf[1] == 0x05) offset = 6;
        if (offset > 0 && offset < (int)*len && ntf[offset] == 0x71) {
            ntf[offset] = 0x00;
            __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                "normalize 61%02X tech 71 -> 00", ntf[1]);
        }
    }
    if (real_process == NULL) return 0;
    return real_process(ntf, len);
}

static int hook_write_internal(unsigned short len, const uint8_t *data) {
    if (qphook_enabled() && data != NULL && len >= 4 &&
        data[0] == 0x21 && data[1] == 0x03) {
        int body = data[2] + 3;
        int count = data[3];
        if (body <= (int)len && 4 + count * 2 <= body) {
            for (int i = 0; i < count; i++) {
                uint8_t *tech = (uint8_t *)(data + 4 + i * 2);
                if (tech[0] == 0x00) {
                    tech[0] = 0x71;
                    __android_log_print(ANDROID_LOG_INFO, LOG_TAG,
                                        "RF_DISCOVER NFC-A -> Q-Poll 71");
                }
            }
        }
    }
    if (real_write_internal == NULL) return 0;
    return real_write_internal(len, data);
}

// Patch every PLT/GOT relocation for one symbol. Returns the number patched.
static int hook_symbol(struct module_info *module, const ElfW(Dyn) *dynamic,
                       const char *wanted, void *hook, void **real_out,
                       const char *label) {
    uintptr_t symtab = 0, strtab = 0, jmprel = 0, rela = 0;
    size_t pltrelsz = 0, relasz = 0;
    long pltrel = DT_RELA;
    for (const ElfW(Dyn) *d = dynamic; d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) {
            case DT_SYMTAB: symtab = dyn_ptr(d->d_un.d_ptr, module->base, module->end); break;
            case DT_STRTAB: strtab = dyn_ptr(d->d_un.d_ptr, module->base, module->end); break;
            case DT_JMPREL: jmprel = dyn_ptr(d->d_un.d_ptr, module->base, module->end); break;
            case DT_PLTRELSZ: pltrelsz = (size_t)d->d_un.d_val; break;
            case DT_PLTREL: pltrel = (long)d->d_un.d_val; break;
            case DT_RELA: rela = dyn_ptr(d->d_un.d_ptr, module->base, module->end); break;
            case DT_RELASZ: relasz = (size_t)d->d_un.d_val; break;
            default: break;
        }
    }
    if (symtab == 0 || strtab == 0 || pltrel != DT_RELA) return 0;

    const ElfW(Sym) *symbols = (const ElfW(Sym) *)symtab;
    const char *strings = (const char *)strtab;
    uintptr_t target = 0;
    int patched = 0;

    for (size_t i = 0; jmprel != 0 && i + sizeof(ElfW(Rela)) <= pltrelsz; i += sizeof(ElfW(Rela))) {
        const ElfW(Rela) *r = (const ElfW(Rela) *)(jmprel + i);
        unsigned sym = (unsigned)ELF64_R_SYM(r->r_info);
        if (sym == 0) continue;
        const char *name = strings + symbols[sym].st_name;
        if (strcmp(name, wanted) != 0) continue;
        target = module->base + symbols[sym].st_value;
        uintptr_t old = 0;
        int rc = patch_got(module->base + r->r_offset, (uintptr_t)hook, &old);
        __android_log_print(rc > 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, LOG_TAG,
                            "%s rela.plt got=%p old=%p real=%p rc=%d", label,
                            (void *)(module->base + r->r_offset), (void *)old,
                            (void *)target, rc);
        if (rc >= 0) patched++;
    }

    for (size_t i = 0; rela != 0 && i + sizeof(ElfW(Rela)) <= relasz; i += sizeof(ElfW(Rela))) {
        const ElfW(Rela) *r = (const ElfW(Rela) *)(rela + i);
        unsigned type = (unsigned)ELF64_R_TYPE(r->r_info);
        unsigned sym = (unsigned)ELF64_R_SYM(r->r_info);
        if (sym == 0 || (type != R_AARCH64_GLOB_DAT && type != R_AARCH64_JUMP_SLOT)) continue;
        const char *name = strings + symbols[sym].st_name;
        if (strcmp(name, wanted) != 0) continue;
        if (target == 0) target = module->base + symbols[sym].st_value;
        uintptr_t old = 0;
        int rc = patch_got(module->base + r->r_offset, (uintptr_t)hook, &old);
        __android_log_print(rc > 0 ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, LOG_TAG,
                            "%s rela.dyn got=%p old=%p real=%p rc=%d", label,
                            (void *)(module->base + r->r_offset), (void *)old,
                            (void *)target, rc);
        if (rc >= 0) patched++;
    }

    if (target != 0) *real_out = (void *)target;
    return patched;
}

__attribute__((constructor)) static void qpoll_hook_init(void) {
    struct module_info module;
    memset(&module, 0, sizeof(module));
    dl_iterate_phdr(callback, &module);
    if (!module.found) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "HAL module not found");
        return;
    }

    const ElfW(Dyn) *dynamic = NULL;
    for (size_t i = 0; i < module.phnum; i++) {
        if (module.phdr[i].p_type == PT_DYNAMIC) {
            dynamic = (const ElfW(Dyn) *)(module.base + module.phdr[i].p_vaddr);
            break;
        }
    }
    if (dynamic == NULL) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "PT_DYNAMIC not found");
        return;
    }

    int p1 = hook_symbol(&module, dynamic, PROCESS_SYMBOL,
                         (void *)hook_process_ext_rsp, (void **)&real_process, "process");
    int p2 = hook_symbol(&module, dynamic, WRITE_SYMBOL,
                         (void *)hook_write_internal, (void **)&real_write_internal, "write");
    __android_log_print((p1 > 0 && p2 > 0) ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, LOG_TAG,
                        "hook process=%p write=%p patched=%d/%d base=%p",
                        (void *)real_process, (void *)real_write_internal, p1, p2,
                        (void *)module.base);
}
