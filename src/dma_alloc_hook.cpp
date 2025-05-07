#include <dlfcn.h>
#include <linux/dma-heap.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <utils/CallStack.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <android/log.h>

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

void print_log_to_file(const char* format, ...) {
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
    //! fd with size time info
    ////! <fd, <size, time>>
    std::map<int, std::pair<size_t, double>> m_alloc_info;
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
                    "unreleased fd: %d, size: %f KB\n", it.first,
                    it.second.first / 1024.0);
        }
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
        auto msg = "pid: " + std::to_string(getpid()) + " fd: " + std::to_string(__fd) +
                   " request: " + std::to_string(__request);
        LOGE("%s", msg.c_str());
        va_list ap;
        va_start(ap, __request);
        void* arg = va_arg(ap, void*);
        va_end(ap);
        int ret = m_ioctl(__fd, __request, arg);
        //! show bactrace when __request is DMA_HEAP_IOCTL_ALLOC
        if (__request == DMA_HEAP_IOCTL_ALLOC) {
            std::lock_guard<std::mutex> lock(m_mutex);
            struct dma_heap_allocation_data* heap_data =
                    (struct dma_heap_allocation_data*)arg;
            m_alloc_count++;
            m_used_size += heap_data->len;
            if (m_used_size > m_dma_peak) {
                m_dma_peak = m_used_size;
            }
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");
            auto non_dma_rss = show_non_dma_info();
            auto _ = m_used_size + non_dma_rss;
            if (_ > m_may_peak_with_dma_and_no_dma) {
                m_may_peak_with_dma_and_no_dma = _;
            }
            print_log_to_file(
                    "dma alloc happened: fd: %d(%d), request: %u size: %f KB used: %f "
                    "KB, peak: %f KB count: %zu \n",
                    __fd, heap_data->fd, __request, heap_data->len / 1024.0,
                    m_used_size / 1024.0, m_dma_peak / 1024.0, m_alloc_count);

            bt();
            show_peak_size();
            show_used_size();
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");

            //! save the alloc info with time info
            m_alloc_info[heap_data->fd] = {heap_data->len, m_timer.get_msecs()};
            //   print_log_to_file("alloc info: fd: %d, time: %ld\n", __fd,
            //   m_alloc_info[heap_data->fd]);
        }

        return ret;
    }

    int close(int __fd) {
        // print_log_to_file("close fd: %d\n", __fd);
        //! check __fd in m_alloc_info
        if (m_alloc_info.find(__fd) != m_alloc_info.end()) {
            //! TODO: recursive_mutex?
            std::lock_guard<std::mutex> lock(m_mutex);
            print_log_to_file(
                    "++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++"
                    "+"
                    "++++\n");
            auto now = m_timer.get_msecs();
            auto diff = now - m_alloc_info[__fd].second;
            auto size = m_alloc_info[__fd].first;
            m_used_size -= size;
            m_alloc_count -= 1;
            print_log_to_file(
                    "dma free happened: fd: %d, size: %f KB, duration: %f ms\n", __fd,
                    size / 1024.0, diff);
            bt();
            m_alloc_info.erase(__fd);
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
