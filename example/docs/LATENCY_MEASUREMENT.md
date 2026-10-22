# 视频传输分阶段延迟测量

返回 [示例说明](../README.md)。测量工具位于 [校时工具](../tools/clock_probe.py)
和 [统计工具](../tools/summarize_latency.py)，以下源树命令均从仓库根目录执行。

## 边界和时钟

启用 `VIDEO_TRANSMISSION_TRACE_PATH=/绝对路径/新文件.csv` 后，SDK 记录 CSV；未设置时不写日志。
每次启动必须使用新文件。单次连续运行、单路视频以原始 UVC PTS 关联，UDP 协议不变。
事件的 `mono_us` 是所在设备的 `CLOCK_MONOTONIC` 微秒，`pts_us` 是天空端源帧标识。
**不能直接用两台设备的单调时钟相减。**

本次 DECXIN USB 摄像头通过 `v4l2-ctl --verbose` 确认是 `ts-monotonic, ts-src-soe`。
`capture_ready - pts_us` 测量驱动提供的 SOE 时间戳到应用拿到帧，包含 USB 与采集队列；
它不等价于光学实测的传感器曝光时间，其他相机必须重新验证 PTS 时钟和语义。

| 阶段 | 起点 → 终点 | 包含内容 |
|---|---|---|
| 采集 | UVC PTS → capture_ready | 驱动时间戳到应用拿帧 |
| 预处理 | capture_ready → encode_submit | MJPEG 解码、裁剪缩放、调用间排队 |
| H.265 编码 | encode_submit → encode_done | SendFrame 前到 GetStream 成功；含取结果调度 |
| 发送排队 | encode_done → tx_first | 输出拷贝、发送线程排队 |
| 网络整帧 | tx_first → rx_complete | 首个 sendto 前到整帧重组，需跨机校时；含分包限速、内核和模组缓存 |
| 接收排队 | rx_complete → decode_submit | 重组完成到解码提交 |
| H.265 解码 | decode_submit → decode_done | SendStream 前到 GetFrame 成功；含调度 |
| GUI | decode_done → gui_presented | 回调、最新帧邮箱、DMA 读取、纹理上传、合成呈现 |

另有 `mjpeg_submit/done`、`scale_begin/done`、`tx_last`、`rx_first`、
`decode_callback`、`gui_begin/submit/return` 等事件，可继续定位子阶段。
各阶段必须取同一批完整关联帧；不能把不同样本的 P95 相加。

## GUI 测量路径

可选程序 `video_transmission_ground_latency` 直接显示 SDK 的 NV12 硬件解码帧：

```
UDP → SDK H.265 硬件解码 → 最新帧邮箱 → SDL/OpenGL ES → Wayland
```

原来的 `--preview-fifo` 路径把 H.265 交给 ffplay 再次解码，因此它的 GUI 延迟不能接在
SDK 硬件解码完成时间后。测量窗口是独立程序，原程序及配置保留。

窗口通过 `wp_presentation` 获得呈现反馈，检查呈现时钟为 CLOCK_MONOTONIC，
并校验时间在本帧提交请求与收到通知之间。始终保留 `gui_presented_raw` 和
`gui_feedback_received`。有效时间才记录 `gui_presented`；无效则记录
`gui_timestamp_invalid`，不会伪造或钳制为 0。

K1 实测发现原始呈现时间戳会冻结或滞后（首轮 1,656 个反馈完全相同；
第二轮 1,601 个反馈中 1,260 个时间戳无效），因此完整样本集采用以下口径：

- GUI 提交下界：`gui_submit - decode_done`。
- 显示调用返回时间：`gui_return - decode_done`，不等同于呈现完成。
- GUI 呈现通知上界：`gui_feedback_received - decode_done`，包含通知和事件循环调度。
- 若呈现通知本身正确，真实系统呈现延迟位于提交下界和通知上界之间。
- 物理屏幕扫描和面板响应还需高速相机/光电测试确认。

## 构建

SDK 增加 latency.hpp / latency.cpp，现有公开帧结构和传输包格式不变。
将 `components/video_transmission` 以 `VIDEO_TRANSMISSION_BUILD_PIPELINE=ON` 构建安装。
地面端额外依赖 `libsdl2-dev libwayland-dev wayland-protocols`，配置原生应用时设置：

```bash
cmake -S components/video_transmission/example -B /tmp/video_transmission-latency-build-app \
  -DCMAKE_PREFIX_PATH=/root/video_transmission-latency-sdk \
  -DCMAKE_INSTALL_PREFIX=/root/video_transmission-latency-sdk -DVIDEO_TRANSMISSION_BUILD_LATENCY_VIEWER=ON
cmake --build /tmp/video_transmission-latency-build-app --target video_transmission_ground_latency --parallel 2
```

