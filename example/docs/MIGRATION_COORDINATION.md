# 示例迁移接口协调

原生天空端现由 MPP UVC/VI 直接采集，MJPEG 使用 pipeline 公共解码器；ROS2 桥接仍需单独迁移。板端媒体链路未经实机验收。

## 原生示例构建与安装

- 原生包名为 `fpv_video_example`，包元数据声明 video_transmission 和系统依赖 `libyaml-cpp-dev`；天空端经 pipeline 的 MPP 目标直接使用 UVC/VI。
- 示例 CMake 安装 `fpv_sky`、`fpv_ground` 和可选的 `fpv_ground_latency`。`FPV_BUILD_SKY_EXAMPLE=OFF` 可关闭空端 target。
- 配置安装路径继续使用 `share/fpv_video/config`，以兼容部署脚本。
- `config/sites/oyang_k3/preview-ground.sh` 经文本补丁迁移后权限为 0644；应通过 `install(PROGRAMS ...)` 安装，源码使用时显式调用 Bash。

## 核心接口与生命周期协调

原生空端串行执行 `CameraAdapter::Get`、可选 MJPEG 解码及 `SubmitFrame`，不再依靠 session 隐式打开相机。NV12 输出租约持有 MPP 设备与 runtime，释放时先归还采集帧；MJPEG 输出租约持有公共解码器，输入租约覆盖同步读取和送入解码器期间。输出 generation 为 0，在提交时绑定会话代际。

退出顺序为停止生产循环、调用适配器 Stop、session Stop，最后释放适配器。采集帧租约持有设备，Stop 仅释放适配器的设备引用，最后一帧归还后才禁用通道。

- 原生示例使用公开 `MppFrame`、`InputMode` 和显式提交 API；ROS2 桥接仍需移除旧 `config.camera` 和 `camera::Frame` 路径。
- 采集适配器与公共 MJPEG helper 使用 `MppRuntime` 成对持有 SYS/VB；帧租约保持采集设备或 VDEC 通道直至归还。
- MJPEG helper 已位于 pipeline，不依赖 camera 类型，保留有界超时、DMA 同步、PTS 校验和 decoder 输出租约。
- 尚未核实设备控制 tag/payload 到 UVC effect 的映射；适配器明确返回不支持，不能视为控制功能迁移完成。
- 公共 MJPEG 解码器属于 pipeline；示例仅链接 `fpv_pipeline` 和 yaml-cpp，不链接 camera 组件。

## ROS2 过渡配置接口

ROS 包名保持 `fpv_video`。启动文件默认从包的 `config/sky.yaml` 或 `config/ground.yaml` 读取配置；ROS CMake 将相邻的 `../config/` 完整安装到包共享目录，因此 profile 和 connections 的相对路径仍然有效。

两个 launch 均向 `fpv_video_bridge/fpv_bridge` 传入以下参数：

- `config`：会话 YAML 文件路径。
- `role`：`sky` 或 `ground`。
- `camera_config`：采集适配器的过渡 YAML 文件路径，默认与 `config` 相同，可单独覆盖。

middleware 必须新增并解析字符串参数 `camera_config`，不再从 SDK 的 `FpvConfig.camera` 读取设备配置。解析应先读取该文件的 `deployment_profile`，再用当前文件的 `camera` 字段覆盖 profile 中的同名字段；profile 路径相对于引用它的 YAML 文件解析。必须保留 driver、device、topic、format、stream、width、height、fps 等现有采集字段，并在重载时同步更新采集配置。地端不应要求存在 camera 节点。

`sky_ros2_mpp.yaml` 通过 profile 指定 ROS2 MPP 话题；选择该文件时，默认 `camera_config` 会随 `config` 一并切换。保留旧 YAML 中的 camera 节点仅用于适配层过渡，不表示 SDK 会打开设备。

当前 middleware 仍直接使用 `config_.camera` 和 `camera::Frame`。本次按任务边界未修改 middleware；在桥接层完成公共 `MppFrame` 和采集配置迁移前，不能宣称 ROS2 示例可构建或运行。

## 文件删除与二进制资源

补丁工具多次报告源文件删除成功，但只读文件系统检查仍看到旧配置、工具、文档、包清单和 ROS2 文件。没有使用终端写入或删除绕过限制；目标文件已写入，源目录清理尚未完成。旧 ROS2 包添加了 `COLCON_IGNORE`，避免 colcon 同时发现两个 `fpv_video`。

原 native/docs 仅发现二进制 `fpv_latency_analysis.xlsx`。文本补丁不能无损迁移该文件，原件保留，目标 docs 中没有伪造替代文件。完成迁移仍需可无损处理二进制及实际删除文件的机制。