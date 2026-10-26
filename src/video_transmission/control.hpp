#ifndef CONTROL_HPP
#define CONTROL_HPP

#include <future>
#include <vector>
#include <video_transmission/config.hpp>
namespace video_transmission {
struct ControlMessage {
    uint32_t tag = 0;
    std::vector<uint8_t> payload;
};
enum class ControlStatus { Applied, Rejected, NotSent, Unknown, Cancelled, QueueFull };
struct ControlReply {
    ControlStatus status = ControlStatus::NotSent;
    ControlMessage message;
};
class ControlSession {
public:
    ControlSession();
    ~ControlSession();
    bool Start(const VideoTransmissionConfig&, std::function<ControlMessage(const ControlMessage&)> handler = {});
    std::future<ControlReply> SendControl(
        ControlMessage, std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
// Explicit module management call; credentials are supplied by the caller and never logged.
ControlReply ExecuteModuleAt(const VideoTransmissionConfig&, const std::string& command,
    const std::string& user = {}, const std::string& password = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1000));
}  // namespace video_transmission

#endif  // CONTROL_HPP
