#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utils/CallStack.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <cstdint>
#include <android/log.h>

// Some Android NDK versions do not expose the legacy ION userspace header,
// while the target device still supports ION_IOC_NEW_ALLOC.
#ifndef ION_IOC_NEW_ALLOC
struct ion_new_allocation_data {
    uint64_t len;
    uint32_t heap_id_mask;
    uint32_t flags;
    uint32_t fd;
    uint32_t unused;
};
#define ION_IOC_MAGIC 'I'
#define ION_IOC_NEW_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_new_allocation_data)
#endif

class Timer {
    std::chrono::high_resolution_clock::time_point m_start;

public:
    Timer() { reset(); }

    void reset() { m_start = std::chrono::high_resolution_clock::now(); }

    double get_secs() const {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_start)
                       .count() *
               1e-9;
    }

    double get_msecs() const { return get_secs() * 1e3; }

    double get_secs_reset() {
        auto ret = get_secs();
        reset();
        return ret;
    }
    double get_msecs_reset() {
        auto ret = get_msecs();
        reset();
        return ret;
    }
};

static bool hook_process_matches() {
    static int cached = -1;
    static pid_t owner_pid = -1;
    static int owner_fd = -1;
    if (owner_pid != -1 && owner_pid != getpid()) {
        // A forked child inherits the cached state and lock fd. Only the
        // process that won the lock may produce the log.
        return false;
    }
    if (cached >= 0) {
        return cached != 0;
    }

    // LD_PRELOAD is inherited by helper/worker processes. Keep one hook log
    // only for the requested executable; the default is the native SVQ test
    // binary used by rawsvq. Use C/POSIX file APIs here because this function
    // can run during shared-library initialization.
    const char* process_filter = getenv("DMA_ALLOC_HOOK_PROCESS");
    if (process_filter == nullptr || process_filter[0] == '\0') {
        process_filter = "svq_test";
    }
    char process_name[256] = {0};
    FILE* cmdline = fopen("/proc/self/cmdline", "r");
    if (cmdline == nullptr || fread(process_name, 1, sizeof(process_name) - 1, cmdline) == 0) {
        if (cmdline != nullptr) {
            fclose(cmdline);
        }
        cached = 0;
        return false;
    }
    fclose(cmdline);

    const char* base = strrchr(process_name, '/');
    base = base == nullptr ? process_name : base + 1;
    if (strcmp(base, process_filter) != 0) {
        cached = 0;
        return false;
    }

    // Multiple same-named target processes can still exist concurrently.
    // Elect exactly one logger without requiring the caller to know its PID.
    owner_fd = open("/data/local/tmp/dma_alloc_hook.owner", O_RDWR | O_CREAT, 0666);
    if (owner_fd < 0 || flock(owner_fd, LOCK_EX | LOCK_NB) != 0) {
        cached = 0;
        if (owner_fd >= 0) {
            syscall(__NR_close, owner_fd);
            owner_fd = -1;
        }
        return false;
    }
    owner_pid = getpid();
    cached = 1;
    return cached != 0;
}

void print_log_to_file(const char* format, ...) {
    if (!hook_process_matches()) {
        return;
    }

    char file_name[64] = {0};
    auto pid = getpid();
    sprintf(file_name, "/data/local/tmp/dma_alloc_hook_%d.log", pid);

    FILE* fp = fopen(file_name, "a+");
    if (fp == NULL) {
        printf("open file %s failed\n", file_name);
        return;
    }
    va_list args;
    va_start(args, format);
    vfprintf(fp, format, args);
    va_end(args);
    fclose(fp);
}

class IoctlHook {
    int (*m_ioctl)(int __fd, int __request, ...);
    int (*m_close)(int __fd);
    std::mutex m_mutex;
    struct AllocationInfo {
        int fd;
        size_t size;
        double alloc_time_ms;
    };

    // A fd number can be reused after close(). The dma-buf inode identifies
    // the underlying buffer and remains stable across that fd reuse.
    //! <dma-buf inode, allocation info>
    std::map<uint64_t, AllocationInfo> m_alloc_info;
    Timer m_timer;
    size_t m_alloc_count = 0;
    size_t m_used_size = 0;
    size_t m_dma_peak = 0;
    //! why is may? now, we only hook dma at every ioctl, so dma_peak is exact peak
    //! but we do not hook all non-dma malloc/free, so we can not get the exact peak a
    //! non-dma so wen can only simulate the total peak by dma_used + non_dma_used
    size_t m_may_peak_with_dma_and_no_dma = 0;

