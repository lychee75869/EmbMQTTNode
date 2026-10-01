# 物种感知智能灌溉系统 —— 项目约定与协作规范

## 1. 项目概述

本项目是**物种感知智能灌溉系统**：通过摄像头识别花卉物种，按「物种档案 + 传感器数据」计算浇水量，驱动执行端完成浇水，并在云端与小程序侧可视可控。系统由四个模块组成——**① 子设备**（STM32 浇花执行端，Modbus 从站）、**② 网关**（Orange Pi 上的 C11 边缘网关）、**③ 云端**（华为云 IoTDA + 微信小程序）、**④ 辅助**（配置 / 设备身份 / Web Dashboard / 构建部署）。

**本项目原为两个独立项目，现已合并为单一仓库**（网关仓库 + STM32 固件工程），目录按模块划分：

| 模块 | 目录 | 形态 | 版本控制 |
|---|---|---|---|
| ① 子设备 | `subdevice/` | STM32F103 Keil MDK 工程（正点原子风格 BSP） | 本地保留，**暂未入库**（`.gitignore` 排除） |
| ② 网关 | `gateway/` | C11 边缘网关（本仓库主体） | 已入库 |
| ③ 云端 / 小程序 | `cloud/`、`miniprogram/` | IoTDA 控制台配置 + 独立小程序工程 | 预留空目录，内容在各自平台 |

## 2. 技术栈与环境

- **网关**：语言 **C11**（`-std=c11 -Wall -Wextra`，零 warning 是硬要求）；库 **libmosquitto**（MQTT + TLS）、**libmodbus**（南向，编译期可关）、**SQLite3**（断网续传队列）、**OpenSSL**（TLS / HMAC-SHA256 / SHA256 / 固件验签）；运行环境 **ARM64 Linux**（Orange Pi Zero 2W / 全志 H618 / 四核 Cortex-A53 / 4GB / Ubuntu 24.04），以 systemd 托管；开发可在 **WSL2 Ubuntu** 本地构建与测试
- **子设备**：语言 **C**；**STM32F103 + HAL 库**（正点原子 BSP 组织方式）；**Keil uVision5（MDK-ARM）** 编译下载，**不走 make**
- 包管理器：系统依赖用 `apt`，Python 开发工具用 `pip`
- 其他关键依赖（如数据库、SDK）：SQLite 数据库文件（板上 `/var/lib/embmqttnode/data.db`）、systemd（服务 + A/B 槽 launcher）、Python 3 + `pymodbus`（仅开发期模拟从站用）

## 3. 项目架构与关键文件

```
Makefile        仓库根构建入口（仅转发到 gateway/，本身不含编译规则）
Agent.md        项目约定与协作规范（本文件）
README.md       四模块总览、构建入口、跨模块契约
.github/        CI（x86_64 / aarch64 / armhf 三架构矩阵）
gateway/        ② 网关（C11，唯一可 make 的模块）
  src/          27 个源文件（15 .c + 12 .h）
  config/       node.conf / subdevices.conf / embmqttnode.service / embmqttnode-launcher
  tests/        11 个单测源（test_*.c，每个编译为同名二进制）+ t02_e2e_broker.c/.sh（e2e）
  tools/        modbus_slave_sim.py · sign_firmware.sh · ota_gen_firmware.sh
  README.txt    网关模块说明、配置项、版本更新历史
subdevice/      ① 子设备（STM32 Keil 工程，暂未入库）
cloud/          ③ 云端（预留）
miniprogram/    ③ 小程序（预留）
docs/           项目文档（本地资料，不提交）
```

- **本仓库覆盖范围**：**四个模块并存于一个仓库**，但**跨模块契约必须保持一致**（否则联调必挂）：
  - 与子设备（模块①）：**Modbus 寄存器契约 40001-40008**（见 `README.md` §3.1 与 `gateway/config/node.conf` 的 `modbus_reg_N`；契约全文见 `docs/项目文档.md`）
  - 与云端（模块③）：**IoTDA 物模型**（服务 / 属性 / 命令）与**子设备注册映射**（`gateway/config/subdevices.conf` 的 device_id 须与控制台一致）
