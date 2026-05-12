#pragma once
#include <sys/types.h>
#include <cstddef>

extern void* (*m_sys_malloc)(size_t);
extern void (*m_sys_free)(void*);
extern void* (*m_sys_calloc)(size_t, size_t);
extern void* (*m_sys_realloc)(void*, size_t);
extern void* (*m_sys_memalign)(size_t, size_t);
extern void* (*m_sys_aligned_alloc)(size_t, size_t);
extern int (*m_sys_posix_memalign)(void**, size_t, size_t);
