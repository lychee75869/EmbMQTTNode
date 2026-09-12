# EmbMQTTNode Bug 台账（2026-09-12 重整版）

> 前史：2026-09-07 全量整合 35 项（团队双线审查 + 用户自查复核）。
> 本版变化：删去 v1.2.2~v1.2.9 已闭环项（含 v1.2.8 的 P1-13/P2-19、v1.2.9 的 P0-5/P1-14）；并入 2026-09-12 用户第二批 8 项疑点核实结果（5 项台账遗漏新发现、1 项自 P2 升级 P1）。
> 维护约定：每次 bug 修复经用户确认 commit 后，同步删去已闭环项、移入附录闭环记录表。
> 所有判定均对照现行源码核实（引用行号），非仅看 commit message。
> 严重度：P0 = 崩溃/安全/核心功能失效；P1 = 功能错误/可靠性；P2 = 隐患/可维护性。

## 一、P0 — 必须立即修复（0 项）

无。全部闭环（P0-5 OTA 签名+HTTPS 已于 v1.2.9 修复）。

## 二、P1 — 尽快修复（9 项）

| # | 位置 | 问题 | 修复方向 |
|---|------|------|---------|
| P1-2 | `src/rule_engine.c`、`src/anomaly_engine.c`、`main.c` 评估调用点 | **规则/异常引擎共享状态数据竞争**：modbus.enabled=1 时 sample_thread 与 modbus_thread 并发调用两个 evaluate，无锁读写 `rate_history`、`window`、`last_triggered`、`g_stats`（C11 UB）。http_thread 读 g_stats 也构成读写竞争。 | 两引擎各加一把 mutex；或架构上串行化。可与"判定框架重构"一并做。 |
| P1-3 | `src/mqtt_client.c:17`、`src/ota.c:61-69` 等 | **跨线程共享变量无同步**：`g_connected`、`g_ota_cb`、`g_mqtt_publish` 被 MQTT 后台线程与业务线程并发读写。2026-09-12 具体化：OTA 状态机组 `g_state/g_target_version/g_download_url/g_expected_checksum` 由 MQTT 网络线程（handle_message→parse 先 memset 后填充）与 upload 线程（check_and_handle 读+改写）并发访问，可读到半写状态。 | C11 `_Atomic` 或 mutex；OTA 状态机组加锁保护。 |
| P1-4 | `src/modbus_master.c:246-255` + `src/config.c` | **Modbus 配置无输入校验**：reg_count 无 `1~32` 上限校验，`reg_buf[32]` 栈缓冲可被写穿；且 `reg_addr - 40001 / - 30001` 无下界校验，0 基地址配置得到大负数传入 libmodbus。 | config 层双重校验（reg_count 范围 + reg_addr 下界）+ 调用前防御性检查。 |
| P1-5 | `src/http_server.c:101-118, 799-822` | **HTTP 无 recv 超时（Slowloris DoS）**：client_fd 未设 SO_RCVTIMEO，慢速攻击可永久阻塞 HTTP 线程，且导致退出时 `pthread_join` 死锁、进程关不掉。 | client_fd 设 SO_RCVTIMEO=10s。 |
| P1-6 | `src/http_server.c:472-502` | **重启接口认证形同虚设**：硬编码 token `"reboot123"` 明文传输（无 TLS）、`strstr` 子串匹配可绕过、`system("reboot")` 可被 PATH 劫持、无限速无 CSRF。 | token 改配置 + 常量时间比较；`reboot(RB_AUTOBOOT)` 替代 system()；加限速。 |
| P1-7 | `src/ota.c` 指令解析 | **OTA 指令 JSON 解析忽略 payload_len**：`(void)len` 后全程 strstr，依赖 NUL 结尾（libmosquitto 不保证）。触发条件苛刻，但违反安全编码。 | 引入 cJSON/jansson 正式解析；或 on_message 先拷贝补 NUL。 |
| P1-8 | `src/ota.c` 指令解析 | **"cmd" 字段未校验值**：`strstr("cmd") && strstr("upgrade")` 双子串检查，`{"cmd":"foo","note":"upgrade"}` 也能通过。 | 真正的 JSON 解析（与 P1-7 一并解决）。 |
| P1-9 | `src/mqtt_client.c:210/249` | **状态上报 JSON 载荷可能截断**：`payload[512]` 固定，hostname+cpu_model+kernel_ver+mac 等字段累加逼近上限，截断产生无效 JSON。 | 动态分配或拆分消息。 |
| P1-10 | `src/main.c` upload_thread | **OTA 下载阻塞整个上传线程**：`ota_http_download` 在 upload_thread 同步执行，下载期间离线缓存堆积无法上传。 | 独立 OTA worker 线程。 |

