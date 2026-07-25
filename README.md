# WL-hosted Coprocessor ESP-IDF Adapter

`wl-hosted-coproc-esp-idf` 将平台无关的 WL-hosted Coprocessor Core
适配到 ESP-IDF。固件使用 FreeRTOS OSAL、真实 `esp_wifi` 后端，并支持两种
硬件传输：

| ESP-IDF target | Transport | Profile | 最大帧 |
|---|---|---|---:|
| ESP32-S3 | CherryUSB vendor bulk | `espressif.esp32s3.coreboard.usb-wifi` | 4096 |
| ESP32-C6 | ESP-IDF SDIO Slave | `espressif.esp32c6.sdio-wifi` | 4092 |

ESP32-S3 没有 SDIO Slave 外设，因此不能在同一块 S3 硬件上切换为 SDIO。
Kconfig 会根据目标芯片只显示硬件支持的传输：S3 默认为 USB，C6 默认为
SDIO。

```text
Host
  ↕ 原始 WL-hosted frame
ESP-IDF Adapter
  ├─ USB bulk（ESP32-S3）或 SDIO Slave（ESP32-C6）
  ├─ core/coproc-core（link/session/credit/RPC）
  ├─ core/common FreeRTOS OSAL
  └─ esp_wifi STA/AP/SoftAP backend
```

Simulator IPC sideband 不进入真实硬件传输。USB 和 SDIO 都直接承载标准
WL-hosted wire frame。

## 构建

要求：

- ESP-IDF 5.5（锁文件当前由 5.5.2 生成）；
- 已初始化递归子模块；
- `protoc` 34.1 和 `protoc-gen-nanopb` 0.4.9.1 位于 `PATH`；
- 首次构建 ESP32-S3 时允许 Component Manager 下载 CherryUSB 1.6.1。

初始化：

```sh
git submodule update --init --recursive
source /path/to/esp-idf/export.sh
```

构建 ESP32-S3 USB 固件：

```sh
idf.py set-target esp32s3
idf.py build
```

构建 ESP32-C6 SDIO 固件：

```sh
idf.py set-target esp32c6
idf.py build
```

如果在同一工作树中交替构建两个目标，建议使用独立目录：

```sh
idf.py -B build-esp32s3 -DIDF_TARGET=esp32s3 build
idf.py -B build-esp32c6 -DIDF_TARGET=esp32c6 build
```

目标专用默认值位于 `sdkconfig.defaults.esp32s3` 和
`sdkconfig.defaults.esp32c6`。也可以通过 `idf.py menuconfig` 在
`WL-hosted Coprocessor Configuration` 中调整当前芯片支持的传输参数。

依赖锁按目标拆分为：

- `dependencies.lock.esp32s3`：ESP-IDF + CherryUSB；
- `dependencies.lock.esp32c6`：ESP-IDF。

## 传输

### USB

ESP32-S3 使用一个 vendor-specific interface：

| 项目 | 值 |
|---|---|
| VID/PID | `0x303A:0x8201` |
| Bulk OUT | `0x01`, Host → Device |
| Bulk IN | `0x81`, Device → Host |
| Max packet | 64 bytes, Full Speed |

USB 是字节流，packet 边界没有 frame 语义；Adapter 根据 24-byte wire header
重组完整 frame。总线 reset 或重枚举会终止旧 session 并重新 Hello。详细规则
见 [USB profile](docs/usb-profile.md)。

### SDIO

ESP32-C6 使用 Function 1、4-bit、512-byte block 和 packet sending mode。
每个 SDIO transaction 精确包含一个完整原始 WL-hosted frame，不增加
ESP-Hosted MCU 的私有 header 或 checksum。

ESP-IDF SDIO Slave 单次 TX 上限为 4092 bytes，因此 SDIO profile 的
`max_frame_size` 也是 4092。RX 使用预注册 DMA buffer，TX 使用有界队列和
DMA bounce buffer。

ESP32-C6 固定引脚、外部上拉、时序和 reset 约定见
[SDIO profile](docs/sdio-profile.md)。当前仓库只实现 Coprocessor 侧，
匹配的 Host SDIO Master 不在本仓库中。

## 代码结构

```text
main/
├─ app/             # app_main 与通用 link reset 控制
├─ backends/        # esp_wifi backend
├─ services/        # Device Info、User Passthrough
├─ transports/
│  ├─ transport.h   # Adapter 内部统一 transport 接口
│  ├─ usb/          # ESP32-S3 CherryUSB bulk
│  └─ sdio/         # ESP32-C6 SDIO Slave
├─ CMakeLists.txt
├─ Kconfig.projbuild
└─ idf_component.yml
```

`core/` 是 `wl-hosted-core` 子模块。Core、Protocol 和 Common 保持平台无关，
ESP-IDF/FreeRTOS/USB/SDIO 细节只存在于本 Adapter。

## 运行模型

- `wlh-core`：Coprocessor Core 状态机与 RPC。
- `wlh-link-ctrl`：在 transport reset 后停止并重启 Core。
- USB：独立 TX/RX task 和 CherryUSB ISR。
- SDIO：独立 TX、TX completion、RX 和 reset task。
- Wi-Fi：ESP-IDF event task 将异步结果上报 Core。

两种 transport 都使用有界队列。Core TX buffer 只有在硬件传输完成或明确
失败后才通过 completion 归还。

## 服务

当前实现：

- Wi-Fi initialize、scan、connect、disconnect、start AP、stop AP；
- STA/AP Ethernet TX/RX；
- AP client join/leave event；
- Device Information；
- RPC 形式的 User Passthrough。

未实现 Bluetooth、OTA、extended Diagnostics、IO、ADC、KV 和
User-Passthrough raw stream channel。

## CI 和格式化

GitHub Actions 会在 ESP-IDF 5.5.2 容器中并行构建：

- ESP32-S3 + USB；
- ESP32-C6 + SDIO。

两种固件的 bootloader、partition table、application binary、ELF、map、
flash arguments 和 sdkconfig 会作为 artifact 上传。

从多仓库工作区根目录格式化：

```sh
./auto_format.sh
```

不要提交 `build*`、`managed_components` 或生成的 `sdkconfig`。未经授权不要
push、创建 PR 或改写远端历史。
