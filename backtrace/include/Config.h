#pragma once

#include <stdint.h>
#include <cstddef>

constexpr uint64_t BACKTRACE = 0x1;                 // 记录堆栈
constexpr uint64_t TRACK_ALLOCS = 0x2;              // 记录内存申请动作
constexpr uint64_t BACKTRACE_SPECIFIC_SIZES = 0x4;  // 记录特定大小的内存申请
constexpr uint64_t RECORD_MEMORY_PEAK = 0x8;        // 记录内存峰值
constexpr uint64_t DUMP_ON_SIGNAL = 0x80;           // 信号触发dump

class Config {
public:
    bool Init();

    uint64_t options() const { return options_; }

    int backtrace_dump_signal() const { return backtrace_dump_signal_; }

    size_t backtrace_frames() const { return backtrace_frames_; }
    bool backtrace_dump_on_exit() const { return backtrace_dump_on_exit_; }
    const char* backtrace_dump_prefix() const { return backtrace_dump_prefix_; }

    size_t backtrace_min_size_bytes() const { return backtrace_min_size_bytes_; }
    size_t backtrace_max_size_bytes() const { return backtrace_max_size_bytes_; }

    size_t backtrace_dump_peak_val() const { return backtrace_dump_peak_val_; }

private:
    int backtrace_dump_signal_ = 0;

    size_t backtrace_frames_ = 0;
    bool backtrace_dump_on_exit_ = false;
    const char* backtrace_dump_prefix_;

    size_t backtrace_min_size_bytes_ = 0;
    size_t backtrace_max_size_bytes_ = 0;

    size_t backtrace_dump_peak_val_ = 0;

    uint64_t options_ = 0;
};