# Video Transmission SDK

本组件有两条不同的使用路径，请先选定所需接口：

- **对外集成**：只包含 `<video_transmission/video_transmission.hpp>`，链接
  `libvideo_transmission.so`（C ABI）。应用提供传输后端；也可直接使用公共的
  reference UDP 后端。原始帧编码、解码及设备控制由应用提供媒体/控制后端。
- **板端实时图传示例**：`video_transmission_sky` 使用 MPP 相机和编码器，
  `video_transmission_ground` 使用 MPP 解码器，可用 SDL2 显示。这两个程序
  依赖内部 C++ pipeline/legacy 实现，**不是**单头文件公共 API 的用法。

reference UDP 是独立的版本化协议，不能与板端原有的 product UDP/USB 协议互通。
下文的 reference 单 AU 测试（默认端口 5006）与实时取图送显（当前 profile
端口 5005）也不能混用。

## 公开头文件、库与接口

安装后公开头文件只有
`<prefix>/include/video_transmission/video_transmission.hpp`。旧的
`api.h`、`backend.h`、`reference_udp.h` 已移至
`src/video_transmission/`，仅供源码兼容，不随 SDK 安装；外部代码统一改为：

```cpp
#include <video_transmission/video_transmission.hpp>
```

外部 CMake 项目使用
`find_package(video_transmission CONFIG REQUIRED)` 并链接
`video_transmission::video_transmission`。公共包仅导出此目标；
`legacy_api.hpp`、`mpp_frame.hpp`、相机适配器及 pipeline 头文件均为内部实现，
不能通过已安装的单头文件调用其 C++ 类。

公共 C ABI 的主要类型与入口：

| 用途 | 接口 |
| --- | --- |
| 会话与限制 | `vt_session_config`：角色、编码/原始输入模式、队列帧数/字节数、单 AU 上限、帧龄 |
| 帧与回调 | `vt_encoded_frame`、`vt_raw_frame`、`vt_callbacks`、`vt_event` |
| 后端 | `vt_transport_backend`（必需）、`vt_media_backend`（原始输入或解码输出时提供）、`vt_control_backend`（可选） |
| reference 传输 | `vt_reference_udp_config`：本地/对端 IPv4、端口、单 AU 上限；`vt_reference_udp_create` |
| 生命周期 | `vt_session_create` → `vt_session_start` → `vt_session_submit_encoded/raw` → `vt_session_stop` / `vt_session_destroy` |
| 状态与控制 | `vt_session_get_state`、`vt_session_drain`、`vt_session_send_control` |

配置与后端结构体应填写 `struct_size = sizeof(该结构体)`；带
`abi_version` 的结构体填写 `VT_ABI_VERSION`。先创建并配置后端及回调，
再创建会话；`vt_session_create` 成功后由会话接管后端 context，失败时由
调用者销毁。完整的发送/接收写法见源码树 `example/reference/`。

天空端每次 `vt_session_submit_encoded` 提交**一个完整 H.265 access unit**，
而非任意 UDP 包或整个多帧文件。提交成功后数据已复制进有界队列；原始帧则依靠
`retain/release` 保持生产者缓冲区有效。回调收到的编码数据只在回调期间有效。
回调由会话派发线程串行调用，不要在回调内停止或销毁会话。会话停止后不可重启，
需要重新 `create`；`vt_session_drain` 只等待已接受帧交给传输层，不保证远端收到。

公开参数只覆盖队列、reference UDP 地址/端口、帧的尺寸/PTS/标志等。
`VT_CONTROL_SET_TARGET_BITRATE` 仅转发给应用提供的控制后端；
reference UDP 本身不实现码率、帧率、GOP、QP 或硬件编解码参数调整。
板端示例的相机、编码器和网络配置见 `example/config/`，不属于公共 C ABI。

## 构建公共库和 reference 单 AU 示例

需要 C++20、CMake、线程库；构建 reference 示例还需 `yaml-cpp`，
但不需要 MPP。以下命令在本组件目录运行：

