#pragma once

#include <sys/types.h>
#include <cstddef>

bool debug_initialize(void* init_space[]);
void debug_finalize();
void debug_dump_heap(const char* file_name);
void* debug_malloc(size_t size);
void debug_free(void* pointer);
void* debug_realloc(void* pointer, size_t bytes);
void* debug_calloc(size_t nmemb, size_t bytes);
void* debug_memalign(size_t alignment, size_t bytes);
void* debug_aligned_alloc(size_t alignment, size_t bytes);
int debug_posix_memalign(void** memptr, size_t alignment, size_t size);
void* debug_mmap(void* addr, size_t size, int prot, int flags, int fd, off_t offset);
int debug_munmap(void* addr, size_t size);
int debug_ioctl(int fd, unsigned int request, void* arg);
int debug_close(int fd);
void* debug_mmap64(void* addr, size_t size, int prot, int flags, int fd, off_t offset);