## 三、P2 — 计划性硬化（19 项）

| # | 位置 | 问题 |
|---|------|------|
| P2-3 | `src/http_server.c:319-323, 476-483` | HTTP 线程退出未优雅收尾现存连接 |
| P2-4 | `src/anomaly_engine.c` 异常值入窗 | 异常值无条件入窗污染后续 Z-score 基线（有意设计但属算法弱点）；冷却期窗口冻结 |
| P2-5 | `src/rule_engine.c:52, 67` | `rate_history` 魔数 16 硬编码（应定义 `RULE_RATE_WINDOW` 宏） |
| P2-6 | `src/gpio_hal.c:79-91` | 真实硬件分支缺 value 合法性校验（mock 分支有） |
| P2-7 | `src/modbus_master.c:158-219` | 重复 init 泄漏上一次的 libmodbus 上下文 |
| P2-8 | `src/sensor.c:260` | `g_inject_counter` 不随 init/close 复位（影响多次 init 场景） |
| P2-9 | `src/config.c` 十余处 | `strncpy(buf, v, sizeof(buf)-1)` 不保证 NUL 结尾 |
| P2-10 | `src/storage.c:58` | `SQLITE_STATIC` 依赖调用方生命周期约定（改异步需重审） |
| P2-11 | `src/common.h` + `src/modbus_master.c:179` | `parity[2]` 空值时传 `'\0'` 给 libmodbus，行为依赖实现 |
| P2-12 | `src/anomaly_engine.c:114-135` | `tree_path_length` 无 `IFOREST_MAX_NODES` 越界防护 |
| P2-13 | `src/ota.c` ota_report_status | snprintf offset 累加无溢出防护 |
| P2-14 | `src/main.c:453-454` | 工作线程未 `pthread_sigmask` 屏蔽信号 |
| P2-16 | `src/mqtt_client.c:65` | OTA topic 子串匹配偏宽松 |
| P2-17 | `src/mqtt_client.c:187-201` | `mqtt_set_will` 死代码；keepalive 60s 硬编码不可配置 |
| P2-18 | `src/ota.c` | 仅 IPv4 解析；`EVP_DigestFinal_ex` 返回值未检查（注：v1.2.9 重写传输层后部分情况可能已覆盖，修复时先核对） |
| P2-20 | `src/rule_engine.c`/`src/anomaly_engine.c` vs `src/http_server.c`（新发现） | last_triggered 时钟语义不一致：引擎写 CLOCK_MONOTONIC 毫秒，HTTP 层当 Unix 时间戳输出 `last_triggered_ms`，前端显示错误（功能正确，错在展示层） |
| P2-21 | `src/main.c:484-491`（新发现） | `http_arg` 块作用域栈变量传线程，严格 UB，当前靠 main 阻塞在 pthread_join 侥幸成立 |
| P2-22 | `src/modbus_master.c:249-255`（新发现） | Modbus 地址约定按 40001/30001 起算，0 基地址配置下溢为大负数，无校验（并入 P1-4 一起修） |

## 四、测试覆盖缺口（修复时需一并补齐）

