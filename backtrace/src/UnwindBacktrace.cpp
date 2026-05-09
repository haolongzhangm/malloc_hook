/*
 * Copyright (C) 2018 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <cxxabi.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
#include "unwindstack/Error.h"

#include <android-base/stringprintf.h>
#include <unwindstack/AndroidUnwinder.h>
#include <unwindstack/Unwinder.h>

#include "UnwindBacktrace.h"

#if defined(__aarch64__)
static uintptr_t SanitizePtr(uintptr_t ptr) {
#if defined(__has_builtin)
#if __has_builtin(__builtin_ptrauth_strip)
    ptr = reinterpret_cast<uintptr_t>(
            __builtin_ptrauth_strip(reinterpret_cast<void*>(ptr), 0));
#endif
#endif
    return ptr;
}

extern "C" size_t android_unsafe_frame_pointer_chase(uintptr_t* buf, size_t num_entries) {
    auto* fp = reinterpret_cast<uintptr_t*>(__builtin_frame_address(0));
    size_t count = 0;

    while (fp != nullptr && count < num_entries) {
        const uintptr_t next = fp[0];
        const uintptr_t lr = fp[1];
        buf[count++] = SanitizePtr(lr);
        if (next <= reinterpret_cast<uintptr_t>(fp)) {
            break;
        }
        fp = reinterpret_cast<uintptr_t*>(next);
    }
    return count;
}
#else
extern "C" size_t android_unsafe_frame_pointer_chase(uintptr_t* buf, size_t num_entries) {
    (void)buf;
    (void)num_entries;
    return 0;
}
#endif

namespace {

constexpr char kUnwindBackendEnv[] = "ALLOC_HOOK_UNWINDER";
constexpr char kBackendFramePointer[] = "frame_pointer";
constexpr char kBackendFramePointerCompat[] = "android_unsafe_frame_pointer_chase";
constexpr char kBackendLibunwindstack[] = "libunwindstack";

unwindstack::AndroidLocalUnwinder& GetLocalUnwinder() {
    [[clang::no_destroy]] static unwindstack::AndroidLocalUnwinder unwinder(
            std::vector<std::string>{"liballoc_hook.so"}, {},
            std::vector<std::string>{
                    "_Z24__init_additional_stacksP18pthread_internal_t",
                    "_Z25__allocate_thread_mappingmm"});
    return unwinder;
}

unwindstack::ErrorCode UnwindWithLibunwindstack(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
    unwindstack::AndroidUnwinderData data(max_frames);
    if (!GetLocalUnwinder().Unwind(data)) {
        frames->clear();
        frame_info->clear();
    } else {
        frames->resize(data.frames.size());
        for (const auto& frame : data.frames) {
            frames->at(frame.num) = frame.pc;
        }
        *frame_info = std::move(data.frames);
    }
    return data.error.code;
}

unwindstack::ErrorCode UnwindWithFramePointer(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
    std::array<uintptr_t, 512> raw_frames{};
    const size_t capture_limit = std::min(max_frames, raw_frames.size());
    const size_t raw_count = android_unsafe_frame_pointer_chase(raw_frames.data(), capture_limit);
    constexpr size_t kInternalFramesToSkip = 4;

    frames->clear();
    frame_info->clear();
    if (raw_count <= kInternalFramesToSkip) {
        return unwindstack::ERROR_UNSUPPORTED;
    }

    frames->reserve(raw_count - kInternalFramesToSkip);
    frame_info->reserve(raw_count - kInternalFramesToSkip);

    for (size_t i = kInternalFramesToSkip; i < raw_count; ++i) {
        unwindstack::FrameData frame{};
        frame.num = frame_info->size();
        frame.pc = raw_frames[i];
        frame.rel_pc = raw_frames[i];
        frames->push_back(frame.pc);
        frame_info->push_back(frame);
        if (frame_info->size() >= max_frames) {
            break;
        }
    }

    return frame_info->empty() ? unwindstack::ERROR_UNSUPPORTED : unwindstack::ERROR_NONE;
}

}  // namespace

UnwindBackend GetConfiguredUnwindBackend() {
    const char* backend = getenv(kUnwindBackendEnv);
    if (backend == nullptr) {
        return UnwindBackend::kLibunwindstack;
    }
    if (strcmp(backend, kBackendFramePointer) == 0 ||
        strcmp(backend, kBackendFramePointerCompat) == 0) {
        return UnwindBackend::kFramePointer;
    }
    if (strcmp(backend, kBackendLibunwindstack) == 0) {
        return UnwindBackend::kLibunwindstack;
    }
    return UnwindBackend::kLibunwindstack;
}

unwindstack::ErrorCode Unwind(
        std::vector<uintptr_t>* frames, std::vector<unwindstack::FrameData>* frame_info,
        size_t max_frames) {
    switch (GetConfiguredUnwindBackend()) {
        case UnwindBackend::kFramePointer:
            return UnwindWithFramePointer(frames, frame_info, max_frames);
        case UnwindBackend::kLibunwindstack:
        default:
            return UnwindWithLibunwindstack(frames, frame_info, max_frames);
    }
}
