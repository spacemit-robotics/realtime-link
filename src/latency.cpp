#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <video_transmission/latency.hpp>

namespace video_transmission::latency {
namespace {
struct Writer {
    std::mutex mutex;
    FILE* file = nullptr;
    char buffer[65536];
    Writer() {
        const char* path = std::getenv("VIDEO_TRANSMISSION_TRACE_PATH");
        if (path && *path) {
            file = std::fopen(path, "wx");
            if (!file) {
                std::perror("VIDEO_TRANSMISSION_TRACE_PATH");
                std::abort();
            }
            std::setvbuf(file, buffer, _IOFBF, sizeof(buffer));
            std::fputs("event,pts_us,mono_us,value,note\n", file);
        }
    }
    ~Writer() {
        if (file) std::fclose(file);
    }
};
Writer& Output() {
    static Writer writer;
    return writer;
}

// note 内的逗号、CR、LF 和控制字符会破坏 CSV 分列，统一替换为 '_'。
constexpr size_t kMaxNoteBytes = 192;
void WriteNote(FILE* file, const char* note) {
    if (!note || !*note) return;
    char sanitized[kMaxNoteBytes + 1];
    size_t written = 0;
    for (const char* p = note; *p != '\0' && written < kMaxNoteBytes; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        sanitized[written++] = (c == ',' || c < 0x20 || c == 0x7f) ? '_' : static_cast<char>(c);
    }
    std::fwrite(sanitized, 1, written, file);
}
}  // namespace
uint64_t NowUs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
}
bool Enabled() { return Output().file != nullptr; }
void Mark(const char* event, uint64_t pts, uint64_t value, uint64_t at_us, const char* note) {
    auto& out = Output();
    if (!out.file) return;
    if (!at_us) at_us = NowUs();
    std::lock_guard lock(out.mutex);
    std::fprintf(out.file, "%s,%llu,%llu,%llu,", event, (unsigned long long)pts,
        (unsigned long long)at_us, (unsigned long long)value);
    WriteNote(out.file, note);
    std::fputc('\n', out.file);
}
}  // namespace video_transmission::latency
