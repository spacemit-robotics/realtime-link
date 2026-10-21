#ifndef LATENCY_HPP
#define LATENCY_HPP

#include <cstdint>

namespace video_transmission::latency {
// All timestamps are local CLOCK_MONOTONIC microseconds. PTS is an identity,
// never a ground-local clock. Cross-host subtraction requires clock calibration.
uint64_t NowUs();
bool Enabled();
// Set FPV_TRACE_PATH before starting the process. A run must use a new file.
// value carries event-specific metadata (wire sequence, bytes, presentation flags).
// note 是可选诊断文本，写入 CSV 第五列；字段内部用 ';' 分隔，不得含逗号、CR、
// LF（写入前会被替换为 '_'，并截断到 192 字节）。该列只承载诊断信息，不参与
// 任何阶段计算，省略时为空。
void Mark(const char* event, uint64_t pts, uint64_t value = 0, uint64_t at_us = 0,
    const char* note = nullptr);
}  // namespace video_transmission::latency

#endif  // LATENCY_HPP
