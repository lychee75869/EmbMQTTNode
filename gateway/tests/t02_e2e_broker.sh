#!/usr/bin/env bash
# tests/t02_e2e_broker.sh
# 真实 broker 端到端验证（手动集成，非 CI 单测）。
# 依赖：WSL/Ubuntu 已安装 mosquitto(-sub/-pub)。
#
# 场景：启动 → 连接成功（online + 重订 OTA）→ 杀 broker（断网，入队）
#       → 重启 broker（自动重连 → 再 online + 重订 OTA → 补发 pending）。
set -u

cd "$(dirname "$0")"
PORT=18830
BIN=t02_e2e_broker

echo "== [E2E] building harness =="
cc -std=c11 -Wall -Wextra -g -pthread -I../src -D_DEFAULT_SOURCE -DBUILD_WITH_MODBUS \
   t02_e2e_broker.c ../src/platform.c ../src/platform_local.c \
   ../src/mqtt_client.c ../src/ota.c ../src/storage.c ../src/config.c \
   ../src/sensor_fields.c -o "$BIN" \
   -pthread -lmosquitto -lsqlite3 -lm -lcrypto -lssl -lmodbus || exit 1

: > harness.log
: > sub.log

echo "== [E2E] starting broker on :$PORT =="
mosquitto -p "$PORT" >boker.log 2>&1 &
MPID=$!
sleep 1

echo "== [E2E] starting subscriber =="
mosquitto_sub -h 127.0.0.1 -p "$PORT" -t 'embmqttnode/#' -v >sub.log 2>&1 &
SPID=$!
sleep 1

echo "== [E2E] starting harness (20s window) =="
./"$BIN" >harness.log 2>&1 &
HPID=$!

# ---- phase 1: online + subscribe (t≈5s) ----
sleep 5
echo "-- phase1 (after initial connect) --"

# ---- phase 2: kill broker => disconnect + offline enqueue ----
echo "== [E2E] killing broker (simulate network loss) =="
kill "$MPID" 2>/dev/null
sleep 6
echo "-- phase2 (after broker killed) --"

# ---- phase 3: restart broker => reconnect + resubscribe + replay ----
echo "== [E2E] restarting broker (simulate recovery) =="
mosquitto -p "$PORT" >>boker.log 2>&1 &
MPID=$!

wait "$HPID"

# ---- cleanup ----
kill "$SPID" 2>/dev/null
kill "$MPID" 2>/dev/null
sleep 1

echo "== [E2E] harness.log =="
cat harness.log
echo "== [E2E] sub.log (received by subscriber) =="
cat sub.log
echo "== [E2E] broker.log =="
cat boker.log