    void bt() {
        android::CallStack stack;
        stack.update();
        android::String8 str = stack.toString();
        print_log_to_file("%s\n", str.string());
    }

    int show_non_dma_info() {
        int rss = -1;
        struct rusage usage;
        getrusage(RUSAGE_SELF, &usage);
        print_log_to_file("NON-DMA info: maxrss: %ld KB\n", usage.ru_maxrss);
        auto parse_line = [](char* line) -> int {
            //! This assumes that a digit will be found and the line ends in " KB".
            int i = strlen(line);
            const char* p = line;
            while (*p < '0' || *p > '9')
                p++;
            i = atoi(p);
            return i;
        };

        char file_name[64] = {0};
        char line_buff[512] = {0};
        auto m_pid = getpid();
        sprintf(file_name, "/proc/%d/status", m_pid);

        auto fp = std::unique_ptr<FILE, int (*)(FILE*)>(fopen(file_name, "r"), fclose);
        if (!fp) {
            return rss;
        }
        char line[128];

        while (fgets(line, 128, fp.get()) != NULL) {
            if (strncmp(line, "VmRSS:", 6) == 0) {
                rss = parse_line(line);
                break;
            }
        }

        return rss * 1024;
    }

    void show_peak_size() {
        print_log_to_file("DMA: peak size: %f KB\n", m_dma_peak / 1024.0);
    }
    void show_used_size() {
        print_log_to_file("DMA: used size: %f KB\n", m_used_size / 1024.0);
    }
    void show_unreleased_fds() {
        for (auto& it : m_alloc_info) {
            print_log_to_file(
                    "unreleased fd: %d, inode: %llu, size: %f KB\n", it.second.fd,
                    static_cast<unsigned long long>(it.first),
                    it.second.size / 1024.0);
        }
    }

    static bool get_inode(int fd, uint64_t* inode) {
        if (inode == nullptr) {
            return false;
        }
        struct stat st = {};
        if (fstat(fd, &st) != 0) {
            return false;
        }
        *inode = static_cast<uint64_t>(st.st_ino);
        return *inode != 0;
    }

