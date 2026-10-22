#ifndef EXAMPLE_ADAPTERS_MJPEG_DECODER_HPP  // NOLINT(build/header_guard)
#define EXAMPLE_ADAPTERS_MJPEG_DECODER_HPP  // NOLINT(build/header_guard)

#include <video_transmission/legacy_api.hpp>

namespace video_transmission::example {

// 兼容旧示例的类型名称，解码接口和实现已移到公共管线层。
using MjpegDecoder = video_transmission::MjpegDecoder;

}  // namespace video_transmission::example

#endif  // EXAMPLE_ADAPTERS_MJPEG_DECODER_HPP
