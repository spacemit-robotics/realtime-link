#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <video_transmission/video_transmission.hpp>

namespace {
using namespace video_transmission;  // NOLINT(build/namespaces)

struct ConfigFiles {
    std::filesystem::path folder;
    ConfigFiles() {
        auto pattern = (std::filesystem::temp_directory_path() / "video_transmission-qp-XXXXXX").string();
        const char* path = mkdtemp(pattern.data());
        if (!path) throw std::runtime_error("mkdtemp failed");
        folder = path;
        std::ofstream root(folder / "sky.yaml");
        root << "schema_version: 2\ndeployment_profile: profile.yaml\n"
                "role: sky\nlocal_device: sky\nconnections:\n"
                "  sky: {device: sky, device_ip: 127.0.0.1, module_ip: 127.0.0.3, "
                "connection_id: air}\n"
                "  ground: {device: ground, device_ip: 127.0.0.2, module_ip: 127.0.0.4, "
                "connection_id: ground}\n";
        root.close();
        if (!root) throw std::runtime_error("write root config failed");
    }
    ~ConfigFiles() {
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    }
    VideoTransmissionConfig Load(const std::string& fields) {
        std::ofstream profile(folder / "profile.yaml");
        profile << "schema_version: 1\ntransport: udp\nbindings: {air: {interface: ''}}\n"
                    "pipeline:\n  width: 640\n  height: 400\n"
                << fields;
        profile.close();
        if (!profile) throw std::runtime_error("write profile failed");
        return LoadConfig((folder / "sky.yaml").string());
    }
};

template <class F>
void Rejected(F action) {
    bool rejected = false;
    try {
        action();
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    assert(rejected);
}
}  // namespace

int main() {
    ConfigFiles files;
    auto config = files.Load("");
    assert(config.pipeline.min_qp == 10 && config.pipeline.max_qp == 48);
    assert(DescribeConfig(config).find("qp=10..48") != std::string::npos);
    config = files.Load("  min_qp: 25\n  max_qp: 48\n");
    assert(config.pipeline.min_qp == 25 && config.pipeline.max_qp == 48);
    assert(DescribeConfig(config).find("qp=25..48") != std::string::npos);
    config = files.Load("  min_qp: 25\n");
    assert(config.pipeline.min_qp == 25 && config.pipeline.max_qp == 48);
    config = files.Load("  max_qp: 32\n");
    assert(config.pipeline.min_qp == 10 && config.pipeline.max_qp == 32);
    for (const auto& fields : {"  min_qp: 0\n  max_qp: 51\n", "  min_qp: 51\n  max_qp: 51\n",
            "  min_qp: 25\n  max_qp: 25\n"}) {
        ValidateConfig(files.Load(fields));
    }
    for (const auto& fields :
        {"  min_qp: 49\n", "  max_qp: 52\n", "  min_qp: 52\n", "  min_qp: 0\n  max_qp: 0\n",
            "  min_qp: -1\n", "  max_qp: -1\n", "  min_qp: 25.5\n", "  max_qp: nope\n",
            "  min_qp: 4294967296\n", "  min_qp: 25\n  min_qp: 26\n", "  minqp: 25\n"}) {
        Rejected([&] { files.Load(fields); });
    }

    // Direct C++ callers receive the same validation, before resources are opened.
    config = files.Load("");
    for (auto range : {std::pair{26U, 25U}, std::pair{0U, 0U}, std::pair{0U, 52U},
            std::pair{std::numeric_limits<uint32_t>::max(), 51U}}) {
        config.pipeline.min_qp = range.first;
        config.pipeline.max_qp = range.second;
        Rejected([&] { ValidateConfig(config); });
    }
    // Encoded passthrough does not create an encoder or apply encoder settings.
    config.input_mode = InputMode::Encoded;
    ValidateConfig(config);
    assert(DescribeConfig(config).find("qp=") == std::string::npos);
    std::puts("qp_config_test: PASS (no hardware or sockets opened)");
}