    void record_allocation(int fd, size_t size, unsigned int request,
                           const char* allocator_name) {
        uint64_t inode = 0;
        if (!get_inode(fd, &inode)) {
            print_log_to_file(
                    "dma alloc happened but fstat failed: fd: %d, request: %u, size: %f KB\n",
                    fd, request, size / 1024.0);
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        auto inserted = m_alloc_info.emplace(
                inode, AllocationInfo{fd, size, m_timer.get_msecs()});
        if (!inserted.second) {
            print_log_to_file(
                    "duplicate dma alloc inode: %llu, old fd: %d, new fd: %d, size: %f KB\n",
                    static_cast<unsigned long long>(inode), inserted.first->second.fd, fd,
                    size / 1024.0);
            return;
        }
        m_alloc_count++;
        m_used_size += size;
        if (m_used_size > m_dma_peak) {
            m_dma_peak = m_used_size;
        }
        auto non_dma_rss = show_non_dma_info();
        auto total = m_used_size + non_dma_rss;
        if (total > m_may_peak_with_dma_and_no_dma) {
            m_may_peak_with_dma_and_no_dma = total;
        }
        print_log_to_file(
                "dma alloc happened: allocator: %s, fd: %d, inode: %llu, request: %u size: %f "
                "KB used: %f KB, peak: %f KB count: %zu\n",
                allocator_name, fd, static_cast<unsigned long long>(inode), request,
                size / 1024.0, m_used_size / 1024.0, m_dma_peak / 1024.0,
                m_alloc_count);
        bt();
        show_peak_size();
        show_used_size();
    }

public:
    IoctlHook() {
#ifdef __OHOS__
        auto handle = dlopen("libc.so", RTLD_LAZY);
        if (!handle) {
            print_log_to_file("dlopen failed: %s\n", dlerror());
            abort();
        }
        m_ioctl = reinterpret_cast<decltype(m_ioctl)>(dlsym(handle, "ioctl"));
        m_close = reinterpret_cast<decltype(m_close)>(dlsym(handle, "close"));
#else
        m_ioctl = reinterpret_cast<decltype(m_ioctl)>(dlsym(RTLD_NEXT, "ioctl"));
        m_close = reinterpret_cast<decltype(m_close)>(dlsym(RTLD_NEXT, "close"));
#endif
        if (!m_ioctl) {
            print_log_to_file("dlsym failed: %s\n", dlerror());
            abort();
        }
        if (!m_close) {
            print_log_to_file("dlsym failed: %s\n", dlerror());
            abort();
        }
        std::atexit([] {
            IoctlHook::GetInstance().show_non_dma_info();
            IoctlHook::GetInstance().show_peak_size();
            IoctlHook::GetInstance().show_used_size();
            IoctlHook::GetInstance().show_unreleased_fds();
            print_log_to_file(
                    "may NON-DMA+DMA peak size(only for reference, not accuracy, "
                    "depends on DMA alloc frequency): %f KB\n",
                    IoctlHook::GetInstance().m_may_peak_with_dma_and_no_dma / 1024.0);
        });
    }
#undef LOG_TAG
#define LOG_TAG "DMA_ALLOC_HOOK"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

    int ioctl(int __fd, unsigned int __request, ...) {
        va_list ap;
        va_start(ap, __request);
        void* arg = va_arg(ap, void*);
        va_end(ap);

        // Some Android linker layouts can resolve RTLD_NEXT back to this
        // interposer. Bypass the dynamic symbol on re-entry to prevent an
        // unbounded ioctl -> hook recursion.
        static thread_local bool forwarding = false;
        if (forwarding) {
            return static_cast<int>(syscall(__NR_ioctl, __fd, __request, arg));
        }

        if (__request == DMA_HEAP_IOCTL_ALLOC || __request == ION_IOC_NEW_ALLOC) {
            LOGE("pid: %d fd: %d request: %u", getpid(), __fd, __request);
        }
        forwarding = true;
        int ret = m_ioctl(__fd, __request, arg);
        forwarding = false;
        if (ret == 0 && arg != nullptr && __request == DMA_HEAP_IOCTL_ALLOC) {
            struct dma_heap_allocation_data* heap_data =
                    static_cast<struct dma_heap_allocation_data*>(arg);
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");
            auto non_dma_rss = show_non_dma_info();
            auto _ = m_used_size + non_dma_rss;
            if (_ > m_may_peak_with_dma_and_no_dma) {
                m_may_peak_with_dma_and_no_dma = _;
            }
            record_allocation(heap_data->fd, heap_data->len, __request, "dma_heap");
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");
        } else if (ret == 0 && arg != nullptr && __request == ION_IOC_NEW_ALLOC) {
            struct ion_new_allocation_data* ion_data =
                    static_cast<struct ion_new_allocation_data*>(arg);
            record_allocation(ion_data->fd, ion_data->len, __request, "ion");
        } else if (__request == DMA_HEAP_IOCTL_ALLOC || __request == ION_IOC_NEW_ALLOC) {
            LOGE("DMA alloc failed: ret=%d errno=%d", ret, errno);
        }

        return ret;
    }

    int close(int __fd) {
        // print_log_to_file("close fd: %d\n", __fd);
        uint64_t inode = 0;
        if (get_inode(__fd, &inode)) {
            auto it = m_alloc_info.find(inode);
            if (it == m_alloc_info.end()) {
                return m_close(__fd);
            }
            //! TODO: recursive_mutex?
            std::lock_guard<std::mutex> lock(m_mutex);
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");
            auto now = m_timer.get_msecs();
            auto diff = now - it->second.alloc_time_ms;
            auto size = it->second.size;
            m_used_size -= size;
            m_alloc_count -= 1;
            print_log_to_file(
                    "dma free happened: fd: %d, inode: %llu, size: %f KB, duration: %f ms\n",
                    __fd, static_cast<unsigned long long>(inode), size / 1024.0, diff);
            // close() can be called from shared-library static destructors during
            // process shutdown. Collecting a CallStack here may lazily initialize
            // libunwindstack's pthread-key cache after runtime teardown has begun
            // (and can abort there). Keep free accounting, but only capture stacks
            // on the allocation path.
            m_alloc_info.erase(it);
            show_peak_size();
            show_used_size();
            show_unreleased_fds();
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");
        }
        return m_close(__fd);
    }

    static IoctlHook& GetInstance() {
        static IoctlHook instance;
        return instance;
    }
};

extern "C" {
int ioctl(int fd, int request, ...) {
    va_list ap;
    va_start(ap, request);
    void* arg = va_arg(ap, void*);
    va_end(ap);

    return IoctlHook::GetInstance().ioctl(fd, request, arg);
}

int close(int fd) {
    return IoctlHook::GetInstance().close(fd);
}
}
