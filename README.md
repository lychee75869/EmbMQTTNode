# 物种感知智能灌溉系统

> **当前版本：v0.1.0** —— 全项目统一版本号（四模块共用），唯一事实源为 `gateway/src/common.h` 的 `EMBMQTTNODE_VERSION`；自 2026-10-01 两项目合并起从头计。

通过摄像头识别花卉物种，按「物种档案 + 传感器数据」计算浇水量，驱动执行端完成浇水，并在云端与小程序侧可视可控。

系统由四个模块组成：**① 子设备**（STM32 浇花执行端，Modbus 从站）、**② 网关**（Orange Pi 上的 C11 边缘网关）、**③ 云端**（华为云 IoTDA + 微信小程序）、**④ 辅助**（配置 / 设备身份 / Web Dashboard / 构建部署）。

当前版本 v0.1.0
---

## 1. 模块总览

| 模块 | 目录 | 形态 | 技术栈 | 构建方式 |
|---|---|---|---|---|
| ① 子设备 | `subdevice/` | STM32 固件（Modbus 从站） | STM32F103 + HAL + Keil MDK | Keil uVision5 打开工程（无 make） |
| ② 网关 | `gateway/` | 香橙派 边缘网关（Modbus 主站） | C11 + libmosquitto / libmodbus / SQLite3 / OpenSSL，systemd 托管 | `make` |
| ③ 云端 | `cloud/` | 华为云 IoTDA 控制台配置 | 物模型 / 子设备注册 / 命令闭环 | 控制台配置（无代码） |
| ③ 小程序 | `miniprogram/` | 微信小程序 | — | 微信开发者工具导入 |
| ④ 辅助 | 见 §4 | 配置 / 设备身份 / Dashboard / CI | INI 配置、内嵌 HTTP、GitHub Actions | 随模块 |

### 数据流

```
① 子设备 (STM32)                 ② 网关 (C11, Orange Pi Zero 2W)                ③ 云端 / 小程序
  Modbus 从站 ─> RTU/TCP 轮询 ─> modbus_master ─> struct sensor_data          ┌─ 华为云 IoTDA
  保持寄存器 40001-40008             │                                            │  物模型 / 子设备
                                    ├─ platform_local      （本地 mosquitto）      └─ 微信小程序
                                    └─ platform_huawei*    ──MQTT over TLS──▶
                                    storage.c（SQLite，断网续传）  ◀── 命令闭环 / OTA ──┘
```

网关只做协议转换、云端接入、数据不丢；本地采集、GPIO 执行、端侧 ML 等职责已下移到子设备与视觉侧。

---

## 2. 各模块构建入口

### ② 网关（唯一可 `make` 的模块）

```bash
# 安装依赖（Ubuntu / WSL2）
sudo apt install build-essential libmosquitto-dev libsqlite3-dev libmodbus-dev libssl-dev

# 构建（根 Makefile 只是转发到 gateway/，两者等价）
make                                  # 含 Modbus 支持（默认）
make BUILD_WITH_MODBUS=0              # 不含 Modbus（必须同样零 warning 通过）
make CROSS_COMPILE=aarch64-linux-gnu- # 交叉编译 ARM64（上板用）

make test                             # 编译并运行全部 11 个单元测试
make install DESTDIR=/path/to/rootfs  # 安装到目标根文件系统（systemd + A/B 槽）
make dist                             # 生成 dist/embmqttnode_<ver>_<arch>.tar.gz
make clean / make distclean           # 清理

# 本地运行（直跑前先建数据目录，否则 Permission denied）
sudo mkdir -p /var/lib/embmqttnode && sudo chown "$USER" /var/lib/embmqttnode
./gateway/src/embmqttnode -c gateway/config/node.conf   # Dashboard: http://localhost:8080

# 无硬件时的从站模拟器
python gateway/tools/modbus_slave_sim.py                # TCP 127.0.0.1:1502
```

顶层 Makefile 支持的 target：`all`（默认）/ `test` / `strip` / `install` / `dist` / `clean` / `distclean` / `help`。

### ① 子设备（Keil）

用 Keil uVision5 打开 `subdevice/Projects/MDK-ARM/auto_dwater(2).uvprojx` 编译下载，走 Keil 而非 make。
工程为 STM32F103（F1）HAL 工程，BSP 含 DHT11 / 土壤与水位 ADC / 电池电量 / RTC / TIM 水泵 PWM / ST7735 LCD 菜单 / 按键 / 低功耗待机。

