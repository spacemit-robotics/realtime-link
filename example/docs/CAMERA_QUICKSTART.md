# 相机接入与图传通路搭建

面向首次接入的开发者和智能体。推荐先完成“两台板卡 + 原生 UVC + UDP + 地端 GUI”闭环，再替换相机生产者、接 ROS2 或切换 USB。本文依据当前源码整理；配置样例不是硬件验收结论。

## 1. 先确认输入与交付目标

开始前记录以下事实，未知项应探测或向设备负责人确认，不能从板名或历史 IP 猜测：

- 空端/地端板型、CPU 架构、运行用户、SDK 安装前缀、匹配的 MPP 驱动与插件。
- 相机设备路径或已有话题、实际像素格式、分辨率、FPS，以及是否已有进程占用相机。
- 两端设备 IP、模组管理 IP、接口名、视频端口、控制端口；两端是否在同一物理设备上。
- 需要原始画面、左右目之一还是完整拼接图；输出分辨率、码率以及 GUI/编码回调/解码回调目标。

按输入选择入口：

- **UVC MJPEG/NV12**：直接使用 `fpv_sky`，其 `CameraAdapter` 显式采集；MJPEG 先在 adapter 解码。
- **板载 VI**：`mpp_vi`，当前适配器要求 NV12；传感器和平台插件必须支持所配模式。
- **已有本进程 MPP NV12**：自己的程序调用 `FpvSession::SubmitFrame`，无需重复打开相机。
- **CPU 内存中的 NV12/RGB 或其他设备 SDK**：先实现转换/复制到合法 MPP VB 的 adapter，或使用已实现对应格式转换的 ROS2 桥接；不能把 CPU 指针包装后直接提交。
- **已有 H.265 完整 AU**：使用 `SkySession`；需要门面控制时可选 `FpvSession` 的 `input_mode: encoded`。原生 `fpv_sky` 只支持 Raw 采集，不是文件或码流发送器。
- **已有 ROS2 相机**：见第 7 节。`external` 和 `ros2_*` 都不能由原生 `CameraAdapter` 打开。

实际链路为：相机 → 可选 MJPEG 解码 → MPP NV12 → ROI/缩放 → H.265 编码 → UDP → 重组 AU → H.265 解码 → 回调/GUI。`SkySession`/`GroundSession` 本身只传编码帧。

## 2. 源码、依赖和构建入口

本文相对链接以本目录为基准。优先阅读：

1. [空端完整入口](../native/sky/main.cpp)与[地端完整入口](../native/ground/main.cpp)：真实参数、启动和停止顺序。
2. [采集适配器](../adapters/camera_adapter.cpp)与[示例配置加载器](../adapters/example_config.cpp)：相机如何转换为公共帧。
3. [公共门面](../../include/video_transmission/fpv_session.hpp)与[MPP 帧契约](../../include/video_transmission/mpp_frame.hpp)。
4. [模块构建定义](../../CMakeLists.txt)与[示例构建定义](../CMakeLists.txt)。

仅拿到本模块时，需要额外取得并安装以下依赖，不能假定相邻仓库已存在：

- 通信核心：Linux、C++20、CMake ≥ 3.16、Threads、yaml-cpp 开发包。
- `fpv_pipeline`：带 `mpp::mpp` CMake 导出目标的匹配 MPP SDK、运行库和板端插件。
- 原生空端：MPP SDK 的 UVC/VI 接口及相应板端驱动；不需要独立的 camera SDK。
- 地端窗口：SDL2 ≥ 2.0.16 开发包和可访问的图形桌面。仅运行 headless 不需要 SDL2。

在本模块根目录构建并安装原生空地端（先安装 MPP 到 `FPV_DEPS`；这里必须使用目标板本机编译器，或另行提供正确交叉工具链）：

