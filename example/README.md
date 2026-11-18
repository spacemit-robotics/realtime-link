# 视频传输示例

本目录包含两种示例：`native/public/` 演示公共会话与 MPP 媒体适配器，`reference/` 演示使用公开参考 UDP 协议传输一个已有的 H.265 access unit（AU）。完整公共 API 和架构内构建方法见 [上级 README](../README.md)。

## Native 公共会话示例

示例空端使用 MPP UVC 获取 MJPEG 帧、解码、可选裁剪缩放，再提交公共会话，由 MPP 编码并经传输插件发送；地面端经插件接收，由 MPP 解码，并可使用 SDL2 显示。相机、MPP 库、插件及其配置须由运行环境提供。`CONFIG` 是传给插件的不透明配置，具体内容及交付方式以插件提供方为准。

这些程序是实验示例，不是相机或显示的强制依赖。应用也可以使用自己的采集、编解码和显示代码，参见下文“接入应用自己的帧”。

### 编译

构建 native 示例需要 CMake、C++ 编译器、yaml-cpp 开发包，以及已经安装的 MPP 头文件和 `libmpp.so`。插件和插件配置需要另行提供。

#### 架构工作区内编译

在架构工作区中加载环境后，进入组件目录并运行 `mm`。将下面的工作区路径替换为实际路径；`$PREFIX` 由 `envsetup.sh` 设置，通常指向工作区的 `output/staging`。

```sh
SDK_ROOT=/absolute/path/to/spacemit_robot
cd "$SDK_ROOT"
source build/envsetup.sh
cd components/realtime-link
mm \
  -DVIDEO_TRANSMISSION_MPP_PREFIX="$PREFIX" \
  -DVIDEO_TRANSMISSION_BUILD_EXAMPLES=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_FRAME_BRIDGE=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_CAMERA=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_MJPEG=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_MEDIA=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_V2D=ON \
  -DVIDEO_TRANSMISSION_BUILD_PUBLIC_NATIVE=ON \
  -DVIDEO_TRANSMISSION_INSTALL_EXAMPLES=ON
```

`mm` 会编译并安装组件；成功后两个公共 native 程序位于 `"$PREFIX/bin/"`：

```sh
ls "$PREFIX/bin/video_transmission_sky_public" \
   "$PREFIX/bin/video_transmission_ground_public"
```

程序也保留在架构构建目录的 `example/` 下。若 MPP 尚未安装，可先在架构中构建 MPP，或按工作区依赖规则使用 `mm --with-deps`。重新配置已有构建目录时，未指定的 CMake 选项可能沿用缓存值。

#### 独立编译

在组件根目录执行以下命令；将 `MPP_PREFIX` 改为 MPP 实际安装前缀：

```sh
MPP_PREFIX=/absolute/path/to/mpp-prefix
cmake -S . -B build \
  -DCMAKE_INSTALL_PREFIX="$PWD/install" \
  -DVIDEO_TRANSMISSION_MPP_PREFIX="$MPP_PREFIX" \
  -DVIDEO_TRANSMISSION_BUILD_EXAMPLES=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_FRAME_BRIDGE=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_CAMERA=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_MJPEG=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_MEDIA=ON \
  -DVIDEO_TRANSMISSION_BUILD_MPP_V2D=ON \
  -DVIDEO_TRANSMISSION_BUILD_PUBLIC_NATIVE=ON \
  -DVIDEO_TRANSMISSION_INSTALL_EXAMPLES=ON
cmake --build build -j
cmake --install build
```

编译结果位于 `build/example/`；安装后位于组件目录下的 `install/bin/`。如只需构建目录中的程序，可将 `VIDEO_TRANSMISSION_INSTALL_EXAMPLES` 设为 `OFF` 并省略安装步骤。安装示例的开关也会安装参考示例及其配置，公开交付前需检查配置内容。

地面 GUI 需要构建时检测到 SDL2 2.0.16 及以上版本，运行时还需可用图形会话；可用 `pkg-config --modversion sdl2` 检查构建环境。缺少 SDL2 时，地面示例仍可无 GUI 运行，但不能使用 `--gui`。公共 native 示例需要运行时提供兼容的传输插件及其配置。

### 启动参数

```text
video_transmission_sky_public PLUGIN CONFIG CAMERA SECONDS CAP_WIDTH CAP_HEIGHT WIDTH HEIGHT FPS BITRATE GOP [ROI_X ROI_Y ROI_WIDTH ROI_HEIGHT]
video_transmission_ground_public PLUGIN CONFIG SECONDS WIDTH HEIGHT [--request-idr] [--gui]
```