- **数据入口**：`gateway/src/modbus_master.c/h` —— Modbus 主站轮询（RTU/TCP），按 `node.conf` 的 `modbus_reg_N` 映射把寄存器写入 `struct sensor_data`；无从站/未编译 Modbus 时回落 mock 造数
- **页面/路由**（本地 HTTP 接口）：`gateway/src/http_server.c/h` —— `GET /`（单页 Dashboard）、`GET /api/status`、`GET /api/data/latest`、`GET /api/data/history?n=`、`GET /api/ota/status`、`POST /api/reboot`（需 token，fail-closed）
- **公共布局/组件**（公共头与共享组件）：`gateway/src/common.h`（全局类型、错误码、哨兵 `SENSOR_VALUE_INVALID`）、`gateway/src/sensor_fields.c/h`（字段描述表 —— 字段名↔值的单一事实源）、`gateway/src/platform.h`（平台 ops 函数指针表）
- **静态资源位置**：无独立静态资源目录 —— Dashboard 的 HTML/CSS/JS 以字符串内嵌在 `http_server.c`，零外部前端依赖
- **配置文件位置**：仓库内 `gateway/config/`；板上部署后运行时读 `/etc/embmqttnode/node.conf` 与 `/etc/embmqttnode/subdevices.conf`（配置改动需 `systemctl restart embmqttnode`，无热加载）
- **特殊生成目录（禁止手动修改）**：`gateway/src/*.o`、`gateway/src/*.d`、`gateway/tests/test_*`（二进制）、`gateway/tests/*.d`、`gateway/dist/`、`gateway/tools/__pycache__/`、`subdevice/` 下 Keil 的 `Objects/`、`Listings/` 等构建产物
- **数据流**：子设备（Modbus 从站）→ `modbus_master` → `struct sensor_data` → 平台抽象层 `platform.c` → `platform_local.c`（本地 mosquitto）或 `platform_huawei*.c`（华为 IoTDA）；离线时先进 `storage.c`（SQLite），恢复后由上报线程补发

## 4. 常用命令

### ② 网关（唯一可 `make` 的模块；根 Makefile 只是转发，两者等价）

```bash
# 安装依赖（Ubuntu / WSL2）
sudo apt install build-essential libmosquitto-dev libsqlite3-dev libmodbus-dev libssl-dev

# 构建
make                                  # 含 Modbus 支持（默认）
make BUILD_WITH_MODBUS=0              # 不含 Modbus（必须同样零 warning 通过）
make CROSS_COMPILE=aarch64-linux-gnu- # 交叉编译 ARM64（上板用）

# 测试（编译并运行全部 11 个单测）
make test

# 安装 / 打包
make install DESTDIR=/                # 安装到目标根文件系统（systemd + A/B 槽）
make dist                             # 生成 gateway/dist/embmqttnode_<ver>_<arch>.tar.gz

# 清理
make clean / make distclean

# 自定义脚本
python gateway/tools/modbus_slave_sim.py                # Modbus 从站模拟器（TCP 127.0.0.1:1502；--rtu <dev> 走串口）
./gateway/tools/sign_firmware.sh <私钥.pem> <固件文件>   # OTA 固件签名（产出 .sig）
./gateway/tools/ota_gen_firmware.sh 2.0.1 [交叉编译前缀] # 编译并打包 OTA 固件（.bin + SHA256 + 指令 JSON）

# 本地运行（WSL 直跑前需先建数据目录，否则 Permission denied）
sudo mkdir -p /var/lib/embmqttnode && sudo chown "$USER" /var/lib/embmqttnode
./gateway/src/embmqttnode -c gateway/config/node.conf   # Dashboard: http://localhost:8080
```

### ① 子设备（Keil，不走 make）

```bash
# Keil uVision5 打开工程后编译下载
subdevice/Projects/MDK-ARM/auto_dwater(2).uvprojx
```

- 顶层 Makefile 支持的 target：`all`（默认）/ `test` / `strip` / `install` / `dist` / `clean` / `distclean` / `help`

## 5. 测试与验证方式

