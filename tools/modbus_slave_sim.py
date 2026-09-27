#!/usr/bin/env python3
"""
Modbus Slave Simulator（TCP / RTU）—— 与 8 寄存器契约对齐
=========================================================
模拟一台「浇花设备」从站，用于在无真实硬件时验证 EmbMQTTNode 南向链路。
寄存器契约见 docs/EmbMQTTNode_项目文档.md「模块一 · 子设备」：

  协议地址  40001+  名称              读写  类型/换算            说明
  ------------------------------------------------------------------------
  0        40001    土壤湿度           读    uint16 ÷ 10  → %      0-100.0%
  1        40002    空气温度           读    int16  ÷ 10  → ℃      可负（补码）
  2        40003    空气湿度           读    uint16 ÷ 10  → %RH    DHT11 整数分辨率
  3        40004    水槽水位           读    uint16 ÷ 10  → %      0-100.0%
  4        40005    电池电压           读    uint16 ÷ 100 → V      单体锂电 3.3-4.2V
  5        40006    浇水秒数           读写  uint16 → s            写=下发浇水时长
  6        40007    泵速               读写  uint16 → %            0-100
  7        40008    状态字             读    位域                  见下方 STATUS_*

状态字位定义：
  bit0 水泵运行中  bit1 水槽缺水  bit2 土壤湿度传感器故障
  bit3 电池低电量  bit4 自动模式使能  bit5-15 保留

设计要点：
  * 量测寄存器（偏移 0-4、7）在**读时**按物理模型刷新；
    设定寄存器（偏移 5、6）由**写事件**捕获并驱动状态机——
    读时不覆盖，否则网关写入的浇水命令会被立刻抹掉。
  * 物理模型：浇水→土壤湿度上升/水箱下降；平时蒸发；电池缓慢下降；
    DHT11 量化到 1℃/1%RH（与真实传感器一致，读数常见「整十」值属正常）。
  * 时间节流（0.5s）：网关每条映射发一次 03 请求，8 条即 8 次读，
    不节流会导致数值一轮内跳变 8 次。

依赖：pip install pymodbus

用法：
  python tools/modbus_slave_sim.py                       # TCP 127.0.0.1:1502
  python tools/modbus_slave_sim.py --port 502            # 用 502 需 root 权限
  # RTU（配合 socat 虚拟串口对，验证最终串口链路）：
  socat -d -d pty,raw,echo=0,link=/tmp/ttyGW pty,raw,echo=0,link=/tmp/ttySIM &
  python tools/modbus_slave_sim.py --rtu /tmp/ttySIM --baudrate 9600
  #   网关侧对应：modbus_mode = rtu / modbus_serial_port = /tmp/ttyGW
"""

import argparse
import random
import time

from pymodbus.server import StartTcpServer
from pymodbus.datastore import ModbusSequentialDataBlock
from pymodbus.datastore import ModbusSlaveContext, ModbusServerContext

# ── 协议地址偏移（40001 → 0）─────────────────────────────
IDX_SOIL, IDX_AIR_T, IDX_AIR_H, IDX_TANK, IDX_BATT = 0, 1, 2, 3, 4
IDX_WATER_SEC, IDX_PUMP_SPD, IDX_STATUS = 5, 6, 7
REG_COUNT = 8

# ── 状态字位定义 ────────────────────────────────────────
STATUS_PUMP_RUN = 1 << 0
STATUS_TANK_LOW = 1 << 1
STATUS_SOIL_FAULT = 1 << 2
STATUS_BATT_LOW = 1 << 3
STATUS_AUTO_MODE = 1 << 4

