# WL-hosted Coprocessor ESP-IDF Adapter

`wl-hosted-coproc-esp-idf` 将平台无关的 WL-hosted Coprocessor Core
适配到 ESP-IDF。固件使用 FreeRTOS OSAL、真实 `esp_wifi` 后端、ESP VHCI
Bluetooth Controller 后端，并支持两种硬件传输：

| ESP-IDF target | Transport | Profile | 最大帧 |
|---|---|---|---:|
| ESP32-S3 | CherryUSB vendor bulk | `espressif.esp32s3.coreboard.usb-wifi-ble` | 4096 |
| ESP32-C6 | ESP-IDF SDIO Slave | `espressif.esp32c6.sdio-wifi-ble` | 4092 |

Profile 名中的 `-ble` 后缀表示固件公布 Bluetooth Controller Service
（`0x0003`）和 HCI Raw Channel（`0x04`）。Host 通过 Hello 广告中的
Service/Channel 表检测该能力，不解析 profile 字符串；关闭
`CONFIG_WLH_ENABLE_BLUETOOTH_CONTROLLER` 后 profile 退回 `*-wifi`，
Hello 不公布 Bluetooth Service/Channel，行为与 Wi-Fi-only 固件一致。

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
  ├─ esp_wifi STA/AP/SoftAP backend
  └─ ESP VHCI Bluetooth Controller backend（controller-only，BLE）
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

两种目标共用的系统默认值位于 `sdkconfig.defaults`；接口、接口参数和缓存
大小等目标专用默认值位于 `sdkconfig.defaults.esp32s3` 和
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
├─ backends/        # esp_wifi 与 ESP VHCI Bluetooth backend
├─ services/        # Device Info、User Passthrough、IO、ADC、KV、pin profile
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
- `wlh-link-ctrl`：在 transport reset 后停止并重启 Core；重启前先同步
  reset Bluetooth backend。
- `wlh-bt`：Bluetooth Controller lifecycle 与双向 HCI 数据路径。
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
- RPC 形式的 User Passthrough；
- IO configure / read / write；
- ADC read；
- KV read / write / erase；
- Bluetooth Controller initialize / enable / disable / deinitialize /
  get info，以及 HCI Raw Channel（H4 Command/ACL/Event）。

未实现 OTA、extended Diagnostics 和 User-Passthrough raw stream
channel。

### Bluetooth Controller backend

`main/backends/bluetooth_backend.c` 将 Coprocessor Core 的 Bluetooth ops
映射到 ESP-IDF controller-only（VHCI）模式，仅启用 BLE：

- 每方向 `CONFIG_WLH_BLUETOOTH_HCI_QUEUE_DEPTH`（默认 16）个固定 HCI
  slot，slot 大小 1 字节 H4 type + `CONFIG_WLH_BLUETOOTH_MAX_HCI_PACKET`
  （默认 1024）字节 payload，初始化时一次性创建，之后无动态分配；
- VHCI callback 只复制数据并唤醒 `wlh-bt` task；RX slot 耗尽视为
  backpressure 违约，进入 ERROR 并发 STATE_CHANGED；
- Core NO_CREDIT 时保留 RX slot，等 `hci_tx_ready` 通知后重试；
- `DEINITIALIZE(release_memory=true)` 被拒绝：ESP32 释放 controller
  memory 是单向操作，恢复需要芯片复位；
- GET_INFO 的 `public_address` 来自 `esp_read_mac(ESP_MAC_BT)`；
  `hci_version`、`manufacturer_id`、`feature_bits` 无公开 API，保持 0。

两种目标均使用 4 MB flash 和 `partitions_ota.csv`，提供两个 1.5 MB OTA
application slot。

### 逻辑 pin 表

IO 和 ADC 的 `pin_id` 是 profile 定义的逻辑编号，不是厂商 GPIO 编号。映射表在
`main/services/pin_profile.c` 中按 target 编译，被 transport、flash、PSRAM、
strapping 和 USB-JTAG 占用的引脚不会出现在表里，host 无法通过 IO 服务扰乱链路。

| 逻辑 pin | ESP32-S3 (USB) | ESP32-C6 (SDIO) |
|---:|---|---|
| 0 | GPIO4 · ADC1_CH3 | GPIO0 · ADC1_CH0 |
| 1 | GPIO5 · ADC1_CH4 | GPIO1 · ADC1_CH1 |
| 2 | GPIO6 · ADC1_CH5 | GPIO2 · ADC1_CH2 |
| 3 | GPIO7 · ADC1_CH6 | GPIO3 · ADC1_CH3 |
| 4 | GPIO15 | GPIO6 · ADC1_CH6 |
| 5 | GPIO16 | GPIO7 |
| 6 | GPIO17 | GPIO10 |
| 7 | GPIO18 | GPIO11 |

只暴露 ADC1：Wi-Fi 运行期间 ADC2 不可用。无 ADC 能力的 pin 返回
`PERIPHERAL/NOT_SUPPORTED`，表外的 pin 返回 `PERIPHERAL/NOT_FOUND`。
读写未配置的 pin，或写一个 INPUT pin，返回 `PERIPHERAL/NOT_READY`
（协议未发布 INVALID_STATE，Core 映射到语义最接近的 NOT_READY）。
OPEN_DRAIN 下 `level=true` 表示释放线路。

### KV 存储布局

ESP-IDF 的 NVS entry 名上限是 15 字符，而协议允许 64 字节 key。因此
`main/services/kv_service.c` 在 namespace `wlh_kv` 下用 key 的 FNV-1a 64-bit
哈希（15 位十六进制）作为 entry 名，blob 内容是 `完整 key` + `\0` + `value`。
读取和擦除都会比对 blob 内嵌的完整 key，哈希碰撞返回 `STORAGE/NOT_FOUND`
而不是另一个 key 的值。每次写入后立即 `nvs_commit`，host 收到 OK 即可依赖该
值在复位后仍然存在。

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