- **构建验证（网关）**：`make` 与 `make BUILD_WITH_MODBUS=0` 都必须**零 warning** 通过
- **单元测试（网关）**：`make test` 必须 **11/11 全绿**；新增用例并入既有测试文件，不随意增加二进制数量
- **等值验证**：涉及 payload / 协议格式的改动，保留 **byte-exact（逐字节 strcmp）** golden 语义，禁止退化成子串匹配
- **联调验证**：`python gateway/tools/modbus_slave_sim.py` + `./gateway/src/embmqttnode -c gateway/config/node.conf`，观察启动日志与 `http://localhost:8080` 的数值变化
- **子设备验证**：Keil 编译零 error；上板前先核对芯片型号与启动文件一致（见 `README.md` §2 的 F1 工程注意事项）
- **跨模块验证**：涉及 Modbus 寄存器契约或 IoTDA 物模型的改动，两侧实现必须同步更新，并在 `README.md` §3 契约表上复核
- **其他检查**：`git status` 确认只改了预期文件；`git diff` 自查改动内容
- **遇到环境问题无法验证时，必须向用户说明，不得假装通过**

## 6. 代码规范与修改边界

### 6.1 文件与目录命名

- 源码与脚本一律 **snake_case 小写**：`modbus_master.c`、`platform_huawei_cmd.c`、`sign_firmware.sh`、`modbus_slave_sim.py`
- 一个模块 = 一对同名 `.c` / `.h`（`storage.c` ↔ `storage.h`）；文件名即模块名，不用拼音、不随意缩写
- **测试文件**统一 `test_<被测模块名>.c`（如 `test_modbus_config.c`）；**禁止**再使用任务编号前缀命名（`t02_e2e_broker.c/.sh` 为历史遗留，勿仿）
- 子设备沿用**正点原子惯例**：BSP 目录名大写（`Drivers/BSP/DHT11/`），目录内文件小写（`dht11.c`），目录名与被驱动外设一致

### 6.2 变量命名

- 变量、函数、字段一律 **snake_case**；函数名 = **模块前缀 + 动词**（`ota_init()`、`config_load()`、`storage_save()`）
- 文件作用域的全局/静态变量加 **`g_` 前缀**（`g_cfg`、`g_mosq`、`g_running`）；`static` 是默认，不得为省事去掉
- 子设备固件沿用正点原子惯例，在 `g_` 之外另允许**模块级局部前缀 `l_`**（`l_moter_q`）
- 不得裸用魔法数：尺寸、上限、哨兵一律走宏（`MODBUS_REG_MAX`、`SENSOR_VALUE_INVALID`）
- **禁止 camelCase** 出现在 C 标识符中（第三方库与内嵌 JS 除外，见 6.4）

### 6.3 类型与宏命名

- `struct` / `enum` 定义名 **小写 snake**（`struct sensor_data`、`enum ota_state`）；不新增无意义的裸 `typedef struct`
- typedef 加 **`_t` 后缀**（`sensor_source_t`）；函数指针表同样以小写结构体承载（`struct platform_ops`）
- 宏与枚举常量一律 **UPPER_SNAKE**（`E_OK`、`SENSOR_VALUE_INVALID`、`OTA_STATE_IDLE`、`MODBUS_REG_MAX`）
- 头文件守卫用 **`<文件名大写>_H`**（`common.h` → `COMMON_H`、`config.h` → `CONFIG_H`）
- 结构体字段名与配置项名对齐（`node.conf` 的键 ↔ `struct node_config` 的字段），避免同义异名

### 6.4 豁免项（不算违规）

- **第三方库自带的 camelCase**：STM32 HAL（`TIM_HandleTypeDef`、`HAL_TIM_OC_Init`）、libmodbus / libmosquitto / SQLite 的 API 名
- **内嵌于 `http_server.c` 的 Dashboard JS**：遵循 JS 惯例用 camelCase（`fmtTime`、`pollStatus`），与 C 代码以字符串边界物理隔离

### 6.5 注释格式

