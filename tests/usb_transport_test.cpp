#include <sys/stat.h>
#include <unistd.h>

#include <cassert>
#include <condition_variable>
#include <filesystem>  // NOLINT(build/c++17)
#include <mutex>
#include <vector>
#include <video_transmission/session.hpp>
using namespace video_transmission;  // NOLINT(build/namespaces)
int main() {
    auto root =
        std::filesystem::temp_directory_path() / ("fpv-usb-test-" + std::to_string(getpid()));
    std::filesystem::create_directories(root / "sky");
    std::filesystem::create_directories(root / "ground");
    for (auto role : {"sky", "ground"})
        for (auto ep : {"ep1", "ep2", "ep3", "ep4", "ep7"})
            assert(mkfifo((root / role / ep).c_str(), 0600) == 0);
    std::filesystem::remove(root / "ground/ep2");
    std::filesystem::create_symlink(root / "sky/ep1", root / "ground/ep2");
    SessionConfig s;
    s.transport = Transport::Usb;
    s.ffs_root = (root / "sky").string();
    auto g = s;
    g.ffs_root = (root / "ground").string();
    GroundSession ground;
    SkySession sky;
    std::mutex mutex;
    std::condition_variable cv;
    EncodedFrame received;
    ground.OnEncodedFrame([&](EncodedFrame frame) {
        std::lock_guard l(mutex);
        received = frame;
        cv.notify_all();
    });
    assert(ground.Start(g));
    assert(sky.Start(s));
    auto bytes = std::make_shared<std::vector<uint8_t>>(128, 0x67);
    EncodedFrame f;
    f.data = {bytes, bytes->data()};
    f.size = bytes->size();
    f.width = 640;
    f.height = 400;
    f.keyframe = true;
    f.parameter_sets = true;
    f.frame_number = 0x100000004;
    f.timestamp_us = 0x200000003;
    assert(sky.SubmitEncodedFrame(f) == SubmitResult::Accepted);
    {
        std::unique_lock l(mutex);
        assert(cv.wait_for(l, std::chrono::seconds(2), [&] { return received.data != nullptr; }));
    }
    assert(received.size == f.size && received.frame_number == f.frame_number &&
        received.timestamp_us == f.timestamp_us && received.parameter_sets && received.keyframe &&
        received.data.get()[0] == 0x67);
    sky.StopSending();
    ground.StopReceiving();
    std::filesystem::remove_all(root);
}