# ── 物理模型参数（按需调）───────────────────────────────
TICK_SEC = 0.5            # 推进节流
SOIL_RISE_PER_S = 1.5     # 浇水时土壤湿度上升速率 %/s（泵速 100% 时）
SOIL_DRY_PER_S = 0.05     # 平时蒸发速率 %/s
TANK_DROP_PER_S = 0.4     # 浇水时水箱消耗速率 %/s（泵速 100% 时）
BATT_DROP_PER_S = 0.0005  # 电池自放电 V/s（模拟器不做充电）
TANK_LOW_PCT = 5.0        # 水槽缺水阈值 %
BATT_LOW_V = 3.30         # 电池低电阈值 V
SOIL_DEGRADE_AT = 95.0    # 土壤湿度超过该值后视为传感器故障（演示 bit2）


class PlantSim:
    """浇花设备物理模型：量测 + 执行器 + 状态字"""

    def __init__(self):
        self.soil = 45.0        # %
        self.air_t = 25.0       # ℃
        self.air_h = 55.0       # %RH
        self.tank = 80.0        # %
        self.batt = 3.85        # V
        self.pump_speed = 100   # %
        self.remain_s = 0.0     # 剩余浇水秒数（40006 读回值）
        self._last = time.monotonic()

    # ── 物理推进（按真实时间节流，避免一次轮询内跳变多次）──
    def tick(self):
        now = time.monotonic()
        dt = now - self._last
        if dt < TICK_SEC:
            return
        self._last = now

        pumping = self.remain_s > 0.0
        if pumping:
            self.remain_s = max(0.0, self.remain_s - dt)
            load = self.pump_speed / 100.0
            self.soil += SOIL_RISE_PER_S * load * dt
            self.tank -= TANK_DROP_PER_S * load * dt
        else:
            self.soil -= SOIL_DRY_PER_S * dt

        self.batt -= BATT_DROP_PER_S * dt
        # 环境量缓慢漂移（DHT11 在读数时量化到整数）
        self.air_t += random.gauss(0, 0.05)
        self.air_h += random.gauss(0, 0.10)
        self._clamp()

    def _clamp(self):
        self.soil = min(100.0, max(0.0, self.soil))
        self.tank = min(100.0, max(0.0, self.tank))
        self.air_t = min(40.0, max(-10.0, self.air_t))
        self.air_h = min(95.0, max(10.0, self.air_h))
        self.batt = min(4.20, max(2.90, self.batt))

    # ── 写寄存器 → 执行器 ────────────────────────────────
    def start_watering(self, seconds):
        self.remain_s = float(max(0, min(65535, int(seconds))))

    def set_pump_speed(self, pct):
        self.pump_speed = max(0, min(100, int(pct)))

    # ── 状态字 ───────────────────────────────────────────
    def status(self):
        st = STATUS_AUTO_MODE                    # 模拟器默认自动模式使能
        if self.remain_s > 0.0:
            st |= STATUS_PUMP_RUN
        if self.tank < TANK_LOW_PCT:
            st |= STATUS_TANK_LOW
        if self.soil >= SOIL_DEGRADE_AT:
            st |= STATUS_SOIL_FAULT
        if self.batt < BATT_LOW_V:
            st |= STATUS_BATT_LOW
        return st

    # ── 量测寄存器（协议格式：定点整数）──────────────────
    def measurements(self):
        return {
            IDX_SOIL:   int(round(self.soil * 10)) & 0xFFFF,
            # DHT11 整数分辨率：整数化后再 ×10，读数末位恒 0 属正常
            IDX_AIR_T:  int(round(round(self.air_t) * 10)) & 0xFFFF,
            IDX_AIR_H:  int(round(round(self.air_h) * 10)) & 0xFFFF,
            IDX_TANK:   int(round(self.tank * 10)) & 0xFFFF,
            IDX_BATT:   int(round(self.batt * 100)) & 0xFFFF,
            IDX_STATUS: self.status(),
        }