```bash
export FPV_DEPS=/opt/fpv-deps
export FPV_PREFIX="$HOME/fpv-sdk"
cmake -S . -B build-native \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$FPV_DEPS" \
  -DCMAKE_INSTALL_PREFIX="$FPV_PREFIX" \
  -DFPV_BUILD_PIPELINE=ON -DFPV_BUILD_EXAMPLES=ON \
  -DFPV_BUILD_SKY_EXAMPLE=ON -DFPV_BUILD_GUI=ON
cmake --build build-native --parallel 4
cmake --install build-native
```

纯地端可设 `FPV_BUILD_SKY_EXAMPLE=OFF`；它仍需要 MPP。`FPV_BUILD_GUI=ON` 在缺少 SDL2 时只给出警告，仍可构建，但 `--gui` 会失败。`FPV_BUILD_LATENCY_VIEWER` 是另一个测量程序的开关，不控制普通地端窗口。

本模块的安装不会复制已安装在其他前缀中的 MPP 依赖。运行前设置：

```bash
export PATH="$FPV_PREFIX/bin:$PATH"
export LD_LIBRARY_PATH="$FPV_PREFIX/lib:$FPV_DEPS/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export MPP_PLUGIN_DIR="$FPV_DEPS/lib"
file "$FPV_PREFIX/bin/fpv_sky"
ldd "$FPV_PREFIX/bin/fpv_sky"
```

`MPP_PLUGIN_DIR` 必须指向实际插件目录；MPP 头文件、动态库、插件和 VB ABI 必须匹配。上例使用 `lib`，若安装配置选择 `lib64` 应相应修改。地端同样检查 `fpv_ground`，不能出现 `not found`。x86 构建成功不证明板端媒体能力可用。

若已拥有完整 hub-cam 工作区，可在工作区根目录使用 `scripts/fpv/build_host.sh`，按 MPP → 图传 → 示例安装，之后加载安装前缀的 `fpv_setup.bash`。详细依赖和交叉部署见工作区[构建指南](../../../../docs/FPV_BUILD_INSTALL_USAGE.md)；只分发本模块时该外部链接不在交付范围内。

## 3. 探测相机，复制并修改配置

在空端安装有 v4l-utils 时执行（将设备节点换成实际值）：

```bash
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
fuser /dev/video0
```

选择设备实际支持的 MJPG 或 NV12 尺寸/FPS。MJPG 在 YAML 中写 `mjpeg`。同一相机不能同时由原相机服务和 `fpv_sky` 独占采集；需要复用时走已有帧/话题输入。

两端均复制整套安装配置，保留 profile 相对路径：

```bash
export FPV_CONFIG_DIR="$HOME/fpv-config"
mkdir -p "$FPV_CONFIG_DIR"
cp -a "$FPV_PREFIX/share/fpv_video/config/." "$FPV_CONFIG_DIR/"
```

复制操作用于首次部署；已有配置升级时先比较差异。无需安装即可从源码的 [config 目录](../config)复制同样内容。

必须核对四份文件：

1. [sky.yaml](../config/sky.yaml)：`role: sky`、`local_device` 与连接表空端身份一致，`camera.device` 改成实际节点。
2. [ground.yaml](../config/ground.yaml)：`role: ground`、地端身份一致；GUI 选择 `output.kind: decoded_callback`。
3. [connections.yaml](../config/connections.yaml)：两端设备 IP 和模组 IP、不同 `connection_id`。两端使用相同连接表。
4. [profiles/k3_udp.yaml](../config/profiles/k3_udp.yaml)：采集格式/尺寸/FPS、输出尺寸/ROI/码率、端口和 bindings。

当前源码 profile 是 **4000×1200 MJPEG @30 → 左侧 2000×1200 ROI → 1920×1080、8 Mbit/s**，空端节点示例为 `/dev/video9`。这不是任意 UVC 的默认可用模式。`stream: left` 只是元数据，裁剪必须靠 ROI。

