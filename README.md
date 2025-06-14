# liballoc_hook.so

use to get malloc and free backtrace, include dmabuffer by hook `ioctl` and `close`

* how to build

  * ./build_android.sh [armeabi-v7a]

* how to use
  * adb shell mkdir `path`, where `path` is the output path for the unwindstack, default: `/data/local/tmp/trace/`
  * LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=. ls
  * then replace `ls` to you real command
  * 如果要抓 trace 请先创建 trace 的输出目录
  * 使用该工具导致程序运行过慢时，可以指定环境 `BACKTRACE_MIN_SIZE` 值，不记录小内存的堆栈信息

* checkpoint
  * 支持在程序指定位置插入检查点，输出当前时刻的未释放的内存的堆栈信息
  * 第一种方式：使用信号的方式触发堆栈输出，默认信号值为 33，可以在上述配置文件 Config.cpp 中修改，trace 文件以当前时间命名
    ``` C++
      #include <unistd.h>
      #include <signal.h>
      #include <errno.h>
      #include <stdlib.h>
      #include <string.h>

      if (kill(getpid(), 33) == -1) {
        fprintf(stderr, "Error in file %s at line %d: %s\n", __FILE__, __LINE__, strerror(errno));
        exit(1);
      }
    ```
  * 第二种方式：在代码中插入调用，支持用户自定义文件名，前提需要用户的可执行程序依赖该项目编译生成的 so, 例如在 cmake 文件中做出下面的修改
    ```
      add_library(hook SHARED IMPORTED)
      set(HOOK_IMPORTED_LOCATION "${CMAKE_CURRENT_SOURCE_DIR}/liballoc_hook.so")
      set_target_properties(hook PROPERTIES IMPORTED_LOCATION "${HOOK_IMPORTED_LOCATION}")

      add_executable(xxxx ./code/xxxxx.cpp)
      target_link_libraries(xxxx hook)
    ```
    用户代码需做以下修改
    ``` C++
      extern "C" void checkpoint(const char* file_name);
      int main() {
          size_t size = 200 * 1024 * 1024;
          void* addr_mmap = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
          checkpoint("/data/local/tmp/trace/check_point.1.txt");
          void* addr_malloc = malloc(size * 2);
          
          return 0;
      }
    ```
    如果以 SHARED 方式生成了 liballoc_hook.so 共享库，则需要将 liballoc_hook.so 文件放在可执行程序可读取的目录中，例如
    ``` bash
      adb push /path/liballoc_hook.so ${test_dir}

      adb exec-out "cd ${test_dir}; \
                export LD_LIBRARY_PATH=.; \
                ./xxxx;"
    ```
  * 第三种方式：使用 `dlsym` 打开 checkpoint 符号，并用 `LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=. ls` 的方式执行
  ```c++
      typedef void (*checkpoint_func)(const char*);
      auto checkpoint = (checkpoint_func)dlsym(RTLD_DEFAULT, "checkpoint");
      if (checkpoint) {
        checkpoint("/data/local/tmp/trace/check_point.1.txt");
      }
  ```

* 如何改造自己的被测试程序以便此工具能`有效`采样

  另外在采样过程中，也请务必保证程序处于`停止`状态，常见的做法是在被测试的代码适当位置加上 checkpoint() 或者 kill(getpid(), 33) 以便触发采样，
  下面解释一下什么叫做`适当`位置

  ```
  比如典型的 pipline:
  create_ctx()
  ....
  use_ctx()
  ....
  free_ctx()
  ```

  一般期望是 free_ctx 后应该资源都释放了（除开常驻的外), 我们改造上述 pipline 来加入 checkpoint()/kill, 以便让此工具有采样点

  ```
  for(;;)
  {
  	create_ctx()
  	....
  	use_ctx()
  	....
  	free_ctx()
  	checkpoint() or kill(getpid(), 33)
  }
  ```

  典型的，我们需要让整个 pipline 运行几次分别得到不同运行次数的采样 trace, 当然除了 checkpoint/kill 让程序`暂停`外，我们也可以通过 `gdb` 等调试工具来达到相同的目的。关键在于加的位置，一定是你认为这个点应该释放了资源，比如典型的上面的 free_ctx，一定不要在其他点去进行采样，比如 `use_ctx` 阶段，这个时候本身属于内存用量高峰，即使没有释放也不能说明`泄漏`对吧。