class ContractDataBlock(ModbusSequentialDataBlock):
    """保持寄存器块：读刷量测、写驱动执行器"""

    def __init__(self, sim):
        self.sim = sim
        super().__init__(0, [0] * REG_COUNT)

    # 读：只刷新量测与状态字，保留设定寄存器（否则写入被抹掉）
    def getValues(self, address, count=1):
        self.sim.tick()
        for idx, val in self.sim.measurements().items():
            self.values[idx] = val
        return super().getValues(address, count)

    # 写：先落地（读回一致），再驱动状态机
    def setValues(self, address, values):
        super().setValues(address, values)
        for i, v in enumerate(values):
            idx = address + i
            if idx == IDX_WATER_SEC:
                self.sim.start_watering(v)
                print(f"[SIM] 写 40006 = {int(v)} s → 开始浇水"
                      f"（泵速 {self.sim.pump_speed}%）", flush=True)
            elif idx == IDX_PUMP_SPD:
                self.sim.set_pump_speed(v)
                print(f"[SIM] 写 40007 = {int(v)} % → 泵速更新", flush=True)


def main():
    ap = argparse.ArgumentParser(
        description="EmbMQTTNode Modbus Slave Simulator（8 寄存器契约）")
    ap.add_argument("--host", default="127.0.0.1", help="TCP 监听地址")
    ap.add_argument("--port", type=int, default=1502,
                    help="TCP 监听端口（默认 1502；502 为特权端口需 root）")
    ap.add_argument("--rtu", metavar="DEVICE", default=None,
                    help="改用 RTU 模式，例如 /dev/ttyUSB0 或 socat 的 /tmp/ttySIM")
    ap.add_argument("--baudrate", type=int, default=9600, help="RTU 波特率")
    ap.add_argument("--slave-id", type=int, default=1, help="从站地址（默认 1）")
    args = ap.parse_args()

    sim = PlantSim()
    store = ModbusSlaveContext(hr=ContractDataBlock(sim))
    context = ModbusServerContext(slaves={args.slave_id: store}, single=False)

    print("=" * 62)
    print(" EmbMQTTNode Modbus Slave Simulator（8 寄存器契约）")
    print("=" * 62)
    if args.rtu:
        print(f" 模式:       RTU  设备={args.rtu}  波特率={args.baudrate}")
    else:
        print(f" 模式:       TCP  {args.host}:{args.port}")
    print(f" 从站地址:   {args.slave_id}")
    print(" 寄存器:     40001 土壤湿度×10 | 40002 空气温度×10(int16) | "
          "40003 空气湿度×10")
    print("             40004 水槽水位×10 | 40005 电池电压×100 | "
          "40006 浇水秒数(读写)")
    print("             40007 泵速%(读写) | 40008 状态字(bit0泵/bit1缺水/"
          "bit2湿度故障/bit3低电/bit4自动)")
    print("-" * 62)
    print(" node.conf 映射示例（5 条量测；40006-40008 待写路径实现后再映射）：")
    for i, (addr, field, scale) in enumerate([
            (40001, "soil_moisture", "0.1"), (40002, "temperature", "0.1"),
            (40003, "humidity", "0.1"), (40004, "water_level", "0.1"),
            (40005, "battery_voltage", "0.01")], start=1):
        print(f"   modbus_reg_{i} = {args.slave_id},{addr},1,3,int16,"
              f"{field},{scale},0")
    print("=" * 62)
    print(" Ctrl+C 停止")
    print(flush=True)

    if args.rtu:
        from pymodbus.server import StartSerialServer
        try:                                   # pymodbus >= 3.7
            from pymodbus.framer import FramerType
            framer = FramerType.RTU
        except ImportError:                    # pymodbus 3.0 ~ 3.6
            from pymodbus.transaction import ModbusRtuFramer
            framer = ModbusRtuFramer
        StartSerialServer(context, port=args.rtu, framer=framer,
                          baudrate=args.baudrate, timeout=1)
    else:
        StartTcpServer(context, address=(args.host, args.port))


if __name__ == "__main__":
    main()