例如，若已确认相机支持 1280×720 MJPEG @30，可将复制后的 profile **整体替换**为以下首轮联调配置（保留入口对该 profile 的引用）：

```yaml
schema_version: 1
transport: udp
port: 5005
rate_mbps: 12
control_port: 8080
control_enabled: true
module_port: 8888
bindings:
  air-link: {interface: "", netns: ""}
  ground-link: {interface: "", netns: ""}
camera:
  driver: mpp_uvc
  format: mjpeg
  width: 1280
  height: 720
  fps: 30
  device_id: 0
  channel_id: 0
  channel: 0
pipeline:
  width: 640
  height: 360
  fps: 30
  bitrate: 4000000
  gop: 30
  roi_x: 0
  roi_y: 0
  roi_width: 0
  roi_height: 0
  encoder_id: 0
  decoder_id: 0
  max_frames: 4
  max_bytes: 33554432
  max_age_ms: 150
```

ROI 四项全零表示完整输入；非零时四项须偶数，区域不能超出实际源图。采集 FPS 必须与 `pipeline.fps` 一致，SDK 不负责重采样。输出宽高须为非零偶数。

`bitrate` 是编码器目标 bit/s；`rate_mbps` 是 UDP 发送节流的 Mbit/s，二者不是同一个参数。需给关键帧和封包留余量，同时不超过实测链路能力。持续排队时先降低编码码率/分辨率并查丢包，不应只扩大队列。

空端 MJPEG 使用 `decoder_id` 创建 VDEC，地端 H.265 解码也使用 VDEC；同一板上须使用不同 VDEC ID，两板可同为 0。`encoder_id` 是 VENC，VENC 0 和 VDEC 0 不因数字相同而冲突。

配置入口为 schema 2，profile 为 schema 1；相对路径按声明文件解析。根 `camera` 字段覆盖 profile 的同名采集字段。`LoadConfig` 只加载 SDK 配置，不打开相机；原生示例实际调用 `LoadExampleConfig` 才读取采集字段。两者不是完全相同的 YAML 解析器：首次接入沿用这里的共有字段和通道 0，不把示例专用 `limits`、`packet_size_bytes` 等直接搬给 SDK 加载器。当前原生示例的 `camera.channel` 不会同步修改 `session.channel`，不能只把它改为 1。

## 4. 确认网络，再分别启动

`device_ip` 是运行进程的主机接口 IP；`module_ip` 仅用于显式 AT 管理，不是视频目的地址。写 YAML 不会设置网卡 IP 或模组无线参数。普通两板部署的空 bindings 表示不强制接口/namespace；多网卡时填写实际接口并保证运行权限。

按当前示例地址在对应设备检查，部署地址不同时一并替换：

```bash
ip -br address
# 空端：确认到地端的出口接口与源地址
ip route get 192.168.144.110
# 地端：确认到空端的路由；控制方向是 ground -> sky TCP 8080
ip route get 192.168.144.108
ss -lunp
ss -ltnp
```

视频方向为 sky → ground UDP 5005，控制方向为 ground → sky TCP 8080；防火墙和模组应放通对应路径。ping 只能辅助确认 IP 可达，不能代替图传验收。

地端在已登录的桌面终端运行：

```bash
fpv_ground --config "$FPV_CONFIG_DIR/ground.yaml" --gui
```

空端另一设备运行：

```bash
fpv_sky --config "$FPV_CONFIG_DIR/sky.yaml"
```

建议先地端后空端，便于观察首帧，但无强制启动顺序。无桌面时去掉 `--gui`，仍应产生编码/解码统计。远程 SSH 没有桌面会话时，不能以窗口打不开判断图传失败。

如果选择外部播放器，地端 `output.kind` 可改成 `encoded_callback` 以避免 SDK 再解码；`fpv_ground` 仍依赖 pipeline/MPP 安装。在地端两个终端分别运行，第二个终端应等 FIFO 已创建：