默认不构建此窗口，不给普通 sky/ground 程序增加 SDL 依赖。
以上命令使用迁移后的示例构建入口；其 CMake 接线与安装规则需由构建迁移任务完成。
另修正媒体循环，使其及时响应 `stopping`，避免 Stop 等待生命周期锁时被循环反复抢锁。

## 当前设备与运行目录

- K3 天空端 `root@10.0.91.42`：独立测量 SDK `/root/video_transmission-latency-sdk`。
- K1 地面端 `root@10.0.91.82`：桌面可访问 SDK `/home/bianbu/video_transmission-latency-sdk`。
- K1 地面配置 `/home/bianbu/video_transmission-latency-config/ground.yaml`。
- 原运行 SDK 和启动脚本保留，测量结果不会自动切换原来的 ffplay 预览路径。
- 开始前关闭旧视频传输进程和 ffplay，确认摄像头和解码设备没有其他持有者。
- 在 K1 以桌面用户 bianbu 运行窗口，使用其 Wayland/DBus 会话。root 会话的图形门户曾超时。
- `VIDEO_TRANSMISSION_TRACE_PATH` 要可写且不存在；进程正常退出后读取 CSV。

两端构建后可按以下顺序重复采样（每次将 RUN 替换为新的目录名）：

1. K3 启动校时应答，管理网络用于测时钟偏移，视频仍走 192.168.144.* 模组网络。

```bash
python3 components/video_transmission/example/tools/clock_probe.py server \
  --host 10.0.91.42 --peer 10.0.91.82 --seconds 100
```

2. K1 另一个终端启动校时客户端，在采样前后都保留校时样本。

```bash
python3 components/video_transmission/example/tools/clock_probe.py client \
  --host 10.0.91.42 --seconds 85 --out /home/bianbu/video_transmission-latency-results/RUN/clock.jsonl
```

3. K1 启动窗口（结果目录须先以 bianbu 身份创建）。

```bash
runuser -u bianbu -- env \
  LD_LIBRARY_PATH=/home/bianbu/video_transmission-latency-sdk/lib \
  MPP_PLUGIN_DIR=/home/bianbu/video_transmission-latency-sdk/lib \
  VIDEO_TRANSMISSION_TRACE_PATH=/home/bianbu/video_transmission-latency-results/RUN/ground.csv \
  XDG_RUNTIME_DIR=/run/user/1000 \
  DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus \
  WAYLAND_DISPLAY=wayland-0 SDL_VIDEODRIVER=wayland \
  /home/bianbu/video_transmission-latency-sdk/bin/video_transmission_ground_latency \
  --config /home/bianbu/video_transmission-latency-config/ground.yaml --seconds 75
```

4. K3 启动 60 秒天空端（结果目录须先创建）。

```bash
timeout --signal=INT --kill-after=15s 60s env \
  VIDEO_TRANSMISSION_PREFIX=/root/video_transmission-latency-sdk \
  VIDEO_TRANSMISSION_TRACE_PATH=/root/video_transmission-latency-results/RUN/sky.csv \
  /root/video_transmission-config/start-sky.sh
```

5. 收集两端 CSV 和 clock.jsonl，运行统计。

```bash
python3 components/video_transmission/example/tools/summarize_latency.py \
  --sky sky.csv --ground ground.csv --clock clock.jsonl --warmup 5 --out summary
```

输出逐帧 `summary.frames.csv` 和统计 `summary.json`。统计脚本排除预热、重复 PTS、
缺阶段帧和无校时覆盖帧；报告不完整记录、各事件计数、呈现时间戳有效性和负值，
不静默丢弃不合理的延迟。`end_to_end_upper_ms` 是呈现通知上界，不能称为精确屏幕延迟。

校时使用四时间戳计算 sky-minus-ground 偏移，每 10 秒选最小 RTT 样本并插值。
不修改系统时钟。半 RTT 是路径非对称引入的估计不确定度；插值假设短时间漂移近似线性。
应同时查看每个窗口的偏移/RTT，不能只报单向传输的小数位。

### 测量通道隔离（2026-09-30 修正）

K1 正常 root 程序使用 decoder_id=0，锁文件保持 root:root 0600。
桌面用户 bianbu 的独立测量配置已改为 decoder_id=1，使用另一个锁文件。
不要通过改正式通道锁文件的属主来启动测量：Linux 的 fs.protected_regular=2
会拒绝 root 在 /tmp 中以 O_CREAT 打开其他用户的既有锁文件，而旧程序会将
此权限错误误报为 MPP resource busy。无需关闭内核保护，也不能删除正在持有的锁。
