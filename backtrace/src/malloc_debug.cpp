#include <signal.h>
#include <sys/mman.h>
#include <sys/param.h>  // powerof2 ---> ((((x) - 1) & (x)) == 0)
#include <unistd.h>
#include <sys/stat.h>
#include <linux/dma-heap.h>

#include <cstring>
#include <string>
#include <unordered_set>
#include <android-base/stringprintf.h>

#include "Config.h"
#include "DebugData.h"
#include "PointerData.h"
#include "debug_disable.h"
#include "malloc_debug.h"

#include "midgard/mali_kbase_ioctl.h"
#include "msm_ksgl/msm_ksgl.h"
#include "mtk_camera/camera_mem.h"

#include "memory_hook.h"

class ScopedConcurrentLock {
public:
    ScopedConcurrentLock() { pthread_rwlock_rdlock(&lock_); }
    ~ScopedConcurrentLock() { pthread_rwlock_unlock(&lock_); }

    static void Init() {
        pthread_rwlockattr_t attr;
        // Set the attribute so that when a write lock is pending, read locks are no
        // longer granted.
#if __ANDROID_API__ >= 23
        pthread_rwlockattr_setkind_np(
                &attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
#endif
        pthread_rwlock_init(&lock_, &attr);
    }

    static void BlockAllOperations() { pthread_rwlock_wrlock(&lock_); }

private:
    static pthread_rwlock_t lock_;
};
pthread_rwlock_t ScopedConcurrentLock::lock_;

DebugData* g_debug;

static void singal_dump_heap(int) {
    if ((g_debug->config().options() & BACKTRACE)) {
        debug_dump_heap(android::base::StringPrintf(
                                "%s.time.%ld.txt",
                                g_debug->config().backtrace_dump_prefix(), time(NULL))
                                .c_str());
    }
}

bool debug_initialize(void* init_space[]) {
    if (!DebugDisableInitialize()) {
        return false;
    }

    DebugData* debug = new (init_space[0]) DebugData();
    if (!debug->Initialize(init_space[1])) {
        DebugDisableFinalize();
        return false;
    }
    g_debug = debug;

    ScopedConcurrentLock::Init();

    if (g_debug->config().options() & DUMP_ON_SIGNAL) {
        struct sigaction enable_act = {};
        enable_act.sa_handler = singal_dump_heap;
        enable_act.sa_flags = SA_RESTART | SA_ONSTACK;
        if (sigaction(
                    g_debug->config().backtrace_dump_signal(), &enable_act, nullptr) !=
            0) {
            return false;
        }
    }

    return true;
}

void debug_finalize() {
    if (g_debug == nullptr) {
        return;
    }

    // Make sure that there are no other threads doing debug allocations
    // before we kill everything.
    ScopedConcurrentLock::BlockAllOperations();

    // Turn off capturing allocations calls.
    DebugDisableSet(true);

    if ((g_debug->config().options() & BACKTRACE) &&
        g_debug->config().backtrace_dump_on_exit()) {
        debug_dump_heap(android::base::StringPrintf(
                                "%s.exit.%ld.txt",
                                g_debug->config().backtrace_dump_prefix(), time(NULL))
                                .c_str());
    }

    if (g_debug->TrackPointers()) {
        g_debug->pointer->DumpPeakInfo();
    }

    // 对于调试工具或在调试模式下运行的代码, 资源管理可能不是首要关注点.
    // 为了避免在清理过程中出现多线程访问冲突, 决定故意不释放这些资源. 包括
    // g_debug、pthread 键等.
}

void debug_dump_heap(const char* file_name) {
    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int fd = open(file_name, O_RDWR | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC, 0644);
    if (fd == -1) {
        return;
    }

    g_debug->pointer->DumpLiveToFile(fd);
    close(fd);
}

static void* InternalMalloc(size_t size) {
    void* result = m_sys_malloc(size);
    if (g_debug->TrackPointers()) {
        g_debug->pointer->Add(result, size);
    }

    return result;
}

static void InternalFree(void* pointer) {
    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(pointer);
    }
    m_sys_free(pointer);
}

