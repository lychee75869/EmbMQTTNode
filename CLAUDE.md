# EmbMQTTNode - 嵌入式 MQTT 边缘网关

基于 C11 的嵌入式 Linux IoT 边缘网关。当前版本 **v1.1.0**。

## 项目状态

```
已发布: v1.1.0
├── 阶段一 ✅ MQTT over TLS + 设备身份
├── 阶段二 ✅ Modbus RTU/TCP 工业协议
├── 阶段三 ✅ 规则引擎 + 本地告警
├── 阶段四 ✅ A/B 分区 OTA 远程升级
├── 阶段五 ✅ 本地 Web Dashboard（v1.0.0 当前）
├── 阶段六 ✅ 构建系统 + 交叉编译 + CI
├── 方向 B ✅ 边缘 AI 异常检测（Z-score + iForest）
└── 后续    接入真实传感器（BMP280/SHT30）

当前分支: main
最后提交: v1.1.0 方向 B 边缘 AI 异常检测
```

## 下一步

启动 **方向 B：边缘 AI 异常检测**。

## 构建

```bash
# 在 WSL Ubuntu-26.04 中（已安装全部依赖）
cd /mnt/d/testcode/EmbMQTTNode

# 顶层 Makefile（推荐）
make                          # 编译（含 Modbus）
make BUILD_WITH_MODBUS=0      # 编译（不含 Modbus）
make test                     # 编译并运行全部 6 个测试
make strip                    # 去除调试符号
make install DESTDIR=/path    # 安装到目标根文件系统
make dist                     # 生成发布 tarball
make distclean                # 深度清理（含 .d 依赖文件）

# 交叉编译 ARM64
make CROSS_COMPILE=aarch64-linux-gnu-
make CROSS_COMPILE=arm-linux-gnueabihf-

# 或直接进入 src/ 构建
cd src
make                          # 含 Modbus
make BUILD_WITH_MODBUS=0      # 不含 Modbus
make obj                      # 仅编译 .o（不链接，用于交叉编译检查）

# 测试
cd ../tests && make && ./test_sensor && ./test_storage && ./test_modbus_config && ./test_rule_engine && ./test_ota && ./test_anomaly_engine
```

## 依赖

```bash
sudo apt install build-essential libmosquitto-dev libsqlite3-dev libmodbus-dev libssl-dev
```

## 架构

```
src/
├── main.c           # 入口，4 线程（采集 + Modbus + 上报 + HTTP Dashboard）
├── common.h         # 全局类型、配置结构体
├── config.c/h       # INI 解析
├── sensor.c/h       # I2C 传感器抽象（mock/BMP280/SHT30）
├── modbus_master.c/h # Modbus RTU/TCP（libmodbus，编译期可选）
├── storage.c/h      # SQLite 离线缓存
├── mqtt_client.c/h  # MQTT + TLS + 遗嘱 + OTA 回调 + 状态上报
├── daemon.c/h       # 双重 fork 守护进程
├── rule_engine.c/h  # 规则引擎 + 本地告警（v0.3）
├── gpio_hal.c/h     # GPIO 硬件抽象层（v0.3）
├── ota.c/h          # A/B 分区 OTA 远程升级（v0.4）
├── http_server.c/h  # 本地 Web Dashboard（v1.0.0）
├── anomaly_engine.c/h # 异常检测引擎：Z-score + iForest（方向 B）
```

## Web Dashboard（阶段五）

启动后访问 `http://<设备IP>:8080`：

```
REST API:
  GET  /                    → HTML 仪表盘（单页应用，暗色主题）
  GET  /api/status          → 设备状态 JSON
  GET  /api/data/latest     → 最新传感器数据
  GET  /api/data/history?n=N → 最近 N 条记录
  GET  /api/rules           → 规则引擎统计
  GET  /api/ota/status      → OTA 升级状态
  POST /api/reboot          → 触发重启（需 token）
```

仪表盘特性：
- 纯 C + 原生 HTML/CSS/JS，零外部前端依赖
- 暗色工业主题，响应式布局（桌面/移动端适配）
- 实时数据 2 秒自动刷新，温度趋势条形图
- 规则引擎触发统计表，MQTT/Modbus/OTA 状态指示灯

## CI

GitHub Actions (`.github/workflows/build.yml`)：push/PR 触发，3 架构矩阵构建：
- **x86_64**: 完整编译 + 链接 + 运行全部 6 个测试 + 上传 artifact
- **aarch64 / armhf**: 交叉编译 `.o` 文件检查（obj 目标，不链接）

## 关键约束

- **Linux only**（epoll/timerfd/eventfd 不需要，但使用了 sysfs、libgpiod 等 Linux API）
- 编译标准: C11 (`-std=c11`)
- 严格兼容性: 必须同时支持 `BUILD_WITH_MODBUS=1` 和 `=0`，零警告
- 条件编译的 `#ifdef BUILD_WITH_MODBUS` 边界要精确，mock 函数必须在 ifdef 外部
- WSL DNS 问题: 需先 `sudo rm /etc/resolv.conf && echo "nameserver 8.8.8.8" | sudo tee /etc/resolv.conf && sudo chattr +i /etc/resolv.conf`
- `.d` 依赖文件由 `-MMD -MP` 自动生成，已加入 `.gitignore`，不会被提交

## 与 HttpFramework 的关系

HttpFramework 是 C++17 HTTP 服务框架（另一个独立项目），后续阶段五可能在 EmbMQTTNode 中嵌入轻量 HTTP 服务作为本地 Web Dashboard。