```bash
fpv_ground --config "$FPV_CONFIG_DIR/ground.yaml" --preview-fifo /tmp/fpv.h265
ffplay -fflags nobuffer -flags low_delay -f hevc /tmp/fpv.h265
```

不要同时启动多个接收进程抢占同一视频端口。使用 GUI 时必须保持 `decoded_callback`。

## 5. 接入自己的相机生产者

首选复用[空端完整程序](../native/sky/main.cpp)的生命周期，仅替换采集 adapter；业务层链接 `video_transmission::fpv_pipeline`，不要包含 SDK 私有实现文件。

1. 加载 SDK 配置，确认 `Role::Sky` / `InputMode::Raw`，建立相机与 runtime 租约。
2. 注册 `OnState` 和所需控制回调，再 `Start` 并检查 `Result`。`Start` 成功不表示远端已收帧。
3. 有界采集，整理成公共 `MppFrame`，设置 `channel` 为会话通道、`generation=0`、帧序号及源 PTS，然后逐帧 `SubmitFrame`。
4. 记录提交结果：`Accepted` 仅表示入队，可能淘汰旧帧；`QueueFull` 表示预算不足；`NotReady` 表示会话状态/角色不允许；`InvalidFrame` 应立即排查布局、ROI、通道和代际。
5. 停止生产并 join 采集线程，停止 source，再 `session.Stop()`，最后释放 adapter/runtime；已交付帧的 owner 必须保持有效到消费者释放。

Raw 的必要条件：合法本进程 MPP VB、非零 buffer ID、NV12、无压缩、恰好两个 plane，宽高为 2～16384 的偶数。两个 stride 至少为宽度，有效容量覆盖 Y 与 UV 行数，地址/fd/容量等描述必须与真实 VB 查询结果一致。源格式或分辨率改变时重新核实契约。可用 `ValidateMppFrame` 提前定位布局异常，再由提交接口检查 ROI/channel/generation。

`AdoptMppFrame` 仅接管释放责任，**不分配 VB、不导入 DMA-BUF、不转换像素、不初始化 runtime**。普通 CPU 缓冲应先分配/导入合法 VB、逐 plane 按 stride 拷贝并完成缓存同步。不能伪造 `ulBufferId`，也不能把 `width*height*3/2` 当成所有 NV12 缓冲的实际大小。参考[现有 adapter 的租约实现](../adapters/camera_adapter.cpp)和[布局验证](../../src/pipeline/mpp_frame.cpp)。

`owner` 持有帧、设备和 runtime；释放回调可能在不同 SDK 线程执行，须不抛异常，线程亲和性由 provider 自行处理。MPP runtime 的初始化/销毁操作须在进程内串行化。避免提前归还源缓冲，或无限持帧导致 VB 池耗尽。

所有 `On*` 在首次 `Start` 前注册；`FpvSession` 是一次性实例，首次启动失败也需新建实例重试。状态/编码/视频回调共享 dispatch 线程，回调只做快速有界转交。禁止在回调内 `Stop`/`UpdateConnections`，应通知应用控制线程执行。远端相机控制与本地 IDR 请求可能并发到达，处理器需自行同步。当前相机 adapter 的专有控制返回不支持，不能把视频出图当成云台/曝光控制也已接通。

已有编码器时，提交的是完整 H.265 Annex-B AU，不是任意网络分片、JPEG 或单个未经汇总的 slice。准确填写真实 IDR `keyframe`、AU 包含参数集的 `parameter_sets`、尺寸和微秒 PTS，保持数据不可变直到异步消费完成。启动/丢帧恢复需提供 IDR 和可解码所需 VPS/SPS/PPS，并响应恢复请求；若仅需收发而无 MPP，链接 `video_transmission::video_transmission` 使用核心会话。

## 6. 验收证据与排错顺序

按顺序观察，每一层通过后再查下一层：

