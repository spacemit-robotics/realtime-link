#ifndef USB_HPP
#define USB_HPP

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <video_transmission/session.hpp>
namespace video_transmission::detail {
class UsbLink {
public:
    ~UsbLink();
    bool Open(const SessionConfig&, bool sky);
    bool Send(const EncodedFrame&);
    bool Receive(EncodedFrame*);
    void Cancel();
    bool Active() const { return running_; }

private:
    void CloseEndpoints();
    bool Transfer(int fd, void* data, size_t size, bool write, int timeout_ms);
    bool Command(uint32_t tag);
    void Controls();
    int video_ = -1, control_in_ = -1, control_out_ = -1, interrupt_ = -1;
    std::atomic<bool> running_{false};
    std::thread control_thread_;
    std::mutex write_mutex_;
    Limits limits_;
};
}  // namespace video_transmission::detail

#endif  // USB_HPP
