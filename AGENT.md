
# 通用视频传输 SDK（video_transmission）

天空端（sky，机器人）↔ 地面端（ground，地面站）之间 H.265 视频的 UDP/USB 传输，TCP 控制反向。
位于 SpacemiT Robotis 工程的 `components/video_transmission`。

## 构建与测试

本地核心库（不依赖 MPP 或相机运行时）：

```bash
cmake -S . -B /tmp/video_transmission-core -DVIDEO_TRANSMISSION_BUILD_PIPELINE=OFF
cmake --build /tmp/video_transmission-core -j4
ctest --test-dir /tmp/video_transmission-core --output-on-failure
```

pipeline + 示例（需 MPP）：

```bash
cmake -S . -B build -DVIDEO_TRANSMISSION_BUILD_PIPELINE=ON -DVIDEO_TRANSMISSION_BUILD_EXAMPLES=ON
cmake --build build -j4
```

远程板端编译（Android envsetup 环境）：

```bash
source build/envsetup.sh
lunch k3-video_transmission-video
m
```

部署 SSH：地面端 k1、天空端 k3，IP 与目标目录按实际环境手动指定。

CMake 选项：`VIDEO_TRANSMISSION_BUILD_PIPELINE`（MPP 管线 + 门面，默认 ON）、`VIDEO_TRANSMISSION_BUILD_EXAMPLES`（原生 sky/ground 示例，默认 ON，依赖 pipeline）。若只需不依赖 MPP 的通信核心，显式传入 `-DVIDEO_TRANSMISSION_BUILD_PIPELINE=OFF -DVIDEO_TRANSMISSION_BUILD_EXAMPLES=OFF`。

## 架构（三层，从下到上依赖增加）

详见 [README.md](README.md)：

1. **通信核心** `src/{sky,ground}/session.cpp`：`SkySession`/`GroundSession`，只收发 `EncodedFrame`，无 MPP 依赖，可独立构建。
2. **管线门面** `src/pipeline/session.cpp`：`VideoTransmission`，叠加采集提交 + ROI/缩放 + MPP 编解码。
3. **控制平面** `src/control/control.cpp`：`ControlSession` 独立 TCP，控制方向与视频方向相反。

产出两个库：`video_transmission`（核心）+ `video_transmission_pipeline`（可选，CMake target 为 `video_transmission::pipeline`）。

## 关键约定（易错点）

- **帧入口严格分型**：`VideoTransmission::SubmitFrame(MppFrame)` 接受**线性 NV12 或 BGRA**（`COMPRESS_MODE_NONE`、偶数尺寸）；首个接收帧确定格式，切换格式须新建会话。NV12 支持 ROI/缩放；BGRA 须全幅同尺寸、宽为 16 的倍数、步幅为宽×4，直接送 VENC。`SubmitEncodedFrame(EncodedFrame)` 仅接受**完整 H.265 AU**；二者由 `InputMode{Raw,Encoded}` 决定，不能混用。字段语义见 `include/video_transmission/video_transmission.hpp` 与 `src/video_transmission/session.hpp` 注释。
- **不对称校验**：`SubmitFrame` 显式校验格式（错给压缩帧返回 `InvalidFrame`）；`SubmitEncodedFrame` **只校验元数据边界、不解析码流**（`src/session_common.hpp` 的 `ValidFrame`），错误内容原样上链路。
- **所有权借用**：`MppFrame.owner` 与 `EncodedFrame.data` 都是借用，必须保证底层内存/资源存活到 SDK 异步消费完成；`MppFrame` 用 `AdoptMppFrame` 打包释放回调。
- **BGRA 异步保活**：SDK 保留输入 owner 至对应 PTS 的编码输出或编码器关闭；生产者提交前完成 DMA 写入，释放前不可覆盖像素。在途队列最多 `max_frames+3` 帧；不能用超时强行释放仍可能被硬件访问的输入。部署前核对 README 中的 MPP packed-RGB 修复依赖。
- **回调线程模型**：`On*` 回调在 SDK 内部 dispatch 线程执行，禁止阻塞，禁止在回调内调 `Start/Stop/UpdateConnections`（SDK 用线程本地哨兵检测，违者抛异常或返回 `NotReady`）。
- **MPP 资源跨进程互斥**：`/tmp/hub-video_transmission-{venc,vdec}-{id}.lock` + `flock(LOCK_EX|LOCK_NB)`，创建 `Encoder/Decoder` 时占用；同设备双模组需分配不同 `encoder_id/decoder_id`。
- **有界队列丢旧不丢新**：`Queue<T>` 按 `max_frames/max_bytes/max_age` 淘汰最旧帧；`SubmitFrame` 队列满仍返回 `Accepted`（新帧保留）。

## 头文件安装边界

SDK 只安装 `include/video_transmission/video_transmission.hpp` 作为公共入口。其余 API 声明头文件保存在 `src/video_transmission/`，由公共入口合并维护；MPP API 受 `VIDEO_TRANSMISSION_HAS_PIPELINE` 控制，该宏由 `video_transmission_pipeline` target 传递。


## 文档索引

- [README.md](README.md)：模块框架、头文件用法、帧格式契约、网卡/相机配置、双端启动。
- [example/README.md](example/README.md)：示例程序、地面端 GUI / FIFO 显示。
- [example/docs/LATENCY_MEASUREMENT.md](example/docs/LATENCY_MEASUREMENT.md)：分阶段延迟测量。
- `../../docs/`：工作区实施记录和构建安装操作指南（若当前工作区提供）。