1. **采集**：空端出现递增的 `camera decoded frames=`；持续 `camera empty polls=` 则查设备占用、模式、权限、采集插件或 MJPEG VDEC。
2. **提交/编码**：没有 `pipeline rejected capture frame layout/channel`，`capture frames rejected=` 不持续增长；检查状态回调中的编码/V2D 错误。
3. **网络/重组**：地端持续打印 `encoded bytes=`。缺失时查配置目的设备 IP、路由、UDP 端口、接口绑定和链路丢包。可在地端用 `tcpdump -ni <实际接口> udp port 5005` 区分“完全没包”和“有包但未组成 AU”。
4. **解码**：地端持续打印 `decoded pts_us=`。只有编码日志时，确认不是有意选择 `encoded_callback`；再查 IDR/参数集、VDEC 通道占用、MPP 插件与资源。
5. **显示**：GUI 画面随相机运动变化。已有解码日志却黑屏时查桌面权限、SDL2 支持与 GUI 日志。地端日志每 120 帧输出一次，短时无日志不等于失败。
6. **稳定性与退出**：记录实际连续运行时长、是否卡顿、提交拒绝/错误、Ctrl+C 是否退出及新进程能否重新启动。退出挂住或 pool/channel 耗尽应保留日志排查持帧/设备释放，不能直接宣称验收通过。

常见启动错误：`find_package` 找不到 MPP → 检查安装导出和 `CMAKE_PREFIX_PATH`；`GUI unavailable` → 安装 SDL2 开发包并重新配置构建；`bind` 失败 → 检查本机 IP 和端口占用；`InvalidFrame` → 查合法 VB、双 plane、ROI 和 channel；VDEC 锁失败 → 查同机通道重复和旧消费者持帧。不要靠随意更换 ID 或清理全机共享内存掩盖所有权问题。

无线验收应在不中断管理连接的条件下断开目标无线链路，确认画面停止并在恢复后重新出图，排除本机或备用网络直通。延迟测量见[分阶段延迟测量](LATENCY_MEASUREMENT.md)，必须区分估算、回调时间与真实显示时间。

## 7. ROS2、单板双模组与 USB

- ROS2 桥接是工作区外部依赖 `middleware/ros2/video_transmission`，不是只拿本模块即可获得的二进制。安装桥接、mpp_msgs 和本模块 [ROS2 launch](../ros2) 后，才使用 `ros2 launch fpv_video sky.launch.py config:=/绝对路径/sky_ros2_mpp.yaml`。`camera_config` 默认跟随 `config`。
- 当前桥接源码已解析 `camera_config`，并有 `ros2_mpp`、`ros2_image`、`ros2_compressed` 分支；历史迁移记录不能当成当前构建结论。接入前核对桥接当前支持的消息/像素格式和 QoS，并实际构建验收。MPP token 共享要求同机兼容 ABI、共享内存权限与有效租约，不能跨机器传 token 代替视频。
- 单板双模组使用 [dual_sky.yaml](../config/dual_sky.yaml) 和 [dual_ground.yaml](../config/dual_ground.yaml)，UDP 需不同且已准备好的 namespace、不同连接 ID 和不同 VDEC ID。给同一网卡增加两个 IP 不等于无线隔离。
- USB 使用 [k3_usb.yaml](../config/profiles/k3_usb.yaml)，实现为 Gadget/FunctionFS；需要实际 UDC、描述符服务持续持有 ep0、独立 `ffs_root`。不提供通用 USB Host/libusb 摄像头直传后端；FIFO 单元测试不代表 USB 固件/拔插已验收。

## 8. 给下一智能体的完成记录

交接时写清：两端板型/架构及源码版本、依赖前缀和插件目录、相机探测结果、修改后的完整配置路径、实际启动命令、采集/编码/解码/显示证据、连续运行时长、退出和重启结果、未完成项。只有编译或 `Start` 成功时应明确写“尚未验证相机到显示闭环”。优先保留已跑通的最小 UDP 配置，再开展性能调优。