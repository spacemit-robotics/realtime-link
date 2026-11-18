# Video Transmission SDK

本模块面向低延时、长距离视频传输，提供空端和地端会话接口。应用可以接入编码后的视频帧，也可以通过媒体后端接入原始帧；地端可接收编码帧，或通过媒体后端取得解码后的帧。模块提供有界队列、帧超时处理、状态事件与可选控制接口，便于构建实时视频应用。

实际传输距离、时延和画质取决于图传设备、无线环境、码率、编解码及显示链路，应结合部署环境确定。

## 构建与安装

依赖 CMake 3.16 或更新版本、支持 C++20 的编译器和线程库；参考示例还需要 yaml-cpp 开发包。默认构建不需要产品兼容库或私有源码。

```sh
cmake -S . -B build -DCMAKE_INSTALL_PREFIX="$PWD/install"
cmake --build build -j
ctest --test-dir build --output-on-failure
cmake --install build
```

仅构建核心库时，可在配置时指定 `-DVIDEO_TRANSMISSION_BUILD_EXAMPLES=OFF`。应用包含 `<video_transmission/video_transmission.h>` 并链接生成的 `libvideo_transmission.so`。使用已安装的 CMake 包可通过 `find_package(video_transmission CONFIG REQUIRED)` 和 `video_transmission::video_transmission` 接入。

### 架构内编译

在包含本组件的 `spacemit_robot` 架构工作区中，先在**当前终端**加载环境，再进入组件目录执行 `mm`：

```sh
cd /root/workplace/spacemit_robot
source build/envsetup.sh
cd components/realtime-link
mm
```

`envsetup.sh` 会设置 `SDK_ROOT`、`PREFIX`、`PATH` 和 `LD_LIBRARY_PATH`。在上述空端环境中，`PREFIX` 为 `/root/workplace/spacemit_robot/output/staging`；`mm` 编译当前组件并安装到该前缀。可用 `echo "$PREFIX"` 确认实际路径，在 `"$PREFIX/include/video_transmission/"` 和 `"$PREFIX/lib/"` 查找安装产物。示例依赖的 `libyaml-cpp-dev` 等由架构依赖检查提示。

`mm` 不带 `--with-deps` 时只构建当前组件；需要先构建架构声明的依赖时可使用 `mm --with-deps`。Docker 和交叉编译使用 `envsetup.sh` 提供的开关，加载环境后默认关闭。`mm` 可直接传递 CMake 的 `-D` 选项；已有构建目录的缓存可能保留先前选项。

如果应用自行采集、编码、解码和显示，默认核心构建即可使用编码帧接口。需要使用本项目的可选 MPP 适配器时，先将架构中的 MPP 安装到同一 `PREFIX`，再按需开启相应构建选项，并可用 `-DVIDEO_TRANSMISSION_MPP_PREFIX="$PREFIX"` 指定该安装前缀。产品兼容插件属于单独的私有交付物，默认公共构建不会生成它。

## 代码规范与测试

在包含 `scripts/lint/` 的架构工作区根目录，分别运行 C/C++ 和 Python 检查；两种脚本需要 `cpplint` 和 `ruff` 在 `PATH` 中：

```sh
./scripts/lint/lint_cpp.sh components/realtime-link
./scripts/lint/lint_python.sh components/realtime-link
```


## MPP 帧所有权桥接

可选的 `VIDEO_TRANSMISSION_BUILD_MPP_FRAME_BRIDGE=ON` 构建 `video_transmission_mpp_frame`，依赖开源 MPP 头文件。`vt_mpp_frame_wrap` 将 MPP 帧描述复制到引用计数的句柄，底层图像缓冲区不复制；提交公共会话后，调用方应释放自己的引用。最后一个引用释放时调用提供的回收函数，回收函数及其设备上下文须保持有效。`vt_mpp_frame_info` 只可在持有引用时使用。

该桥接只管理帧所有权，不提供相机采集、MPP 编解码或显示；这些功能不能仅靠启用该选项获得。

## 可选 MPP 相机与媒体后端

公开 SDK 只安装 `video_transmission/video_transmission.h`；插件及 MPP 适配接口也在该头文件中。使用 MPP 帧具体类型时，调用方需另外包含 MPP 自身的 `<sys/vb_type.h>`。源码树中的兼容转发头位于 `src/video_transmission/`，不作为 SDK 公共头安装。

