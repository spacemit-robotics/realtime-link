## 地面端实时显示

目标板安装 SDL2 并登录桌面后，可直接显示 SDK 硬件解码的 NV12 帧：

	fpv_ground --config path/to/ground.yaml --gui [--fullscreen]

在 k1 已登录的桌面终端启动窗口，配置需使用 `output.kind: decoded_callback`。
加 `--fullscreen` 可按当前屏幕分辨率全屏显示并隐藏鼠标指针。
GUI 不使用第二个解码器或编码帧 FIFO，窗口关闭或按 Escape 时停止接收。
不传 `--gui` 时，原有无窗口和 `--preview-fifo` 模式保持不变。
构建默认检测 SDL2 >= 2.0.16；可用 `FPV_BUILD_GUI=OFF` 禁用显示支持。
# FPV 调用示例

本目录承接原生 sky / ground 调用程序、配置和测量工具，不导出业务库。
构建和部署步骤见 [操作指南](../../../docs/FPV_BUILD_INSTALL_USAGE.md)，
延迟测量见 [分阶段延迟测量](docs/LATENCY_MEASUREMENT.md)。

```bash
fpv_sky --config /path/to/config/sky.yaml
fpv_ground --config /path/to/config/ground.yaml
```

## 最小接入示例（从 UVC 取图到提交）

从 UVC 相机取图开始的最小调用链路，只需几步即可跑通图传发送侧：

1. **加载配置**：`example::LoadExampleConfig(path)` 解析 schema 2 根配置与 schema 1
   profile，同时得到会话参数与相机采集参数；`RequireRole(config.session, Role::Sky)`
   确认角色为天空端，并保证 `input_mode` 为 `raw`（相机输入）。

2. **打开相机**：`example::CameraAdapter camera(config)` 打开 UVC 设备；MJPEG 帧会
   在适配器内先解码为线性 NV12，符合 `SubmitFrame` 的输入契约。取图用
   `camera.Get(timeout)`，超时返回空 `MppFrame`。

3. **注册回调并启动**：创建 `FpvSession`，用 `OnState` 注册状态回调（回调须在
   `Start` 前注册），再 `session.Start(config.session)` 并检查返回的 `Result`。

4. **循环取图提交**：每帧 `camera.Get` 拿到 `MppFrame` 后，把 `generation` 置 0
   （绑定当前代际），调用 `session.SubmitFrame(std::move(frame))`；返回
   `Accepted` 表示已入队（可能淘汰旧帧），`InvalidFrame` 表示帧布局/通道不符。

5. **停止**：退出循环后先 `camera.Stop()` 停采集，再 `session.Stop()` 停会话，
   顺序不能反；已交付帧的 owner 在停止后仍有效。

`example::CameraAdapter` 是示例层自行封装的相机适配模块，SDK 本身不打开相机，也不
规定相机 SDK。它的职责是把任意来源的原始帧整理成合法的线性 NV12 `MppFrame`（含
设备/缓冲区生命周期与释放回调），再交给 `FpvSession::SubmitFrame`；MJPEG 输入时还
负责先用 MPP 解码器转成 NV12。

- **UVC / VI 相机**：MPP 模块已内置取图接口，适配器直接调用即可。UVC 取帧用
  `UVC_GetFrame(id, channel, &info, timeout)`、释放用 `UVC_ReleaseFrame(...)`；
  VI 取帧用 `VI_GetChnFrame(id, channel, &info, timeout)`、释放用
  `VI_ReleaseChnFrame(...)`。二者都返回 `VideoFrameInfo`，由适配器用
  `AdoptMppFrame` 打包成 `MppFrame`。
- **其他类型相机**：需先获取其自有 SDK，再按上述契约自行封装适配层——把采集帧
  拷贝/转换成本进程合法 MPP VB 的线性 NV12（偶数尺寸、双 plane、真实 buffer ID），
  并用 `AdoptMppFrame` 挂上释放回调。`AdoptMppFrame` 不分配 VB、不导入 DMA-BUF、
  不转换像素格式，这些都由适配层自行完成。

完整实现（含采集控制、提交统计与故障重试）见 `native/sky/main.cpp`。


修改共用的 connections.yaml 注入设备 IP、模组 IP 和设备身份；驱动/设备由 sky.yaml 选择，媒体默认值由 profiles 提供。默认配置是两台设备的 UDP 示例，地址需按实际部署修改。

配置保留旧 YAML 的 `camera` 节点，作为 example adapter 的过渡配置。
设备选择、采集和设备控制由示例适配层负责，不表示 SDK 内部仍按此节点打开设备。

ROS2 launch 的 `camera_config` 默认跟随 `config`；桥接参数接入和当前迁移阻塞项见
[接口协调记录](docs/MIGRATION_COORDINATION.md)。