- `PLUGIN`：与当前设备架构、公共接口及插件配置相匹配的动态库路径。两端各自使用本机可用的库。
- `CONFIG`：对应端插件的配置文件路径；由插件决定格式和内容。
- `CAMERA`：空端 V4L2 相机设备路径。空端示例要求相机支持指定的 MJPEG 采集模式。
- `SECONDS`：运行秒数，须大于零；可用 `Ctrl+C` 提前结束。
- `CAP_WIDTH`、`CAP_HEIGHT`：相机采集尺寸。`WIDTH`、`HEIGHT`：空端编码和地面端解码尺寸，两端需一致。
- `FPS`：帧率；`BITRATE`：编码目标码率，单位 bit/s；`GOP`：关键帧间隔，单位帧。
- 可选的四个 `ROI_*` 指定空端裁剪区域：左上角坐标和裁剪宽高。须整体提供，区域须在采集图像内；NV12 裁剪参数和输出尺寸需满足偶数对齐要求。
- 地面端 `--gui` 显示画面；不指定时只接收并计数。`--request-idr` 在收到第一帧后请求一次关键帧，要求插件提供支持该动作的控制后端。

### 使用仓库中的连接配置

`config/connections.yaml` 保存原有设备及模组 IP：空端设备 `192.168.144.108`、空端模组 `192.168.144.10`、地面端设备 `192.168.144.101`、地面端模组 `192.168.144.66`。两台设备上的配置应保持一致；如部署网络改变，请在两端同步修改。这里的 IP 是插件配置，不会自动设置操作系统网卡地址。

`config/sky.yaml` 和 `config/ground.yaml` 分别是空端和地面端入口，它们引用同目录的 `connections.yaml`，以及仓库中私有兼容层的 `private/example/config/product/profiles/k3_udp.yaml`。使用产品动态库时，运行环境还需提供该私有 profile 和动态库，且两端的 profile 必须匹配。文件中的相对路径以各 YAML 文件所在目录为基准；示例安装过程不会自动安装这三份产品配置及私有 profile，部署时请保留相对目录结构，或调整入口配置中的 profile 路径。

以下命令以各自设备的组件根目录为工作目录，**先启动地面端**。请按实际工作区路径调整 `cd` 指令，并根据空端相机支持的模式调整尺寸、帧率和裁剪参数；空端编码尺寸与地面端解码尺寸应一致：

```sh
# 地面端：先加载架构环境，再进入组件目录
cd /home/bianbu/workplace/realtimelink_sdk
source build/envsetup.sh
cd components/realtime-link

video_transmission_ground_public \
  ./lib/libvideo_transmission_product.so example/config/ground.yaml \
  3600 1280 720 --gui --request-idr
```

`--gui` 需要可用图形会话；不需要显示时可移除该参数。通过 SSH 启动并希望在地面端本机 Wayland 桌面显示时，在启动命令前设置对应图形会话的环境变量；当前测试环境可使用：

```sh
export XDG_RUNTIME_DIR=/run/user/1000
export WAYLAND_DISPLAY=wayland-0
export SDL_VIDEODRIVER=wayland
```

不同用户或桌面会话的路径和显示名称可能不同，应按实际会话调整。

```sh
# 空端：先加载架构环境，再进入组件目录
cd /root/workplace/spacemit_robot
source build/envsetup.sh
cd components/realtime-link

# 当前测试相机：4000×1200 MJPEG，裁剪后输出 1280×720
CAMERA=/dev/video9
video_transmission_sky_public \
  ./lib/libvideo_transmission_product.so example/config/sky.yaml "$CAMERA" \
  3600 4000 1200 1280 720 30 2000000 30 176 0 1824 1200
```

上述空端参数针对当前测试相机：采集 `4000×1200`，裁剪区域为 `(176, 0, 1824, 1200)`，缩放并编码为 `1280×720`、30 fps。`/dev/video9` 可能随设备枚举发生变化；更换相机时，用 `v4l2-ctl -d "$CAMERA" --list-formats-ext` 查询其 MJPEG 模式，并相应调整采集尺寸、帧率及 ROI。两端的 `3600` 表示运行秒数，可按需修改。

`source` 后，`$PREFIX/bin` 已加入 `PATH`，因此已安装的程序可以在任意目录直接按名称运行；命令中的 `./lib/...` 和 `example/config/...` 是相对于当前组件目录的路径。在其他目录执行时，请将这两个参数改为当前设备上对应文件的绝对路径；地面端选择 `ground.yaml`，空端选择 `sky.yaml`。如果工作区位置不同，请将 `cd` 路径改为实际路径。独立编译且未安装到架构环境时，可分别使用 `./build/example/video_transmission_ground_public` 和 `./build/example/video_transmission_sky_public`。