1. **HIGH**：`test_rule_engine_rate` —— rate 算子真正触发路径（现有测试只验证"首次不触发"）
2. **HIGH**：`test_storage_init_fail` —— DB 只读/损坏路径
3. **HIGH**：`test_gpio_hal` —— GPIO 完全无测试
4. **MED**：`test_modbus_runtime` —— mock 模式 poll 行为；`BUILD_WITH_MODBUS` 双编译路径
5. **MED**：`test_anomaly_stdzero` —— std=0 退化分支；异常值入窗行为
6. **MED**：`test_http_request_parse` —— 超长方法名/超长路径（验证 P0-1 修复）
7. **LOW**：storage 并发正确性

## 五、非阻塞遗留（严过关 QA 体检 + 工程师观察，攒批处理）

1. `embmqttnode.service` ExecReload 缺 SIGHUP handler（现为硬重启语义）
2. boot_count 文件被破坏为非数字时 `atoi=0` 误判已确认（建议 strspn 预校验）
3. `test_confirm_clears_counter` sleep(2) 偏紧改 sleep(3)；mkdtemp 加 atexit 兜底
4. `src/ota.h:57-63` 注释略过期；`docs/` 历史残留 2 处
5. `ota_init` 首次补写 current_slot 的返回值被忽略（现有 LOG_ERROR 运行时兜底，后果轻微）
6. `tests/test_mac_addr` 二进制未被 .gitignore 覆盖（v1.2.5 遗留，应补 .gitignore 条目）
7. `test_anomaly_engine.c:216`、`test_mac_addr.c:65` 两个预先存在的编译警告（missing-field-initializers / format-truncation）

## 六、建议修复顺序（2026-09-12 更新）

```
已完成：
  第一批（止血）：P0-2 → P0-1 → P0-4            [v1.2.2 / v1.2.3 / v1.2.4]
  第三批（可靠）：P0-3+P1-11 → P1-1 → P1-4*      [v1.2.6 / v1.2.5]（*P1-4 未做）
  台账外：OTA fail-safe 5 处 + confirm 顺序        [v1.2.7]
  订阅生命周期：P1-13 + P2-19                     [v1.2.8]
  安全批次：P0-5（签名+HTTPS 双管齐下）+ P1-14 连带 [v1.2.9] ← P0 清零

下一批（进行中）：
  P1-6 reboot 认证 → P1-5 HTTP 超时（同文件 http_server.c，各一 commit）
再后：
  P1-4+P2-22 Modbus 校验 → P1-7/P1-8 JSON 解析
  P1-2/P1-3 并发（建议与"判定框架重构"合并做，一次加锁）
最后：
  P1-9/P1-10 + 全部 P2 + 测试缺口 + 非阻塞遗留
```

## 附：已闭环记录（v1.2.2 ~ v1.2.9）

| 原编号 | 内容 | commit |
|--------|------|--------|
| P0-1 | HTTP 请求行栈溢出 | v1.2.3 (07d753f) |
| P0-2 | daemon 模式 MQTT 线程丢失（连带消解 P2-15） | v1.2.2 (4a5a68c) |
| P0-3 | OTA 回滚失效（连带 P1-11 fsync 原子写） | v1.2.6 (eb30fd2) |
| P0-4 | 断网续传反丢数据（连带 P2-2 source 列 migration） | v1.2.4 (a22fef3) |
| P1-1 | get_mac double fclose | v1.2.5 (abc4230) |
| P1-11 | 关键文件无 fsync | v1.2.6 (eb30fd2) |
| P1-12 | common.h 注释 | 现行源码已正确，判定不成立，闭环 |
| 台账外 | OTA 健康指示器 fail-safe：5 处返回值未检查 + confirm 顺序颠倒 + 写函数内部静默失败 | v1.2.7 (d09b93a) |
| P1-13 | OTA 订阅断网重连永久丢失（on_connect 驱动重订阅+重发 online；连带 P2-19 启动竞态，usleep 定时猜测删除） | v1.2.8 (ee8af84) |
| P0-5 | OTA 明文 HTTP 下载 + 无签名（固件签名 + HTTPS 双管齐下，fail-closed；连带 P1-14 头/体切分累积缓冲重写） | v1.2.9 (a3e3603) |
