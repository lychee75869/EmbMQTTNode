# EmbMQTTNode Bug 台账（2026-09-07 全量整合版）

> 来源：团队双线静态审查（核心模块 + 业务模块）+ 用户自查 10 项疑点逐条源码复核。
> 判定均已对照源码核实（引用行号），未修改任何源码。
> 严重度：P0 = 崩溃/安全/核心功能失效；P1 = 功能错误/可靠性；P2 = 隐患/可维护性。

## 一、P0 — 必须立即修复（5 项）

| # | 位置 | 问题 | 修复方向 |
|---|------|------|---------|
| P0-1 | `src/http_server.c:189-192`（调用方 `:805`） | **HTTP 请求行解析栈溢出（远程可利用）**：`sscanf(line, "%31s %255s %*s%n", ...)` 硬编码 31 字符，但调用方 `method` 缓冲仅 16 字节；`(void)mlen` 显式丢弃长度参数。服务器绑定 0.0.0.0:8080，局域网内任意主机发送超长方法名即可溢出栈帧。 | 放弃 sscanf 手动切分 token，或按 mlen 动态构造格式串；同时校验方法名长度。 |
| P0-2 | `src/main.c:393` vs `src/main.c:447` | **daemon 模式下 MQTT 网络线程丢失**：`mqtt_init()`（内部 `mosquitto_loop_start()` 创建后台线程）先于 `daemonize()`（双重 fork）执行。POSIX fork 只保留调用线程 → `-d` 启动后 MQTT 100% 失联。 | 把 `daemonize()` 移到**所有** pthread_create 之前（配置加载后立即执行）。不能只对调两行。 |
| P0-3 | `src/ota.c:288-350` | **A/B 回滚机制实际不可触发**：`g_boot_attempt` 首次启动恒为 0 且函数在 0 时直接 return 从不递增，新固件崩溃后永远不会回滚。 | 启动时无条件写 boot_count=1，运行稳定 N 秒后清零（健康指示器），崩溃由重启自然递增。 |
| P0-4 | `src/main.c:297-302` | **断网续传反丢数据**：`mqtt_publish()` 返回值未检查，循环后无条件 `storage_delete_sent(max_ts)`。发布失败（E_NET）的数据被永久删除。 | 逐条检查返回值，仅删除确认成功的；建议改按主键 id 删除（避免时钟回退误删）。 |
| P0-5 | `src/ota.c:475-616` | **OTA 固件下载走明文 HTTP、无数字签名**：仅支持 `http://`，完整性仅靠 MQTT 下发的 SHA256（可被伪造指令绕过）。 | 增加 HTTPS + 证书校验，或固件签名（ECDSA/RSA over SHA256）双重校验。 |

## 二、P1 — 尽快修复（12 项）

| # | 位置 | 问题 | 修复方向 |
|---|------|------|---------|
| P1-1 | `src/main.c:50-76` | **get_mac_address() 双重 fclose + lo 兜底失效**：`i == 6` 永假（有效下标 0-5），lo 接口读到 MAC 时先 fclose 后落到函数尾再次 fclose 同一 FILE*（UB）。设备仅有回环网卡时 100% 触发。 | 重构为单点关闭（goto cleanup）；让 lo 也正常返回。 |
| P1-2 | `src/rule_engine.c`、`src/anomaly_engine.c`、`src/main.c:253/273` | **规则/异常引擎共享状态数据竞争**：modbus.enabled=1 时 sample_thread 与 modbus_thread 并发调用两个 evaluate，无锁读写 `rate_history`、`window`、`last_triggered`、`g_stats`（C11 UB）。http_thread 读 g_stats 也构成读写竞争。 | 两引擎各加一把 mutex；或架构上串行化（单一评估队列）。注意加锁后异源数据混窗口的语义问题仍在。 |
| P1-3 | `src/mqtt_client.c:17`、`src/ota.c:62` 等 | **跨线程共享变量无同步**：`g_connected`、`g_ota_cb`、`g_mqtt_publish` 被 MQTT 后台线程与业务线程并发读写。 | 改用 C11 `_Atomic` 或 mutex。 |
| P1-4 | `src/modbus_master.c:246-255` + `src/config.c:153` | **Modbus reg_count 无上限校验**：`reg_buf[MODBUS_REG_MAX=32]` 栈缓冲，配置写 `reg_count>32` 即写穿（仅真实 Modbus 编译路径）。 | 双层防护：config 层校验 `1<=reg_count<=32`（含 data_type 最少寄存器数）+ 调用前防御性检查。 |
| P1-5 | `src/http_server.c:101-118, 799-822` | **HTTP 无 recv 超时（Slowloris DoS）**：client_fd 未设 SO_RCVTIMEO，慢速攻击可永久阻塞 HTTP 线程，且导致退出时 `pthread_join` 死锁、进程关不掉。 | client_fd 设 SO_RCVTIMEO=10s。 |
| P1-6 | `src/http_server.c:433-464` | **重启接口认证形同虚设**：硬编码 token `"reboot123"` 明文传输（无 TLS）、`strstr` 子串匹配可绕过、`system("reboot")` 可被 PATH 劫持、无限速无 CSRF。 | token 改配置 + 常量时间比较；`reboot(RB_AUTOBOOT)` 替代 system()；加限速。 |
| P1-7 | `src/ota.c:724-796` | **OTA 指令 JSON 解析忽略 payload_len**：`(void)len` 后全程 strstr，依赖 NUL 结尾（libmosquitto 不保证）。触发条件苛刻（复核后自 P0 降级），但违反安全编码。 | 引入 cJSON/jansson 正式解析；或 on_message 先拷贝补 NUL。 |
| P1-8 | `src/ota.c:730` | **"cmd" 字段未校验值**：`strstr("cmd") && strstr("upgrade")` 双子串检查，`{"cmd":"foo","note":"upgrade"}` 也能通过。 | 用真正的 JSON 解析（与 P1-7 一并解决）。 |
| P1-9 | `src/mqtt_client.c:238-280` | **状态上报 JSON 载荷可能截断**：`payload[512]` 固定，hostname+cpu_model+kernel_ver+mac 等字段累加逼近上限，截断产生无效 JSON。 | 动态分配或拆分消息。 |
| P1-10 | `src/main.c:283-309` + `src/ota.c` | **OTA 下载阻塞整个上传线程**：`ota_http_download` 在 upload_thread 同步执行，下载期间离线缓存堆积无法上传。 | 独立 OTA worker 线程。 |
| P1-11 | `src/ota.c:435-449, 674-701` | **关键文件写盘无 fsync**：boot_count、current_slot、固件文件均未 fsync。断电后可能出现"slot 已切换但固件不完整"，叠加 P0-3 回滚失效。 | 写完调用 `fdatasync()`/`fsync()`。 |
| P1-12 | `src/common.h:132`（**未提交改动**） | **注释语义错误**：`OP_OUT` 实际只读 `threshold_lo/hi`，不读 `threshold`；改动把 `out` 加进 threshold 注释与代码行为及 `config.c:233` 注释矛盾。 | 回滚该注释改动；如需标注，加在 `threshold_lo/hi` 注释上。 |

