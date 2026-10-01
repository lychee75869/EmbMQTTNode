EmbMQTTNode - 嵌入式 MQTT 边缘网关
======================================

基于 Linux 的嵌入式 MQTT 边缘网关（纯网关形态），使用 C 语言开发。
采集端统一对接 Modbus 从站（子设备）；网关侧支持 MQTT over TLS 加密上报、
断网本地缓存续传、设备身份管理、配置文件化、systemd 托管运行，
并可选接入华为云 IoTDA（网关 + 子设备物模型）。

当前版本 v0.1.0

注：华为云 IoTDA 接入（鉴权 / 主题 / 物模型 / 子设备管理 / 命令闭环）
详见 docs/项目文档.md。

快速开始
--------

1. 安装依赖（Ubuntu / WSL / Debian）

    sudo apt update
    sudo apt install build-essential libmosquitto-dev libsqlite3-dev libssl-dev

    # Modbus 模块（可选）
    sudo apt install libmodbus-dev

2. 编译主程序

    cd src
    make                              # 默认启用 Modbus
    make BUILD_WITH_MODBUS=0          # 禁用 Modbus 模块

3. 启动 MQTT Broker（本地测试）

    sudo apt install mosquitto
    mosquitto -v

    或使用 Docker：
    docker run -it -p 1883:1883 eclipse-mosquitto

4. 启动 Modbus 从站模拟器（可选，测试 Modbus 模块用）

    pip install pymodbus
    python ../tools/modbus_slave_sim.py      # TCP 默认监听 :1502

5. 运行程序

    ./embmqttnode -c ../config/node.conf

6. 订阅查看数据

    # 网关数据（子设备属性，默认客户端 ID 决定 topic）
    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data"

    # Modbus 子设备数据（如已启用）
    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data/modbus"

    # 设备状态
    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data/status"

7. 运行单元测试

    # 顶层一键（推荐）
    make test

    # 或逐个子目录运行
    cd ../tests
    make
    ./test_storage
    ./test_modbus_config
    ./test_ota
    ./test_mac_addr
    ./test_mqtt_client
    ./test_subdev_registry
    ./test_sensor_fields
    ./test_platform_local
    ./test_platform_huawei
    ./test_platform_huawei_subdev
    ./test_platform_huawei_cmd

项目结构
--------

EmbMQTTNode/
├── docs/              # 项目文档（需求、设计、华为 IoTDA 接入说明等）
├── src/               # 源代码
│   ├── main.c         # 程序入口（多线程编排：Modbus/上报/OTA/HTTP）
│   ├── common.h       # 公共返回码 / 哨兵 / json_get_string 提取器 / 版本号
│   ├── config.c/h     # 配置文件解析
│   ├── sensor_fields.c/h  # 传感器字段描述表（单一事实源）
│   ├── modbus_master.c/h  # Modbus 主站模块（RTU + TCP）
│   ├── subdev_registry.c/h # 子设备注册表（Modbus 从站 → 华为物模型映射）
│   ├── storage.c/h    # SQLite 本地缓存（断网续传 + 列迁移）
│   ├── mqtt_client.c/h    # MQTT 客户端封装（TLS + 遗嘱）
│   ├── platform.c/h   # 平台抽象层（local/huawei 单一路由分发）
│   ├── platform_local.c   # local 后端实现（mosquitto 本地路径）
│   ├── platform_huawei.c/h        # 华为 IoTDA 纯函数（鉴权/主题/payload）
│   ├── platform_huawei_subdev.c   # 华为子设备管理（注册/属性/状态/告警）
│   ├── platform_huawei_cmd.c      # 华为命令闭环（解析/回执/OTA 桥接）
│   ├── http_server.c/h    # 本地 Web Dashboard（含常量时间 token 认证）
│   ├── mac_addr.c/h   # 设备 MAC 地址读取
│   ├── ota.c/h        # A/B 分区 OTA 远程升级（签名 + HTTPS）
│   └── Makefile       # 构建（支持交叉编译 + 条件编译）
├── tests/             # 单元测试（11 个用例）
├── tools/             # 开发工具
│   ├── modbus_slave_sim.py   # Modbus 从站模拟器（8 寄存器契约，TCP 默认 :1502）
│   ├── ota_gen_firmware.sh   # OTA 固件打包脚本
│   └── sign_firmware.sh      # OTA 固件签名脚本
└── config/            # 示例配置

配置文件
--------

