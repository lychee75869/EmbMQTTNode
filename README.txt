EmbMQTTNode - 嵌入式 MQTT 环境监测节点
======================================

项目简介
--------
基于 Linux 的嵌入式 MQTT 环境监测节点，使用 C 语言开发。
支持环境数据采集、MQTT 上报、断网本地缓存续传、配置文件化、守护进程运行。

当前版本为软件模拟传感器版本，无需硬件即可编译运行。
后续可在 sensor.c 中接入 BMP280/SHT30 等真实 I²C 传感器。

快速开始
--------

1. 安装依赖（Ubuntu / WSL / Debian）

    sudo apt update
    sudo apt install build-essential libmosquitto-dev libsqlite3-dev

2. 编译主程序

    cd src
    make

3. 启动 MQTT Broker（本地测试）

    sudo apt install mosquitto
    mosquitto -v

    或使用 Docker：
    docker run -it -p 1883:1883 eclipse-mosquitto

4. 运行程序

    ./embmqttnode -c ../config/node.conf

5. 订阅查看数据

    mosquitto_sub -h 127.0.0.1 -t "embmqttnode/data"

6. 运行单元测试

    cd ../tests
    make
    ./test_sensor
    ./test_storage

项目结构
--------

EmbMQTTNode/
├── docs/          # 项目文档
├── src/           # 源代码
├── tests/         # 单元测试
└── config/        # 示例配置

配置文件
--------

config/node.conf 示例：

    broker_host = 127.0.0.1
    broker_port = 1883
    topic = embmqttnode/data
    client_id = emb-node-01
    sample_interval_ms = 5000
    sensor_type = mock

命令行参数
----------

    ./embmqttnode -c <config>    指定配置文件
    ./embmqttnode -d             以守护进程运行
    ./embmqttnode -h             显示帮助

模块说明
--------

| 模块 | 文件 | 说明 |
|------|------|------|
| config | config.c/h | 配置文件解析 |
| sensor | sensor.c/h | 传感器抽象层（模拟/真实） |
| storage | storage.c/h | SQLite 本地缓存 |
| mqtt_client | mqtt_client.c/h | MQTT 客户端封装 |
| daemon | daemon.c/h | 守护进程化 |
| main | main.c | 程序入口 |

后续计划
--------

1. 采购香橙派/树莓派 + BMP280 传感器
2. 实现真实 I²C 传感器读取
3. 增加 GPIO LED 状态指示灯
4. 交叉编译到 ARM 开发板
5. 编写 systemd 自启动服务
6. 长时间稳定性测试与简历条目整理

作者
----
lychee75869
