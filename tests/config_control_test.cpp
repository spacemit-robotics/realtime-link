#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <filesystem>  // NOLINT(build/c++17)
#include <fstream>
#include <thread>
#include <video_transmission/config.hpp>
#include <video_transmission/control.hpp>
using namespace video_transmission;  // NOLINT(build/namespaces)
uint16_t Port() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (sockaddr*)&a, sizeof(a)) == 0);
    socklen_t n = sizeof(a);
    getsockname(fd, (sockaddr*)&a, &n);
    close(fd);
    return ntohs(a.sin_port);
}
int main(int argc, char** argv) {
    assert(argc == 2);
    auto base = std::filesystem::path(argv[1]);
    auto sky = LoadConfig((base / "sky.yaml").string());
    auto ground = LoadConfig((base / "ground.yaml").string());
    assert(sky.role == Role::Sky && ground.role == Role::Ground);
    bool rejected = false;
    try {
        RequireRole(sky, Role::Ground);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    auto dual = LoadConfig((base / "dual_sky.yaml").string());
    assert(!ResolveLink(dual).netns.empty());
    dual.bindings["air-link"].netns = "";
    rejected = false;
    try {
        ValidateConfig(dual);
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    auto folder =
        std::filesystem::temp_directory_path() / ("fpv-config-test-" + std::to_string(getpid()));
    std::filesystem::create_directory(folder);
    {
        std::ofstream f(folder / "bad.yaml");
        f << "schema_version: 2\nrole: sky\nrole: ground\n";
    }
    rejected = false;
    try {
        LoadConfig((folder / "bad.yaml").string());
    } catch (...) {
        rejected = true;
    }
    assert(rejected);
    std::filesystem::remove_all(folder);
    auto port = Port();
    for (auto* c : {&sky, &ground}) {
        c->connections.sky.device_ip = "127.0.0.1";
        c->connections.ground.device_ip = "127.0.0.2";
        c->control_port = port;
    }
    ControlSession server, client;
    assert(server.Start(
        sky, [](const ControlMessage& m) { return ControlMessage{m.tag + 1, m.payload}; }));
    assert(client.Start(ground));
    auto f = client.SendControl({0xa006, {1, 2, 3}});
    assert(f.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    auto reply = f.get();
    assert(reply.status == ControlStatus::Applied &&
        reply.message.payload == std::vector<uint8_t>({1, 2, 3}));
    client.Stop();
    server.Stop();
    // No listener: requests terminate rather than hanging during stop/reconnect.
    assert(client.Start(ground));
    auto nopeer = client.SendControl({0xa006, {}}, std::chrono::milliseconds(100));
    assert(nopeer.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(nopeer.get().status == ControlStatus::NotSent);
    client.Stop();
    // A sent non-idempotent request without a reply is Unknown, never auto-retried.
    assert(server.Start(sky, [](const ControlMessage& m) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        return ControlMessage{m.tag + 1, {}};
    }));
    assert(client.Start(ground));
    auto unknown = client.SendControl({0xa00a, {}}, std::chrono::milliseconds(70));
    assert(unknown.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    assert(unknown.get().status == ControlStatus::Unknown);
    client.Stop();
    server.Stop();
}