示例命令末尾四个数字是 ROI 参数；使用其他相机时可替换它们，或在不需要裁剪时删除这四个参数。确保运行时能找到构建产物、MPP 和插件依赖库；必要时将对应库目录加入 `LD_LIBRARY_PATH`。上述参数不是插件配置；实际环境须提供匹配的动态库和私有 profile。

### 控制与运行状态

- 两端均可用 `Ctrl+C` 或 `SIGTERM` 退出；地面 GUI 可关闭窗口或按 `Esc` 退出。
- 地面端 `--request-idr` 是示例提供的关键帧请求开关。发生关键帧需求或丢帧事件时，示例也会尝试请求关键帧；后端不支持时会返回相应状态。
- 应用可构造 `vt_control_request` 并调用 `vt_session_send_control`；实际可用动作取决于控制后端。公共头文件还定义了目标码率和视频查询动作，但本 native 示例没有为它们提供命令行开关，也不保证插件支持。
- 收到 `VT_EVENT_LINK_DOWN` 时，示例尝试重建会话；具体传输插件是否能检测相应链路故障取决于其实现。

- `sky submitted` 是提交给会话的帧数；`ground decoded` / `ground frames` 是解码回调帧数；`gui presented` 是示例调用呈现的计数，不是物理屏幕时延测量。`keyframe_request status=0` 表示请求返回 `VT_OK`，不是画面已恢复的证明。

应用在已启动的地面会话上请求关键帧的示意代码如下；返回值和是否支持取决于后端，不要在会话回调内做长时间控制操作：

```c
#include <video_transmission/video_transmission.h>

vt_status request_keyframe(vt_session *ground) {
    vt_control_request request = {0};
    request.struct_size = sizeof(request);
    request.action = VT_CONTROL_REQUEST_KEYFRAME;
    return vt_session_send_control(ground, &request);
}
```

### 接入应用自己的帧

应用自行从相机取图并编码后，可跳过 MPP 适配器，创建传输后端并以 `VT_INPUT_ENCODED` 创建空端会话，逐个提交完整 H.265 AU。地面端注册 `on_encoded_frame`，复制数据后由应用自行解码和显示：

```c
#include <video_transmission/video_transmission.h>

/* 应用实现：返回前复制数据到自身队列，由其他线程解码、显示。 */
void app_enqueue_h265_copy(void *user, const uint8_t *data, size_t size,
                           uint32_t width, uint32_t height, uint64_t number,
                           uint64_t pts_us, uint32_t flags);

void on_encoded_frame(void *user, const vt_encoded_frame *frame) {
    app_enqueue_h265_copy(user, frame->data, frame->size, frame->width, frame->height,
                          frame->frame_number, frame->pts_us, frame->flags);
}

vt_status send_h265_au(vt_session *sky, const uint8_t *data, size_t size,
                       uint32_t width, uint32_t height, uint64_t number,
                       uint64_t pts_us, uint32_t flags) {
    vt_encoded_frame frame = {0};
    frame.struct_size = sizeof(frame);
    frame.data = data;
    frame.size = size;
    frame.width = width;
    frame.height = height;
    frame.frame_number = number;
    frame.pts_us = pts_us;
    frame.flags = flags; /* 按实际内容设置 VT_FRAME_KEYFRAME / VT_FRAME_PARAMETER_SETS。 */
    return vt_session_submit_encoded(sky, &frame);
}
```

先创建传输后端，填好 `vt_session_config`、`vt_callbacks`、`vt_backends`，调用 `vt_session_create` 和 `vt_session_start`；停止后调用 `vt_session_destroy`。编码帧提交成功后由会话复制，地面接收回调里的数据仅在回调返回前有效。需要传原始帧时，需提供可编码的 `vt_media_backend` 和帧 `retain` / `release`；当前 native 示例的空端在 `sky.cpp` 中调用 `vt_session_submit_raw`，并释放自己持有的帧引用。

## 参考 UDP 示例

默认构建生成 `video_transmission_sky_reference` 和 `video_transmission_ground_reference`。它们不采集、编解码或显示，使用公开参考协议，不能直接与产品协议互通。

复制 `config/reference_sky.yaml` 和 `config/reference_ground.yaml`，按环境修改两端 `bind`、空端 `peer` 和相同端口；本机回环可使用 `127.0.0.1`。先启动地面端，再在其等待时间内发送：

```sh
./build/example/video_transmission_ground_reference --config ground.yaml --output received.au --once
./build/example/video_transmission_sky_reference --config sky.yaml --au-file idr.au
```

`idr.au` 由应用准备，应为包含必要参数集的完整 H.265 关键帧 AU。两个路径相对于组件根目录执行；实际可执行文件位于所用的构建目录。