## 三、P2 — 计划性硬化（18 项）

| # | 位置 | 问题 |
|---|------|------|
| P2-1 | `src/ota.c:582-598` | OTA header 跨 recv 边界解析失败（不支持 chunked/超大头） |
| P2-2 | `src/main.c:293-303` + `src/storage.c:14-22` | 离线缓存的 Modbus 数据补发走错 topic（表无 source 字段，需 migration） |
| P2-3 | `src/http_server.c:319-323, 476-483` | HTTP 线程退出未优雅收尾现存连接 |
| P2-4 | `src/anomaly_engine.c:204-209` | 异常值无条件入窗污染后续 Z-score 基线（有意设计但属算法弱点）；冷却期窗口冻结 |
| P2-5 | `src/rule_engine.c:52, 67` | `rate_history` 魔数 16 硬编码（应定义 `RULE_RATE_WINDOW` 宏） |
| P2-6 | `src/gpio_hal.c:79-91` | 真实硬件分支缺 value 合法性校验（mock 分支有） |
| P2-7 | `src/modbus_master.c:158-219` | 重复 init 泄漏上一次的 libmodbus 上下文 |
| P2-8 | `src/sensor.c:260` | `g_inject_counter` 不随 init/close 复位（影响多次 init 场景） |
| P2-9 | `src/config.c` 十余处 | `strncpy(buf, v, sizeof(buf)-1)` 不保证 NUL 结尾 |
| P2-10 | `src/storage.c:58` | `SQLITE_STATIC` 依赖调用方生命周期约定（改异步需重审） |
| P2-11 | `src/common.h` + `src/modbus_master.c:179` | `parity[2]` 空值时传 `'\0'` 给 libmodbus，行为依赖实现 |
| P2-12 | `src/anomaly_engine.c:114-135` | `tree_path_length` 无 `IFOREST_MAX_NODES` 越界防护 |
| P2-13 | `src/ota.c:378-409` | `ota_report_status` snprintf offset 累加无溢出防护 |
| P2-14 | `src/main.c:453-454` | 工作线程未 `pthread_sigmask` 屏蔽信号 |
| P2-15 | `src/daemon.c:14-40` | 守护化未设 umask(0)、未关继承 fd、未忽略 SIGPIPE |
| P2-16 | `src/mqtt_client.c:65` | OTA topic 子串匹配偏宽松 |
| P2-17 | `src/mqtt_client.c:187-201` | `mqtt_set_will` 死代码；keepalive 60s 硬编码不可配置 |
| P2-18 | `src/ota.c:516-518, 628-647` | 仅 IPv4 解析；`EVP_DigestFinal_ex` 返回值未检查 |

## 四、测试覆盖缺口（修复时需一并补齐）

1. **HIGH**：`test_rule_engine_rate` —— rate 算子真正触发路径（现有测试只验证"首次不触发"）
2. **HIGH**：`test_storage_init_fail` —— DB 只读/损坏路径
3. **HIGH**：`test_gpio_hal` —— GPIO 完全无测试
4. **MED**：`test_modbus_runtime` —— mock 模式 poll 行为；`BUILD_WITH_MODBUS` 双编译路径
5. **MED**：`test_anomaly_stdzero` —— std=0 退化分支；异常值入窗行为
6. **MED**：`test_http_request_parse` —— 超长方法名/超长路径（验证 P0-1 修复）
7. **LOW**：storage 并发正确性

## 五、建议修复顺序

```
第一批（止血）：P0-2 daemon → P0-1 HTTP溢出 → P0-4 丢数据
第二批（安全）：P0-5 OTA签名 → P1-6 token → P1-5 超时
第三批（可靠）：P0-3 回滚 → P1-11 fsync → P1-1 double fclose → P1-4 reg_count
第四批（正确性）：P1-2/P1-3 并发 → P1-7/P1-8 JSON → P1-9/P1-10 → 回滚 P1-12 注释
第五批（硬化+补测试）：全部 P2 + 测试缺口
```

## 附：判定口径说明

- 用户自查 10 项经复核：7 项完全确认、2 项部分成立（P1-7 触发条件苛刻、P2-4 属设计弱点）、0 项证伪。
- 其中 4 项（P0-2、P1-2、P0-4、P1-4）为团队首轮审查漏报、由用户自查发现。