> 注意：工程内 `Device` 型号（`STM32F103C8`）与编译宏（`STM32F103xE`）、启动文件（`startup_stm32f103xe.s`）目前不一致，需按实际芯片统一后再烧录。

### ③ 云端 / 小程序

`cloud/`、`miniprogram/` 为预留目录，当前内容在华为云 IoTDA 控制台与独立的微信小程序工程中，尚未纳入本仓库。

---

## 3. 跨模块契约

跨模块接口必须两边同时改，否则联调必挂。

### 3.1 子设备 Modbus 寄存器布局（40001-40008）

保持寄存器（Holding Register）+ 功能码 **03**（读）/ **06·16**（写），读写在同一窗口；数值一律定点缩放存整数。

| 地址 | 名称 | 方向 | 类型 / 换算 | 单位 | 说明 |
|---|---|---|---|---|---|
| 40001 | 土壤湿度 | 读 | uint16 ÷ 10 | % | 0-100 |
| 40002 | 空气温度 | 读 | int16 ÷ 10 | ℃ | 可负 |
| 40003 | 空气湿度 | 读 | uint16 ÷ 10 | %RH | 0-100 |
| 40004 | 水槽水位 | 读 | uint16 ÷ 10 | % | 0-100 |
| 40005 | 电池电压 | 读 | uint16 ÷ 100 | V | 例如 1260 → 12.60V |
| 40006 | 浇水秒数 | 读写 | uint16 | s | 执行器设定 |
| 40007 | 泵速 | 读写 | uint16 | % | 执行器设定 |
| 40008 | 状态字 | 读 | uint16 位域 | — | bit0 水泵运行 / bit1 缺水 / bit2 土壤传感器故障 / bit3 低电量 / bit4 自动模式，bit5-15 保留 |

- **权威实现**：`gateway/config/node.conf` 的 `modbus_reg_N = slave_id,reg_addr,reg_count,func_code,data_type,field_name,scale,offset`（网关侧映射的单一事实源）
- **模拟从站**：`gateway/tools/modbus_slave_sim.py`（按同一契约造数，可用于无硬件联调）
- **回归用例**：`gateway/tests/test_modbus_config.c`
- 当前子设备固件的从站实现尚未上板，网关侧 `40006/40007/40008` 写路径也待接入。

### 3.2 子设备身份与物模型映射（IoTDA）

`gateway/config/subdevices.conf` 的 `subdevice_N = <data_source>,<source_key>,<device_id>,<name>[,<service_id>]`，其中 `device_id` 必须与华为云 IoTDA 控制台注册的子设备一致，`service_id` 缺省 `SensorData`；映射逻辑见 `gateway/src/subdev_registry.c/h`。

---

## 4. 辅助部分位置

| 内容 | 位置 |
|---|---|
| 运行配置（node.conf / subdevices.conf / systemd service + A/B launcher） | `gateway/config/` |
| 设备身份（MAC 读取） | `gateway/src/mac_addr.c/h` |
| Web Dashboard + HTTP API（单页，HTML/CSS/JS 内嵌） | `gateway/src/http_server.c/h`（`http://<host>:8080`） |
| 构建 / 交叉编译 / 打包 | 根 `Makefile`、`gateway/Makefile`、`gateway/src/Makefile`、`gateway/tools/` |
| CI（x86_64 / aarch64 / armhf 三架构矩阵） | `.github/workflows/build.yml` |

---

## 5. 验证方式

- **构建**：`make` 与 `make BUILD_WITH_MODBUS=0` 都必须零 warning 通过
- **单测**：`make test` 必须 11/11 全绿
- **联调**：`python gateway/tools/modbus_slave_sim.py` + `./gateway/src/embmqttnode -c gateway/config/node.conf`，观察启动日志与 Dashboard 数值变化

## 6. 相关文档

- `gateway/README.txt`：网关模块说明、`node.conf` 配置项、模块文件清单、版本更新历史
- `docs/项目文档.md`：契约全文、设计取舍、华为 IoTDA 接入说明
- `Agent.md`：项目约定与协作规范（含提交信息格式、硬约束）

> 后两者为**本地资料**，已纳入本仓库；契约要点与硬约束同时固化在本 README，便于快速查阅。
