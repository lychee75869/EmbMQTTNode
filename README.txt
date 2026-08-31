EmbMQTTNode - 嵌入式 MQTT 边缘网关
======================================

基于 Linux 的嵌入式 MQTT 边缘计算网关，使用 C 语言开发。
支持多协议传感器数据采集（I²C + Modbus）、MQTT over TLS 加密上报、
断网本地缓存续传、设备身份管理、配置文件化、守护进程运行。

当前版本 v1.2.1

快速开始
--------

1. 安装依赖（Ubuntu / WSL / Debian）

    sudo apt update
    sudo apt install build-essential libmosquitto-dev libsqlite3-dev

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

4. 启动 Modbus 模拟器（可选，测试 Modbus 模块用）

    pip install pymodbus
    python ../tools/modbus_slave_sim.py

5. 运行程序

    ./embmqttnode -c ../config/node.conf

6. 订阅查看数据

    # 传感器数据
    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data"

    # Modbus 数据（如已启用）
    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data/modbus"

    # 设备状态
    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data/status"

7. 运行单元测试

    cd ../tests
    make
    ./test_sensor
    ./test_storage
    ./test_modbus_config
    ./test_rule_engine
    ./test_ota
    ./test_anomaly_engine

项目结构
--------

EmbMQTTNode/
├── docs/              # 项目文档（需求、设计、硬件选型、实施计划）
├── src/               # 源代码
│   ├── main.c         # 程序入口
│   ├── config.c/h     # 配置文件解析
│   ├── sensor.c/h     # 传感器抽象层（I²C SHT30/ADS1115/mock）
│   ├── modbus_master.c/h  # Modbus 主站模块（RTU + TCP）
│   ├── storage.c/h    # SQLite 本地缓存（断网续传）
│   ├── mqtt_client.c/h    # MQTT 客户端封装（TLS + 遗嘱）
│   ├── rule_engine.c/h  # 规则引擎 + 本地告警
│   ├── http_server.c/h  # 本地 Web Dashboard
│   ├── anomaly_engine.c/h  # 异常检测引擎（Z-score + Isolation Forest）
│   ├── gpio_hal.c/h     # GPIO 硬件抽象层
│   ├── ota.c/h        # A/B 分区 OTA 远程升级
│   ├── daemon.c/h     # 守护进程化
│   └── Makefile       # 构建（支持交叉编译 + 条件编译）
├── tests/             # 单元测试
├── tools/             # 开发工具
│   ├── modbus_slave_sim.py   # Modbus TCP 从站模拟器
│   ├── anomaly_train.py    # iForest 模型训练 + C 导出
│   └── ota_gen_firmware.sh   # OTA 固件打包脚本
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
    modbus_tcp_port = 502
    modbus_reg_1 = 1,40001,1,3,int16,temperature,0.1,0

命令行参数
----------

    ./embmqttnode -c <config>    指定配置文件
    ./embmqttnode -d             以守护进程运行
    ./embmqttnode -h             显示帮助

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
| config | config.c/h | INI 风格配置文件解析，含 TLS + Modbus 配置段 |
| sensor | sensor.c/h | 传感器抽象层，支持 SHT30/ADS1115/Mock |
| modbus_master | modbus_master.c/h | Modbus RTU/TCP 主站，寄存器映射 + 类型转换 |
| storage | storage.c/h | SQLite 本地缓存，线程安全 |
| mqtt_client | mqtt_client.c/h | MQTT client，TLS 1.2+、遗嘱消息、设备状态上报 |
| daemon | daemon.c/h | 标准双重 fork 守护进程化 |
| rule_engine | rule_engine.c/h | 规则引擎，支持 gt/lt/eq/ne/outside/rate 运算符 + 冷却防抖 |
| anomaly_engine | anomaly_engine.c/h | 异常检测引擎：Z-score 统计 + Isolation Forest 推理 |
| http_server | http_server.c/h | 内嵌 HTTP 服务器 + Web Dashboard（暗色主题单页应用）|
| gpio_hal | gpio_hal.c/h | GPIO 抽象层，mock 模式（开发）/ libgpiod（真实硬件）|
| ota | ota.c/h | A/B 分区 OTA 远程升级：HTTP 下载→SHA256 校验→安装→重启→回滚 |
| main | main.c | 多线程编排（采集 + Modbus + 规则引擎 + 上报） |

后续计划
--------

1. ~~MQTT over TLS + 设备身份~~ ✅ 阶段一完成
2. ~~Modbus 工业协议接入~~ ✅ 阶段二完成
3. ~~规则引擎 + 本地告警 + GPIO 联动~~ ✅ 阶段三完成
4. ~~A/B 分区 OTA 远程升级~~ ✅ 阶段四完成
5. ~~本地 Web Dashboard~~ ✅ 阶段五完成
6. ~~构建系统 + 交叉编译 + CI~~ ✅ 阶段六完成
7. ~~边缘 AI 异常检测~~ ✅ 方向 B 完成（Z-score + iForest）
8. 硬件上板实测：ADS1115 / SHT30 真实采集（Orange Pi Zero 2W）

作者
----
lychee75869
