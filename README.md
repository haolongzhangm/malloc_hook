# dmabuffer_alloc_trace



use to get dma alloc and free backtrace

* how to build

  * ./build_android.sh

* how to use

  * LD_PRELOAD=libdma_alloc_hook.cpp.so LD_LIBRARY_PATH=. ls
  * then replace `ls` to you real command

The hook only writes logs for `svq_test` by default, so helper processes that
inherit `LD_PRELOAD` do not create extra `dma_alloc_hook_<pid>.log` files. A
process-shared lock also ensures that concurrent same-named processes elect one
logger. Set
`DMA_ALLOC_HOOK_PROCESS` to override the executable name when tracing another
target, for example:

  DMA_ALLOC_HOOK_PROCESS=vendor.qti.camera.provider-service_64 \
    LD_PRELOAD=libdma_alloc_hook.so <command>