config/node.conf 主要配置项：

    # MQTT 连接
    broker_host = 127.0.0.1
    broker_port = 1883
    topic = embmqttnode/data

    # TLS 安全（可选）
    tls_enabled = 0         # 0=关闭 1=单向认证 2=双向认证
    tls_ca_file = /etc/embmqttnode/tls/ca.crt

    # Modbus 工业协议（可选）
    modbus_enabled = 1
    modbus_mode = tcp
    modbus_tcp_host = 127.0.0.1
    modbus_tcp_port = 1502          # 502 为特权端口，普通用户无法绑定
    modbus_reg_1 = 1,40001,1,3,int16,soil_moisture,0.1,0

    # HTTP Dashboard（可选）
    http_enabled = 1
    # POST /api/reboot 认证 token（未配置 = 一律 403，fail-closed）
    #http_reboot_token = <openssl rand -hex 32 生成>

    # OTA 远程升级（fail-closed：必须配公钥才允许升级）
    # ota_public_key = /etc/embmqttnode/ota_pub.pem   # ed25519 公钥
    # ota_ca_file / ota_ca_path                        # HTTPS 下载 CA

    # 华为云 IoTDA 平台接入（可选）
    platform = local                 # local = 本地 mosquitto；huawei = 华为 IoTDA
    # huawei_device_id / huawei_secret / huawei_auth_type / huawei_keepalive
    # subdev_offline_sec / subdevices_conf / huawei_ca_file

命令行参数
----------

    ./embmqttnode -c <config>    指定配置文件
    ./embmqttnode -h             显示帮助

    # 后台运行交给 systemd 管理（移除内置 daemon 模式）

编译选项
--------

    # 默认编译（含 Modbus）
    make

    # 禁用 Modbus 模块
    make BUILD_WITH_MODBUS=0

    # 交叉编译 ARM64
    make CROSS_COMPILE=aarch64-linux-gnu-

    # 安装到目标根文件系统
    make install DESTDIR=/path/to/rootfs

模块说明
--------

| 模块 | 文件 | 说明 |
|------|------|------|
| config | config.c/h | INI 风格配置文件解析，含 TLS/Modbus/OTA/HTTP/华为配置段 |
| sensor_fields | sensor_fields.c/h | 传感器字段描述表（单一事实源，字段名 ↔ 值；新增字段只许追加表尾） |
| modbus_master | modbus_master.c/h | Modbus RTU/TCP 主站，寄存器映射 + 类型转换；读前统一置哨兵 |
| subdev_registry | subdev_registry.c/h | 子设备注册表：Modbus 从站 → 华为物模型 device_id/service_id 映射 |
| storage | storage.c/h | SQLite 本地缓存，线程安全；含旧库列迁移 |
| mqtt_client | mqtt_client.c/h | MQTT client，TLS 1.2+、遗嘱消息、设备状态上报（_Atomic 线程安全）|
| platform | platform.c/h | 平台抽象层：单一路由分发 + local/huawei 装配 |
| platform_local | platform_local.c | local 后端：mosquitto 本地路径（等价历史行为） |
| platform_huawei | platform_huawei.c/h | 华为 IoTDA 纯函数：鉴权（HMAC）/主题/payload builders |
| platform_huawei_subdev | platform_huawei_subdev.c | 华为子设备管理：注册/属性上报/上下线/告警/退避 |
| platform_huawei_cmd | platform_huawei_cmd.c | 华为命令闭环：解析/回执/OTA 状态桥接 |
| http_server | http_server.c/h | 内嵌 HTTP 服务器 + Web Dashboard（暗色主题单页应用）|
| mac_addr | mac_addr.c/h | 设备 MAC 地址读取（设备身份）|
| ota | ota.c/h | A/B 分区 OTA：HTTPS 下载→ed25519 签名校验→安装→重启→回滚（互斥锁保护状态机）|
| main | main.c | 多线程编排（Modbus 轮询 + 上报续传 + OTA + HTTP）|
| common | common.h | 返回码、哨兵 SENSOR_VALUE_INVALID、版本号、json_get_string 严格 JSON 提取器 |

后续计划（按模块）
------------------

① 子设备（STM32 浇花执行端，Modbus 从站）
   - 从站固件上板联调；补齐 40006/40007/40008（浇水秒数 / 泵速 / 状态字）写寄存器通道。

② 网关（本模块）
   已具备：MQTT over TLS + 设备身份、Modbus 主站采集、A/B 分区 OTA 远程升级、
           本地 Web Dashboard、断网本地缓存续传、构建 / 交叉编译 / CI。
   - 华为云 IoTDA 网关 + 子设备接入完善（详见 docs/项目文档.md）。
   - Modbus 写寄存器路径：接入 40006/40007/40008（浇水秒数 / 泵速 / 状态字）。
   - 硬件上板实测：Modbus 从站真实采集与联调。

③ 云端 / 小程序
   - 华为云 IoTDA 物模型与子设备注册；微信小程序工程纳入仓库。

④ 辅助
   - 配置 / 设备身份 / Web Dashboard / 构建部署的持续维护。

> 说明：早期版本曾含规则引擎、异常引擎（Z-score / iForest）与 GPIO 执行层，均已随网关
>   纯化移除、不再维护；网关现定位为纯协议转换（南向 Modbus 采集 + 北向 MQTT/TLS 上云）。

作者
----
lychee75869