原生空端直接通过 MPP UVC/VI 接口采集 NV12，或通过公共 MJPEG 解码器解码后提交
`MppFrame`，不依赖独立 camera 组件。`FPV_BUILD_SKY_EXAMPLE=OFF` 可关闭空端构建；
地端仍链接 `fpv_pipeline`，可选 GUI 由 `FPV_BUILD_GUI` 控制。

同一 K3 双模组分别使用 dual_sky.yaml、dual_ground.yaml，并预先准备 fpv-air / fpv-ground 网络空间及接口。两端 decoder_id 分开分配，不能只给同一网卡添加两个本机 IP。

`native/ground/main.cpp` 的回调展示帧接收位置；默认周期打印信息，不创建窗口。可传 `--preview-fifo /path/to/fifo` 将 H.265 帧以有界异步队列送给本机播放器。双板实机的 [站点预览脚本](config/sites/oyang_k3/preview-ground.sh) 会在地面端 `bianbu` 的 Wayland 桌面启动 `ffplay`，并运行带预览参数的 `fpv_ground`。`encoded_callback` 无需创建解码器；原始采集和硬解示例依赖 fpv_pipeline 及 MPP，纯码流示例的核心链接由构建迁移任务接线。

详见 [实施与验证记录](../../../docs/FPV_VIDEO_TRANSMISSION_IMPLEMENTATION.md)。

## 可选 PTS 播放缓冲

地面端已经收到完整帧，但大 I 帧后紧随一批 P 帧时，可以按发送端的 PTS
间隔安排显示，平滑到帧抖动。例如使用已验证的低延迟折中档：

```bash
fpv_ground --config path/to/ground.yaml --gui --playout-delay-ms 250 --playout-max-delay-ms 400 --playout-max-frames 16
```

- 不传参数时 `playout-delay-ms=0`，保持最新帧优先；缓冲不是默认开启。
  `playout-max-delay-ms` 默认 600，`playout-max-frames` 默认 16。
  使用上面的命令才是 **250 / 400 / 16** 档。
- 参数只用于 `--gui`，不改变 FIFO、网络协议、码率、GOP、编码 QP 或发送端。
  要求整数 `0 <= delay < max-delay <= 10000`、`1 <= max-frames <= 120`；
  非法参数在加载配置、打开显示或网络前拒绝。
- PTS 单位为微秒，用**相对间隔**映射到本机单调时钟，不直接与两端系统时间相减。
  首帧可能受解码启动积压影响，后续更早到达的帧可提前播放基准；网络变慢不会
  无限向后推迟基准。PTS 回滚、大间隔和连接代际切换会重新建立显示时序。
- 缓冲中保留独立的 SDL/GPU 纹理，不长期占用 MPP 解码输出。上传前的短队列
  最多保留 4 帧（关闭缓冲为 1 帧），上传后立即释放解码帧；DMA CPU 读同步保留。
  纹理数量最多 `max-frames+2`，名义 NV12 像素容量上限 64 MiB，不含驱动额外开销。
- 启用缓冲时，程序将解码前的 `pipeline.max_frames/max_age_ms` 提高到至少所选
  缓冲预算，避免突发到帧被过小的解码输入队列提前丢弃。原有字节上限不变，
  不增加 MPP 解码池，不修改配置文件。较大的已有配置不会被缩小。
- `max-delay` 是**各本地队列的驻留上限**，不是相机到屏幕的端到端延迟上限；
  各阶段时间相加还会更长。`delay=250` 也不表示每帧强制睡眠 250 ms。
  GUI 按事件及下一帧截止时间等待，而不是固定睡眠轮询。
- 超出容量、长时间停顿或错过显示截止时间时仍会丢弃已解码的显示帧以恢复实时性，
  不保证每帧显示，也不能修复上游丢失的数据或持续断网。编码参考帧恢复仍由 SDK 处理。

日志 `GUI playout` 报告提交 FPS、排队数量、各类丢帧、时钟重置和基准提前次数。
设置 `FPV_TRACE_PATH` 可记录上传、排队、截止时间和 SDL 提交时刻；这些不是面板
真正扫描输出的时间。先前 K1 实测 250 ms 档 GUI 提交约 29.9 FPS，间隔 P95
约 42.3 ms，解码回调到提交平均约 143 ms；它是特定链路的测试结果，不是延迟保证。

纯软件回归（无窗口、无网络连接、无编解码硬件）：

```bash
ctest --test-dir <build> -R 'fpv_pts_playout|fpv_playout_cli' --output-on-failure --repeat until-fail:3
```

`fpv_pts_playout` 用合成时钟验证突发、启动积压、容量、过期、回滚、所有权与
零延迟模式；`fpv_playout_cli` 验证帮助及参数拒绝。完整 GUI 验证仍需匹配的 MPP、
SDL2 和已登录的桌面。