* 抓取峰值步骤
  - 首先运行一次程序，当程序结束时，会输出如下信息
  ```
  +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
  host peak used: 415MB, dma peak used 206MB, total peak used: 619MB
  ```
  - 然后，根据 total peak used 的值，通过环境变量 `DUMP_PEAK_VALUE_MB` 设置 backtrace_dump_peak_val_ 的值，通常要小于 total peak used 50MB 左右，设置方式如下
  ```
  export DUMP_PEAK_VALUE_MB=xxx

  或者

  DUMP_PEAK_VALUE_MB=xxx LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=. ls
  ```
  - DUMP_PEAK_VALUE_MB 的单位默认为 MB

* 内存泄露分析步骤
  - 利用 cheakpoint 机制执行两次程序，并对两次的内存调用堆栈输出进行对比，分析内存调用的增量，此时的内存调用是以时间排序，可以从后向前对比
  ``` c++
    void test() {
      ....
    }

    int main() {
       for (int i = 0; i < 2; ++i) {
        test();
        kill(getpid(), 33);
       }
    }

  ```

* 在线 trace 抓取
  以相机程序为例：
  - 首先打开相机，使用 top / ps / pidof / pgrep 查看相机服务进程 id, 相机服务名称一般为 camerahalserver
  - 查看相机服务的 cmdline 信息, `cat /proc/<pid>/cmdline`
  - 查看启动命令
    - 使用 `find -L /system -name "*.rc" | xargs grep "CMDLINE"` 搜索相机服务启动命令, "CMDLINE" 为第二步的输出
      - 该步骤输出为: "/path/xx.rc", 例如, "/system/vendor/etc/init/camerahalserver.rc"
    - 使用 `cat /path/xx.rc` 查看启动命令所需的参数，"/path/xx.rc" 为上一步骤的输出, rc 文件的格式如下
      ```
      service <name> <pathname> [ <argument> ]*
       <option>
       <option>
      ```
    如下所示, 启动命令为 /vendor/bin/hw/camerahalserver [或者, `start camerahalserver`], 一般需要在根目录启动服务
      ```
      service camerahalserver /vendor/bin/hw/camerahalserver
        class main
        user cameraserver
        group audio camera input drmrpc sdcard_rw system media graphics
        ioprio rt 4
        capabilities SYS_NICE
        task_profiles CameraServiceCapacity MaxPerformance
      ```
  - 使用 `stop camerahalserver` 停止相机服务
  - 根据第三步的 rc 文件的输出, 重新启动服务, 此时, 可以增加 LD_PRELOAD 抓取堆栈, 如果相机服务卡顿严重，建议使用 BACKTRACE_MIN_SIZE 过滤小于 1KB 的内存
    ```
    LD_PRELOAD=liballoc_hook.so LD_LIBRARY_PATH=/path /vendor/bin/hw/camerahalserver
    ```
  - 在拍照完后, 使用 `kill -33 <pid>` 输出当前时刻的堆栈
    - 一般返回桌面，等待几秒再调用 kill 命令发送信号，保证相机程序申请的内存已经释放，防止统计错误

* 配置参数意义
  - `backtrace_dump_on_exit_`: 程序退出时，打印堆栈
  - `backtrace_frames_`: 抓取堆栈的最大深度，默认 128
  - `backtrace_dump_prefix_`: 输出堆栈信息的文件名前缀
  - `BACKTRACE_SPECIFIC_SIZES`: 分配内存时，是否抓取指定 alloc size 的堆栈信息
  - `backtrace_min_size_bytes_`: 开启 BACKTRACE_SPECIFIC_SIZES 标志时，抓取 alloc size 大于该值的堆栈信息
  - `backtrace_max_size_bytes_`: 开启 BACKTRACE_SPECIFIC_SIZES 标志时，抓取 alloc size 小于该值的堆栈信息
  - `RECORD_MEMORY_PEAK`: 是否抓取峰值时刻的堆栈信息
  - `backtrace_dump_peak_val_`: 当峰值大于该值时，记录峰值时刻的堆栈
  - `DUMP_ON_SIGNAL`: 开启 checkpoint 信号机制
  - `backtrace_dump_signal_`: checkpoint 信号机制的信号值，默认 33
  - `DUMP_PEAK_VALUE_MB`：环境变量，单位: MB，当内存峰值大于该值时记录峰值内存
  - `BACKTRACE_MIN_SIZE`：环境变量，单位: Byte，当申请内存的 size 大于该值时，才抓取堆栈信息
  - `配置文件位于 backtrace/src/Config.cpp, 可在该文件中修改上述参数`