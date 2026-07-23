# WL-hosted Coprocessor ESP-IDF Adapter

`wl-hosted-coproc-esp-idf` 是 WL-hosted 的 ESP32-S3 协处理器固件。它将 `core/coproc-core` 适配到 ESP-IDF：使用 FreeRTOS OSAL、CherryUSB 设备 bulk 传输，以及 `esp_wifi` STA/AP/SoftAP 后端。固件配置文件（profile）为 `espressif.esp32s3.coreboard.usb-wifi`。

```text
Host（POSIX host-sim over USB bulk）
  ↕ 原始 WL-hosted 帧（24 字节 wire header，无额外封装）
ESP32-S3（本固件）
  ├─ CherryUSB device，vendor 接口，bulk OUT 0x01 / bulk IN 0x81
  ├─ core/coproc-core（link/session/credit/RPC，FreeRTOS OSAL）
  └─ esp_wifi STA/AP/SoftAP 后端 + Device Information + User Passthrough
```

## 1. 仓库定位

在 WL-hosted 多仓库工作区中，各仓库的职责边界如下：

```text
wl-hosted-coproc-esp-idf -> wl-hosted-core/coproc-core
                           -> wl-hosted-core/protocol
                           -> wl-hosted-core/common
```

- `core/coproc-core`：平台无关的 Coproc Core，包含标准 WL-hosted v1 协议状态机（Hello、Session、Credit、Channel、Heartbeat 等）。
- `core/protocol`：标准 Wire/RPC codec 以及 Simulator IPC sideband protobuf；本固件在构建时通过 nanopb 生成 C 代码。
- `core/common`：共享平台契约，OSAL 唯一来源位于 `osal/include/wlh/osal.h`。

本仓库是真实硬件固件，不是模拟器。它与 macOS Host Sim 的区别在于：

- 使用 `wlh_freertos_osal` 而非 `wlh_posix_osal`。
- 使用 USB bulk 传输原始 WL-hosted 帧，不经过 Simulator IPC record 层，也没有 sideband。
- Wi-Fi 后端调用真实 `esp_wifi` API，Ethernet TX 路径将帧转发到 AP。

## 2. 构建要求

- ESP-IDF v5.5（已测试版本；`idf_component.yml` 声明 `>=5.0`）
- 已正确设置 `IDF_PATH` 并 source `export.sh`
- `protoc` 34.1 和 `protoc-gen-nanopb` 0.4.9.1 在 `PATH` 中
- 网络访问权限，用于 ESP-IDF component manager 首次构建时拉取 `cherry-embedded/cherryusb` 1.6.1
- ESP32-S3 开发板（Core Board 或兼容板）

## 3. 构建与烧录

首次克隆后初始化子模块：

```sh
git submodule update --init --recursive
```

设置目标、构建、烧录并打开 monitor：

```sh
idf.py set-target esp32s3 build
idf.py flash monitor
```

常用命令：

```sh
idf.py build
idf.py flash
idf.py monitor
idf.py fullclean
```

## 4. 硬件与 USB 配置

本固件使用 USB 配置：`espressif.esp32s3.coreboard.usb-wifi`。

枚举信息：

| 项目 | 值 |
|------|-----|
| VID / PID | `0x303A` / `0x8201`（Espressif VID，开发用 PID；产品需申请正式 VID/PID） |
| Device class | 0x00（per-interface），USB 2.0，`bcdDevice` 0x0100 |
| Interface 0 | class 0xFF（vendor-specific），subclass 0x00，protocol 0x00 |
| Bulk OUT | EP `0x01`，Host → Device，wMaxPacketSize 64（Full Speed） |
| Bulk IN | EP `0x81`，Device → Host，wMaxPacketSize 64（Full Speed） |
| Interrupt IN | 未实现（可选的唤醒/doorbell 端点） |
| 字符串 | manufacturer `WL-hosted`，product `WL-hosted ESP32-S3 Coprocessor`，serial = 工厂 STA MAC 十六进制 |

帧格式：

- bulk 字节流直接承载标准 WL-hosted 帧，从 24 字节 frame header（magic `0x4C57`，protocol major 1）开始。
- USB packet 边界没有帧语义：一帧可能跨多个 bulk packet，一次 bulk transfer 也可能包含多帧。
- short packet 和 ZLP 仅用于终止 transfer，不表示帧边界。
- 接收方读取 header offset 6 处的 little-endian `payload_size`，等待 `24 + payload_size` 字节后校验整帧。
- 本版本不使用 `AGGREGATED` flag，每帧只携带一个 payload。
- 协商的 `max_frame_size` 为 4096 字节（含 header）。默认 checksum 使用 SUM32，后续可协商启用 CRC32C。

详细规则见 `docs/usb-profile.md` 与 `core/protocol/spec/transports/usb.md`。

## 5. 代码结构

