#include "memory_hook.h"

void* (*m_sys_malloc)(size_t) = nullptr;
void (*m_sys_free)(void*) = nullptr;
void* (*m_sys_calloc)(size_t, size_t) = nullptr;
void* (*m_sys_realloc)(void*, size_t) = nullptr;
void* (*m_sys_memalign)(size_t, size_t) = nullptr;
void* (*m_sys_aligned_alloc)(size_t, size_t) = nullptr;
int (*m_sys_posix_memalign)(void**, size_t, size_t) = nullptr;