```sh
cmake -S . -B build-reference \
  -DVIDEO_TRANSMISSION_BUILD_LEGACY=OFF \
  -DVIDEO_TRANSMISSION_BUILD_PIPELINE=OFF \
  -DVIDEO_TRANSMISSION_INSTALL_EXAMPLES=ON
cmake --build build-reference -j
ctest --test-dir build-reference --output-on-failure
cmake --install build-reference --prefix /path/to/sdk-prefix
```

仅需公共库、无示例和 `yaml-cpp` 时，可设置
`-DVIDEO_TRANSMISSION_BUILD_EXAMPLES=OFF`。默认安装只包含公共库、单头文件、
README/LICENSE 和 CMake 包；`INSTALL_EXAMPLES=ON` 才安装 reference 可执行文件
与其 YAML。

reference 接收端先运行：

```sh
video_transmission_ground_reference --config example/config/reference_ground.yaml \
  --output /tmp/received.au --once
```

随后在发送端运行：

```sh
video_transmission_sky_reference --config example/config/reference_sky.yaml \
  --au-file /path/to/idr.au
```

`idr.au` 必须是自行提供的**单个完整 H.265 AU**；若后续需要解码，
宜包含 VPS/SPS/PPS 与 IDR。不能拿 `README.md` 充当视频，也不能把整个
多帧 `.h265` 文件直接交给此示例。用 `cmp /path/to/idr.au /tmp/received.au`
核对传输结果。当前双板配置中，sky 为 `192.168.144.108`、ground 为
`192.168.144.101`，reference UDP 使用端口 `5006`；换设备时修改两端
`reference_*.yaml` 的 `bind/peer/port`。此 reference 接收程序不支持 `--gui`。

## 构建和运行板端实时图传

需要 `yaml-cpp`、MPP 头文件与 `libmpp`；地端显示还需
SDL2 >= 2.0.16 和已登录的桌面会话。公共库单独构建不会生成实时
`video_transmission_sky/ground`。配置时必须同时启用 legacy 与 pipeline，
否则 CMake 会报 `PIPELINE requires LEGACY`：

```sh
cmake -S . -B build-native \
  -DVIDEO_TRANSMISSION_BUILD_LEGACY=ON \
  -DVIDEO_TRANSMISSION_BUILD_PIPELINE=ON \
  -DVIDEO_TRANSMISSION_BUILD_GUI=ON \
  -DVIDEO_TRANSMISSION_INSTALL_EXAMPLES=ON
cmake --build build-native -j
ctest --test-dir build-native --output-on-failure
cmake --install build-native --prefix /path/to/sdk-prefix
```

`INSTALL_EXAMPLES=ON` 安装原生可执行文件及其内部动态库，但内部库不会成为
公共 CMake 导出目标。当前安装规则**不安装**原生 `sky.yaml/ground.yaml`、
`connections.yaml` 和 profile；下列命令须在各自设备的组件源码目录运行，
或自行部署整套 `example/config/` 后改用对应路径。使用工作区的
`source build/envsetup.sh`、`mm --with-deps` 时，也须确保构建配置启用了
上述选项。

地端先接好 HDMI、登录桌面，并确认 `ground.yaml` 使用
`output: {kind: decoded_callback}`：

```sh
video_transmission_ground --config example/config/ground.yaml --gui --fullscreen
```

空端随后启动：

```sh
video_transmission_sky --config example/config/sky.yaml
```

按当前部署修改两端 `example/config/connections.yaml` 的设备/模组 IP，以及
`sky.yaml` 的相机设备；`profiles/k3_udp.yaml` 决定板端 UDP、控制端口及
媒体参数。两端需使用相容配置。地端 `--gui` 是 SDL2 实时显示，
`--fullscreen` 只在 `--gui` 下有效；不传 `--gui` 可无窗口接收，
也可使用 `--preview-fifo`。GUI 可选 PTS 缓冲参数
`--playout-delay-ms`、`--playout-max-delay-ms`、`--playout-max-frames`。