设置 `VIDEO_TRANSMISSION_BUILD_MPP_FRAME_BRIDGE=ON` 后，可分别启用 `VIDEO_TRANSMISSION_BUILD_MPP_CAMERA=ON`、`VIDEO_TRANSMISSION_BUILD_MPP_MJPEG=ON` 和 `VIDEO_TRANSMISSION_BUILD_MPP_MEDIA=ON`，需要匹配版本的公开 MPP 头文件与库。该目录下的 `src/mpp_adapter` 是适配层，调用外部 MPP 的 UVC、VDEC、VENC 和 V2D 接口，不复制 MPP 实现。在本工作区，MPP 源码位于 `components/multimedia/mpp`，其编译安装到 `output/staging` 后，图传模块使用该目录中的头文件和 `libmpp.so`。构建时可用 `-DVIDEO_TRANSMISSION_MPP_PREFIX=/path/to/output/staging` 指定该前缀；指定后仅在此前缀查找 MPP 头文件和库，缺失时配置失败。未指定时使用 CMake 默认查找规则。调用方负责初始化相应 MPP 模块，并在全部帧引用释放后退出。相机适配可返回 MPP 帧；MJPEG 适配支持解码及 MPP 输出尺寸缩放；H.265 媒体后端接收与配置尺寸及像素格式一致的 MPP 原始帧。可选 V2D 适配支持 NV12 ROI 裁剪和缩放；产品链路由单独的插件提供。

## 公共头文件接口

应用只需包含 `<video_transmission/video_transmission.h>`。带 `struct_size` 的结构体必须填写其大小；带 `abi_version` 的结构体还需设置 `VT_ABI_VERSION`。函数返回 `vt_status`；调用方应检查返回值，并处理状态事件。

### 会话与传输

- `vt_session_create`：使用角色、输入模式、队列限制、回调及后端创建会话。成功后会话接管传入的后端上下文；失败时由调用方销毁已创建的后端。
- `vt_session_start`：启动会话及其后端。会话是一次性的，停止后要重新创建。
- `vt_session_submit_encoded`：空端提交一个完整的 H.265 编码 access unit（AU）。成功后会话复制输入数据；支持在 AU 中标记关键帧和参数集。
- `vt_session_submit_raw`：空端提交原始帧，需配置可编码的媒体后端以及帧的 `retain` / `release`。仅使用自有编码器时不需要此接口。
- `vt_session_drain`：等待已接受的发送帧交给传输后端，不代表对端已接收或显示；超时参数单位为毫秒。
- `vt_session_get_state`：获取最近一次状态事件；调用前需设置输出 `vt_event.struct_size`。
- `vt_session_send_control`：向已配置的控制后端发送通用控制请求；没有对应后端时可能返回 `VT_UNSUPPORTED`。
- `vt_session_stop`、`vt_session_destroy`：分别停止会话，以及停止并释放会话和其接管的后端。不要在会话回调内调用这两个函数。
- `vt_reference_udp_create`：创建公开的参考 UDP 传输后端。它采用参考协议，不能直接与现有产品协议互通。

`vt_callbacks` 的 `on_event` 用于接收状态、丢帧和关键帧请求等事件；`on_encoded_frame` 用于接收编码帧；配置可解码的媒体后端时，`on_raw_frame` 可接收原始帧。这些回调由会话派发线程串行调用，应尽快返回。

`vt_transport_backend`、`vt_media_backend` 和 `vt_control_backend` 是后端接口：应用可自行实现，也可使用单独提供的兼容插件。`vt_backend_plugin_create_v1_fn` 是插件入口的函数指针类型；插件配置由插件解释，插件库必须保持加载，直到其创建的后端上下文全部销毁。

### 可选 MPP 适配接口

下列接口只有在相应 MPP 适配库构建并链接后才可用；仅自行采集、编码和解码的应用无需使用它们。MPP 初始化与退出由应用负责，底层帧及设备须在所有引用释放前保持有效。

- `vt_mpp_frame_wrap`、`vt_mpp_frame_info`：包装 MPP 帧为引用计数的 `vt_raw_frame`，以及在持有引用期间获取其 MPP 帧信息。
- `vt_mpp_camera_open`、`vt_mpp_camera_get`、`vt_mpp_camera_close`：创建、取帧和关闭 MPP UVC 相机适配器；成功取出的帧须由调用方释放。
- `vt_mpp_media_create`：创建 MPP H.265 编码或解码媒体后端，供会话使用。
- `vt_mpp_mjpeg_open`、`vt_mpp_mjpeg_open_scaled`、`vt_mpp_mjpeg_decode`、`vt_mpp_mjpeg_close`：创建 MJPEG 解码器、可选设置输出缩放、解码一帧和关闭；解码失败后需关闭并重新创建。
- `vt_mpp_crop_resize`：对 MPP NV12 帧执行 ROI 裁剪和缩放；输入与输出缓冲区由调用方准备和管理。


## 模块使用方式

应用可自行完成相机取图、H.265 编码、解码和显示：空端将完整编码帧通过 `vt_session_submit_encoded` 送入会话，地面端在 `on_encoded_frame` 回调中取出编码帧，再交给应用自己的解码和显示流程。接收回调中的数据仅在回调返回前有效，异步处理前须复制。

`example/native/public/` 展示了另一种方式：使用可选 MPP 适配器实现取图、编解码和地面显示，再通过公共会话连接传输插件。它可作为应用接入公共接口的参考；应用并不需要采用其中的相机或显示实现。示例的构建、空地端参数、控制操作，以及自有帧接入方法见 [示例使用说明](example/README.md)。
