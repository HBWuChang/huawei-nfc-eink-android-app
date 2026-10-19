// Minimal arm64 injector: remote mmap + dlopen in an existing Android process.
// Root only. No files are modified; only the target process memory is used.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

struct arm64_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

static int read_file(const char *path, char *buf, size_t size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, size - 1);
    close(fd);
    if (n < 0) return -1;
    buf[n] = 0;
    return (int)n;
}

static int find_pid(void) {
    DIR *dir = opendir("/proc");
    if (!dir) return -1;
    struct dirent *entry;
    int pid = -1;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        char path[64], cmdline[512];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        if (read_file(path, cmdline, sizeof(cmdline)) <= 0) continue;
        if (strstr(cmdline, "android.hardware.nqnfc-service.nxp") != NULL) {
            pid = atoi(entry->d_name);
            break;
        }
    }
    if (pid <= 0) {
        rewinddir(dir);
        while ((entry = readdir(dir)) != NULL) {
            if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
            char path[64], maps[8192];
            snprintf(path, sizeof(path), "/proc/%s/maps", entry->d_name);
            int n = read_file(path, maps, sizeof(maps));
            if (n > 0 && strstr(maps, "nfc_nci.nqx.default.hw.so") != NULL) {
                pid = atoi(entry->d_name);
                break;
            }
        }
    }
    closedir(dir);
    return pid;
}

// Base address of the first mapping of a file whose path ends in suffix.
static uint64_t map_base(int pid, const char *suffix) {
    char path[64], line[1024];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *fp = fopen(path, "re");
    if (!fp) return 0;
    uint64_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        unsigned long start = 0, end = 0, offset = 0;
        char perms[8] = {0};
        int n = 0;
        if (sscanf(line, "%lx-%lx %7s %lx %*s %*s %n", &start, &end, perms, &offset, &n) < 4) continue;
        const char *p = line + n;
        while (*p == ' ') p++;
        size_t len = strlen(p);
        while (len && (p[len - 1] == '\n' || p[len - 1] == '\r')) len--;
        if (len < strlen(suffix)) continue;
        if (strncmp(p + len - strlen(suffix), suffix, strlen(suffix)) != 0) continue;
        if (offset == 0) {
            base = start;
            break;
        }
        if (!base) base = start;
    }
    fclose(fp);
    return base;
}

static int mem_write(int pid, uint64_t addr, const void *data, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = pwrite(fd, data, size, (off_t)addr);
    close(fd);
    return n == (ssize_t)size ? 0 : -1;
}

static int mem_read(int pid, uint64_t addr, void *data, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = pread(fd, data, size, (off_t)addr);
    close(fd);
    return n == (ssize_t)size ? 0 : -1;
}

static int get_regs(int pid, struct arm64_regs *regs) {
    struct iovec iov = {regs, sizeof(*regs)};
    return ptrace(PTRACE_GETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &iov) == 0 ? 0 : -1;
}

static int set_regs(int pid, const struct arm64_regs *regs) {
    struct iovec iov = {(void *)regs, sizeof(*regs)};
    return ptrace(PTRACE_SETREGSET, pid, (void *)(uintptr_t)NT_PRSTATUS, &iov) == 0 ? 0 : -1;
}

// Run a target function with the given args. Returns in x0. LR is deliberately 0:
// a normal return (or a fault) stops the target, and we then restore its state.
static int remote_call(int pid, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t *out) {
    struct arm64_regs saved, call;
    if (get_regs(pid, &saved) != 0) return -1;
    call = saved;
    call.regs[0] = a0;
    call.regs[1] = a1;
    call.regs[2] = a2;
    call.regs[3] = a3;
    call.regs[4] = a4;
    call.regs[5] = a5;
    call.regs[30] = 0;
    call.pc = fn;
    if (set_regs(pid, &call) != 0) return -1;
    if (ptrace(PTRACE_CONT, pid, 0, 0) != 0) return -1;
    int status;
    for (;;) {
        if (waitpid(pid, &status, __WALL) < 0) return -1;
        if (WIFSTOPPED(status)) {
            int sig = WSTOPSIG(status);
            struct arm64_regs now;
            if (get_regs(pid, &now) == 0 && now.pc == 0) {
                *out = now.regs[0];
                return set_regs(pid, &saved);
            }
            // Forward asynchronous or unrelated stops.
            if (ptrace(PTRACE_CONT, pid, 0, sig) != 0) return -1;
            continue;
        }
        return -1;
    }
}


static int find_elf_symbol(const char *path, const char *wanted, uint64_t *value) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < (off_t)sizeof(Elf64_Ehdr)) { close(fd); return -1; }
    unsigned char *buf = malloc((size_t)size);
    if (!buf) { close(fd); return -1; }
    if (pread(fd, buf, (size_t)size, 0) != size) { free(buf); close(fd); return -1; }
    close(fd);
    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_shoff == 0 || eh->e_shentsize != sizeof(Elf64_Shdr) ||
        eh->e_shoff + (uint64_t)eh->e_shnum * sizeof(Elf64_Shdr) > (uint64_t)size) {
        free(buf); return -1;
    }
    Elf64_Shdr *sh = (Elf64_Shdr *)(buf + eh->e_shoff);
    int rc = -1;
    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || sh[i].sh_link >= eh->e_shnum) continue;
        Elf64_Sym *syms = (Elf64_Sym *)(buf + sh[i].sh_offset);
        size_t count = sh[i].sh_size / sizeof(Elf64_Sym);
        uint64_t str_off = sh[sh[i].sh_link].sh_offset;
        for (size_t j = 0; j < count; j++) {
            const char *name = (const char *)(buf + str_off + syms[j].st_name);
            if (strcmp(name, wanted) == 0) {
                *value = syms[j].st_value;
                rc = 0;
                break;
            }
        }
        if (rc == 0) break;
    }
    free(buf);
    return rc;
}

