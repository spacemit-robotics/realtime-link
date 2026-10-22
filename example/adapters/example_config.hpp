#ifndef EXAMPLE_CONFIG_HPP
#define EXAMPLE_CONFIG_HPP

#include <cstdint>
#include <filesystem>  // NOLINT(build/c++17)
#include <string>
#include <video_transmission/video_transmission.hpp>

namespace video_transmission::example {

/** 示例采集参数；channel 为码流标识，与设备 channel_id 独立。 */
struct CaptureConfig {
    std::string driver = "mpp_uvc";
    std::string device = "/dev/video0";
    std::string format = "mjpeg";
    std::string stream = "left";
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t fps = 30;
    uint32_t depth = 2;
    int device_id = 0;
    int channel_id = 0;
    uint32_t channel = 0;
};

/** 将 SDK 会话参数与示例采集参数分别保存。 */
struct ExampleConfig {
    video_transmission::VideoTransmissionConfig session;
    CaptureConfig capture;
};

/** 加载 v2 根配置及 v1 profile，按声明文件解析路径；配置错误抛出 invalid_argument。 */
ExampleConfig LoadExampleConfig(const std::filesystem::path& path);

/** 校验 SDK 会话，仅 Sky Raw 校验采集；不访问设备，错误抛出 invalid_argument。 */
void ValidateExampleConfig(const ExampleConfig& config);

/** 返回会话链路与采集配置摘要；链路解析失败可抛出 invalid_argument。 */
std::string DescribeExampleConfig(const ExampleConfig& config);

}  // namespace video_transmission::example

#endif  // EXAMPLE_CONFIG_HPP