- 注释以**中文**为主，只需简述这个函数或变量等的功能
- **文件头**用 `/* */` 块注释说明模块职责；**函数 / 结构体 / 字段**用 `/* */` 说明语义与取值约定；**行内短注**可用 `//`
- **变更注释不带版本号**，格式 `/*<什么功能> */`（如 `/* 每从站每轮一条记录*/`）
- 长文件内部用分隔线分节：`/* ─── Modbus 协议配置 ─────────────────────── */`
- 禁止留下与代码不符的过期注释；改动逻辑时同步改注释

### 6.6 代码规范与修改边界

- 文件/函数职责单一；**单文件目标 ≤ 600 行**。**已知超限（历史遗留，实测）：`gateway/src/ota.c` 1633 行、`gateway/src/http_server.c` 931 行、`gateway/src/mqtt_client.c` 682 行** —— 改动时不做无关重构，但**新增逻辑优先放进新文件/新模块**，不要再往这几个文件里堆
- 优先复用现有组件/工具函数：字段取值/设值一律走 `sensor_fields` 的 `sensor_get_field` / `sensor_set_field` / `for_each_field`，**禁止新增字段名 strcmp 链**
- **禁止引入新的第三方库或框架**（项目定位「零框架、零新增依赖」）；确有必要的，先向用户说明理由
- 不修改生成目录：`gateway/src/*.o`、`gateway/src/*.d`、`gateway/tests/test_*`、`gateway/dist/`、`gateway/tools/__pycache__/`、Keil 的 `Objects/`、`Listings/`
- **硬约束（任何改动都必须遵守）**：
  - **表序不变式**：`SENSOR_FIELDS[]` 新增字段只许**追加表尾**，严禁插入/重排（local 路径 payload 基线依赖表序），并同步 `SENSOR_FIELD_COUNT` 与静态断言
  - **哨兵统一**：无效/未采集值一律 `SENSOR_VALUE_INVALID`（-999.0）；新增字段的初始化必须走 `for_each_field` 统一置哨兵（否则 0.0 会被下游当成有效值上报假数据）
  - **`SOURCE_LOCAL` 枚举保留**（旧数据库回放兼容），不得删除
  - **`BUILD_WITH_MODBUS=0/1` 双配置都必须零 warning 编过**
  - **跨模块契约禁止单侧修改**：Modbus 寄存器布局、IoTDA 物模型、子设备 `device_id` 映射任一侧改动，必须同步另一侧与本仓库契约文档

### 6.7 版本号与提交信息

- **全项目统一使用一个版本号**（四个模块共用同一个号），**自 2026-10-01 两项目合并起从 `v0.1.0` 从头计**；此前的 `v1.6.x` 属合并前网关旧线，不再延续
- **唯一事实源**：`gateway/src/common.h` 的 `EMBMQTTNODE_VERSION` 宏；该宏与提交信息中的版本号**必须一致**，改一处必须同步另一处
- **递增规则**：`X` 主版本 = 架构级破坏性变更；`Y` 次版本 = 新增功能 / 新模块 / 新协议支持；`Z` 修订号 = 修 bug、重构、注释与文档
- **0.x 语义**：`v1.0.0` 之前视为开发期，四模块跑通首个完整可用版本后再升 `v1.0.0`
- **提交信息格式**：`v<版本号>:<这件事做了什么>`，**纯中文**；一次提交含多项改动时**分点描述**；不写任何内部编号，不使用英文 `feat/fix/chore` 等前缀
- 每个 commit 自带版本号（随提交递增），不再有独立的 release bump 提交
- **不得修改**：本文件 `Agent.md`（由用户本人维护，或在用户本人授权下交由 AI 维护）、`BUG_AUDIT.md`、`docs/`、`.workbuddy/`（均为本地资料，已在 `.gitignore` 中，不提交）

## 7. 工作原则（Agent 行为准则）

- **先思考再编码**：实施前明确假设，不确定时主动询问；如有多个解释，列出选项，不擅自选择；更简单的方案优先，敢于拒绝不合理要求。
- **简洁优先**：只写解决问题所需的最少代码，不增加未要求的抽象、配置或“灵活性”。
- **改动前先理解**：读懂相关文件现有逻辑，再动手修改。
- **最小改动原则**：优先局部修改，避免无关重构。
- **遇模糊即停止**：指出哪里不清楚，请求澄清，不猜测。
