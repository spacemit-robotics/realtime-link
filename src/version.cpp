// 构建身份实现：只读编译期注入的宏，保证同一二进制每次输出一致。
#include <video_transmission/version.hpp>

namespace video_transmission {
namespace {
// 宏在 CMakeLists.txt 中用 target_compile_definitions 注入；缺失时保留可读回退值，
// 便于脱离 CMake 直接编译源文件时仍能链接。
#ifndef VIDEO_TRANSMISSION_VERSION
#define VIDEO_TRANSMISSION_VERSION "unknown"
#endif
#ifndef VIDEO_TRANSMISSION_REVISION
#define VIDEO_TRANSMISSION_REVISION "unknown"
#endif
#ifndef VIDEO_TRANSMISSION_BUILD_STAMP
#define VIDEO_TRANSMISSION_BUILD_STAMP "unknown"
#endif
#ifndef VIDEO_TRANSMISSION_BUILD_OPTIONS
#define VIDEO_TRANSMISSION_BUILD_OPTIONS "unknown"
#endif

// 日志行按空格分列、trace note 按 ';' 分列，因此只清洗这两类分隔符；
// '=' 与 ',' 保留（前者是字段内键值分隔，后者供 note 写入时由 WriteNote 兜底替换）。
std::string Normalize(std::string text) {
    for (char& c : text) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';') c = '_';
    }
    return text;
}
}  // namespace

const std::string& SourceRevision() {
    static const std::string revision = VIDEO_TRANSMISSION_REVISION;
    return revision;
}

const std::string& BuildInfo() {
    // 只清洗各字段的取值，字段之间保留空格与字面量分隔，保证日志行可直接阅读。
    static const std::string info = "version=" + Normalize(VIDEO_TRANSMISSION_VERSION) +
        " revision=" + Normalize(VIDEO_TRANSMISSION_REVISION) +
        " built=" + Normalize(VIDEO_TRANSMISSION_BUILD_STAMP) +
        " options=" + Normalize(VIDEO_TRANSMISSION_BUILD_OPTIONS) +
        " compiler=" + Normalize(__VERSION__);
    return info;
}
}  // namespace video_transmission
