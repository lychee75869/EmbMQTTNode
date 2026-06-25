# EmbMQTTNode - 嵌入式 MQTT 边缘网关

基于 C11 的嵌入式 Linux IoT 边缘网关。当前版本 **v0.2**。

## 项目状态

```
已发布: v0.2 (2026-06-25)
├── 阶段一 ✅ MQTT over TLS + 设备身份
├── 阶段二 ✅ Modbus RTU/TCP 工业协议
├── 阶段三 ⏳ 规则引擎 + 本地告警（任务清单已确认，待编码）
├── 阶段四 ❌ A/B 分区 OTA 远程升级
├── 阶段五 ❌ 本地 Web Dashboard
├── 阶段六 ❌ 构建系统 + 交叉编译 + CI
└── 方向 B ❌ 边缘 AI 异常检测（后续）

当前分支: main (领先 origin/main 2 commits)
最后提交: 0288b91 v0.2: Build compatibility fixes
```

## 下一步

实施 **阶段三：规则引擎 + 本地告警**。任务清单见 docs/09_方案A_编码实施计划.md 阶段三部分。
涉及文件：
- 新增: src/rule_engine.c/h, src/gpio_hal.c/h, tests/test_rule_engine.c
- 修改: src/common.h, src/config.c, src/main.c, src/Makefile, config/node.conf, README.txt

## 构建

```bash
# 在 WSL Ubuntu 中
cd /mnt/d/testcode/EmbMQTTNode/src
make                          # 含 Modbus
make BUILD_WITH_MODBUS=0      # 不含 Modbus

# 测试
cd ../tests && make && ./test_sensor && ./test_storage && ./test_modbus_config
```

## 依赖

```bash
sudo apt install build-essential libmosquitto-dev libsqlite3-dev libmodbus-dev
```

## 架构

```
src/
├── main.c           # 入口，3 线程（采集 + Modbus + 上报）
├── common.h         # 全局类型、配置结构体
├── config.c/h       # INI 解析
├── sensor.c/h       # I2C 传感器抽象（mock/BMP280/SHT30）
├── modbus_master.c/h # Modbus RTU/TCP（libmodbus，编译期可选）
├── storage.c/h      # SQLite 离线缓存
├── mqtt_client.c/h  # MQTT + TLS + 遗嘱 + 状态上报
├── daemon.c/h       # 双重 fork 守护进程
├── rule_engine.c/h  # （待实现）规则引擎
└── gpio_hal.c/h     # （待实现）GPIO 抽象层
```

## 关键约束

- **Linux only**（epoll/timerfd/eventfd 不需要，但使用了 sysfs、libgpiod 等 Linux API）
- 编译标准: C11 (`-std=c11`)
- 严格兼容性: 必须同时支持 `BUILD_WITH_MODBUS=1` 和 `=0`，零警告
- 条件编译的 `#ifdef BUILD_WITH_MODBUS` 边界要精确，mock 函数必须在 ifdef 外部
- WSL DNS 问题: 需先 `sudo rm /etc/resolv.conf && echo "nameserver 8.8.8.8" | sudo tee /etc/resolv.conf && sudo chattr +i /etc/resolv.conf`

## 与 HttpFramework 的关系

HttpFramework 是 C++17 HTTP 服务框架（另一个独立项目），后续阶段五可能在 EmbMQTTNode 中嵌入轻量 HTTP 服务作为本地 Web Dashboard。