void* debug_malloc(size_t size) {
    if (DebugCallsDisabled()) {
        return m_sys_malloc(size);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    return InternalMalloc(size);
}

void debug_free(void* pointer) {
    if (DebugCallsDisabled() || pointer == nullptr) {
        return m_sys_free(pointer);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    InternalFree(pointer);
}

void* debug_realloc(void* pointer, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_realloc(pointer, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (pointer == nullptr) {
        return InternalMalloc(bytes);
    }

    if (bytes == 0) {
        InternalFree(pointer);
        return nullptr;
    }

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(pointer);
    }

    void* new_pointer = m_sys_realloc(pointer, bytes);

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Add(new_pointer, bytes);
    }

    return new_pointer;
}

void* debug_calloc(size_t nmemb, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_calloc(nmemb, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    size_t size;
    if (__builtin_mul_overflow(nmemb, bytes, &size)) {
        // Overflow
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = m_sys_calloc(1, size);
    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, size);
    }

    return pointer;
}

void* debug_memalign(size_t alignment, size_t bytes) {
    if (DebugCallsDisabled()) {
        return m_sys_memalign(alignment, bytes);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (bytes > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* pointer = m_sys_memalign(alignment, bytes);

    if (pointer != nullptr && g_debug->TrackPointers()) {
        g_debug->pointer->Add(pointer, bytes);
    }

    return pointer;
}

int debug_posix_memalign(void** memptr, size_t alignment, size_t size) {
    if (DebugCallsDisabled()) {
        return m_sys_posix_memalign(memptr, alignment, size);
    }

    if (alignment < sizeof(void*) || !powerof2(alignment)) {
        return EINVAL;
    }
    int saved_errno = errno;
    *memptr = debug_memalign(alignment, size);
    errno = saved_errno;
    return (*memptr != nullptr) ? 0 : ENOMEM;
}

namespace DMA_BUF {

static thread_local bool gpu_ioctl_alloc = false;  // TLS to store a unique flag per thread

static bool is_dma_buf(int fd, size_t* size) {
    static std::unordered_set<uint64_t> inode_set;
    std::string fdinfo = android::base::StringPrintf("/proc/self/fdinfo/%d", fd);
    auto fp = std::unique_ptr<FILE, decltype(&fclose)>{fopen(fdinfo.c_str(), "re"), fclose};
    if (fp == nullptr) {
        return false;
    }

    bool is_dmabuf_file = false;
    uint64_t inode = -1;
    char* line = nullptr;
    size_t len = 0;
    while (getline(&line, &len, fp.get()) > 0) {
        switch (line[0]) {
            case 'i':
                if (strncmp(line, "ino:", 4) == 0) {
                    char* c = line + 4;
                    inode = strtoull(c, nullptr, 10);
                }
                break;
            case 's':
                if (strncmp(line, "size:", 5) == 0) {
                    char* c = line + 5;
                    *size = strtoull(c, nullptr, 10);
                }
                break;
            case 'e':
                if (strncmp(line, "exp_name:", 9) == 0) {
                    is_dmabuf_file = true;
                }
                break;
            default:
                break;
        }
    }
    m_sys_free(line);

    if (!is_dmabuf_file) {
        return false;
    }

    if (inode == static_cast<uint64_t>(-1)) {
        // Fallback to stat() on the fd path to get inode number
        std::string fd_path = android::base::StringPrintf("/proc/self/fd/%d", fd);

        struct stat sb;
        if (stat(fd_path.c_str(), &sb) < 0) {
            return false;
        }
        inode = sb.st_ino;
    }

    return inode_set.insert(inode).second;
}

static bool handle_dma_node(unsigned int request, void* arg, int* fd, size_t* size) {
    // delay parsing the backtrace until mmap64.
    auto set_gpu_ioctl_alloc_and_return_false = []() -> bool {
        gpu_ioctl_alloc = true;
        return false;
    };

    switch (request) {
        case KBASE_IOCTL_MEM_ALLOC:
        case KBASE_IOCTL_MEM_ALLOC_EX:
        case IOCTL_KGSL_GPUOBJ_ALLOC:
            return set_gpu_ioctl_alloc_and_return_false();
        // parse the backtrace immediately
        case DMA_HEAP_IOCTL_ALLOC: {
                struct dma_heap_allocation_data* heap = (struct dma_heap_allocation_data*)arg;
                *fd = heap->fd;
            }
            return is_dma_buf(*fd, size);
        case CAM_MEM_ION_MAP_PA: {
                struct CAM_MEM_DEV_ION_NODE_STRUCT* heap = (struct CAM_MEM_DEV_ION_NODE_STRUCT*)arg;
                *fd = heap->memID;
            }
            return is_dma_buf(*fd, size);
        default:
            return false;
    }
}

}  // namespace DMA_BUF

int debug_ioctl(int fd, unsigned int request, void* arg) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_ioctl, fd, request, arg);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    int ret = (int)syscall(SYS_ioctl, fd, request, arg);

    int node_fd = -1;
    size_t node_sz = 0;
    if (g_debug->TrackPointers() && DMA_BUF::handle_dma_node(request, arg, &node_fd, &node_sz)) {
        void* ptr = reinterpret_cast<void*>(node_fd);
        g_debug->pointer->Add(ptr, node_sz, DMA);
    }

    return ret;
}

int debug_close(int fd) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_close, fd);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (g_debug->TrackPointers()) {
        void* ptr = reinterpret_cast<void*>(fd);
        g_debug->pointer->Remove(ptr);
    }

    return (int)syscall(SYS_close, fd);
}

void* debug_mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabled()) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* result = (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);

    if (g_debug->TrackPointers() && DMA_BUF::gpu_ioctl_alloc) {
        DMA_BUF::gpu_ioctl_alloc = false;  // Reset the flag immediately after processing
        g_debug->pointer->Add(result, size, DMA);
    }

    return result;
}

void* debug_mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset) {
    if (DebugCallsDisabled()) {
        return (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (size > PointerInfoType::MaxSize()) {
        errno = ENOMEM;
        return nullptr;
    }

    void* result = (void*)syscall(SYS_mmap, addr, size, prot, flags, fd, offset);
    if (g_debug->TrackPointers()) {
        size_t node_sz = 0;
        if (fd < 0)
            g_debug->pointer->Add(result, size, MMAP);
        else if (DMA_BUF::is_dma_buf(fd, &node_sz)) {
            void* ptr = reinterpret_cast<void*>(fd);
            g_debug->pointer->Add(ptr, node_sz, DMA);
        }
    }

    return result;
}

int debug_munmap(void* addr, size_t size) {
    if (DebugCallsDisabled()) {
        return (int)syscall(SYS_munmap, addr, size);
    }

    ScopedConcurrentLock lock;
    ScopedDisableDebugCalls disable;

    if (g_debug->TrackPointers()) {
        g_debug->pointer->Remove(addr);
    }

    return (int)syscall(SYS_munmap, addr, size);
}