int main(int argc, char **argv) {
    if (geteuid() != 0) {
        fprintf(stderr, "root required\n");
        return 2;
    }
    const char *so = argc > 1 ? argv[1] : "/data/vendor/nfc/qphook/libqpollhook.so";
    if (access(so, R_OK) != 0) {
        fprintf(stderr, "cannot read %s: %s\n", so, strerror(errno));
        return 2;
    }
    int pid = find_pid();
    if (pid <= 0) {
        fprintf(stderr, "nfc HAL process not found\n");
        return 3;
    }

    Dl_info info;
    if (!dladdr((void *)dlopen, &info) || !info.dli_fbase) {
        fprintf(stderr, "dladdr(dlopen) failed\n");
        return 4;
    }
    uint64_t dlopen_off = (uint64_t)dlopen - (uint64_t)info.dli_fbase;
    if (!dladdr((void *)mmap, &info) || !info.dli_fbase) {
        fprintf(stderr, "dladdr(mmap) failed\n");
        return 4;
    }
    uint64_t mmap_off = (uint64_t)mmap - (uint64_t)info.dli_fbase;
    uint64_t open_off = 0;
    if (dladdr((void *)open, &info) && info.dli_fbase)
        open_off = (uint64_t)open - (uint64_t)info.dli_fbase;
    uint64_t dl_base = map_base(pid, "/libdl.so");
    uint64_t c_base = map_base(pid, "/libc.so");
    uint64_t l_base = map_base(pid, "/linker64");
    if (!dl_base || !c_base || !l_base) {
        fprintf(stderr, "target libc/libdl/linker not found (pid %d)\n", pid);
        return 5;
    }
    uint64_t dlerror_off = 0;
    if (dladdr((void *)dlerror, &info) && info.dli_fbase)
        dlerror_off = (uint64_t)dlerror - (uint64_t)info.dli_fbase;
    uint64_t dlopen_addr = dl_base + dlopen_off;
    uint64_t mmap_addr = c_base + mmap_off;
    uint64_t dlerror_addr = dl_base + dlerror_off;
    uint64_t caller_addr = c_base + open_off;
    uint64_t loader_off = 0;
    const char *linker_path = "/apex/com.android.runtime/bin/linker64";
    if (find_elf_symbol(linker_path, "__dl___loader_dlopen", &loader_off) != 0 &&
        find_elf_symbol(linker_path, "__loader_dlopen", &loader_off) != 0 &&
        find_elf_symbol("/system/bin/linker64", "__dl___loader_dlopen", &loader_off) != 0 &&
        find_elf_symbol("/system/bin/linker64", "__loader_dlopen", &loader_off) != 0) {
        fprintf(stderr, "cannot resolve internal loader dlopen offset\n");
        return 5;
    }
    uint64_t loader_dlopen = l_base + loader_off;
    fprintf(stderr, "pid=%d dl_base=0x%llx c_base=0x%llx l_base=0x%llx\n",
            pid, (unsigned long long)dl_base, (unsigned long long)c_base,
            (unsigned long long)l_base);
    fprintf(stderr, "dlopen=0x%llx mmap=0x%llx dlerror=0x%llx loader=0x%llx caller=0x%llx\n",
            (unsigned long long)dlopen_addr, (unsigned long long)mmap_addr,
            (unsigned long long)dlerror_addr, (unsigned long long)loader_dlopen,
            (unsigned long long)caller_addr);

    if (ptrace(PTRACE_ATTACH, pid, 0, 0) != 0) {
        fprintf(stderr, "PTRACE_ATTACH failed: %s\n", strerror(errno));
        return 6;
    }
    int status;
    if (waitpid(pid, &status, __WALL) < 0 || !WIFSTOPPED(status)) {
        fprintf(stderr, "wait attach failed\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 6;
    }

    int rc = 1;
    struct arm64_regs saved;
    if (get_regs(pid, &saved) != 0) {
        fprintf(stderr, "get regs failed\n");
        goto done;
    }

    uint64_t page = 0;
    if (remote_call(pid, mmap_addr, 0, 0x1000, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0, &page) != 0 ||
        page == 0 || (int64_t)page < 0) {
        fprintf(stderr, "remote mmap failed\n");
        goto restore;
    }
    size_t len = strlen(so) + 1;
    if (len > 0x1000 || mem_write(pid, page, so, len) != 0) {
        fprintf(stderr, "write path failed\n");
        goto restore;
    }
    char check[512] = {0};
    mem_read(pid, page, check, sizeof(check) - 1);
    fprintf(stderr, "page=0x%llx path_readback=%s\n", (unsigned long long)page, check);
    uint64_t handle = 0;
    if (remote_call(pid, loader_dlopen, page, RTLD_NOW, caller_addr, 0, 0, 0, &handle) != 0) {
        fprintf(stderr, "remote internal dlopen call failed\n");
        goto restore;
    }
    if (handle == 0) {
        uint64_t errp = 0;
        char errbuf[512] = {0};
        if (remote_call(pid, dlerror_addr, 0, 0, 0, 0, 0, 0, &errp) == 0 && errp)
            mem_read(pid, errp, errbuf, sizeof(errbuf) - 1);
        fprintf(stderr, "dlopen returned NULL: %s\n", errbuf);
        goto restore;
    }
    printf("injected %s into pid %d handle=0x%llx\n", so, pid, (unsigned long long)handle);
    rc = 0;

restore:
    if (set_regs(pid, &saved) != 0) {
        fprintf(stderr, "restore regs failed\n");
        rc = 1;
    }
done:
    ptrace(PTRACE_DETACH, pid, 0, 0);
    return rc;
}