| 路径 | 说明 |
|------|------|
| `main/app_main.c` | 程序入口：初始化 NVS、网络栈、事件循环，配置并启动 Coproc Core、Wi-Fi 后端、USB 传输与链路控制任务。 |
| `core/common/osal/src/freertos_osal.c` | FreeRTOS OSAL 适配，实现全部 `wlh_osal` 操作。 |
| `core/common/osal/include/wlh/freertos_osal.h` | FreeRTOS OSAL 公共头文件。 |
| `main/transport_usb.c/h` | CherryUSB 设备 bulk 传输实现，包括帧重组、TX 提交与总线复位处理。 |
| `main/wifi_backend.c/h` | `esp_wifi` STA/AP/SoftAP 后端，包括初始化、扫描、连接、断开、start_ap/stop_ap、AP client 事件上报与 Ethernet TX 路径。 |
| `main/device_info.c/h` | Device Information 服务提供者。 |
| `main/user_passthrough.c/h` | User Passthrough 服务实现（RPC SEND + 可选 RESULT 事件回显）。 |
| `main/CMakeLists.txt` | ESP-IDF component 注册与 Coproc Core 子目录引入。 |
| `main/idf_component.yml` | IDF component 依赖声明：`cherry-embedded/cherryusb` 1.6.1。 |
| `core/` | `wl-hosted-core` submodule（包含 protocol/、common/、host-core/ 与 coproc-core/）。 |
| `docs/usb-profile.md` | USB binding profile 详细说明。 |
| `sdkconfig.defaults` | 默认 sdkconfig：FreeRTOS 1000 Hz、启用 CherryUSB 设备栈。 |
| `dependencies.lock` | IDF component manager 锁定的依赖版本。 |

## 6. 架构与任务模型

固件启动后创建以下主要任务：

| 任务 | 栈大小 | 优先级 | 职责 |
|------|--------|--------|------|
| `wlh-core` | 8192 字节 | 5 | 运行 Coproc Core 主循环，处理链路状态机、RPC、credit、心跳等。 |
| USB TX/RX 任务 | 4096 字节 | - | CherryUSB 传输控制与 bulk 数据收发（由 `transport_usb.c` 内部创建）。 |
| `wlh-link-ctrl` | 4096 字节 | 7 | 监听 USB 总线复位事件，重启 Core 并重新协商 Hello。 |
| Wi-Fi 事件任务 | - | - | ESP-IDF 默认 Wi-Fi 事件任务，通过回调向 Core 上报扫描/连接/断开结果。 |

关键配置：

- `CONFIG_FREERTOS_HZ=1000`：为 OSAL 提供真实的 1 ms 单调时钟分辨率与心跳精度。
- `max_frame_size = 4096`
- `heartbeat_interval_ms = 1000`
- `initial_credit = 64`
- `core_queue_depth = 16`
- Core 本身保持有界/静态资源模型；OSAL 对象在底层动态分配 native handle。

USB 总线复位或重新枚举时，固件会：

1. 在 ISR 中设置 `LINK_EVENT_USB_RESET` 事件位。
2. `wlh-link-ctrl` 任务等待 100 ms 让重新枚举稳定。
3. 停止并重新启动 Coproc Core。
4. 双方使用新的 session id 重新运行 Link Hello。
5. 旧 session 的帧被丢弃。

## 7. 实现的服务

当前已实现：

- **Wi-Fi**：initialize、scan、connect、disconnect、start_ap、stop_ap，以及 Ethernet TX/RX（STA 接口）。
  - Wi-Fi 模式由 Host 在 `initialize` 时通过 `interface_flags`（bit0=STA，bit1=AP）决定，可配置为 STA-only、AP-only 或 STA+SoftAP 并存。当前 host 默认请求 STA-only。
  - SoftAP 支持 WPA2/WPA3/开放网络（空密码即开放），`max_clients` 上限 10。
  - 注意：ESP32 在 APSTA 模式下，一旦 STA 关联成功，SoftAP 信道会跟随 STA 信道；显式配置的 AP 信道可能被覆盖。
  - AP client 加入/离开通过 `WifiApClientJoinedEvent`（`0x8005`）/ `WifiApClientLeftEvent`（`0x8006`）上报。
- **Device Information**：返回厂商、MCU 型号、板级 profile、UID 等信息。
- **User Passthrough**：处理 RPC SEND，可返回可选的 RESULT 事件。

## 8. 故意未实现的内容

以下服务或扩展在本固件中故意未实现：

- Bluetooth
- OTA
- extended Diagnostics
- IO
- ADC
- KV
- User-Passthrough raw stream channel

这些省略是为了保持固件聚焦在 USB-Wi-Fi 核心路径；未来可按需扩展。

## 9. 与 macOS Host Sim 联调

在 Host 端使用 `wl-hosted-host-macos-sim` 通过 USB 连接本固件：

```sh
wl-hosted-host-macos-sim --usb 303A:8201 --scenario connect \
    --ssid <AP> --credential <passphrase>

wl-hosted-host-macos-sim --usb 303A:8201 --scenario services
```

USB 总线复位或热插拔时，Host Sim 会重新打开设备并重启链路；固件会重新协商 Hello 与 session。

## 10. 子模块与锁文件

本目录是独立 Git 仓库。修改后应单独提交，不要在工作区根目录执行全局 `git` 操作。

本仓库依赖的 `core` 子模块信息记录在：

- `.gitmodules`
- `core/` gitlink
- `SUBMODULE.lock`

更新子模块后应同步 `SUBMODULE.lock` 中的 commit，并确保：

- `core.commit` 与 gitlink 一致。

完成后执行：

```sh
git submodule update --init --recursive
git submodule status --recursive
```

`dependencies.lock` 由 ESP-IDF component manager 维护，用于锁定 `cherry-embedded/cherryusb` 等托管组件版本。

## 11. 格式化

本仓库遵循工作区统一的 `.clang-format`。不要手动格式化，应从工作区根目录运行：

```sh
./auto_format.sh
```

该脚本会格式化 Protocol、Common、两个 Core、两个 Sim 和 Manager 中的 C/C++ 文件，并排除 submodule、`third_party`、生成的 `*.pb.*`、构建目录和 Rust `target`。格式化后建议重新构建受影响目标并再次运行脚本确认幂等。

未经授权不要 push、创建 PR 或改写远端历史。