原生 sky 示例通过 `example/adapters/camera_adapter.cpp` 打开 UVC/VI；MJPEG
先转为线性 NV12，再把带有效 MPP VB/双 plane 和释放回调的帧交给内部管线。
自接相机需要自行完成采集、像素转换和缓冲区所有权管理；内部
`AdoptMppFrame` 只包装现有帧，不分配 VB 或导入 DMA-BUF。应用层原始帧
和已有 H.265 AU 是两种输入模式，同一会话只选择一种。原生发送、接收
实现分别见 `example/native/sky/main.cpp` 与 `example/native/ground/main.cpp`；
这些内部 C++ 类型不是已安装的公共 API。`--preview-fifo` 将编码流交给
外部播放器，其解码/显示链路不同于 `--gui` 的 SDK 硬解显示。

### 可选 PTS 播放缓冲

```sh
video_transmission_ground --config example/config/ground.yaml --gui \
  --playout-delay-ms 250 --playout-max-delay-ms 400 --playout-max-frames 16
```

默认 `delay=0`（最新帧优先）、`max-delay=600`、`max-frames=16`；只有显式
设置后才使用上例的 250/400/16 档。参数只适用于 `--gui`，须满足
`0 <= delay < max-delay <= 10000`（毫秒）及 `1 <= max-frames <= 120`。
缓冲按发送端 PTS 的**相对间隔**映射到地端单调时钟，不直接相减两机时间；
代际变化、PTS 回滚等会重建播放基准。它使用有界纹理队列并在上传后
释放解码帧，超容量、超时或错过截止时间仍会丢帧以维持实时性。
`max-delay` 是本地队列驻留上限，**不是**采集到屏幕的端到端延迟保证；
该选项不改变发送端、网络协议或 FIFO 播放。可用以下纯软件测试验证
时序和命令行校验：

```sh
ctest --test-dir build-native \
  -R 'video_transmission_pts_playout|video_transmission_playout_cli' \
  --output-on-failure
```

排查时先看地端 `GUI playout: received=... uploaded=... presented=...`：
三项持续增长表示解码帧已进入 GUI 并提交给 SDL；仅有 UDP 包或
`encoded bytes` 不能证明画面已解码。SDL 提交统计也不等同于显示器实际扫描输出。
若 `received=0` 且报 `decoded frame has no physical plane address`，
应确认地端已重新编译并安装当前 pipeline 库；当前实现允许 MPP/VB
不公开物理地址，但仍校验本地 VB、DMA-BUF 与映射布局。

## 分阶段延迟测量

在空端和地端进程启动前分别设置
`VIDEO_TRANSMISSION_TRACE_PATH=/绝对路径/本次新文件.csv`；文件必须可写且
尚不存在，未设置时不写 trace。CSV 的 `pts_us` 用于关联同一源帧，
`mono_us` 是**各自设备**的单调时钟，不能直接跨机相减。可按
`capture_ready → encode_submit → encode_done → tx_first → rx_complete →
decode_submit → decode_done → gui_submit` 分段排查；相机 PTS 到曝光的关系
须按具体驱动核实，不应直接称作光学曝光时间。

需要跨机统计时，在管理链路上同时运行
`example/tools/clock_probe.py server --host <空端管理IP> --peer <地端管理IP> --seconds 100`
与地端的
`example/tools/clock_probe.py client --host <空端管理IP> --seconds 85 --out clock.jsonl`，
再运行：

```sh
python3 example/tools/summarize_latency.py \
  --sky sky.csv --ground ground.csv --clock clock.jsonl \
  --warmup 5 --out summary
```

脚本输出逐帧 `summary.frames.csv` 和统计 `summary.json`；需检查样本关联、
校时 RTT/偏移和异常负值，不可将不同帧的分段 P95 直接相加。该统计脚本
要求 `gui_feedback_received` 等事件，因此须另行启用可选的 Wayland 测量
程序 `VIDEO_TRANSMISSION_BUILD_LATENCY_VIEWER=ON` 并从构建目录运行
`video_transmission_ground_latency`；普通 `--gui` 不提供呈现反馈事件。
`gui_submit` 是显示提交下界，`gui_feedback_received` 是通知上界，
都不等于显示器实际扫描/面板响应；后者仍需光学测量。`--preview-fifo`
由外部播放器再解码，不能把它的显示时刻接在 SDK 解码完成时间之后。
