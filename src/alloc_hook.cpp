#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <dlfcn.h>
#include <fcntl.h>
#include <link.h>

#include "DebugData.h"
#include "PointerData.h"
#include "malloc_debug.h"
#include "memory_hook.h"

#define RESOLVE(name)                                                              \
    do {                                                                           \
        if (m_sys_##name == nullptr) {                                             \
            void* handle = dlopen("libc.so", RTLD_LAZY);                           \
            if (handle) {                                                          \
                auto addr = dlsym(handle, #name);                                  \
                if (addr) {                                                        \
                    m_sys_##name = reinterpret_cast<decltype(m_sys_##name)>(addr); \
                }                                                                  \
                dlclose(handle);                                                   \
            }                                                                      \
        }                                                                          \
    } while (0)

struct InitState {
    InitState() { allocHook_setup = true; }
    ~InitState() { allocHook_setup = false; }
    static volatile bool allocHook_setup;
};
volatile bool InitState::allocHook_setup = false;

namespace {

struct AddressRange {
    uintptr_t begin = 0;
    uintptr_t end = 0;

    bool Contains(const void* address) const {
        uintptr_t value = reinterpret_cast<uintptr_t>(address);
        return begin <= value && value < end;
    }

    bool Valid() const { return begin < end; }
};

AddressRange g_libc_range;

int CollectLibcRange(dl_phdr_info* info, size_t, void*) {
    if (info == nullptr || info->dlpi_name == nullptr) {
        return 0;
    }

    const char* so_name = strrchr(info->dlpi_name, '/');
    so_name = (so_name == nullptr) ? info->dlpi_name : so_name + 1;
    if (strcmp(so_name, "libc.so") != 0) {
        return 0;
    }

    uintptr_t begin = UINTPTR_MAX;
    uintptr_t end = 0;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i) {
        const ElfW(Phdr)& phdr = info->dlpi_phdr[i];
        if (phdr.p_type != PT_LOAD || phdr.p_memsz == 0) {
            continue;
        }

        uintptr_t seg_begin = info->dlpi_addr + phdr.p_vaddr;
        uintptr_t seg_end = seg_begin + phdr.p_memsz;
        if (seg_begin < begin) {
            begin = seg_begin;
        }
        if (seg_end > end) {
            end = seg_end;
        }
    }

    if (begin < end) {
        g_libc_range.begin = begin;
        g_libc_range.end = end;
        return 1;
    }
    return 0;
}

void CacheLibcRange() {
    if (!g_libc_range.Valid()) {
        dl_iterate_phdr(CollectLibcRange, nullptr);
    }
}

bool ShouldBypassLibcInternalMmap(const void* caller) {
    CacheLibcRange();
    return g_libc_range.Valid() && g_libc_range.Contains(caller);
}

}  // namespace

class AllocHook {
public:
    AllocHook() {
        InitState state;
        void* ptr[2] = {&Db_storage, &Pd_storage};
        debug_initialize(ptr);
        CacheLibcRange();
    }
    ~AllocHook() { debug_finalize(); }

    void* malloc(size_t size) { return debug_malloc(size); }
    void free(void* ptr) { debug_free(ptr); }
    void* calloc(size_t a, size_t b) { return debug_calloc(a, b); }
    void* realloc(void* ptr, size_t size) { return debug_realloc(ptr, size); }
    void* aligned_alloc(size_t alignment, size_t size) {
        return debug_aligned_alloc(alignment, size);
    }
    int posix_memalign(void** ptr, size_t alignment, size_t size) {
        return debug_posix_memalign(ptr, alignment, size);
    }
    void* memalign(size_t alignment, size_t bytes) { return debug_memalign(alignment, bytes); }
    void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return debug_mmap(addr, size, prot, flags, fd, offset);
    }
    int munmap(void* addr, size_t size) { return debug_munmap(addr, size); }
    int ioctl(int fd, int request, void* arg) { return debug_ioctl(fd, request, arg); }
    int close(int fd) { return debug_close(fd); }
    void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
        return debug_mmap64(addr, size, prot, flags, fd, offset);
    }

    void checkpoint(const char* file_name) { return debug_dump_heap(file_name); }

    static AllocHook& inst();

private:
    static std::aligned_storage<sizeof(DebugData), alignof(DebugData)>::type Db_storage;
    static std::aligned_storage<sizeof(PointerData), alignof(PointerData)>::type
            Pd_storage;
};
std::aligned_storage<sizeof(DebugData), alignof(DebugData)>::type AllocHook::Db_storage;
std::aligned_storage<sizeof(PointerData), alignof(PointerData)>::type
        AllocHook::Pd_storage;

AllocHook& AllocHook::inst() {
    static AllocHook hook;
    return hook;
}

static volatile bool in_preinit_phase = true;
__attribute__((constructor(201))) void mark_init_done() {
    in_preinit_phase = false;
}

extern "C" {
// 程序初始化会间接调用 malloc 和 free
void* malloc(size_t size) {
    RESOLVE(malloc);
    if (InitState::allocHook_setup) {
        return m_sys_malloc(size);
    }
    return AllocHook::inst().malloc(size);
}

void free(void* ptr) {
    RESOLVE(free);
    if (InitState::allocHook_setup) {
        return m_sys_free(ptr);
    }
    return AllocHook::inst().free(ptr);
}

// calloc 和 realloc 属于用户级函数
void* calloc(size_t a, size_t b) {
    RESOLVE(calloc);
    if (InitState::allocHook_setup) {
        return m_sys_calloc(a, b);
    }
    return AllocHook::inst().calloc(a, b);
}

void* realloc(void* ptr, size_t size) {
    RESOLVE(realloc);
    if (InitState::allocHook_setup) {
        return m_sys_realloc(ptr, size);
    }
    return AllocHook::inst().realloc(ptr, size);
}

void* aligned_alloc(size_t alignment, size_t size) {
    RESOLVE(aligned_alloc);
    if (InitState::allocHook_setup) {
        return m_sys_aligned_alloc(alignment, size);
    }
    return AllocHook::inst().aligned_alloc(alignment, size);
}

void* memalign(size_t alignment, size_t bytes)  {
    RESOLVE(memalign);
    return AllocHook::inst().memalign(alignment, bytes);
}

// 进程初始化 和 debug init 的过程不应该调用 posix_memalign
int posix_memalign(void** ptr, size_t alignment, size_t size) {
    RESOLVE(memalign);
    RESOLVE(posix_memalign);
    return AllocHook::inst().posix_memalign(ptr, alignment, size);
}

void* mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    if (fd < 0 && ShouldBypassLibcInternalMmap(__builtin_return_address(0))) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap(addr, size, prot, flags, fd, offset);
    return result;
}

int munmap(void* addr, size_t size) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (int)syscall(SYS_munmap, addr, size);
    }
    return AllocHook::inst().munmap(addr, size);
}

int ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    return AllocHook::inst().ioctl(fd, request, arg);
}

int close(int fd) {
    return AllocHook::inst().close(fd);
}

void* mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (in_preinit_phase || InitState::allocHook_setup) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    if (fd < 0 && ShouldBypassLibcInternalMmap(__builtin_return_address(0))) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }
    void* result = AllocHook::inst().mmap64(addr, size, prot, flags, fd, offset);
    return result;
}

void checkpoint(const char* file_name) {
    AllocHook::inst().checkpoint(file_name);
}
}
